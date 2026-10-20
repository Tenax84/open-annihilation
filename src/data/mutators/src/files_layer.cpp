// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The files half of one mutator's plan, turned into the mutator layer.

#include "oa/data/mutators/files_layer.hpp"

#include "oa/base/bytes.hpp"
#include "oa/data/defs/layout.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

/// One file a mutator contributes, before it is stored in the layer.
struct Candidate {
    /// Resource path, with '/' between parts, in the spelling that was read.
    std::string path;
    /// `path` folded, the key the layer keeps one file under.
    std::string folded;
    /// Loose file or archive file.
    oa::MutatorSourceKind kind = oa::MutatorSourceKind::loose;
    /// Host path of a loose file.
    fs::path host;
    /// The open archive, when `kind` is archive.
    std::shared_ptr<const oa::HpiArchive> archive;
    /// Host path of that archive.
    fs::path archive_location;
    /// Path of the file inside the archive.
    std::string archive_path;
    /// When the archive was opened. Earlier archives have smaller numbers.
    std::uint64_t archive_sequence = 0;
};

/// One file kept in the working table.
struct Stored {
    /// Resource path, with '/' between parts.
    std::string path;
    /// Id of the mutator that currently provides the file.
    std::string owner;
    /// Loose file or archive file.
    oa::MutatorSourceKind kind = oa::MutatorSourceKind::loose;
    /// Host path of a loose file.
    fs::path host;
    /// The open archive, when `kind` is archive.
    std::shared_ptr<const oa::HpiArchive> archive;
    /// Host path of that archive.
    fs::path archive_location;
    /// Path of the file inside the archive.
    std::string archive_path;
    /// When the archive was opened.
    std::uint64_t archive_sequence = 0;
    /// When the path was first added. Later folders do not change it.
    std::uint64_t order = 0;
};

/// The working table of one build. A failed mutator is thrown away with its copy.
struct State {
    /// Folded resource path to the file that wins it.
    std::map<std::string, Stored> table;
    /// Masks of the mutators kept so far, in list order.
    std::vector<MutatorMask> masks;
    /// Notes for the mutators kept so far.
    std::vector<FileNote> notes;
    /// Ids of the mutators kept so far, in list order.
    std::vector<std::string> ids;
    /// The next `Stored::order`.
    std::uint64_t next_order = 0;
    /// The next `Candidate::archive_sequence`.
    std::uint64_t next_archive = 0;
};

/// One name directory_iterator returned, before the listing is sorted.
struct Listed {
    /// The name, in the spelling the directory returned.
    std::string name;
    /// Host path of the entry.
    fs::path path;
    /// True when the entry is a directory and not a link.
    bool directory = false;
    /// True when the entry is a link.
    bool link = false;
    /// True when the entry is a regular file and not a link.
    bool file = false;
};

/// Returns a path's UTF-8 spelling.
///
/// @param path the path
/// @return its UTF-8 spelling
std::string utf8_text(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

/// Folds a path for comparison: '\\' becomes '/' and ASCII letters drop their case.
///
/// @param text the path or name
/// @return the folded spelling
std::string folded_path(std::string_view text) {
    std::string folded;
    folded.reserve(text.size());
    for (const unsigned char character : text) {
        if (character == '\\')
            folded.push_back('/');
        else if (character >= 'A' && character <= 'Z')
            folded.push_back(static_cast<char>(character - 'A' + 'a'));
        else
            folded.push_back(static_cast<char>(character));
    }
    return folded;
}

/// Rewrites '\\' as '/' without changing case.
///
/// @param text the path
/// @return the path with '/' between parts
std::string slash_path(std::string_view text) {
    std::string result{text};
    for (char& character : result) {
        if (character == '\\')
            character = '/';
    }
    return result;
}

/// Splits a resource path on '/' and '\\', dropping empty parts.
///
/// @param text the path
/// @return the parts, in order
std::vector<std::string> split_path(std::string_view text) {
    std::vector<std::string> parts;
    std::string part;
    const auto flush = [&]() {
        if (!part.empty())
            parts.push_back(std::move(part));
        part.clear();
    };
    for (const char character : text) {
        if (character == '/' || character == '\\')
            flush();
        else
            part.push_back(character);
    }
    flush();
    return parts;
}

/// Joins a directory prefix and a file name with '/'.
///
/// @param prefix the resource path of the directory, or empty at the add folder
/// @param name the file or directory name
/// @return the resource path
std::string join_resource(std::string_view prefix, std::string_view name) {
    if (prefix.empty())
        return std::string{name};
    std::string joined;
    joined.reserve(prefix.size() + 1 + name.size());
    joined.append(prefix);
    joined.push_back('/');
    joined.append(name);
    return joined;
}

/// Returns whether `candidate` is `root` or a file inside it.
///
/// Both paths are already absolute and have had their links resolved.
///
/// @param root the directory that must contain the candidate
/// @param candidate the resolved path
/// @return true when the candidate does not climb out of the root
bool inside_directory(const fs::path& root, const fs::path& candidate) {
    const fs::path relative = candidate.lexically_relative(root);
    if (relative.empty())
        return false;
    for (const fs::path& part : relative) {
        if (part == "..")
            return false;
    }
    return true;
}

/// Returns whether a directory entry is a link.
///
/// A Windows junction is its own file type on the compiler that has one, and
/// neither a directory nor a regular file. libstdc++ and Apple's libc++ have
/// no enumerator for it, so the type is recognised by what it is not.
///
/// @param status the entry's symlink status
/// @return true for a link
bool is_link(const fs::file_status& status) {
    if (fs::is_symlink(status))
        return true;
    return status.type() != fs::file_type::none && status.type() != fs::file_type::not_found &&
           !fs::is_directory(status) && !fs::is_regular_file(status);
}

/// Returns whether a Windows path is a reparse point.
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

/// Returns the stem of a unit file under the layout's units folder, or empty.
///
/// @param resource the resource path
/// @param layout the mod's directory names
/// @return the stem, in the path's own spelling
std::string unit_stem(std::string_view resource, const oa::data::defs::DataLayout& layout) {
    const std::vector<std::string> parts = split_path(resource);
    if (parts.size() != 2)
        return {};
    const std::string& units =
        layout.directories[static_cast<std::size_t>(oa::data::defs::DataDirectory::units)];
    if (folded_path(parts[0]) != folded_path(units))
        return {};
    const std::string suffix = "." + layout.unit_extension;
    if (parts[1].size() <= suffix.size())
        return {};
    const std::string_view extension =
        std::string_view{parts[1]}.substr(parts[1].size() - suffix.size());
    if (folded_path(extension) != folded_path(suffix))
        return {};
    return parts[1].substr(0, parts[1].size() - suffix.size());
}

/// Returns whether a file name is an archive the files layer opens.
///
/// Only `.ccx` and `.ufo` are archives, compared without ASCII case.
///
/// @param name the file name
/// @return true for those two extensions
bool archive_name(std::string_view name) {
    const std::string folded = folded_path(name);
    return folded.size() >= 4 && (folded.ends_with(".ccx") || folded.ends_with(".ufo"));
}

/// Returns whether the name is a `.ccx` archive.
///
/// @param name the file name
/// @return true for `.ccx`
bool ccx_name(std::string_view name) {
    const std::string folded = folded_path(name);
    return folded.size() >= 4 && folded.ends_with(".ccx");
}

/// Stores a refusal and returns false.
///
/// @param reason receives the message
/// @param message the reason
/// @return false
bool fail(std::string& reason, std::string message) {
    reason = std::move(message);
    return false;
}

/// Names a mutator and a mask or folder in a refusal.
///
/// @param id the mutator's id
/// @param kind "mask" or "add folder"
/// @param item the mask or folder as written
/// @param detail the rest of the reason
/// @return the message
std::string refusal(
    std::string_view id, std::string_view kind, std::string_view item, std::string_view detail
) {
    std::string message;
    message.reserve(id.size() + kind.size() + item.size() + detail.size() + 8);
    message.append(id);
    message.append(": ");
    message.append(kind);
    message.append(" '");
    message.append(item);
    message.append("': ");
    message.append(detail);
    return message;
}

/// Says why an add folder string is not a relative folder inside a package.
///
/// A missing folder is not a reason: the caller treats that as a note.
///
/// @param folder the folder as written
/// @return the reason, or empty when the shape is acceptable
std::string folder_shape_problem(std::string_view folder) {
    if (folder.empty())
        return "the folder is empty";
    if (folder.find('\0') != std::string_view::npos)
        return "the folder contains a NUL";
    if (folder.find(':') != std::string_view::npos)
        return "the folder is drive-qualified";
    if (folder.front() == '/' || folder.front() == '\\')
        return "the folder is absolute";
    std::string segment;
    bool empty_segment = false;
    const auto flush = [&]() {
        if (segment.empty())
            empty_segment = true;
        else if (segment == "..")
            return false;
        segment.clear();
        return true;
    };
    for (const char character : folder) {
        if (character == '/' || character == '\\') {
            if (!flush())
                return "the folder leaves the package";
        } else {
            segment.push_back(character);
        }
    }
    if (!segment.empty() && !flush())
        return "the folder leaves the package";
    if (empty_segment)
        return "the folder has an empty name";
    return {};
}

/// Joins the package and a folder whose shape folder_shape_problem accepted.
///
/// @param package_root the package folder
/// @param folder the relative folder
/// @return the host path, with empty names and "." skipped
fs::path folder_host(const fs::path& package_root, std::string_view folder) {
    fs::path relative;
    std::string segment;
    const auto flush = [&]() {
        if (!segment.empty() && segment != ".")
            relative /= segment;
        segment.clear();
    };
    for (const char character : folder) {
        if (character == '/' || character == '\\')
            flush();
        else
            segment.push_back(character);
    }
    flush();
    return package_root / relative;
}

/// Lists one directory and sorts it by the folded name.
///
/// Host directory order is never kept. Names the fold leaves equal are
/// ordered by their bytes, so the listing is the same on every machine.
///
/// @param directory the directory
/// @param[out] listed the entries, sorted by folded name
/// @param[out] reason the listing error, when one happens
/// @return false when the directory cannot be listed
bool list_directory(const fs::path& directory, std::vector<Listed>& listed, std::string& reason) {
    listed.clear();
    std::error_code status;
    fs::directory_iterator cursor(directory, status);
    if (status)
        return fail(reason, status.message());
    const fs::directory_iterator end;
    while (cursor != end) {
        const fs::directory_entry& item = *cursor;
        std::error_code link_status;
        const fs::file_status linked = item.symlink_status(link_status);
        Listed entry;
        entry.name = utf8_text(item.path().filename());
        entry.path = item.path();
        if (reparse_point(item.path()) || (!link_status && is_link(linked)))
            entry.link = true;
        else if (!link_status && linked.type() == fs::file_type::directory)
            entry.directory = true;
        else if (!link_status && linked.type() == fs::file_type::regular)
            entry.file = true;
        listed.push_back(std::move(entry));
        cursor.increment(status);
        if (status)
            return fail(reason, status.message());
    }
    std::sort(listed.begin(), listed.end(), [](const Listed& before, const Listed& after) {
        const std::string before_path = folded_path(before.name);
        const std::string after_path = folded_path(after.name);
        if (before_path != after_path)
            return before_path < after_path;
        return before.name < after.name;
    });
    return true;
}

/// Copies a candidate into the working table.
///
/// @param candidate the file
/// @param id the mutator that adds it
/// @param order when the path was first added
/// @return the stored file
Stored store_candidate(const Candidate& candidate, const std::string& id, std::uint64_t order) {
    Stored stored;
    stored.path = candidate.path;
    stored.owner = id;
    stored.kind = candidate.kind;
    stored.host = candidate.host;
    stored.archive = candidate.archive;
    stored.archive_location = candidate.archive_location;
    stored.archive_path = candidate.archive_path;
    stored.archive_sequence = candidate.archive_sequence;
    stored.order = order;
    return stored;
}

/// Puts one folder's files into the table, in the vector's order.
///
/// A path the mutator already holds is replaced with no note. A path an
/// earlier mutator holds is replaced with an add_replaced note when `notes`
/// is not null.
///
/// @param table the working table
/// @param next_order the next order number for a path that is new
/// @param files the folder's files, already sorted by folded path
/// @param id the mutator
/// @param notes receives replacement notes, or null when only projecting
void overlay(
    std::map<std::string, Stored>& table,
    std::uint64_t& next_order,
    const std::vector<Candidate>& files,
    const std::string& id,
    std::vector<FileNote>* notes
) {
    for (const Candidate& candidate : files) {
        const auto found = table.find(candidate.folded);
        if (found == table.end()) {
            const std::uint64_t order = next_order;
            ++next_order;
            table.emplace(candidate.folded, store_candidate(candidate, id, order));
            continue;
        }
        if (notes != nullptr && found->second.owner != id) {
            FileNote note;
            note.kind = FileNoteKind::add_replaced;
            note.path = candidate.path;
            note.mutator = id;
            note.other = found->second.owner;
            notes->push_back(std::move(note));
        }
        const std::uint64_t order = found->second.order;
        found->second = store_candidate(candidate, id, order);
    }
}

/// Counts the archives the table still reads.
///
/// @param table the working table
/// @return the number of distinct archives a stored file points at
std::size_t archive_count(const std::map<std::string, Stored>& table) {
    std::set<const oa::HpiArchive*> used;
    for (const auto& item : table) {
        const Stored& stored = item.second;
        if (stored.kind == oa::MutatorSourceKind::archive && stored.archive)
            used.insert(stored.archive.get());
    }
    return used.size();
}

/// Judges the paths one mutator contributes, with the unit files it can see.
///
/// @param table the table after this mutator's folders are projected on
/// @param paths the caller's layout and unit names
/// @param id the mutator
/// @param[out] reason the first refused path
/// @return false when one of the mutator's paths is refused
bool paths_allowed(
    const std::map<std::string, Stored>& table,
    const PathContext& paths,
    const std::string& id,
    std::string& reason
) {
    const oa::data::defs::DataLayout& layout =
        paths.layout != nullptr ? *paths.layout : oa::data::defs::data_layout();
    std::set<std::string> stems;
    for (const auto& item : table) {
        const std::string stem = unit_stem(item.second.path, layout);
        if (!stem.empty())
            stems.insert(folded_path(stem));
    }
    PathContext checking = paths;
    const std::function<bool(std::string_view)> known = paths.is_unit_name;
    checking.is_unit_name = [&stems, &known](std::string_view name) {
        if (stems.contains(folded_path(name)))
            return true;
        return static_cast<bool>(known) && known(name);
    };
    for (const auto& item : table) {
        const Stored& stored = item.second;
        if (stored.owner != id)
            continue;
        const PathVerdict verdict = check_mutable_path(stored.path, checking);
        if (!verdict.allowed())
            return fail(reason, id + ": '" + stored.path + "': " + verdict.message);
    }
    return true;
}

/// Reads the file paths one open archive contributes.
///
/// A record lookup cannot reach is dropped. Two records that fold to one
/// path keep one spelling.
///
/// @param archive the open archive
/// @param location the archive's host path
/// @param sequence when this archive was opened
/// @param[in,out] files folded path to the file that wins it so far
void read_archive(
    const std::shared_ptr<const oa::HpiArchive>& archive,
    const fs::path& location,
    std::uint64_t sequence,
    std::map<std::string, Candidate>& files
) {
    std::map<std::uint32_t, std::string> path_of_node;
    for (const oa::ArchiveEntry& entry : archive->entries()) {
        const std::optional<std::uint32_t> node = archive->lookup(entry.path);
        if (!node)
            continue;
        if (archive->nodes()[*node].directory())
            continue;
        path_of_node[*node] = slash_path(entry.path);
    }
    for (const auto& item : path_of_node) {
        const std::string& path = item.second;
        const std::string folded = folded_path(path);
        if (files.contains(folded))
            continue;
        Candidate candidate;
        candidate.path = path;
        candidate.folded = folded;
        candidate.kind = oa::MutatorSourceKind::archive;
        candidate.archive = archive;
        candidate.archive_location = location;
        candidate.archive_path = path;
        candidate.archive_sequence = sequence;
        files.emplace(folded, std::move(candidate));
    }
}

/// Walks the loose files under an add folder.
///
/// A link fails the mutator. An archive is not opened here: only the top of
/// the add folder opens archives, and that happens before this walk.
///
/// @param walk_root the add folder, with links resolved
/// @param directory the directory being listed
/// @param prefix the resource path of `directory` below the add folder
/// @param depth how many directories below the add folder `directory` is
/// @param id the mutator
/// @param folder the add folder as written, for messages
/// @param[in,out] file_count loose files walked so far
/// @param[in,out] files the folder's files; a loose file replaces an archive
/// @param[out] reason the refusal
/// @return false when the walk must fail the mutator
bool walk_loose(
    const fs::path& walk_root,
    const fs::path& directory,
    const std::string& prefix,
    std::size_t depth,
    const std::string& id,
    std::string_view folder,
    std::size_t& file_count,
    std::map<std::string, Candidate>& files,
    std::string& reason
) {
    std::vector<Listed> listed;
    std::string listed_error;
    if (!list_directory(directory, listed, listed_error))
        return fail(reason, refusal(id, "add folder", folder, listed_error));
    for (const Listed& item : listed) {
        if (item.name.empty() || item.name == "." || item.name == ".." ||
            item.name.find('/') != std::string::npos || item.name.find('\\') != std::string::npos ||
            item.name.find(':') != std::string::npos) {
            return fail(reason, refusal(id, "add folder", folder, "a file name is not usable"));
        }
        const std::string resource = join_resource(prefix, item.name);
        if (item.link)
            return fail(reason, refusal(id, "file", resource, "the file is a link"));
        std::error_code canonical_error;
        const fs::path resolved = fs::weakly_canonical(item.path, canonical_error);
        if (canonical_error || !inside_directory(walk_root, resolved))
            return fail(reason, refusal(id, "file", resource, "the file is outside the package"));
        if (item.directory) {
            if (depth == add_folder_depth_limit) {
                return fail(
                    reason,
                    refusal(
                        id,
                        "add folder",
                        folder,
                        "the folder is nested more than " + std::to_string(add_folder_depth_limit) +
                            " folders deep"
                    )
                );
            }
            if (!walk_loose(
                    walk_root, item.path, resource, depth + 1, id, folder, file_count, files, reason
                ))
                return false;
            continue;
        }
        if (!item.file)
            return fail(reason, refusal(id, "file", resource, "the file is not a regular file"));
        if (file_count == add_folder_file_limit) {
            return fail(
                reason,
                refusal(
                    id,
                    "add folder",
                    folder,
                    "the folder holds more than " + std::to_string(add_folder_file_limit) + " files"
                )
            );
        }
        ++file_count;
        const std::string folded = folded_path(resource);
        Candidate candidate;
        candidate.path = resource;
        candidate.folded = folded;
        candidate.kind = oa::MutatorSourceKind::loose;
        candidate.host = item.path;
        files[folded] = std::move(candidate);
    }
    return true;
}

/// Opens one add folder into the files it contributes.
///
/// @param id the mutator
/// @param folder the folder as written
/// @param host the folder's host path
/// @param[in,out] next_archive the next archive sequence
/// @param[out] files the files, sorted by folded path
/// @param[out] reason the refusal
/// @return false when the folder fails the mutator
bool read_add_folder(
    const std::string& id,
    std::string_view folder,
    const fs::path& host,
    std::uint64_t& next_archive,
    std::vector<Candidate>& files,
    std::string& reason
) {
    std::error_code canonical_error;
    const fs::path walk_root = fs::weakly_canonical(host, canonical_error);
    if (canonical_error)
        return fail(reason, refusal(id, "add folder", folder, "the folder cannot be resolved"));

    std::vector<Listed> listed;
    std::string listed_error;
    if (!list_directory(host, listed, listed_error))
        return fail(reason, refusal(id, "add folder", folder, listed_error));

    std::vector<Listed> ccx_archives;
    std::vector<Listed> ufo_archives;
    std::vector<Listed> others;
    for (const Listed& item : listed) {
        if (item.link)
            return fail(reason, refusal(id, "file", item.name, "the file is a link"));
        if (item.file && archive_name(item.name)) {
            if (ccx_name(item.name))
                ccx_archives.push_back(item);
            else
                ufo_archives.push_back(item);
            continue;
        }
        others.push_back(item);
    }
    if (ccx_archives.size() + ufo_archives.size() > add_folder_archive_limit) {
        return fail(
            reason,
            refusal(
                id,
                "add folder",
                folder,
                "the folder holds more than " + std::to_string(add_folder_archive_limit) +
                    " archives"
            )
        );
    }

    std::map<std::string, Candidate> gathered;
    const auto open_group = [&](const std::vector<Listed>& group) {
        for (const Listed& item : group) {
            auto opened = oa::open_hpi_file(item.path);
            if (!opened.ok()) {
                const char* message = opened.error.message != nullptr ? opened.error.message
                                                                      : "cannot open the archive";
                const std::string code{oa::base::bytes::decode_code_name(opened.error.code)};
                reason =
                    refusal(id, "archive", item.name, std::string{message} + " (" + code + ")");
                return false;
            }
            const std::uint64_t sequence = next_archive;
            ++next_archive;
            const std::shared_ptr<const oa::HpiArchive> held =
                std::make_shared<oa::HpiArchive>(std::move(*opened.value));
            read_archive(held, item.path, sequence, gathered);
        }
        return true;
    };
    // `.ccx` before `.ufo`. Each group is already in folded-name order, which
    // is NTFS order for these names, and the earlier name keeps the path.
    if (!open_group(ccx_archives) || !open_group(ufo_archives))
        return false;

    std::size_t file_count = 0;
    for (const Listed& item : others) {
        if (item.name.empty() || item.name == "." || item.name == ".." ||
            item.name.find('/') != std::string::npos || item.name.find('\\') != std::string::npos ||
            item.name.find(':') != std::string::npos) {
            return fail(reason, refusal(id, "add folder", folder, "a file name is not usable"));
        }
        std::error_code resolved_error;
        const fs::path resolved = fs::weakly_canonical(item.path, resolved_error);
        if (resolved_error || !inside_directory(walk_root, resolved))
            return fail(reason, refusal(id, "file", item.name, "the file is outside the package"));
        if (item.directory) {
            if (!walk_loose(
                    walk_root, item.path, item.name, 1, id, folder, file_count, gathered, reason
                ))
                return false;
            continue;
        }
        if (!item.file)
            return fail(reason, refusal(id, "file", item.name, "the file is not a regular file"));
        if (file_count == add_folder_file_limit) {
            return fail(
                reason,
                refusal(
                    id,
                    "add folder",
                    folder,
                    "the folder holds more than " + std::to_string(add_folder_file_limit) + " files"
                )
            );
        }
        ++file_count;
        Candidate candidate;
        candidate.path = item.name;
        candidate.folded = folded_path(item.name);
        candidate.kind = oa::MutatorSourceKind::loose;
        candidate.host = item.path;
        gathered[candidate.folded] = std::move(candidate);
    }

    files.clear();
    files.reserve(gathered.size());
    for (auto& item : gathered)
        files.push_back(std::move(item.second));
    std::sort(files.begin(), files.end(), [](const Candidate& before, const Candidate& after) {
        return before.folded < after.folded;
    });
    return true;
}

/// Checks one add folder's shape and, when it exists, that it stays in the package.
///
/// @param mutator the mutator
/// @param folder the folder as written
/// @param[out] reason the refusal
/// @return false when the folder fails the mutator
bool accept_add_folder(const MutatorFiles& mutator, std::string_view folder, std::string& reason) {
    if (const std::string shape = folder_shape_problem(folder); !shape.empty())
        return fail(reason, refusal(mutator.id, "add folder", folder, shape));
    const fs::path host = folder_host(mutator.package_root, folder);
    std::error_code status_error;
    const fs::file_status status = fs::symlink_status(host, status_error);
    if (status.type() == fs::file_type::not_found)
        return true;
    if (status_error)
        return fail(reason, refusal(mutator.id, "add folder", folder, status_error.message()));
    if (is_link(status) || reparse_point(host))
        return fail(reason, refusal(mutator.id, "add folder", folder, "the folder is a link"));
    if (status.type() != fs::file_type::directory) {
        return fail(
            reason, refusal(mutator.id, "add folder", folder, "the folder is not a directory")
        );
    }
    std::error_code root_error;
    std::error_code host_error;
    const fs::path root = fs::weakly_canonical(mutator.package_root, root_error);
    const fs::path resolved = fs::weakly_canonical(host, host_error);
    if (root_error || host_error || !inside_directory(root, resolved)) {
        return fail(
            reason,
            refusal(mutator.id, "add folder", folder, "the folder is not inside the package")
        );
    }
    return true;
}

/// Hides every earlier file one mask matches, and records a note for each.
///
/// @param state the working table
/// @param id the masking mutator
/// @param pattern the mask
void apply_mask(State& state, const std::string& id, const std::string& pattern) {
    std::vector<std::string> matched;
    for (const auto& item : state.table) {
        const Stored& stored = item.second;
        if (stored.owner != id && oa::mutator_mask_matches(pattern, stored.path))
            matched.push_back(item.first);
    }
    std::sort(
        matched.begin(),
        matched.end(),
        [&state](const std::string& before, const std::string& after) {
            return state.table.at(before).order < state.table.at(after).order;
        }
    );
    for (const std::string& key : matched) {
        const Stored& stored = state.table.at(key);
        FileNote note;
        note.kind = FileNoteKind::add_masked;
        note.path = stored.path;
        note.mutator = id;
        note.other = stored.owner;
        state.notes.push_back(std::move(note));
        state.table.erase(key);
    }
    MutatorMask mask;
    mask.pattern = pattern;
    mask.owner = id;
    state.masks.push_back(std::move(mask));
}

/// Builds one mutator into a copy of the working table.
///
/// @param mutator the mutator
/// @param paths the layout and the unit names
/// @param[in,out] state the table, changed only when the mutator is kept
/// @param[out] reason the refusal when the mutator is left out
/// @return false when the mutator is left out
bool apply_mutator(
    const MutatorFiles& mutator, const PathContext& paths, State& state, std::string& reason
) {
    if (mutator.plan.mask.size() > files_plan_mask_limit) {
        return fail(
            reason,
            mutator.id + ": the plan lists " + std::to_string(mutator.plan.mask.size()) +
                " masks, and a mutator lists at most " + std::to_string(files_plan_mask_limit)
        );
    }
    if (mutator.plan.add.size() > files_plan_add_limit) {
        return fail(
            reason,
            mutator.id + ": the plan lists " + std::to_string(mutator.plan.add.size()) +
                " add folders, and a mutator lists at most " + std::to_string(files_plan_add_limit)
        );
    }
    if (state.masks.size() >
        oa::formats::hpi::MutatorLayerPatternLimit - mutator.plan.mask.size()) {
        return fail(
            reason,
            mutator.id + ": the layer would list more than " +
                std::to_string(oa::formats::hpi::MutatorLayerPatternLimit) + " masks"
        );
    }
    for (const std::string& pattern : mutator.plan.mask) {
        const PathVerdict verdict = check_mutable_mask(pattern, paths);
        if (!verdict.allowed())
            return fail(reason, refusal(mutator.id, "mask", pattern, verdict.message));
    }
    for (const std::string& folder : mutator.plan.add) {
        if (!accept_add_folder(mutator, folder, reason))
            return false;
    }

    for (const std::string& pattern : mutator.plan.mask)
        apply_mask(state, mutator.id, pattern);

    std::vector<std::vector<Candidate>> folders;
    folders.reserve(mutator.plan.add.size());
    for (const std::string& folder : mutator.plan.add) {
        const fs::path host = folder_host(mutator.package_root, folder);
        std::error_code status_error;
        const fs::file_status status = fs::symlink_status(host, status_error);
        if (status.type() == fs::file_type::not_found) {
            FileNote note;
            note.kind = FileNoteKind::add_folder_missing;
            note.path = folder;
            note.mutator = mutator.id;
            state.notes.push_back(std::move(note));
            continue;
        }
        std::vector<Candidate> files;
        if (!read_add_folder(mutator.id, folder, host, state.next_archive, files, reason))
            return false;
        folders.push_back(std::move(files));
    }

    std::map<std::string, Stored> projected = state.table;
    std::uint64_t projected_order = state.next_order;
    for (const std::vector<Candidate>& files : folders)
        overlay(projected, projected_order, files, mutator.id, nullptr);
    if (projected.size() > oa::formats::hpi::MutatorLayerEntryLimit) {
        return fail(
            reason,
            mutator.id + ": the layer would list more than " +
                std::to_string(oa::formats::hpi::MutatorLayerEntryLimit) + " paths"
        );
    }
    if (archive_count(projected) > oa::formats::hpi::MutatorLayerArchiveLimit) {
        return fail(
            reason,
            mutator.id + ": the layer would hold more than " +
                std::to_string(oa::formats::hpi::MutatorLayerArchiveLimit) + " archives"
        );
    }
    if (!paths_allowed(projected, paths, mutator.id, reason))
        return false;

    for (const std::vector<Candidate>& files : folders)
        overlay(state.table, state.next_order, files, mutator.id, &state.notes);
    return true;
}

/// Writes the working table into the layer's spec.
///
/// @param state the mutators that were kept
/// @param[out] result the layer and its notes
void fill_layer(const State& state, FilesLayer& result) {
    std::vector<const Stored*> ordered;
    ordered.reserve(state.table.size());
    for (const auto& item : state.table)
        ordered.push_back(&item.second);
    std::sort(ordered.begin(), ordered.end(), [](const Stored* before, const Stored* after) {
        return before->order < after->order;
    });

    struct HeldArchive {
        /// When the archive was opened.
        std::uint64_t sequence = 0;
        /// The archive record the spec keeps.
        oa::MutatorLayerArchive archive;
    };

    std::vector<HeldArchive> held;
    std::set<const oa::HpiArchive*> seen;
    for (const Stored* stored : ordered) {
        if (stored->kind != oa::MutatorSourceKind::archive || !stored->archive)
            continue;
        if (!seen.insert(stored->archive.get()).second)
            continue;
        oa::MutatorLayerArchive archive;
        archive.archive = stored->archive;
        archive.location = stored->archive_location;
        archive.owner = stored->owner;
        held.push_back(HeldArchive{stored->archive_sequence, std::move(archive)});
    }
    std::sort(held.begin(), held.end(), [](const HeldArchive& before, const HeldArchive& after) {
        return before.sequence < after.sequence;
    });

    std::map<const oa::HpiArchive*, std::size_t> index_of;
    result.spec.archives.reserve(held.size());
    for (const HeldArchive& item : held) {
        index_of.emplace(item.archive.archive.get(), result.spec.archives.size());
        result.spec.archives.push_back(item.archive);
    }

    result.spec.entries.reserve(ordered.size());
    for (const Stored* stored : ordered) {
        oa::MutatorLayerEntry entry;
        entry.path = stored->path;
        entry.owner = stored->owner;
        entry.kind = stored->kind;
        if (stored->kind == oa::MutatorSourceKind::loose) {
            entry.host = stored->host;
        } else if (stored->kind == oa::MutatorSourceKind::archive) {
            entry.archive = index_of.at(stored->archive.get());
            entry.archive_path = stored->archive_path;
        }
        result.spec.entries.push_back(std::move(entry));
    }
    result.spec.masks = state.masks;
    result.spec.label = "mutators";
    for (const std::string& id : state.ids) {
        result.spec.label.push_back(' ');
        result.spec.label += id;
    }
    result.notes = state.notes;
}

} // namespace

FilesLayer build_files_layer(std::span<const MutatorFiles> mutators, const PathContext& paths) {
    State state;
    FilesLayer result;
    for (const MutatorFiles& mutator : mutators) {
        State attempt = state;
        std::string reason;
        if (!apply_mutator(mutator, paths, attempt, reason)) {
            MutatorFailure failure;
            failure.mutator = mutator.id;
            failure.message = std::move(reason);
            result.failures.push_back(std::move(failure));
            continue;
        }
        state = std::move(attempt);
        state.ids.push_back(mutator.id);
    }
    fill_layer(state, result);
    return result;
}

} // namespace oa::data::mutators
