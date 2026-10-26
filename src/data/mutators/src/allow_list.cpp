// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/mutators/allow_list.hpp"

#include "oa/formats/hpi.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::mutators {
namespace {

namespace defs = oa::data::defs;

/// A path of this many bytes, or more, is over-long.
constexpr std::size_t kPathByteLimit = 4096;

/// A row whose folder name is a fixed string, not a layout directory.
constexpr uint8_t kFixedFolder = 255;

/// One folder, or one file in a folder. A path moves sides by editing one row.
struct AllowRow {
    /// Index into DataLayout::directories, or kFixedFolder.
    uint8_t layout_index = kFixedFolder;
    /// Folder name when layout_index is kFixedFolder.
    const char* fixed_name = nullptr;
    /// File name when only that file follows the rule. Null for the whole folder.
    const char* file_name = nullptr;
    /// The rule for a path this row matches.
    PathRule rule = PathRule::unknown_folder;
    /// True when every file under the folder may change, so a mask may too.
    bool whole_tree = false;
    /// True when the folder is allowed file by file, so a mask must name one file.
    bool file_by_file = false;
    /// True when a unit's build picture, anims/<unit><digits>.gaf, and its
    /// download-button art, anims/<unit>_gadget.gaf, are allowed, and every
    /// other file under the folder is not.
    bool build_picture = false;
    /// The sentence after the path and its colon.
    const char* sentence = nullptr;
};

/// This file is read again for every match.
constexpr const char* kReadEachMatch = "this file is read again for every match";

/// This is loaded once when the game starts.
constexpr const char* kReadOnce = "this is loaded once when the game starts";

/// Not yet shown to be read again for every match.
constexpr const char* kNotConfirmed = "not yet shown to be read again for every match";

// The order is the order a reader meets the folders. A file row is consulted
// only after its folder's row has matched, so the file rows can sit anywhere.
constexpr AllowRow kAllowRows[] = {
    {static_cast<uint8_t>(defs::DataDirectory::units),
     nullptr,
     nullptr,
     PathRule::allowed,
     true,
     false,
     false,
     kReadEachMatch},
    {static_cast<uint8_t>(defs::DataDirectory::weapons),
     nullptr,
     nullptr,
     PathRule::allowed,
     true,
     false,
     false,
     kReadEachMatch},
    {static_cast<uint8_t>(defs::DataDirectory::ai),
     nullptr,
     nullptr,
     PathRule::allowed,
     true,
     false,
     false,
     kReadEachMatch},
    {static_cast<uint8_t>(defs::DataDirectory::unitpics),
     nullptr,
     nullptr,
     PathRule::allowed,
     true,
     false,
     false,
     kReadEachMatch},
    {static_cast<uint8_t>(defs::DataDirectory::download),
     nullptr,
     nullptr,
     PathRule::allowed,
     true,
     false,
     false,
     kReadEachMatch},
    {kFixedFolder, "features", nullptr, PathRule::allowed, true, false, false, kReadEachMatch},
    {kFixedFolder, "objects3d", nullptr, PathRule::allowed, true, false, false, kReadEachMatch},
    {kFixedFolder, "scripts", nullptr, PathRule::allowed, true, false, false, kReadEachMatch},
    {static_cast<uint8_t>(defs::DataDirectory::gamedata),
     nullptr,
     "moveinfo.tdf",
     PathRule::allowed,
     false,
     false,
     false,
     kReadEachMatch},
    {static_cast<uint8_t>(defs::DataDirectory::gamedata),
     nullptr,
     "sidedata.tdf",
     PathRule::allowed,
     false,
     false,
     false,
     kReadEachMatch},
    {static_cast<uint8_t>(defs::DataDirectory::gamedata),
     nullptr,
     "los.tdf",
     PathRule::allowed,
     false,
     false,
     false,
     kReadEachMatch},
    {static_cast<uint8_t>(defs::DataDirectory::gamedata),
     nullptr,
     "meteor.tdf",
     PathRule::allowed,
     false,
     false,
     false,
     kReadEachMatch},
    {static_cast<uint8_t>(defs::DataDirectory::gamedata),
     nullptr,
     "sound.tdf",
     PathRule::read_once,
     false,
     false,
     false,
     kReadOnce},
    {static_cast<uint8_t>(defs::DataDirectory::gamedata),
     nullptr,
     nullptr,
     PathRule::not_confirmed,
     false,
     true,
     false,
     kNotConfirmed},
    {static_cast<uint8_t>(defs::DataDirectory::guis),
     nullptr,
     nullptr,
     PathRule::menus_only,
     false,
     false,
     false,
     "build pages and screens change only through a mutator's menus"},
    {kFixedFolder,
     "maps",
     nullptr,
     PathRule::maps_only,
     false,
     false,
     false,
     "maps change only through a mutator's maps hook"},
    {kFixedFolder, "sounds", nullptr, PathRule::read_once, false, false, false, kReadOnce},
    {kFixedFolder, "music", nullptr, PathRule::read_once, false, false, false, kReadOnce},
    {kFixedFolder, "palettes", nullptr, PathRule::read_once, false, false, false, kReadOnce},
    {kFixedFolder, "fonts", nullptr, PathRule::read_once, false, false, false, kReadOnce},
    {kFixedFolder, "bitmaps", nullptr, PathRule::read_once, false, false, false, kReadOnce},
    {kFixedFolder, "camps", nullptr, PathRule::read_once, false, false, false, kReadOnce},
    {kFixedFolder,
     "textures",
     nullptr,
     PathRule::not_confirmed,
     false,
     false,
     false,
     kNotConfirmed},
    // A unit's build picture and its download-button art are read again for
    // every match. Every other file under anims/ is interface art.
    {kFixedFolder,
     "anims",
     nullptr,
     PathRule::interface_art,
     false,
     true,
     true,
     "this is interface art, which changes with the screens"},
};

static_assert(static_cast<uint8_t>(defs::DataDirectory::units) == 0);
static_assert(static_cast<uint8_t>(defs::DataDirectory::download) == 6);
static_assert(defs::data_directory_count == 7);
static_assert(kFixedFolder > defs::data_directory_count);

/// One path split into the pieces between slashes.
struct SplitPath {
    /// True when a piece is empty, "." or "..".
    bool invalid = false;
    /// Why the path was refused, when invalid.
    const char* reason = nullptr;
    /// The pieces, in order, pointing into the path that was split.
    std::vector<std::string_view> segments;
};

/// Returns the lower-case ASCII letter, leaving every other byte as it is.
///
/// @param byte the byte
/// @return the folded byte
char ascii_lower(unsigned char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return static_cast<char>(byte);
}

/// Compares two texts without ASCII case.
///
/// @param left one text
/// @param right the other text
/// @return true when the texts are the same length and the same letters
bool ascii_equal(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto left_byte = static_cast<unsigned char>(left[index]);
        const auto right_byte = static_cast<unsigned char>(right[index]);
        if (ascii_lower(left_byte) != ascii_lower(right_byte))
            return false;
    }
    return true;
}

/// Returns whether a piece holds '*' or '?'.
///
/// @param segment one piece of a path
/// @return true when the piece holds a wildcard
bool holds_wildcard(std::string_view segment) {
    return segment.find('*') != std::string_view::npos ||
           segment.find('?') != std::string_view::npos;
}

/// Builds a verdict whose message starts with the path.
///
/// @param path the path as given
/// @param rule the rule
/// @param sentence the reason, after the colon
/// @return the verdict
PathVerdict verdict_for(std::string_view path, PathRule rule, std::string_view sentence) {
    PathVerdict verdict;
    verdict.rule = rule;
    verdict.message.reserve(path.size() + 2 + sentence.size());
    verdict.message.append(path);
    verdict.message.append(": ");
    verdict.message.append(sentence);
    return verdict;
}

/// Splits a path on '/' and '\\' and refuses an empty, "." or ".." piece.
///
/// @param path the path, already known to be non-empty and relative
/// @return the pieces, or a refusal
SplitPath split_path(std::string_view path) {
    SplitPath split;
    std::size_t begin = 0;
    for (std::size_t cursor = 0; cursor <= path.size(); ++cursor) {
        if (cursor != path.size() && path[cursor] != '/' && path[cursor] != '\\')
            continue;
        const std::string_view segment = path.substr(begin, cursor - begin);
        if (segment.empty()) {
            split.invalid = true;
            split.reason = "the path contains an empty segment";
            return split;
        }
        if (segment == "." || segment == "..") {
            split.invalid = true;
            split.reason = "the path contains a '.' or '..' segment";
            return split;
        }
        split.segments.push_back(segment);
        begin = cursor + 1;
    }
    return split;
}

/// Returns the layout the check reads.
///
/// @param context the caller's context
/// @return the caller's layout, or the layout in use when that is null
const defs::DataLayout& layout_of(const PathContext& context) {
    return context.layout != nullptr ? *context.layout : defs::data_layout();
}

/// Returns the folder name a row matches.
///
/// @param row the row
/// @param layout the layout whose directory names are in use
/// @return the folder name; a view into `row` or `layout`
std::string_view folder_name(const AllowRow& row, const defs::DataLayout& layout) {
    if (row.layout_index == kFixedFolder)
        return row.fixed_name != nullptr ? std::string_view{row.fixed_name} : std::string_view{};
    return layout.directories[row.layout_index];
}

/// Returns whether a path piece is a folder or its `<folder>-<variant>` form.
///
/// @param segment the first piece of the path
/// @param folder the folder name
/// @return true when the piece is that folder or a variant of it
bool folder_matches(std::string_view segment, std::string_view folder) {
    if (folder.empty() || segment.size() < folder.size())
        return false;
    if (!ascii_equal(segment.substr(0, folder.size()), folder))
        return false;
    if (segment.size() == folder.size())
        return true;
    return segment[folder.size()] == '-' && segment.size() > folder.size() + 1;
}

/// Finds the folder row for the first piece of a path.
///
/// A layout directory wins over a fixed folder of the same name, because a
/// mod's renamed directory is the name the game reads. The longer name wins
/// when two directories could match.
///
/// @param segment the first piece
/// @param layout the layout in use
/// @return the folder row, or null when no row names the piece
const AllowRow* folder_row(std::string_view segment, const defs::DataLayout& layout) {
    const AllowRow* found = nullptr;
    std::size_t found_length = 0;
    bool found_from_layout = false;
    for (const AllowRow& row : kAllowRows) {
        if (row.file_name != nullptr)
            continue;
        const std::string_view folder = folder_name(row, layout);
        if (!folder_matches(segment, folder))
            continue;
        const bool from_layout = row.layout_index != kFixedFolder;
        const bool longer = folder.size() > found_length;
        const bool same_length_preferred =
            folder.size() == found_length && from_layout && !found_from_layout;
        if (found == nullptr || longer || same_length_preferred) {
            found = &row;
            found_length = folder.size();
            found_from_layout = from_layout;
        }
    }
    return found;
}

/// Finds the file row of a folder, when the path names exactly that file.
///
/// @param folder the folder row already matched
/// @param file the file piece
/// @return the file row, or null when the folder has no row for this file
const AllowRow* file_row(const AllowRow& folder, std::string_view file) {
    for (const AllowRow& row : kAllowRows) {
        if (row.file_name == nullptr || row.layout_index != folder.layout_index)
            continue;
        if (row.layout_index == kFixedFolder &&
            std::string_view{row.fixed_name} != folder.fixed_name)
            continue;
        if (ascii_equal(file, row.file_name))
            return &row;
    }
    return nullptr;
}

/// Returns the unit name of anims/<unit><digits>.gaf, or empty when the
/// file is not a builder's button art.
///
/// @param segments the path's pieces; the first is the anims folder
/// @return the unit name, in the path's own spelling
std::string build_picture_unit(const std::vector<std::string_view>& segments) {
    if (segments.size() != 2)
        return {};
    const std::string_view file = segments[1];
    constexpr std::string_view kExtension = ".gaf";
    if (file.size() <= kExtension.size())
        return {};
    if (!ascii_equal(file.substr(file.size() - kExtension.size()), kExtension))
        return {};
    const std::string_view stem = file.substr(0, file.size() - kExtension.size());
    std::size_t stem_end = stem.size();
    while (stem_end > 0) {
        const auto byte = static_cast<unsigned char>(stem[stem_end - 1]);
        if (byte < '0' || byte > '9')
            break;
        --stem_end;
    }
    if (stem_end == 0 || stem_end == stem.size())
        return {};
    return std::string{stem.substr(0, stem_end)};
}

/// Returns the unit name of anims/<unit>_gadget.gaf, or empty when the
/// file is not a unit's download-button art.
///
/// The path has two pieces. The suffix `_gadget.gaf` matches without case,
/// and the unit is the non-empty text before that suffix, in the path's
/// own spelling.
///
/// @param segments the path's pieces; the first is the anims folder
/// @return the unit name, or empty
std::string download_button_unit(const std::vector<std::string_view>& segments) {
    if (segments.size() != 2)
        return {};
    const std::string_view file = segments[1];
    constexpr std::string_view kSuffix = "_gadget.gaf";
    if (file.size() <= kSuffix.size())
        return {};
    if (!ascii_equal(file.substr(file.size() - kSuffix.size()), kSuffix))
        return {};
    return std::string{file.substr(0, file.size() - kSuffix.size())};
}

/// Judges a path that has already been split.
///
/// @param path the path as given
/// @param split the pieces
/// @param context the layout and the unit-name test
/// @return the verdict
PathVerdict
judge_segments(std::string_view path, const SplitPath& split, const PathContext& context) {
    const defs::DataLayout& layout = layout_of(context);
    const AllowRow* folder = folder_row(split.segments[0], layout);
    if (folder == nullptr || split.segments.size() < 2) {
        return verdict_for(
            path, PathRule::unknown_folder, "this folder is not one a mutator may change"
        );
    }
    if (split.segments.size() == 2) {
        if (const AllowRow* file = file_row(*folder, split.segments[1]); file != nullptr)
            return verdict_for(path, file->rule, file->sentence);
    }
    if (folder->build_picture) {
        const std::string unit = build_picture_unit(split.segments);
        if (!unit.empty() && context.is_unit_name && context.is_unit_name(unit))
            return verdict_for(path, PathRule::allowed, kReadEachMatch);
        const std::string button = download_button_unit(split.segments);
        if (!button.empty() && context.is_unit_name && context.is_unit_name(button))
            return verdict_for(path, PathRule::allowed, kReadEachMatch);
    }
    return verdict_for(path, folder->rule, folder->sentence);
}

} // namespace

PathVerdict check_mutable_path(std::string_view path, const PathContext& context) {
    if (path.empty())
        return verdict_for(path, PathRule::invalid_path, "the path is empty");
    if (path.size() > kPathByteLimit)
        return verdict_for(path, PathRule::invalid_path, "the path is over-long");
    if (path.find('\0') != std::string_view::npos)
        return verdict_for(path, PathRule::invalid_path, "the path contains a NUL");
    if (path.front() == '/' || path.front() == '\\')
        return verdict_for(path, PathRule::invalid_path, "the path is absolute");
    if (path.find(':') != std::string_view::npos)
        return verdict_for(path, PathRule::invalid_path, "the path is drive-qualified");
    const SplitPath split = split_path(path);
    if (split.invalid)
        return verdict_for(path, PathRule::invalid_path, split.reason);
    return judge_segments(path, split, context);
}

PathVerdict check_mutable_mask(std::string_view pattern, const PathContext& context) {
    const std::string problem = oa::mutator_mask_problem(pattern);
    if (!problem.empty())
        return verdict_for(pattern, PathRule::invalid_path, problem);
    if (pattern.find('*') == std::string_view::npos && pattern.find('?') == std::string_view::npos)
        return check_mutable_path(pattern, context);
    const SplitPath split = split_path(pattern);
    if (split.invalid || split.segments.empty()) {
        const char* reason = split.reason != nullptr ? split.reason : "the path is empty";
        return verdict_for(pattern, PathRule::invalid_path, reason);
    }
    if (holds_wildcard(split.segments[0])) {
        return verdict_for(
            pattern, PathRule::unknown_folder, "a mask must name a folder before a wildcard"
        );
    }
    const AllowRow* folder = folder_row(split.segments[0], layout_of(context));
    if (folder == nullptr) {
        return verdict_for(
            pattern, PathRule::unknown_folder, "this folder is not one a mutator may change"
        );
    }
    if (folder->whole_tree) {
        return verdict_for(
            pattern, PathRule::allowed, "every file under this folder is read again for every match"
        );
    }
    if (folder->file_by_file)
        return verdict_for(pattern, folder->rule, "a mask here must name one file");
    return verdict_for(pattern, folder->rule, folder->sentence);
}

std::string_view rule_name(PathRule rule) noexcept {
    switch (rule) {
    case PathRule::allowed:
        return "allowed";
    case PathRule::invalid_path:
        return "invalid-path";
    case PathRule::menus_only:
        return "menus-only";
    case PathRule::maps_only:
        return "maps-only";
    case PathRule::read_once:
        return "read-once";
    case PathRule::not_confirmed:
        return "not-confirmed";
    case PathRule::interface_art:
        return "interface-art";
    case PathRule::unknown_folder:
        return "unknown-folder";
    }
    return "unknown";
}

} // namespace oa::data::mutators
