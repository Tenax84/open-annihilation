// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The files half of one mutator's plan, turned into the mutator layer.
// Masks hide what earlier mutators added, then add folders contribute loose
// files and the archives at their top. A mutator that cannot be built is
// left out of the layer, and the rest of the list is still built.
#pragma once

#include "oa/data/mutators/allow_list.hpp"
#include "oa/formats/hpi.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace oa::data::mutators {

/// Most mask patterns one mutator's plan may list.
inline constexpr std::size_t files_plan_mask_limit = 64;

/// Most add folders one mutator's plan may list.
inline constexpr std::size_t files_plan_add_limit = 16;

/// Most archives opened from the top of one add folder.
inline constexpr std::size_t add_folder_archive_limit = 16;

/// Most loose files walked under one add folder.
inline constexpr std::size_t add_folder_file_limit = 16384;

/// Most nested folders walked under one add folder.
inline constexpr std::size_t add_folder_depth_limit = 32;

/// The files part of a mutator's plan, in the order the plan listed it.
struct FilesPlan {
    /// Mask patterns, applied before the add folders.
    std::vector<std::string> mask;
    /// Package-relative folders whose files are added, in the order listed.
    std::vector<std::string> add;
};

/// One mutator whose files plan is built into the layer.
struct MutatorFiles {
    /// The mutator's id, kept as the owner of its paths and masks.
    std::string id;
    /// The installed package folder. Add folders stay inside it.
    std::filesystem::path package_root;
    /// The files plan, already checked for shape by the caller.
    FilesPlan plan;
};

/// Which records a mutator file holds, or `none` when the path is one whole file.
enum class RecordFileKind : uint8_t {
    none,     ///< the path is served as one file
    unit,     ///< one unit: an FBI one level under the units folder
    weapon,   ///< weapon sections: a TDF one level under the weapons folder
    download, ///< download entries: a TDF one level under the download folder
};

/// Classifies a resource path as a record file or a whole file.
///
/// A record file sits one level under the layout's units, weapons or download
/// folder. A unit file uses the layout's unit extension. A weapon or download
/// file ends in `.tdf`. Names are compared without ASCII case. A nested path
/// is a whole file, and so is every other folder.
///
/// @param path resource path, with '/' or '\\' between parts
/// @param layout the mod's directory names and unit extension
/// @return the records the file holds, or `none`
[[nodiscard]] RecordFileKind
record_file_kind(std::string_view path, const oa::data::defs::DataLayout& layout);

/// The bytes of one record file a kept mutator added.
///
/// The spec still keeps one host file per path, so a later mutator wins that
/// path for a whole-file mount. Record files are also kept here, one snapshot
/// per mutator that added the path, including a path a later mutator replaced
/// in the spec. A mask that hides the path drops these snapshots. A later add
/// folder of the same mutator replaces only its own snapshot.
struct RecordFile {
    /// Id of the mutator that added the file.
    std::string mutator;
    /// Resource path, in the spelling that was read.
    std::string path;
    /// The file's bytes.
    std::shared_ptr<const std::vector<uint8_t>> bytes;
};

/// Why a note was recorded while the layer was built.
enum class FileNoteKind : uint8_t {
    add_replaced,       ///< a later mutator replaced an earlier mutator's file
    add_masked,         ///< a mask hid a file an earlier mutator added
    add_folder_missing, ///< an add folder is not in the package
};

/// One note for the change journal.
///
/// For add_replaced, `mutator` is the later mutator that won and `other` is
/// the earlier one. For add_masked, `mutator` is the masking mutator and
/// `other` is the one whose file was hidden. For add_folder_missing, `path`
/// is the folder and `other` is empty.
struct FileNote {
    /// Which situation the note describes.
    FileNoteKind kind{};
    /// The resource path, or the add folder when it is missing.
    std::string path;
    /// The mutator the note is about.
    std::string mutator;
    /// The other mutator, when the note names two.
    std::string other;
};

/// One file a later mutator's file took a path from.
///
/// A loose file is read from `host`, and a file inside an archive from
/// `archive` at `archive_path`. The archive is held here because the
/// layer's archive list keeps only the archives a served path reads.
struct DisplacedFile {
    /// Resource path, in the spelling that was read.
    std::string path;
    /// Id of the mutator whose file it was.
    std::string owner;
    /// Loose file or archive file.
    oa::MutatorSourceKind kind{};
    /// Host path of a loose file.
    std::filesystem::path host;
    /// The archive of an archive file; null for a loose file.
    std::shared_ptr<const oa::HpiArchive> archive;
    /// Path of an archive file inside its archive.
    std::string archive_path;
};

/// One mutator that was left out of the layer, with the reason.
struct MutatorFailure {
    /// The mutator's id.
    std::string mutator;
    /// The reason, naming the mutator and the mask, folder or file.
    std::string message;
};

/// The layer built from a list of mutators, with the journal notes.
struct FilesLayer {
    /// The paths, archives and masks to mount. The function does not mount them.
    /// A record file still occupies one entry here: the later mutator wins the
    /// path. The pipeline reads every mutator's copy from `records` instead.
    oa::MutatorLayerSpec spec;
    /// Notes, in the order the situations were found.
    std::vector<FileNote> notes;
    /// Record files of the mutators that were kept, in the order they were added.
    std::vector<RecordFile> records;
    /// Files a later mutator's file displaced, in the order that happened,
    /// so the files that held one path read from the first to the one the
    /// spec serves. A mask that hides a path drops its displaced files too.
    /// A mutator whose later folder replaces its own file displaces nothing.
    std::vector<DisplacedFile> displaced;
    /// One entry for each mutator that was left out, in list order.
    std::vector<MutatorFailure> failures;
};

/// Reads the bytes of a file a later mutator displaced.
///
/// @param file the displaced file
/// @return the bytes, or null when the file cannot be read or is empty
[[nodiscard]] std::shared_ptr<const std::vector<uint8_t>>
displaced_bytes(const DisplacedFile& file);

/// Builds the mutator file layer from each mutator's files plan.
///
/// Mutators are taken in list order. One that fails is left out, its masks,
/// files, archives and notes with it, and the mutators after it are still
/// built. Each file a later mutator's file replaces is kept in `displaced`. For each mutator that is kept, every mask is applied first and
/// then each add folder in the order listed. An add folder is found one
/// segment at a time, matching the written name without ASCII case, so the
/// same plan names the same directory on every machine. In one add folder,
/// `.ccx` archives are read before `.ufo` archives, each group in NTFS name
/// order (byte by byte after ASCII upper-casing), an earlier archive keeps a
/// shared path, and a loose file keeps a path over an archive. A later folder of the same mutator
/// replaces an earlier path without a note. Paths of one folder are stored
/// in folded-path order, after the paths its earlier folders stored. The
/// same bytes are produced on every machine. A unit FBI, a weapon TDF or a
/// download TDF is also snapshotted into `records` for each mutator that
/// added it. The spec entry for that path is still the one whole file the
/// later mutator won.
///
/// A path is judged with check_mutable_path, and a unit's build picture is
/// allowed when the mod already has that unit or when this mutator or an
/// earlier one still adds its unit file. A mask is judged with
/// check_mutable_mask.
///
/// @param mutators the mutators, in the player's list order
/// @param paths the layout and the unit names already in the mod
/// @return the layer, the notes and one failure for each mutator left out
[[nodiscard]] FilesLayer
build_files_layer(std::span<const MutatorFiles> mutators, const PathContext& paths);

} // namespace oa::data::mutators
