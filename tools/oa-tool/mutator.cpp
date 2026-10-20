// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// mutator pack, mutator check and mutator import. Pack and check refuse a
// package that is not a mutator and otherwise run the same work as the
// generic command. Import reads TA: Mutation files and writes one mutator
// folder per mutator.

#include "command.hpp"

#include "arguments.hpp"

#include "oa/app/package_install.hpp"
#include "oa/data/mutator_import/legacy.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/formats/oamut/manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef OA_ENGINE_VERSION
#error OA_ENGINE_VERSION is set from the project version
#endif

namespace oa::tool {
namespace {

namespace fs = std::filesystem;
namespace install = oa::app::package_install;

/// Tells whether two texts are equal with ASCII letters folded.
///
/// @param left one text
/// @param right the other
/// @return true when they are equal
bool same_ascii(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](unsigned char byte) {
            return byte >= 'A' && byte <= 'Z' ? static_cast<unsigned char>(byte - 'A' + 'a') : byte;
        };
        if (fold(static_cast<unsigned char>(left[index])) !=
            fold(static_cast<unsigned char>(right[index])))
            return false;
    }
    return true;
}

/// Returns a path made from UTF-8 text.
///
/// @param text the text
/// @return the path
fs::path path_of(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

/// Tells whether a folder's top holds oamut.yaml.
///
/// @param folder the folder
/// @return true when a file there is named oamut.yaml, without regard to case
bool top_holds_oamut(const fs::path& folder) {
    std::error_code error;
    if (!fs::is_directory(fs::symlink_status(folder, error)))
        return false;
    const fs::directory_iterator end;
    for (fs::directory_iterator cursor(folder, error); !error && cursor != end;
         cursor.increment(error)) {
        std::error_code status_error;
        if (fs::is_directory(cursor->symlink_status(status_error)))
            continue;
        const auto name = cursor->path().filename().generic_u8string();
        const std::string text(name.begin(), name.end());
        if (same_ascii(text, "oamut.yaml"))
            return true;
    }
    return false;
}

/// Prints a usage failure for a mutator command.
///
/// @param output where it is printed
/// @param command `mutator pack`, `mutator check` or `mutator import`
/// @param problem what is wrong
void report_mutator_usage(Output& output, std::string_view command, std::string_view problem) {
    output.err << "oa-tool " << command << ": " << problem << '\n';
    output.err << "run 'oa-tool help " << command << "'\n";
}

namespace importing = oa::data::mutator_import;
namespace oamut = oa::formats::oamut;

/// The most bytes of one zip this command reads.
constexpr uintmax_t most_zip_bytes = uintmax_t{256} * 1024 * 1024;

/// Why a capped read stopped.
enum class ReadFault : uint8_t { ok, missing, too_big, failed };

/// A path as UTF-8, with generic separators.
///
/// @param path the path
/// @return the text
std::string utf8_of(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

/// The last component of a path, as UTF-8.
///
/// @param path the path
/// @return the file name
std::string file_name_of(const fs::path& path) {
    return utf8_of(path.filename());
}

/// Tells whether a file name ends in a suffix, ignoring ASCII case.
///
/// @param name the file name
/// @param suffix the suffix, including the dot
/// @return true when it ends that way
bool ends_with_ascii(std::string_view name, std::string_view suffix) noexcept {
    return name.size() >= suffix.size() &&
           same_ascii(name.substr(name.size() - suffix.size()), suffix);
}

/// The last component of a legacy name. A backslash is a slash.
///
/// @param raw the name as stored
/// @return the file name
std::string last_component(std::string_view raw) {
    std::string text{raw};
    for (char& character : text) {
        if (character == '\\')
            character = '/';
    }
    const auto slash = text.rfind('/');
    if (slash == std::string::npos)
        return text;
    return text.substr(slash + 1);
}

/// Tells whether a legacy name is a .tam.
///
/// @param name the name or a zip path
/// @return true when the last component ends in .tam
bool is_tam_name(std::string_view name) {
    return ends_with_ascii(last_component(name), ".tam");
}

/// Tells whether a path is a symlink or a junction. A missing path is not.
///
/// @param path the path
/// @return true when opening it would follow a link
bool path_is_link(const fs::path& path) {
    std::error_code error;
    const fs::file_status status = fs::symlink_status(path, error);
    if (error)
        return false;
    if (fs::is_symlink(status))
        return true;
    return install::is_link_or_junction(path);
}

/// Reads a file that is not larger than a cap. A link is not checked here.
///
/// @param path the file
/// @param cap the most bytes
/// @param[out] bytes the contents, cleared when the read fails
/// @return why it stopped
ReadFault read_capped(const fs::path& path, uintmax_t cap, std::vector<uint8_t>& bytes) {
    bytes.clear();
    std::error_code error;
    const fs::file_status status = fs::symlink_status(path, error);
    if (error || !fs::exists(status))
        return ReadFault::missing;
    if (fs::is_directory(status))
        return ReadFault::failed;
    const auto size = fs::file_size(path, error);
    if (error)
        return ReadFault::failed;
    if (size > cap)
        return ReadFault::too_big;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return ReadFault::failed;
    bytes.resize(static_cast<std::size_t>(size));
    if (bytes.empty())
        return ReadFault::ok;
    if (!in.read(
            reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        )) {
        bytes.clear();
        return ReadFault::failed;
    }
    return ReadFault::ok;
}

/// Tells whether text is lower-case kebab-case of 1 to `most` bytes.
///
/// @param text the text
/// @param most the most bytes
/// @return true when it is such an id
bool kebab_text(std::string_view text, std::size_t most) noexcept {
    if (text.empty() || text.size() > most || text.front() == '-' || text.back() == '-')
        return false;
    char previous = 0;
    for (const char character : text) {
        const bool word =
            (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
        if (!word && character != '-')
            return false;
        if (character == '-' && previous == '-')
            return false;
        previous = character;
    }
    return true;
}

/// Tells whether a works-with code is 1 to 8 letters and digits.
///
/// @param text the code
/// @return true when it is
bool code_text(std::string_view text) noexcept {
    if (text.empty() || text.size() > 8)
        return false;
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        const bool word = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                          (byte >= '0' && byte <= '9');
        if (!word)
            return false;
    }
    return true;
}

/// Tells whether a works-with entry is `*`, `ta-3.1c` or a mutator id.
///
/// @param text the entry
/// @return true when it is
bool works_entry(std::string_view text) noexcept {
    return text == "*" || text == "ta-3.1c" || kebab_text(text, oamut::most_id_bytes);
}

/// Folds ASCII letters to lower case.
///
/// @param text the text
/// @return the folded text
std::string folded_ascii(std::string_view text) {
    std::string folded;
    folded.reserve(text.size());
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        folded.push_back(
            byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte - 'A' + 'a') : character
        );
    }
    return folded;
}

/// The stem of a .tam name, without a `{}` or `}{` prefix or a suffix.
///
/// @param name the file name
/// @return the stem
std::string tam_stem(std::string_view name) {
    std::string_view stem = name;
    if (stem.starts_with("}{"))
        stem.remove_prefix(2);
    else if (stem.starts_with("{}"))
        stem.remove_prefix(2);
    const auto dot = stem.rfind('.');
    if (dot != std::string_view::npos)
        stem = stem.substr(0, dot);
    return std::string{stem};
}

/// The companions a .tam names, in the order the library looks them up.
///
/// A soft mutator names its rule file and its numbered archives. A hard
/// mutator names its archive, the fifth line when that line is set, and its
/// numbered archives. An unknown prefix names nothing.
///
/// @param tam the .tam
/// @param shown the .tam's file name
/// @return the file names
std::vector<std::string> companion_names(const importing::Tam& tam, std::string_view shown) {
    std::vector<std::string> names;
    const bool hard = shown.starts_with("}{");
    const bool soft = shown.starts_with("{}");
    if (!hard && !soft)
        return names;
    const std::string prefix = hard ? "}{" : "{}";
    const std::string stem = tam_stem(shown);
    if (soft)
        names.push_back(prefix + stem + ".sm");
    if (hard) {
        if (tam.archive_name.empty())
            names.push_back(prefix + stem + ".ccx");
        else
            names.push_back(last_component(tam.archive_name));
    }
    for (int number = 1; number <= tam.companion_count; ++number)
        names.push_back(prefix + stem + "(" + std::to_string(number) + ").ccx");
    return names;
}

/// Appends one Unicode scalar as UTF-8.
///
/// @param[in,out] out the text
/// @param character the scalar
void append_utf8(std::string& out, char32_t character) {
    if (character < 0x80) {
        out.push_back(static_cast<char>(character));
        return;
    }
    if (character < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (character >> 6)));
        out.push_back(static_cast<char>(0x80 | (character & 0x3F)));
        return;
    }
    if (character < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (character >> 12)));
        out.push_back(static_cast<char>(0x80 | ((character >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (character & 0x3F)));
        return;
    }
    out.push_back(static_cast<char>(0xF0 | ((character >> 18) & 0x07)));
    out.push_back(static_cast<char>(0x80 | ((character >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((character >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (character & 0x3F)));
}

/// Reads one UTF-8 scalar. An ill-formed byte is taken on its own.
///
/// @param text the text
/// @param[in,out] index the first byte; moved past the scalar
/// @param[out] character the scalar
/// @return false at the end
bool take_code(std::string_view text, std::size_t& index, char32_t& character) {
    if (index >= text.size())
        return false;
    const auto lead = static_cast<unsigned char>(text[index]);
    if (lead < 0x80) {
        character = lead;
        ++index;
        return true;
    }
    std::size_t length = 0;
    char32_t value = 0;
    if (lead >= 0xC2 && lead < 0xE0) {
        length = 2;
        value = lead & 0x1F;
    } else if (lead >= 0xE0 && lead < 0xF0) {
        length = 3;
        value = lead & 0x0F;
    } else if (lead >= 0xF0 && lead < 0xF5) {
        length = 4;
        value = lead & 0x07;
    } else {
        character = lead;
        ++index;
        return true;
    }
    if (index + length > text.size()) {
        character = lead;
        ++index;
        return true;
    }
    for (std::size_t extra = 1; extra < length; ++extra) {
        const auto byte = static_cast<unsigned char>(text[index + extra]);
        if ((byte & 0xC0) != 0x80) {
            character = lead;
            ++index;
            return true;
        }
        value = (value << 6) | (byte & 0x3F);
    }
    index += length;
    character = value;
    return true;
}

/// Appends a Latin-1 letter in decomposed form, or the scalar itself.
///
/// @param[in,out] out the text
/// @param character the scalar
void append_decomposed(std::string& out, char32_t character) {
    char base = 0;
    unsigned char mark = 0;
    switch (character) {
    case 0x00C0:
    case 0x00E0:
        base = character == 0x00C0 ? 'A' : 'a';
        mark = 0x80;
        break;
    case 0x00C1:
    case 0x00E1:
        base = character == 0x00C1 ? 'A' : 'a';
        mark = 0x81;
        break;
    case 0x00C2:
    case 0x00E2:
        base = character == 0x00C2 ? 'A' : 'a';
        mark = 0x82;
        break;
    case 0x00C3:
    case 0x00E3:
        base = character == 0x00C3 ? 'A' : 'a';
        mark = 0x83;
        break;
    case 0x00C4:
    case 0x00E4:
        base = character == 0x00C4 ? 'A' : 'a';
        mark = 0x88;
        break;
    case 0x00C5:
    case 0x00E5:
        base = character == 0x00C5 ? 'A' : 'a';
        mark = 0x8A;
        break;
    case 0x00C7:
    case 0x00E7:
        base = character == 0x00C7 ? 'C' : 'c';
        mark = 0xA7;
        break;
    case 0x00C8:
    case 0x00E8:
        base = character == 0x00C8 ? 'E' : 'e';
        mark = 0x80;
        break;
    case 0x00C9:
    case 0x00E9:
        base = character == 0x00C9 ? 'E' : 'e';
        mark = 0x81;
        break;
    case 0x00CA:
    case 0x00EA:
        base = character == 0x00CA ? 'E' : 'e';
        mark = 0x82;
        break;
    case 0x00CB:
    case 0x00EB:
        base = character == 0x00CB ? 'E' : 'e';
        mark = 0x88;
        break;
    case 0x00CC:
    case 0x00EC:
        base = character == 0x00CC ? 'I' : 'i';
        mark = 0x80;
        break;
    case 0x00CD:
    case 0x00ED:
        base = character == 0x00CD ? 'I' : 'i';
        mark = 0x81;
        break;
    case 0x00CE:
    case 0x00EE:
        base = character == 0x00CE ? 'I' : 'i';
        mark = 0x82;
        break;
    case 0x00CF:
    case 0x00EF:
        base = character == 0x00CF ? 'I' : 'i';
        mark = 0x88;
        break;
    case 0x00D1:
    case 0x00F1:
        base = character == 0x00D1 ? 'N' : 'n';
        mark = 0x83;
        break;
    case 0x00D2:
    case 0x00F2:
        base = character == 0x00D2 ? 'O' : 'o';
        mark = 0x80;
        break;
    case 0x00D3:
    case 0x00F3:
        base = character == 0x00D3 ? 'O' : 'o';
        mark = 0x81;
        break;
    case 0x00D4:
    case 0x00F4:
        base = character == 0x00D4 ? 'O' : 'o';
        mark = 0x82;
        break;
    case 0x00D5:
    case 0x00F5:
        base = character == 0x00D5 ? 'O' : 'o';
        mark = 0x83;
        break;
    case 0x00D6:
    case 0x00F6:
        base = character == 0x00D6 ? 'O' : 'o';
        mark = 0x88;
        break;
    case 0x00D9:
    case 0x00F9:
        base = character == 0x00D9 ? 'U' : 'u';
        mark = 0x80;
        break;
    case 0x00DA:
    case 0x00FA:
        base = character == 0x00DA ? 'U' : 'u';
        mark = 0x81;
        break;
    case 0x00DB:
    case 0x00FB:
        base = character == 0x00DB ? 'U' : 'u';
        mark = 0x82;
        break;
    case 0x00DC:
    case 0x00FC:
        base = character == 0x00DC ? 'U' : 'u';
        mark = 0x88;
        break;
    case 0x00DD:
    case 0x00FD:
        base = character == 0x00DD ? 'Y' : 'y';
        mark = 0x81;
        break;
    case 0x00FF:
        base = 'y';
        mark = 0x88;
        break;
    default:
        append_utf8(out, character);
        return;
    }
    out.push_back(base);
    out.push_back(static_cast<char>(0xCC));
    out.push_back(static_cast<char>(mark));
}

/// Decomposes Latin-1 letters so a composed name matches a decomposed one.
///
/// @param text the file name
/// @return the decomposed text
std::string decomposed_name(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size()) {
        char32_t character = 0;
        if (!take_code(text, index, character))
            break;
        append_decomposed(out, character);
    }
    return out;
}

/// Tells whether two file names match, ignoring ASCII case and composition.
///
/// @param one the first name
/// @param other the other name
/// @return true when they are the same name
bool same_filename(std::string_view one, std::string_view other) {
    return same_ascii(decomposed_name(one), decomposed_name(other));
}

/// Finds a file in a directory by name, ignoring ASCII case.
///
/// The file is opened as `directory / name` first. That lookup accepts either
/// Unicode composition, which a directory listing does not. The scan then
/// compares decomposed names and does not follow a link. A directory of the
/// same name is skipped.
///
/// @param directory the directory
/// @param name the file name
/// @return the path, or nothing when no file has that name
std::optional<fs::path> find_named(const fs::path& directory, std::string_view name) {
    const fs::path direct = directory / path_of(name);
    std::error_code error;
    const fs::file_status direct_status = fs::symlink_status(direct, error);
    if (!error && fs::exists(direct_status) && !fs::is_directory(direct_status))
        return direct;

    error.clear();
    const fs::directory_iterator end;
    for (fs::directory_iterator cursor(directory, error); !error && cursor != end;
         cursor.increment(error)) {
        std::error_code status_error;
        const fs::file_status status = cursor->symlink_status(status_error);
        if (status_error || fs::is_directory(status))
            continue;
        if (!same_filename(file_name_of(cursor->path()), name))
            continue;
        return cursor->path();
    }
    return std::nullopt;
}

/// A path in canonical form, or the absolute path when that cannot be made.
///
/// @param path the path
/// @return the form used to compare directories
fs::path canonical_of(const fs::path& path) {
    std::error_code error;
    const fs::path made = fs::weakly_canonical(path, error);
    if (!error)
        return made;
    const fs::path absolute = fs::absolute(path, error);
    if (error)
        return path.lexically_normal();
    return absolute.lexically_normal();
}

/// The directory that holds a file. A bare name is in the current directory.
///
/// @param path the file
/// @return the directory
fs::path parent_of(const fs::path& path) {
    const fs::path parent = path.parent_path();
    if (parent.empty())
        return fs::path{"."};
    return parent;
}

/// Tells whether an output directory is an input's folder or inside it.
///
/// @param out_dir the output directory
/// @param input the input file
/// @return true when writing there would write into the input's folder
bool out_is_inside(const fs::path& out_dir, const fs::path& input) {
    const fs::path relative =
        canonical_of(out_dir).lexically_relative(canonical_of(parent_of(input)));
    if (relative.empty())
        return false;
    const auto first = relative.begin();
    if (first == relative.end())
        return false;
    return *first != "..";
}

/// How many .tam files a set holds.
///
/// @param files the files
/// @return the count
int tam_count_of(std::span<const importing::LegacyFile> files) {
    int count = 0;
    for (const importing::LegacyFile& file : files) {
        if (is_tam_name(file.name))
            ++count;
    }
    return count;
}

/// One input, after it has been read or refused.
struct Loaded {
    std::string given;                        ///< the path the user wrote
    fs::path path;                            ///< that path
    bool is_zip{};                            ///< true when the input is a zip
    bool loaded{};                            ///< true when `files` may be imported
    std::vector<importing::LegacyFile> files; ///< the .tam and the companions found
    std::string problem;                      ///< why it was not loaded
};

/// Reads a zip. A link is refused and not opened.
///
/// @param[in,out] item the input
/// @return true when the zip was read
bool load_zip(Loaded& item) {
    if (path_is_link(item.path)) {
        item.problem = item.given + ": refusing to follow a link";
        return false;
    }
    std::vector<uint8_t> bytes;
    const ReadFault fault = read_capped(item.path, most_zip_bytes, bytes);
    if (fault == ReadFault::too_big) {
        item.problem = item.given + ": the zip is larger than 256 MiB";
        return false;
    }
    if (fault != ReadFault::ok) {
        item.problem = item.given + ": cannot read";
        return false;
    }
    std::string zip_problem;
    if (!importing::files_from_zip(bytes, item.files, zip_problem)) {
        item.problem = item.given + ": " + zip_problem;
        item.files.clear();
        return false;
    }
    item.loaded = true;
    return true;
}

/// Reads one .tam and the companions its folder holds.
///
/// A link is refused and not opened. A missing companion is left for the
/// library to report. A companion that is a link refuses this mutator.
///
/// @param[in,out] item the input
/// @return true when the files may be imported
bool load_tam(Loaded& item) {
    if (path_is_link(item.path)) {
        item.problem = item.given + ": refusing to follow a link";
        return false;
    }
    const std::string shown = file_name_of(item.path);
    std::vector<uint8_t> bytes;
    const ReadFault fault = read_capped(item.path, importing::most_tam_bytes, bytes);
    if (fault == ReadFault::too_big) {
        item.problem = shown + ": the .tam is larger than 64 KiB";
        return false;
    }
    if (fault != ReadFault::ok) {
        item.problem = item.given + ": cannot read";
        return false;
    }
    importing::LegacyFile tam_file;
    tam_file.name = shown;
    tam_file.bytes = std::move(bytes);
    item.files.push_back(std::move(tam_file));

    importing::Tam tam;
    std::string tam_problem;
    if (!importing::read_tam(item.files.back().bytes, tam, tam_problem)) {
        item.loaded = true;
        return true;
    }
    std::vector<std::string> added;
    for (const std::string& name : companion_names(tam, shown)) {
        const std::optional<fs::path> found = find_named(parent_of(item.path), name);
        if (!found)
            continue;
        if (path_is_link(*found)) {
            item.files.clear();
            item.problem = shown + ": refusing to follow a link " + name;
            return false;
        }
        bool seen = false;
        for (const std::string& earlier : added) {
            if (same_filename(earlier, name))
                seen = true;
        }
        if (seen)
            continue;
        std::vector<uint8_t> companion;
        const ReadFault companion_fault =
            read_capped(*found, importing::most_archive_bytes, companion);
        if (companion_fault == ReadFault::too_big) {
            item.files.clear();
            item.problem = shown + ": " + name + " is larger than 64 MiB";
            return false;
        }
        if (companion_fault != ReadFault::ok) {
            item.files.clear();
            item.problem = shown + ": cannot read " + name;
            return false;
        }
        importing::LegacyFile file;
        file.name = name;
        file.bytes = std::move(companion);
        item.files.push_back(std::move(file));
        added.push_back(name);
    }
    item.loaded = true;
    return true;
}

/// Makes a folder ready to receive a mutator. A link is left in place.
///
/// @param folder the folder
/// @param force whether an existing folder is replaced
/// @param[out] problem why the folder was not made ready
/// @return true when the folder is empty and ready
/// @throws Failure when a folder cannot be replaced or created
bool prepare_folder(const fs::path& folder, bool force, std::string& problem) {
    if (path_is_link(folder)) {
        problem = utf8_of(folder) + ": refusing to replace a link";
        return false;
    }
    std::error_code error;
    const fs::file_status status = fs::symlink_status(folder, error);
    if (!error && fs::exists(status)) {
        if (!fs::is_directory(status)) {
            problem = utf8_of(folder) + ": a file is in the way";
            return false;
        }
        if (!force) {
            problem = utf8_of(folder) + ": the folder already exists";
            return false;
        }
        fs::remove_all(folder, error);
        if (error)
            throw Failure("cannot replace " + utf8_of(folder));
    }
    fs::create_directories(folder, error);
    if (error)
        throw Failure("cannot write " + utf8_of(folder));
    return true;
}

/// Writes one imported mutator's files.
///
/// @param folder the mutator's folder
/// @param imported the mutator
/// @throws Failure when a file cannot be written
void write_tree(const fs::path& folder, const importing::Imported& imported) {
    for (const auto& file : imported.files) {
        const fs::path path = folder / path_of(file.first);
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        if (error)
            throw Failure("cannot write " + utf8_of(path));
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            throw Failure("cannot write " + utf8_of(path));
        if (!file.second.empty() && !out.write(
                                        reinterpret_cast<const char*>(file.second.data()),
                                        static_cast<std::streamsize>(file.second.size())
                                    ))
            throw Failure("cannot write " + utf8_of(path));
        out.close();
        if (!out)
            throw Failure("cannot write " + utf8_of(path));
    }
}

/// How many rule files and archives a package wrote.
///
/// @param imported the mutator
/// @param[out] rules rule files
/// @param[out] archives archives
void written_counts(const importing::Imported& imported, int& rules, int& archives) {
    rules = 0;
    archives = 0;
    for (const auto& file : imported.files) {
        if (file.first.starts_with("rules/"))
            ++rules;
        else if (file.first.starts_with("files/"))
            ++archives;
    }
}

} // namespace

int run_mutator_pack(std::span<const std::string> arguments, Output& output) {
    static constexpr OptionSpec options[] = {
        {"out", true, false},
        {"force", false, false},
        {"game-dir", true, false},
    };
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, problem);
    if (!parsed || parsed->positional.size() != 1)
        return run_pack(arguments, output);
    if (!top_holds_oamut(path_of(parsed->positional[0]))) {
        report_mutator_usage(output, "mutator pack", "the folder holds no oamut.yaml");
        return exit_usage;
    }
    return run_pack(arguments, output);
}

int run_mutator_check(std::span<const std::string> arguments, Output& output) {
    static constexpr OptionSpec options[] = {
        {"game-dir", true, false},
        {"mod", true, true},
        {"accept-unimplemented-hacks", false, false},
        {"json", false, false},
    };
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, problem);
    if (!parsed || parsed->positional.size() != 1)
        return run_check(arguments, output);
    const install::PackageKind* kind = install::kind_for_file(path_of(parsed->positional[0]));
    if (kind == nullptr || std::string_view{kind->name} != "oamut") {
        report_mutator_usage(output, "mutator check", "the file is not a .oamut");
        return exit_usage;
    }
    return run_check(arguments, output);
}

int run_mutator_import(std::span<const std::string> arguments, Output& output) {
    static constexpr OptionSpec options[] = {
        {"out", true, false},
        {"id", true, false},
        {"version", true, false},
        {"tag", true, true},
        {"works-with", true, true},
        {"pack", false, false},
        {"force", false, false},
    };
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, problem);
    if (!parsed) {
        report_mutator_usage(output, "mutator import", problem);
        return exit_usage;
    }
    if (parsed->positional.empty()) {
        report_mutator_usage(output, "mutator import", "name an input");
        return exit_usage;
    }
    const std::string* const out = parsed->value("out");
    if (out == nullptr) {
        report_mutator_usage(output, "mutator import", "--out is required");
        return exit_usage;
    }
    if (out->empty()) {
        report_mutator_usage(output, "mutator import", "option '--out' needs a value");
        return exit_usage;
    }
    const std::string* const id_text = parsed->value("id");
    if (id_text != nullptr && !kebab_text(*id_text, oamut::most_id_bytes)) {
        report_mutator_usage(output, "mutator import", "the id is not lower-case kebab-case");
        return exit_usage;
    }
    const std::string* const version_option = parsed->value("version");
    if (version_option != nullptr && !oamut::parse_version(*version_option)) {
        report_mutator_usage(output, "mutator import", "the version is not a mutator version");
        return exit_usage;
    }
    const auto tags = parsed->values.find("tag");
    if (tags != parsed->values.end()) {
        if (tags->second.size() > oamut::most_tags) {
            report_mutator_usage(output, "mutator import", "there are more than 4 tags");
            return exit_usage;
        }
        for (const std::string& tag : tags->second) {
            if (!oa::formats::oamod::tag_valid(tag)) {
                report_mutator_usage(
                    output, "mutator import", "a tag is not lower-case kebab-case"
                );
                return exit_usage;
            }
        }
    }
    importing::ImportOptions import_options;
    import_options.engine = ">= " OA_ENGINE_VERSION;
    if (tags != parsed->values.end())
        import_options.tags = tags->second;
    const auto mappings = parsed->values.find("works-with");
    if (mappings != parsed->values.end()) {
        std::vector<std::string> seen_codes;
        for (const std::string& mapping : mappings->second) {
            const auto split = mapping.find('=');
            if (split == std::string::npos || split == 0 || split + 1 == mapping.size()) {
                report_mutator_usage(
                    output, "mutator import", "a --works-with value is CODE=ENTRY"
                );
                return exit_usage;
            }
            const std::string code = mapping.substr(0, split);
            const std::string entry = mapping.substr(split + 1);
            if (!code_text(code)) {
                report_mutator_usage(
                    output, "mutator import", "a works-with code is 1 to 8 letters and digits"
                );
                return exit_usage;
            }
            if (same_ascii(code, "OTA") || same_ascii(code, "TACC")) {
                report_mutator_usage(output, "mutator import", "OTA and TACC are already mapped");
                return exit_usage;
            }
            if (!works_entry(entry)) {
                report_mutator_usage(
                    output,
                    "mutator import",
                    "a works-with entry is *, ta-3.1c, or lower-case kebab-case"
                );
                return exit_usage;
            }
            const std::string folded = folded_ascii(code);
            for (const std::string& earlier : seen_codes) {
                if (earlier == folded) {
                    report_mutator_usage(output, "mutator import", "a works-with code is repeated");
                    return exit_usage;
                }
            }
            seen_codes.push_back(folded);
            import_options.code_map.emplace_back(code, entry);
        }
    }

    std::vector<Loaded> inputs;
    int tam_paths = 0;
    for (const std::string& given : parsed->positional) {
        Loaded item;
        item.given = given;
        item.path = path_of(given);
        item.is_zip = ends_with_ascii(file_name_of(item.path), ".zip");
        const bool tam = ends_with_ascii(file_name_of(item.path), ".tam");
        if (!item.is_zip && !tam) {
            report_mutator_usage(output, "mutator import", "an input is a .tam or a .zip");
            return exit_usage;
        }
        if (tam)
            ++tam_paths;
        inputs.push_back(std::move(item));
    }
    const bool override_one = id_text != nullptr || version_option != nullptr;
    if (tam_paths > 1 && override_one) {
        report_mutator_usage(output, "mutator import", "--id and --version apply to one mutator");
        return exit_usage;
    }

    int tam_total = tam_paths;
    for (Loaded& item : inputs) {
        if (!item.is_zip)
            continue;
        if (!load_zip(item))
            continue;
        tam_total += tam_count_of(item.files);
    }
    if (tam_total > 1 && override_one) {
        report_mutator_usage(output, "mutator import", "--id and --version apply to one mutator");
        return exit_usage;
    }
    if (tam_total == 1) {
        if (id_text != nullptr)
            import_options.id = *id_text;
        if (version_option != nullptr)
            import_options.version = *version_option;
    }

    const fs::path out_dir = path_of(*out);
    if (path_is_link(out_dir)) {
        output.out << utf8_of(out_dir) << ": refusing to follow a link\n";
        return exit_failed;
    }
    for (const Loaded& item : inputs) {
        // A zip is read on its own. Only a .tam has companions in its folder.
        if (item.is_zip)
            continue;
        if (out_is_inside(out_dir, item.path)) {
            output.out << "refusing to write into the input's folder\n";
            return exit_failed;
        }
    }

    const bool pack = parsed->flags.contains("pack");
    const bool force = parsed->flags.contains("force");
    bool failed = false;
    for (Loaded& item : inputs) {
        if (!item.is_zip && !item.loaded)
            load_tam(item);
        if (!item.loaded) {
            output.out << item.problem << '\n';
            failed = true;
            continue;
        }
        const importing::ImportResult result = importing::import_legacy(item.files, import_options);
        for (const importing::Imported& imported : result.mutators) {
            const fs::path folder = out_dir / path_of(imported.manifest.id);
            std::string place_problem;
            if (!prepare_folder(folder, force, place_problem)) {
                output.out << place_problem << '\n';
                failed = true;
                continue;
            }
            write_tree(folder, imported);
            int rules = 0;
            int archives = 0;
            written_counts(imported, rules, archives);
            output.out << "imported " << imported.manifest.id << " from " << item.given << ": "
                       << rules << " rule files, " << archives << " archives\n";
            for (const std::string& warning : imported.warnings)
                output.out << "  " << warning << '\n';
            if (!pack)
                continue;
            const fs::path package =
                out_dir / (imported.manifest.id + "-" +
                           oamut::version_text(imported.manifest.version) + ".oamut");
            std::vector<std::string> pack_arguments{utf8_of(folder), "--out", utf8_of(package)};
            if (force)
                pack_arguments.emplace_back("--force");
            if (run_pack(pack_arguments, output) != exit_done)
                failed = true;
        }
        for (const std::string& refusal : result.problems) {
            output.out << refusal << '\n';
            failed = true;
        }
        for (const std::string& warning : result.warnings)
            output.out << "  " << warning << '\n';
    }
    return failed ? exit_failed : exit_done;
}

} // namespace oa::tool
