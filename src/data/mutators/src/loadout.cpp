// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The loadout record, its canonical text and the digests taken from it.

#include "oa/data/mutators/loadout.hpp"

#include "oa/data/mod_profile/value.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/clashes.hpp"
#include "oa/data/mutators/pipeline.hpp"
#include "oa/formats/json.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/formats/oamut/manifest.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace oa::data::mutators {
namespace {

namespace fs = std::filesystem;
namespace oamod = oa::formats::oamod;
namespace oamut = oa::formats::oamut;
namespace json = oa::formats::json;
namespace profile = oa::data::mod_profile;

/// Stores a refusal where the caller asked for one.
///
/// @param error the caller's message, or null to drop it
/// @param message what was refused
void set_error(std::string* error, std::string message) {
    if (error != nullptr)
        *error = std::move(message);
}

/// Appends bytes to a digest.
///
/// @param[in,out] hasher the digest
/// @param bytes the bytes
void feed(base::sha256::Hasher& hasher, std::string_view bytes) noexcept {
    base::sha256::update(
        hasher,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size())
    );
}

/// Appends one byte to a digest.
///
/// @param[in,out] hasher the digest
/// @param byte the byte
void feed_byte(base::sha256::Hasher& hasher, uint8_t byte) noexcept {
    base::sha256::update(hasher, std::span<const uint8_t>(&byte, 1));
}

/// Writes a digest as 64 lower-case hex digits.
///
/// @param digest the digest
/// @return the hex text
std::string hex_text(const base::sha256::Digest& digest) {
    const std::array<char, base::sha256::hex_size> digits = base::sha256::to_hex(digest);
    return std::string(digits.begin(), digits.end());
}

/// The JSON kind an option's value is written as.
enum class OptionKind : uint8_t {
    boolean, ///< a toggle
    string,  ///< a string choice
    number,  ///< a number option or a numeric choice
};

/// Returns the JSON kind an option's value is written as.
///
/// @param option the option
/// @return boolean for a toggle, string for a string choice, number otherwise
OptionKind option_kind(const LoadoutOption& option) noexcept {
    if (option.type == oa::formats::oamut::OptionType::toggle)
        return OptionKind::boolean;
    if (option.type == oa::formats::oamut::OptionType::choice && !option.value.number)
        return OptionKind::string;
    return OptionKind::number;
}

/// Returns an option's canonical JSON value.
///
/// A toggle is a boolean. A string choice is a string. A number, and a
/// numeric choice, are numbers.
///
/// @param option the option
/// @return the value
profile::Value option_json(const LoadoutOption& option) {
    switch (option_kind(option)) {
    case OptionKind::boolean:
        return profile::make_boolean(option.toggle);
    case OptionKind::string:
        return profile::make_string(option.value.text);
    case OptionKind::number:
        break;
    }
    return profile::make_number(option.value.number_value);
}

/// Returns one entry's canonical JSON object.
///
/// Every option is a member. Two options with one id are both written.
///
/// @param entry the entry
/// @return the object
profile::Value entry_json(const LoadoutEntry& entry) {
    profile::Value options = profile::make_map();
    options.members.reserve(entry.options.size());
    for (const LoadoutOption& option : entry.options)
        options.members.push_back(profile::Member{option.id, option_json(option)});

    profile::Value object = profile::make_map();
    profile::set_member(
        object, "clashes", profile::make_string(std::string{oamut::clash_mode_name(entry.clashes)})
    );
    profile::set_member(object, "folder", profile::make_boolean(entry.folder));
    profile::set_member(object, "id", profile::make_string(entry.id));
    profile::set_member(object, "options", std::move(options));
    profile::set_member(object, "sha256", profile::make_string(hex_text(entry.package)));
    profile::set_member(object, "version", profile::make_string(entry.version));
    return object;
}

/// Tells whether a key is one an entry object may hold.
///
/// @param key the key
/// @return true for clashes, folder, id, options, sha256 and version
bool entry_key(std::string_view key) noexcept {
    return key == "clashes" || key == "folder" || key == "id" || key == "options" ||
           key == "sha256" || key == "version";
}

/// Tells whether a text is 64 lower-case hex digits.
///
/// @param text the text
/// @return true when it is a lower-case digest
bool lower_hex(std::string_view text) noexcept {
    if (text.size() != base::sha256::hex_size)
        return false;
    for (const char digit : text) {
        const bool decimal = digit >= '0' && digit <= '9';
        const bool letter = digit >= 'a' && digit <= 'f';
        if (!decimal && !letter)
            return false;
    }
    return true;
}

/// Finds one member, and reports a second of the same name.
///
/// @param object the object
/// @param key the member name
/// @param[out] duplicate set when the name occurs twice
/// @return the first member, or null when there is none
const json::Json* find_member(const json::Json& object, std::string_view key, bool& duplicate) {
    const json::Json* found = nullptr;
    const std::span<const std::string> names = object.names();
    const std::span<const json::Json> values = object.values();
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (names[index] != key)
            continue;
        if (found != nullptr) {
            duplicate = true;
            return found;
        }
        found = &values[index];
    }
    return found;
}

/// Reads one option member.
///
/// @param name the option id
/// @param value the JSON value
/// @param[out] option the option
/// @param[out] error the refusal; may be null
/// @return true when the value is a boolean, a canonical number or a string
bool read_option(
    std::string_view name, const json::Json& value, LoadoutOption& option, std::string* error
) {
    option = LoadoutOption{};
    option.id = std::string{name};
    if (value.type() == json::JsonType::boolean) {
        option.type = oa::formats::oamut::OptionType::toggle;
        option.toggle = value.boolean().value_or(false);
        return true;
    }
    if (value.type() == json::JsonType::string) {
        option.type = oa::formats::oamut::OptionType::choice;
        const std::string* text = value.string();
        option.value.text = text == nullptr ? std::string{} : *text;
        return true;
    }
    if (value.type() == json::JsonType::number) {
        const std::string* written = value.number_text();
        oamod::Number number{};
        if (written == nullptr ||
            oamod::parse_number(*written, number) != oamod::NumberStatus::ok ||
            oamod::canonical_number_text(number) != *written) {
            set_error(error, "option " + std::string{name} + " is not a canonical number");
            return false;
        }
        option.type = oa::formats::oamut::OptionType::number;
        option.value.number = true;
        option.value.number_value = number;
        option.value.text = *written;
        return true;
    }
    set_error(error, "option " + std::string{name} + " is not a toggle, number or choice");
    return false;
}

/// Reads one mutator object.
///
/// @param value the JSON value
/// @param index the mutator's place in the list, from 0
/// @param[out] entry the entry
/// @param[out] error the refusal; may be null
/// @return true when the object is one canonical entry
bool read_entry(
    const json::Json& value, std::size_t index, LoadoutEntry& entry, std::string* error
) {
    const std::string place = "mutators[" + std::to_string(index) + "]";
    if (value.type() != json::JsonType::object) {
        set_error(error, place + " is not an object");
        return false;
    }
    for (const std::string& key : value.names()) {
        if (!entry_key(key)) {
            set_error(error, place + " has unknown key " + key);
            return false;
        }
    }

    bool duplicate = false;
    const json::Json* clashes = find_member(value, "clashes", duplicate);
    const json::Json* folder = find_member(value, "folder", duplicate);
    const json::Json* id = find_member(value, "id", duplicate);
    const json::Json* options = find_member(value, "options", duplicate);
    const json::Json* sha = find_member(value, "sha256", duplicate);
    const json::Json* version = find_member(value, "version", duplicate);
    if (duplicate) {
        set_error(error, place + " repeats a key");
        return false;
    }
    if (clashes == nullptr || folder == nullptr || id == nullptr || options == nullptr ||
        sha == nullptr || version == nullptr) {
        set_error(error, place + " lacks clashes, folder, id, options, sha256 or version");
        return false;
    }
    if (clashes->type() != json::JsonType::string || clashes->string() == nullptr) {
        set_error(error, place + " clashes is not a string");
        return false;
    }
    const std::optional<oamut::ClashMode> clash_mode = oamut::parse_clash_mode(*clashes->string());
    if (!clash_mode.has_value()) {
        set_error(error, place + " clashes is not add or replace");
        return false;
    }
    if (folder->type() != json::JsonType::boolean) {
        set_error(error, place + " folder is not a boolean");
        return false;
    }
    if (id->type() != json::JsonType::string || version->type() != json::JsonType::string) {
        set_error(error, place + " id or version is not a string");
        return false;
    }
    if (sha->type() != json::JsonType::string || sha->string() == nullptr ||
        !lower_hex(*sha->string())) {
        set_error(error, place + " sha256 is not lower-case hexadecimal");
        return false;
    }
    if (options->type() != json::JsonType::object) {
        set_error(error, place + " options is not an object");
        return false;
    }

    entry = LoadoutEntry{};
    entry.clashes = *clash_mode;
    entry.folder = folder->boolean().value_or(false);
    entry.id = id->string() == nullptr ? std::string{} : *id->string();
    entry.version = version->string() == nullptr ? std::string{} : *version->string();
    const std::optional<base::sha256::Digest> digest = base::sha256::parse_hex(*sha->string());
    if (!digest) {
        set_error(error, place + " sha256 is not lower-case hexadecimal");
        return false;
    }
    entry.package = *digest;

    const std::span<const std::string> names = options->names();
    const std::span<const json::Json> values = options->values();
    entry.options.reserve(names.size());
    for (std::size_t option_index = 0; option_index < names.size(); ++option_index) {
        for (std::size_t earlier = 0; earlier < option_index; ++earlier) {
            if (names[earlier] == names[option_index]) {
                set_error(error, "duplicate option " + names[option_index]);
                return false;
            }
        }
        LoadoutOption option;
        if (!read_option(names[option_index], values[option_index], option, error))
            return false;
        entry.options.push_back(std::move(option));
    }
    return true;
}

/// Folds a resource path: '\\' becomes '/' and ASCII letters lose their case.
///
/// @param text the path or mask
/// @return the folded path
std::string fold_path(std::string_view text) {
    std::string folded;
    folded.reserve(text.size());
    for (unsigned char character : text) {
        if (character == '\\')
            character = '/';
        else if (character >= 'A' && character <= 'Z')
            character = static_cast<unsigned char>(character - 'A' + 'a');
        folded.push_back(static_cast<char>(character));
    }
    return folded;
}

/// Returns a path's UTF-8 spelling with '/' between its parts.
///
/// The narrow spelling is in the ANSI code page on Windows, which cannot
/// hold every name and differs from the spelling elsewhere.
///
/// @param path the path
/// @return its UTF-8 spelling
std::string utf8_text(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

/// Returns a path's form relative to a folder, in UTF-8 with '/' between parts.
///
/// @param folder the folder
/// @param file a file inside it, or the folder itself
/// @return the relative path, or the folder's own text when there is none
std::string relative_text(const fs::path& folder, const fs::path& file) {
    const std::string text = utf8_text(file.lexically_relative(folder));
    if (text.empty() || text == ".")
        return utf8_text(folder);
    return text;
}

/// Tells whether a path is a reparse point.
///
/// symlink_status can report a junction as a directory. The attribute is the
/// check that still sees it.
///
/// @param path the host path
/// @return true when the path is a reparse point
bool reparse_point(const fs::path& path) {
#if defined(_WIN32)
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    (void)path;
    return false;
#endif
}

/// What one path of a folder walk is.
enum class FolderItem : uint8_t {
    missing, ///< nothing is there, or its type cannot be told
    link,    ///< a symbolic link, a junction or another reparse point
    folder,  ///< a folder that is not a link
    file,    ///< a regular file that is not a link
    other,   ///< a pipe, a socket, a device or anything else
};

/// Tells what one path of a folder walk is.
///
/// A junction is a reparse point, so the attribute finds it whatever type
/// the library gives it.
///
/// @param status the path's symlink status
/// @param path the host path
/// @return what the path is
FolderItem folder_item(const fs::file_status& status, const fs::path& path) {
    if (status.type() == fs::file_type::none || status.type() == fs::file_type::not_found)
        return FolderItem::missing;
    if (fs::is_symlink(status) || reparse_point(path))
        return FolderItem::link;
    if (fs::is_directory(status))
        return FolderItem::folder;
    if (fs::is_regular_file(status))
        return FolderItem::file;
    return FolderItem::other;
}

/// Reads a regular file's size and digest.
///
/// @param path the host path
/// @param[out] size the file's size in bytes
/// @param[out] digest the SHA-256 of its bytes
/// @return true when the file was read
bool digest_file(const fs::path& path, uint64_t& size, base::sha256::Digest& digest) {
    std::error_code failure;
    const uintmax_t bytes = fs::file_size(path, failure);
    if (failure)
        return false;
    size = static_cast<uint64_t>(bytes);
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return false;
    base::sha256::Hasher hasher;
    std::array<uint8_t, 8192> buffer{};
    while (stream) {
        stream.read(
            reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size())
        );
        const std::streamsize count = stream.gcount();
        if (count > 0) {
            base::sha256::update(
                hasher, std::span<const uint8_t>(buffer.data(), static_cast<std::size_t>(count))
            );
        }
    }
    if (stream.bad())
        return false;
    digest = base::sha256::finish(hasher);
    return true;
}

/// One file a folder digest names, before the files are sorted.
struct FolderFile {
    /// The `/`-separated path relative to the folder.
    std::string path;
    /// The file's size in bytes.
    uint64_t size{};
    /// The SHA-256 of the file's bytes.
    base::sha256::Digest digest{};
};

/// How a relative path is left out of a folder digest.
///
/// These are the names `oa-tool mutator pack` leaves out of a package. A
/// name that starts with '.' and is not one of them is still hashed.
enum class FolderOmit : uint8_t {
    keep,  ///< the path is part of the walk
    entry, ///< this path is left out; a folder of this name is still walked
    tree,  ///< this path and everything under it are left out
};

/// Tells whether any `/`-separated part of a folded path is `name`.
///
/// @param folded_path the path, already folded, '/' between parts
/// @param name the part
/// @return true when a part is that name
bool part_is(std::string_view folded_path, std::string_view name) {
    std::size_t start = 0;
    while (start < folded_path.size()) {
        const std::size_t slash = folded_path.find('/', start);
        const std::string_view part = slash == std::string_view::npos
                                          ? folded_path.substr(start)
                                          : folded_path.substr(start, slash - start);
        if (part == name)
            return true;
        if (slash == std::string_view::npos)
            return false;
        start = slash + 1;
    }
    return false;
}

/// Says whether a relative path is one the folder digest leaves out.
///
/// `__MACOSX` and `.backup` are left out only as the first part. `.git` and
/// `.oa-origin.yaml` are left out at any depth. `.DS_Store`, in any ASCII
/// case, and a name that starts with `._` are left out at any depth. The
/// comparison folds ASCII case, as the packer does.
///
/// @param relative the path inside the folder, '/' between parts
/// @return how it is left out
FolderOmit folder_omit(std::string_view relative) {
    const std::string folded = fold_path(relative);
    const std::size_t first_slash = folded.find('/');
    const std::string_view first = first_slash == std::string::npos
                                       ? std::string_view(folded)
                                       : std::string_view(folded).substr(0, first_slash);
    if (first == "__macosx" || first == ".backup")
        return FolderOmit::tree;
    if (part_is(folded, ".git") || part_is(folded, ".oa-origin.yaml"))
        return FolderOmit::tree;
    const std::size_t last_slash = folded.rfind('/');
    const std::string_view last = last_slash == std::string::npos
                                      ? std::string_view(folded)
                                      : std::string_view(folded).substr(last_slash + 1);
    if (last == ".ds_store" || last.starts_with("._"))
        return FolderOmit::entry;
    return FolderOmit::keep;
}

} // namespace

bool LoadoutOption::operator==(const LoadoutOption& other) const {
    if (id != other.id)
        return false;
    const OptionKind kind = option_kind(*this);
    if (kind != option_kind(other))
        return false;
    switch (kind) {
    case OptionKind::boolean:
        return toggle == other.toggle;
    case OptionKind::string:
        return value.text == other.value.text;
    case OptionKind::number:
        break;
    }
    return oamod::canonical_number_text(value.number_value) ==
           oamod::canonical_number_text(other.value.number_value);
}

bool LoadoutEntry::operator==(const LoadoutEntry& other) const {
    return profile::canonical_json(entry_json(*this)) == profile::canonical_json(entry_json(other));
}

bool Loadout::operator==(const Loadout& other) const {
    return canonical_loadout(*this) == canonical_loadout(other);
}

LoadoutOption default_option(const oa::formats::oamut::Option& option) {
    LoadoutOption made;
    made.id = option.id;
    made.type = option.type;
    if (option.type == oa::formats::oamut::OptionType::toggle) {
        made.toggle = option.toggle_default;
        return made;
    }
    if (option.type == oa::formats::oamut::OptionType::choice) {
        if (option.default_index < option.values.size())
            made.value = option.values[option.default_index];
        return made;
    }
    made.value.number = true;
    made.value.number_value = option.number_default;
    made.value.text = oamod::canonical_number_text(option.number_default);
    return made;
}

std::string canonical_loadout(const Loadout& loadout) {
    profile::Value mutators = profile::make_list();
    mutators.items.reserve(loadout.entries.size());
    for (const LoadoutEntry& entry : loadout.entries)
        mutators.items.push_back(entry_json(entry));
    profile::Value root = profile::make_map();
    profile::set_member(root, "mutators", std::move(mutators));
    profile::set_member(root, "oaloadout", profile::make_integer(1));
    return profile::canonical_json(root);
}

std::optional<Loadout> parse_loadout(std::string_view text, std::string* error) {
    if (text.size() > loadout_text_most_bytes) {
        set_error(
            error,
            "loadout text is longer than " + std::to_string(loadout_text_most_bytes) + " bytes"
        );
        return std::nullopt;
    }

    json::JsonError json_error;
    const std::optional<json::Json> root = json::parse_json(text, json_error);
    if (!root) {
        if (json_error.message == "text after the value")
            set_error(error, "trailing text after the loadout");
        else
            set_error(error, "loadout text is not json: " + json_error.message);
        return std::nullopt;
    }
    if (root->type() != json::JsonType::object) {
        set_error(error, "loadout text is not an object");
        return std::nullopt;
    }
    for (const std::string& key : root->names()) {
        if (key != "mutators" && key != "oaloadout") {
            set_error(error, "unknown key " + key);
            return std::nullopt;
        }
    }

    bool duplicate = false;
    const json::Json* mutators = find_member(*root, "mutators", duplicate);
    const json::Json* version = find_member(*root, "oaloadout", duplicate);
    if (duplicate) {
        set_error(error, "loadout text repeats a key");
        return std::nullopt;
    }
    if (version == nullptr || version->type() != json::JsonType::number ||
        version->number_text() == nullptr || *version->number_text() != "1") {
        set_error(error, "oaloadout is not 1");
        return std::nullopt;
    }
    if (mutators == nullptr || mutators->type() != json::JsonType::array) {
        set_error(error, "mutators is not an array");
        return std::nullopt;
    }
    const std::span<const json::Json> elements = mutators->elements();
    if (elements.size() > loadout_most_entries) {
        set_error(
            error, "loadout has more than " + std::to_string(loadout_most_entries) + " mutators"
        );
        return std::nullopt;
    }

    Loadout loadout;
    loadout.entries.reserve(elements.size());
    for (std::size_t index = 0; index < elements.size(); ++index) {
        LoadoutEntry entry;
        if (!read_entry(elements[index], index, entry, error))
            return std::nullopt;
        loadout.entries.push_back(std::move(entry));
    }
    if (canonical_loadout(loadout) != text) {
        set_error(error, "loadout text is not canonical");
        return std::nullopt;
    }
    return loadout;
}

base::sha256::Digest loadout_digest(const Loadout& loadout) {
    const std::string text = canonical_loadout(loadout);
    return base::sha256::digest_of(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size())
    );
}

std::optional<base::sha256::Digest> folder_digest(const fs::path& folder, std::string* error) {
    std::error_code failure;
    const fs::file_status root = fs::symlink_status(folder, failure);
    const FolderItem root_item = failure ? FolderItem::missing : folder_item(root, folder);
    if (root_item == FolderItem::missing) {
        set_error(error, utf8_text(folder) + " cannot be read");
        return std::nullopt;
    }
    if (root_item == FolderItem::link) {
        set_error(error, utf8_text(folder) + " is a symbolic link");
        return std::nullopt;
    }
    if (root_item != FolderItem::folder) {
        set_error(error, utf8_text(folder) + " is not a folder");
        return std::nullopt;
    }

    fs::recursive_directory_iterator cursor(folder, fs::directory_options::none, failure);
    if (failure) {
        set_error(error, utf8_text(folder) + " cannot be read");
        return std::nullopt;
    }
    const fs::recursive_directory_iterator end;
    std::vector<FolderFile> files;
    while (cursor != end) {
        const fs::path current = cursor->path();
        const std::string relative = relative_text(folder, current);
        const fs::file_status status = cursor->symlink_status(failure);
        const FolderItem item = failure ? FolderItem::missing : folder_item(status, current);
        if (item == FolderItem::missing) {
            set_error(error, relative + " cannot be read");
            return std::nullopt;
        }
        if (item == FolderItem::link) {
            set_error(error, relative + " is a symbolic link");
            return std::nullopt;
        }
        if (item == FolderItem::other) {
            set_error(error, relative + " is not a regular file");
            return std::nullopt;
        }
        const FolderOmit omit = folder_omit(relative);
        if (omit == FolderOmit::tree && item == FolderItem::folder)
            cursor.disable_recursion_pending();
        if (omit == FolderOmit::keep && item == FolderItem::file) {
            FolderFile file;
            file.path = relative;
            if (!digest_file(current, file.size, file.digest)) {
                set_error(error, relative + " cannot be read");
                return std::nullopt;
            }
            files.push_back(std::move(file));
        }
        cursor.increment(failure);
        if (failure) {
            set_error(error, relative + " cannot be read");
            return std::nullopt;
        }
    }

    std::sort(files.begin(), files.end(), [](const FolderFile& left, const FolderFile& right) {
        return left.path < right.path;
    });
    base::sha256::Hasher hasher;
    for (const FolderFile& file : files) {
        feed(hasher, file.path);
        feed_byte(hasher, 0);
        std::array<uint8_t, 8> encoded{};
        uint64_t remaining = file.size;
        for (uint8_t& encoded_byte : encoded) {
            encoded_byte = static_cast<uint8_t>(remaining & 0xffu);
            remaining >>= 8u;
        }
        base::sha256::update(hasher, encoded);
        base::sha256::update(hasher, file.digest);
    }
    return base::sha256::finish(hasher);
}

namespace {

/// One line for each entry of a folder: its relative path, what it is, and
/// a file's size and last write time, in walk order. No file is opened.
///
/// @param folder the folder
/// @return the lines, or nothing when the walk or an entry cannot be read
std::optional<std::string> folder_stamp(const fs::path& folder) {
    std::error_code failure;
    fs::recursive_directory_iterator cursor(folder, fs::directory_options::none, failure);
    if (failure)
        return std::nullopt;
    std::string stamp;
    for (const fs::recursive_directory_iterator end; cursor != end; cursor.increment(failure)) {
        if (failure)
            return std::nullopt;
        const fs::directory_entry& entry = *cursor;
        const fs::file_status status = entry.symlink_status(failure);
        if (failure)
            return std::nullopt;
        stamp += relative_text(folder, entry.path());
        stamp.push_back('\t');
        stamp += std::to_string(static_cast<int>(status.type()));
        if (fs::is_regular_file(status)) {
            const uintmax_t size = entry.file_size(failure);
            if (failure)
                return std::nullopt;
            const fs::file_time_type written = entry.last_write_time(failure);
            if (failure)
                return std::nullopt;
            stamp.push_back('\t');
            stamp += std::to_string(size);
            stamp.push_back('\t');
            stamp += std::to_string(
                static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(written.time_since_epoch())
                        .count()
                )
            );
        }
        stamp.push_back('\n');
    }
    if (failure)
        return std::nullopt;
    return stamp;
}

} // namespace

std::optional<base::sha256::Digest>
FolderDigests::digest(const fs::path& folder, std::string* error) {
    const std::optional<std::string> stamp = folder_stamp(folder);
    if (stamp) {
        base::threads::LockGuard guard(mutex_);
        const auto found = kept_.find(folder);
        if (found != kept_.end() && found->second.stamp == *stamp)
            return found->second.digest;
    }
    ++reads_;
    const std::optional<base::sha256::Digest> made = folder_digest(folder, error);
    base::threads::LockGuard guard(mutex_);
    if (made && stamp)
        kept_.insert_or_assign(folder, Kept{*stamp, *made});
    else
        kept_.erase(folder);
    return made;
}

base::sha256::Digest layer_digest(
    const oa::MutatorLayerSpec& layer,
    const std::function<std::vector<uint8_t>(std::string_view path)>& served_bytes
) {
    struct RankedPath {
        std::string folded;
        std::size_t index{};
    };

    std::vector<RankedPath> paths;
    paths.reserve(layer.entries.size());
    for (std::size_t index = 0; index < layer.entries.size(); ++index)
        paths.push_back(RankedPath{fold_path(layer.entries[index].path), index});
    std::sort(paths.begin(), paths.end(), [](const RankedPath& left, const RankedPath& right) {
        if (left.folded != right.folded)
            return left.folded < right.folded;
        return left.index < right.index;
    });

    std::vector<std::string> masks;
    masks.reserve(layer.masks.size());
    for (const oa::MutatorMask& mask : layer.masks)
        masks.push_back(fold_path(mask.pattern));
    std::sort(masks.begin(), masks.end());

    base::sha256::Hasher hasher;
    feed(hasher, "oa-mutator-layer-1\n");
    for (const RankedPath& ranked : paths) {
        const oa::MutatorLayerEntry& entry = layer.entries[ranked.index];
        feed(hasher, ranked.folded);
        feed_byte(hasher, 0);
        if (entry.kind == oa::MutatorSourceKind::hidden) {
            feed_byte(hasher, static_cast<uint8_t>('H'));
            continue;
        }
        feed_byte(hasher, static_cast<uint8_t>('F'));
        const std::vector<uint8_t> bytes =
            served_bytes ? served_bytes(entry.path) : std::vector<uint8_t>{};
        const base::sha256::Digest digest = base::sha256::digest_of(bytes);
        base::sha256::update(hasher, digest);
    }
    for (const std::string& pattern : masks) {
        feed_byte(hasher, static_cast<uint8_t>('M'));
        feed(hasher, pattern);
        feed_byte(hasher, 0);
    }
    return base::sha256::finish(hasher);
}

base::sha256::Digest mutator_digest(const Loadout& loadout, const base::sha256::Digest& layer) {
    const std::string text = canonical_loadout(loadout);
    base::sha256::Hasher hasher;
    feed(hasher, "oa-mutators-1\n");
    feed(hasher, text);
    base::sha256::update(hasher, layer);
    return base::sha256::finish(hasher);
}

base::sha256::Digest fold_sim_hash(
    const base::sha256::Digest& sim_hash, const std::optional<base::sha256::Digest>& mutators
) {
    if (!mutators)
        return sim_hash;
    base::sha256::Hasher hasher;
    feed(hasher, "oa-sim-hash-mutators-1\n");
    base::sha256::update(hasher, sim_hash);
    base::sha256::update(hasher, *mutators);
    return base::sha256::finish(hasher);
}

std::string option_text(const LoadoutOption& option) {
    switch (option_kind(option)) {
    case OptionKind::boolean:
        return option.toggle ? "true" : "false";
    case OptionKind::string:
        return option.value.text;
    case OptionKind::number:
        break;
    }
    return oamod::canonical_number_text(option.value.number_value);
}

namespace {

/// Quotes an id the way a refusal names it.
///
/// @param text the id
/// @return the id in double quotes
std::string in_quotes(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

} // namespace

std::string options_text(const LoadoutEntry& entry) {
    std::vector<const LoadoutOption*> ordered;
    ordered.reserve(entry.options.size());
    for (const LoadoutOption& option : entry.options)
        ordered.push_back(&option);
    std::sort(
        ordered.begin(), ordered.end(), [](const LoadoutOption* left, const LoadoutOption* right) {
            return left->id < right->id;
        }
    );
    std::string text;
    for (const LoadoutOption* option : ordered) {
        if (!text.empty())
            text += ',';
        text += option->id;
        text += '=';
        text += option_text(*option);
    }
    return text;
}

Loadout
loadout_of(std::span<const ResolvedMutator> chosen, oa::formats::oamut::ClashMode clash_default) {
    Loadout loadout;
    loadout.entries.reserve(chosen.size());
    for (const ResolvedMutator& one : chosen) {
        LoadoutEntry entry;
        entry.id = one.manifest.id;
        entry.version = oa::formats::oamut::version_text(one.manifest.version);
        entry.package = one.package;
        entry.folder = one.folder;
        entry.clashes = effective_clash(one.clashes, one.manifest.clashes, clash_default);
        entry.options = one.options;
        std::sort(
            entry.options.begin(),
            entry.options.end(),
            [](const LoadoutOption& left, const LoadoutOption& right) { return left.id < right.id; }
        );
        loadout.entries.push_back(std::move(entry));
    }
    return loadout;
}

Loadout applied_loadout(const Loadout& chosen, const PipelineResult& result) {
    Loadout kept;
    const std::size_t count = chosen.entries.size() < result.mutators.size()
                                  ? chosen.entries.size()
                                  : result.mutators.size();
    kept.entries.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        if (result.mutators[index].outcome != Outcome::applied)
            continue;
        kept.entries.push_back(chosen.entries[index]);
    }
    return kept;
}

std::optional<std::vector<ResolvedMutator>>
resolve_loadout(const Loadout& loadout, const fs::path& mutators_folder, std::string* error) {
    const oa::data::mutator_library::Listing listing =
        oa::data::mutator_library::list_installed(mutators_folder);
    std::vector<ResolvedMutator> resolved;
    resolved.reserve(loadout.entries.size());
    for (const LoadoutEntry& entry : loadout.entries) {
        const oa::data::mutator_library::InstalledMutator* installed =
            oa::data::mutator_library::find(listing, entry.id);
        if (installed == nullptr) {
            set_error(error, in_quotes(entry.id) + " is not an installed mutator");
            return std::nullopt;
        }
        const std::string version = oa::formats::oamut::version_text(installed->manifest.version);
        if (version != entry.version) {
            set_error(
                error, in_quotes(entry.id) + " is version " + version + ", not " + entry.version
            );
            return std::nullopt;
        }
        if (entry.folder) {
            std::string digest_error;
            const std::optional<base::sha256::Digest> digest =
                folder_digest(installed->folder, &digest_error);
            if (!digest || *digest != entry.package) {
                set_error(error, in_quotes(entry.id) + " is a package folder");
                return std::nullopt;
            }
        } else {
            const std::optional<base::sha256::Digest> recorded =
                base::sha256::parse_hex(installed->package_sha256);
            if (!recorded || *recorded != entry.package) {
                set_error(error, in_quotes(entry.id) + " is a different package");
                return std::nullopt;
            }
        }
        const std::string options = options_text(entry);
        std::string text = entry.id;
        if (!options.empty()) {
            text.push_back(':');
            text += options;
        }
        std::string choice_error;
        const std::optional<MutatorChoice> choice = parse_choice(text, &choice_error);
        if (!choice) {
            set_error(error, in_quotes(entry.id) + ": " + choice_error);
            return std::nullopt;
        }
        std::optional<ResolvedMutator> one =
            resolve_choice(*choice, mutators_folder, &choice_error);
        if (!one) {
            set_error(
                error,
                choice_error.empty() ? in_quotes(entry.id) + " cannot be resolved" : choice_error
            );
            return std::nullopt;
        }
        one->clashes = entry.clashes;
        resolved.push_back(std::move(*one));
    }
    return resolved;
}

} // namespace oa::data::mutators
