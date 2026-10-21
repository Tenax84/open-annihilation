// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The loadout record, its canonical text and the digests taken from it.

#include "oa/data/mutators/loadout.hpp"

#include "oa/data/mod_profile/value.hpp"
#include "oa/formats/json.hpp"
#include "oa/formats/oamod.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <span>
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

/// Returns an option's canonical JSON value.
///
/// A toggle is a boolean. A string choice is a string. A number, and a
/// numeric choice, are numbers.
///
/// @param option the option
/// @return the value
profile::Value option_json(const LoadoutOption& option) {
    if (option.type == oa::formats::oamut::OptionType::toggle)
        return profile::make_boolean(option.toggle);
    if (option.type == oa::formats::oamut::OptionType::choice && !option.value.number)
        return profile::make_string(option.value.text);
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
/// @return true for folder, id, options, sha256 and version
bool entry_key(std::string_view key) noexcept {
    return key == "folder" || key == "id" || key == "options" || key == "sha256" ||
           key == "version";
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
    const json::Json* folder = find_member(value, "folder", duplicate);
    const json::Json* id = find_member(value, "id", duplicate);
    const json::Json* options = find_member(value, "options", duplicate);
    const json::Json* sha = find_member(value, "sha256", duplicate);
    const json::Json* version = find_member(value, "version", duplicate);
    if (duplicate) {
        set_error(error, place + " repeats a key");
        return false;
    }
    if (folder == nullptr || id == nullptr || options == nullptr || sha == nullptr ||
        version == nullptr) {
        set_error(error, place + " lacks folder, id, options, sha256 or version");
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

/// Returns a path's form relative to a folder, with '/' between parts.
///
/// @param folder the folder
/// @param file a file inside it, or the folder itself
/// @return the relative path, or the folder's own text when there is none
std::string relative_text(const fs::path& folder, const fs::path& file) {
    const std::string text = file.lexically_relative(folder).generic_string();
    if (text.empty() || text == ".")
        return folder.generic_string();
    return text;
}

/// Tells whether a directory entry is a link.
///
/// A junction is its own file type on the compiler that has one, and neither
/// a directory nor a regular file, so it is recognised by what it is not.
///
/// @param status the entry's symlink status
/// @return true for a link
bool is_link(const fs::file_status& status) {
    if (fs::is_symlink(status))
        return true;
    return status.type() != fs::file_type::none && status.type() != fs::file_type::not_found &&
           !fs::is_directory(status) && !fs::is_regular_file(status);
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

/// Reads a regular file's size and digest.
///
/// @param path the host path
/// @param[out] size the file's size in bytes
/// @param[out] digest the SHA-256 of its bytes
/// @return true when the file was read
bool digest_file(const fs::path& path, uint64_t& size, base::sha256::Digest& digest) {
    std::error_code failure;
    const std::uintmax_t bytes = fs::file_size(path, failure);
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

/// The text one option contributes after `=`.
///
/// @param option the option
/// @return true, false, the number's canonical text, or the choice's text
std::string option_value_text(const LoadoutOption& option) {
    if (option.type == oa::formats::oamut::OptionType::toggle)
        return option.toggle ? "true" : "false";
    if (option.type == oa::formats::oamut::OptionType::number)
        return oamod::canonical_number_text(option.value.number_value);
    return option.value.text;
}

} // namespace

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
    if (failure || root.type() == fs::file_type::not_found || root.type() == fs::file_type::none) {
        set_error(error, folder.generic_string() + " cannot be read");
        return std::nullopt;
    }
    if (is_link(root) || reparse_point(folder)) {
        set_error(error, folder.generic_string() + " is a symbolic link");
        return std::nullopt;
    }
    if (!fs::is_directory(root)) {
        set_error(error, folder.generic_string() + " is not a folder");
        return std::nullopt;
    }

    fs::recursive_directory_iterator cursor(folder, fs::directory_options::none, failure);
    if (failure) {
        set_error(error, folder.generic_string() + " cannot be read");
        return std::nullopt;
    }
    const fs::recursive_directory_iterator end;
    std::vector<FolderFile> files;
    while (cursor != end) {
        const fs::path current = cursor->path();
        const std::string relative = relative_text(folder, current);
        const fs::file_status status = cursor->symlink_status(failure);
        if (failure) {
            set_error(error, relative + " cannot be read");
            return std::nullopt;
        }
        if (is_link(status) || reparse_point(current)) {
            set_error(error, relative + " is a symbolic link");
            return std::nullopt;
        }
        if (fs::is_regular_file(status)) {
            FolderFile file;
            file.path = relative;
            if (!digest_file(current, file.size, file.digest)) {
                set_error(error, relative + " cannot be read");
                return std::nullopt;
            }
            files.push_back(std::move(file));
        } else if (!fs::is_directory(status)) {
            set_error(error, relative + " cannot be read");
            return std::nullopt;
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
        text += option_value_text(*option);
    }
    return text;
}

} // namespace oa::data::mutators
