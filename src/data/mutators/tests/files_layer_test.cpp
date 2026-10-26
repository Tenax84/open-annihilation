// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The files layer: loose files over a mutator's own archives, later
// mutators over earlier ones, masks, notes and a mutator that fails alone.

#include "oa/data/mutators/files_layer.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace mutators = oa::data::mutators;

struct Scratch {
    /// The directory this run owns.
    fs::path root;

    Scratch() : root(oa::test::make_scratch_directory("F03-files-layer")) {}

    ~Scratch() {
        std::error_code error;
        fs::remove_all(root, error);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    /// Returns a fresh directory under the scratch root.
    ///
    /// @param name the directory's name
    /// @return the directory
    fs::path dir(std::string_view name) const {
        const fs::path path = root / std::string{name};
        fs::create_directories(path);
        return path;
    }
};

/// Prints a failed requirement before counting it.
///
/// @param condition the requirement
/// @param message what failed
void require(bool condition, const std::string& message) {
    if (!condition)
        std::fprintf(stderr, "files layer: %s\n", message.c_str());
    OA_CHECK(condition);
}

/// Folds a path the way the layer compares paths.
///
/// @param text the path
/// @return '\\' as '/' and ASCII letters in lower case
std::string fold(std::string_view text) {
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

/// Returns the bytes of a short text.
///
/// @param text the text
/// @return the bytes
std::vector<uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

/// Writes a file, creating its parent directories.
///
/// @param path the host path
/// @param text the bytes
void write_text(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream)
        throw std::runtime_error("cannot write " + path.string());
}

/// Writes bytes to a file.
///
/// @param path the host path
/// @param bytes the bytes
void write_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    if (!stream)
        throw std::runtime_error("cannot write " + path.string());
}

/// Reads a whole file.
///
/// @param path the host path
/// @return the bytes
std::string read_text(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{stream}, {}};
}

/// Writes an archive.
///
/// @param path the host path
/// @param files the files inside the archive
void write_archive(const fs::path& path, std::vector<oa::HpiWriteFile> files) {
    write_bytes(path, oa::write_hpi(files));
}

/// Finds a layer entry by its folded path.
///
/// `layer` must outlive the pointer.
///
/// @param layer the built layer
/// @param path the resource path
/// @return the entry, or null
const oa::MutatorLayerEntry* find_entry(const mutators::FilesLayer& layer, std::string_view path) {
    const std::string folded = fold(path);
    for (const oa::MutatorLayerEntry& entry : layer.spec.entries) {
        if (fold(entry.path) == folded)
            return &entry;
    }
    return nullptr;
}

/// Returns whether any entry, mask or archive belongs to a mutator.
///
/// @param layer the built layer
/// @param id the mutator's id
/// @return true when the spec still names the mutator
bool spec_names(const mutators::FilesLayer& layer, std::string_view id) {
    for (const oa::MutatorLayerEntry& entry : layer.spec.entries) {
        if (entry.owner == id)
            return true;
    }
    for (const oa::MutatorMask& mask : layer.spec.masks) {
        if (mask.owner == id)
            return true;
    }
    for (const oa::MutatorLayerArchive& archive : layer.spec.archives) {
        if (archive.owner == id)
            return true;
    }
    return false;
}

/// Returns whether a listing contains a path, ignoring ASCII case.
///
/// @param names the listing
/// @param path the path
/// @return true when one name folds to the path
bool listing_contains(const std::vector<std::string>& names, std::string_view path) {
    const std::string folded = fold(path);
    for (const std::string& name : names) {
        if (fold(name) == folded)
            return true;
    }
    return false;
}

/// Compares the parts of two specs the layer promises are identical.
///
/// @param first one build
/// @param second another build of the same plans
/// @return true when the documented fields match
bool same_spec(const mutators::FilesLayer& first, const mutators::FilesLayer& second) {
    if (first.spec.label != second.spec.label ||
        first.spec.entries.size() != second.spec.entries.size() ||
        first.spec.archives.size() != second.spec.archives.size() ||
        first.spec.masks.size() != second.spec.masks.size() ||
        first.notes.size() != second.notes.size() ||
        first.failures.size() != second.failures.size())
        return false;
    for (std::size_t index = 0; index < first.spec.entries.size(); ++index) {
        const oa::MutatorLayerEntry& earlier = first.spec.entries[index];
        const oa::MutatorLayerEntry& later = second.spec.entries[index];
        if (earlier.path != later.path || earlier.owner != later.owner ||
            earlier.kind != later.kind || earlier.host != later.host ||
            earlier.archive != later.archive || earlier.archive_path != later.archive_path)
            return false;
    }
    for (std::size_t index = 0; index < first.spec.archives.size(); ++index) {
        const oa::MutatorLayerArchive& earlier = first.spec.archives[index];
        const oa::MutatorLayerArchive& later = second.spec.archives[index];
        if (earlier.location != later.location || earlier.owner != later.owner)
            return false;
    }
    for (std::size_t index = 0; index < first.spec.masks.size(); ++index) {
        if (first.spec.masks[index].pattern != second.spec.masks[index].pattern ||
            first.spec.masks[index].owner != second.spec.masks[index].owner)
            return false;
    }
    for (std::size_t index = 0; index < first.notes.size(); ++index) {
        const mutators::FileNote& earlier = first.notes[index];
        const mutators::FileNote& later = second.notes[index];
        if (earlier.kind != later.kind || earlier.path != later.path ||
            earlier.mutator != later.mutator || earlier.other != later.other)
            return false;
    }
    return true;
}

/// A plan with one add folder.
///
/// @param folder the folder
/// @return the plan
mutators::FilesPlan add_plan(std::string folder) {
    mutators::FilesPlan plan;
    plan.add.push_back(std::move(folder));
    return plan;
}

/// Builds one mutator.
///
/// @param id the id
/// @param package the package folder
/// @param plan the files plan
/// @return the mutator
mutators::MutatorFiles mutator(std::string id, fs::path package, mutators::FilesPlan plan) {
    mutators::MutatorFiles files;
    files.id = std::move(id);
    files.package_root = std::move(package);
    files.plan = std::move(plan);
    return files;
}

/// The layout and unit names used when a test does not name a unit.
const mutators::PathContext kPlain;

/// Checks a layer whose bad mutator must fail and whose neighbours must stand.
///
/// The bad mutator also masks `ai/*.txt` and adds `units/RIDGEB.fbi` when
/// its plan says so. Neither may remain.
///
/// @param scratch the scratch directory
/// @param name the case directory
/// @param bad the mutator that must fail
/// @param token text the failure message must contain
void expect_isolated_failure(
    const Scratch& scratch,
    std::string_view name,
    mutators::MutatorFiles bad,
    std::string_view token
) {
    const fs::path before_root = scratch.dir(std::string{name} + "/before");
    const fs::path after_root = scratch.dir(std::string{name} + "/after");
    write_text(before_root / "files" / "units" / "RIDGEA.fbi", "before-unit");
    write_text(before_root / "files" / "ai" / "ridge.txt", "before-ai");
    write_text(after_root / "files" / "weapons" / "ridge.tdf", "after-weapon");
    const mutators::MutatorFiles listed[] = {
        mutator("before-files", before_root, add_plan("files")),
        std::move(bad),
        mutator("after-files", after_root, add_plan("files")),
    };
    const mutators::FilesLayer layer = mutators::build_files_layer(listed, kPlain);
    require(layer.failures.size() == 1, std::string{name} + " failure count");
    if (layer.failures.size() == 1) {
        require(layer.failures[0].mutator == "bad-files", std::string{name} + " failure id");
        require(
            layer.failures[0].message.find("bad-files") != std::string::npos,
            std::string{name} + " message names the mutator: " + layer.failures[0].message
        );
        require(
            layer.failures[0].message.find(token) != std::string::npos,
            std::string{name} + " message names '" + std::string{token} +
                "': " + layer.failures[0].message
        );
    }
    require(!spec_names(layer, "bad-files"), std::string{name} + " leaves the bad mutator out");
    const oa::MutatorLayerEntry* unit = find_entry(layer, "units/RIDGEA.fbi");
    const oa::MutatorLayerEntry* profile = find_entry(layer, "ai/ridge.txt");
    const oa::MutatorLayerEntry* weapon = find_entry(layer, "weapons/ridge.tdf");
    require(
        unit != nullptr && unit->owner == "before-files",
        std::string{name} + " keeps the earlier unit"
    );
    require(
        profile != nullptr && profile->owner == "before-files",
        std::string{name} + " keeps the earlier profile"
    );
    require(
        weapon != nullptr && weapon->owner == "after-files",
        std::string{name} + " keeps the later weapon"
    );
    require(
        find_entry(layer, "units/RIDGEB.fbi") == nullptr, std::string{name} + " drops the bad unit"
    );
    require(layer.spec.label == "mutators before-files after-files", std::string{name} + " label");
    for (const mutators::FileNote& note : layer.notes)
        require(
            note.mutator != "bad-files" && note.other != "bad-files", std::string{name} + " note"
        );
}

/// Loose files win over the same mutator's archive, in folded-path order.
///
/// @param scratch the scratch directory
void loose_file_wins_over_own_archive(const Scratch& scratch) {
    const fs::path package = scratch.dir("loose/ridge-models");
    write_text(package / "files" / "units" / "RIDGEA.fbi", "loose-fbi");
    write_text(package / "files" / "objects3d" / "ridgea.3do", "model");
    write_archive(
        package / "files" / "extra.ufo",
        {
            {"units/RIDGEA.fbi", bytes_of("arch-fbi")},
            {"scripts/ridgea.cob", bytes_of("script")},
        }
    );
    const mutators::MutatorFiles listed[] = {
        mutator("ridge-models", package, add_plan("files")),
    };
    const mutators::FilesLayer layer = mutators::build_files_layer(listed, kPlain);
    require(layer.failures.empty(), "loose: no failure");
    require(layer.notes.empty(), "loose: no note");
    require(layer.spec.label == "mutators ridge-models", "loose: label");
    require(layer.spec.entries.size() == 3, "loose: three files");
    require(layer.spec.archives.size() == 1, "loose: one archive");
    if (layer.spec.entries.size() == 3) {
        require(layer.spec.entries[0].path == "objects3d/ridgea.3do", "loose: model first");
        require(layer.spec.entries[1].path == "scripts/ridgea.cob", "loose: script second");
        require(layer.spec.entries[2].path == "units/RIDGEA.fbi", "loose: unit third");
        require(
            layer.spec.entries[0].kind == oa::MutatorSourceKind::loose, "loose: model is loose"
        );
        require(
            layer.spec.entries[1].kind == oa::MutatorSourceKind::archive,
            "loose: script is archived"
        );
        require(layer.spec.entries[2].kind == oa::MutatorSourceKind::loose, "loose: unit is loose");
        require(layer.spec.entries[0].owner == "ridge-models", "loose: model owner");
        require(layer.spec.entries[1].owner == "ridge-models", "loose: script owner");
        require(layer.spec.entries[2].owner == "ridge-models", "loose: unit owner");
        require(layer.spec.entries[1].archive == 0, "loose: script archive index");
        require(
            layer.spec.entries[1].archive_path == "scripts/ridgea.cob", "loose: script archive path"
        );
        require(read_text(layer.spec.entries[2].host) == "loose-fbi", "loose: the loose unit wins");
        require(
            layer.spec.archives[0].location.filename() == "extra.ufo", "loose: archive file name"
        );
        const auto script = layer.spec.archives[0].archive->read("scripts/ridgea.cob");
        require(script.ok() && *script.value == bytes_of("script"), "loose: script bytes");
    }
    const mutators::FilesLayer again = mutators::build_files_layer(listed, kPlain);
    require(same_spec(layer, again), "loose: a second build matches");
}

/// `.ccx` wins over `.ufo`, and the earlier `.ccx` name wins over the later.
///
/// @param scratch the scratch directory
void archive_group_and_name_order(const Scratch& scratch) {
    const fs::path ccx_over_ufo = scratch.dir("order/ccx-over-ufo");
    write_archive(ccx_over_ufo / "files" / "b.ccx", {{"units/RIDGEA.fbi", bytes_of("from-ccx")}});
    write_archive(ccx_over_ufo / "files" / "a.ufo", {{"units/RIDGEA.fbi", bytes_of("from-ufo")}});
    const mutators::MutatorFiles ccx_listed[] = {
        mutator("ridge-models", ccx_over_ufo, add_plan("files")),
    };
    const mutators::FilesLayer ccx_layer = mutators::build_files_layer(ccx_listed, kPlain);
    require(ccx_layer.failures.empty() && ccx_layer.spec.entries.size() == 1, "ccx: one file");
    require(ccx_layer.spec.archives.size() == 1, "ccx: the ufo is not kept");
    if (ccx_layer.spec.entries.size() == 1 && ccx_layer.spec.archives.size() == 1) {
        require(ccx_layer.spec.entries[0].kind == oa::MutatorSourceKind::archive, "ccx: archived");
        require(ccx_layer.spec.archives[0].location.filename() == "b.ccx", "ccx: b.ccx wins");
        const auto read = ccx_layer.spec.archives[0].archive->read("units/RIDGEA.fbi");
        require(read.ok() && *read.value == bytes_of("from-ccx"), "ccx: bytes");
    }

    const fs::path earlier = scratch.dir("order/earlier-ccx");
    write_archive(earlier / "files" / "a.ccx", {{"units/RIDGEA.fbi", bytes_of("from-a")}});
    write_archive(earlier / "files" / "b.ccx", {{"units/RIDGEA.fbi", bytes_of("from-b")}});
    const mutators::MutatorFiles earlier_listed[] = {
        mutator("ridge-models", earlier, add_plan("files")),
    };
    const mutators::FilesLayer earlier_layer = mutators::build_files_layer(earlier_listed, kPlain);
    require(earlier_layer.spec.archives.size() == 1, "earlier: one archive");
    if (!earlier_layer.spec.archives.empty()) {
        require(
            earlier_layer.spec.archives[0].location.filename() == "a.ccx", "earlier: a.ccx wins"
        );
        const auto read = earlier_layer.spec.archives[0].archive->read("units/RIDGEA.fbi");
        require(read.ok() && *read.value == bytes_of("from-a"), "earlier: bytes");
    }
}

/// An underscore sorts after letters in NTFS order, so `aab.ufo` wins.
///
/// @param scratch the scratch directory
void underscore_archive_follows_ntfs_order(const Scratch& scratch) {
    const fs::path package = scratch.dir("ntfs/ridge-models");
    write_archive(
        package / "files" / "a_b.ufo", {{"units/RIDGEA.fbi", bytes_of("from-underscore")}}
    );
    write_archive(package / "files" / "aab.ufo", {{"units/RIDGEA.fbi", bytes_of("from-aab")}});
    const mutators::MutatorFiles listed[] = {
        mutator("ridge-models", package, add_plan("files")),
    };
    const mutators::FilesLayer layer = mutators::build_files_layer(listed, kPlain);
    require(layer.failures.empty(), "ntfs: no failure");
    require(layer.spec.archives.size() == 1, "ntfs: one archive");
    if (!layer.spec.archives.empty()) {
        require(layer.spec.archives[0].location.filename() == "aab.ufo", "ntfs: aab.ufo wins");
        const auto read = layer.spec.archives[0].archive->read("units/RIDGEA.fbi");
        require(read.ok() && *read.value == bytes_of("from-aab"), "ntfs: bytes");
    }
}

/// `FILES` finds the directory `files` whatever the host does with case.
///
/// @param scratch the scratch directory
void add_folder_ignores_letter_case(const Scratch& scratch) {
    const fs::path package = scratch.dir("case/ridge-models");
    write_text(package / "files" / "units" / "RIDGEA.fbi", "from-files");
    const mutators::MutatorFiles listed[] = {
        mutator("ridge-models", package, add_plan("FILES")),
    };
    const mutators::FilesLayer layer = mutators::build_files_layer(listed, kPlain);
    require(layer.failures.empty() && layer.notes.empty(), "case: FILES finds files");
    const oa::MutatorLayerEntry* unit = find_entry(layer, "units/RIDGEA.fbi");
    require(unit != nullptr && read_text(unit->host) == "from-files", "case: the unit");

    const fs::path deeper = scratch.dir("case/deeper");
    write_text(deeper / "more" / "files" / "units" / "RIDGEA.fbi", "from-deeper");
    mutators::FilesPlan nested;
    nested.add.push_back("MORE/FILES");
    const mutators::MutatorFiles nested_listed[] = {
        mutator("ridge-models", deeper, std::move(nested)),
    };
    const mutators::FilesLayer nested_layer = mutators::build_files_layer(nested_listed, kPlain);
    require(nested_layer.failures.empty() && nested_layer.notes.empty(), "case: each segment");
    const oa::MutatorLayerEntry* deeper_unit = find_entry(nested_layer, "units/RIDGEA.fbi");
    require(
        deeper_unit != nullptr && read_text(deeper_unit->host) == "from-deeper",
        "case: MORE/FILES finds more/files"
    );
}

/// Two directory entries that fold together refuse the mutator.
///
/// A case-insensitive volume cannot hold both names, and the case is skipped.
///
/// @param scratch the scratch directory
void two_folded_names_refuse_the_folder(const Scratch& scratch) {
    const fs::path package = scratch.dir("fail-fold/bad");
    std::error_code create_error;
    fs::create_directories(package / "files", create_error);
    fs::create_directories(package / "FILES", create_error);
    write_text(package / "files" / "units" / "RIDGEB.fbi", "bad-unit");
    std::size_t matches = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(package)) {
        if (fold(entry.path().filename().string()) == "files")
            ++matches;
    }
    if (matches < 2) {
        std::fprintf(stderr, "files layer: skipped the folded-name case\n");
        return;
    }
    mutators::FilesPlan plan = add_plan("files");
    plan.mask.push_back("ai/*.txt");
    expect_isolated_failure(
        scratch,
        "fail-fold",
        mutator("bad-files", package, std::move(plan)),
        "two entries fold to the same name"
    );
}

/// The later mutator keeps a shared path, with one note naming both.
///
/// @param scratch the scratch directory
void later_mutator_replaces(const Scratch& scratch) {
    const fs::path earlier = scratch.dir("replace/ridge-models");
    const fs::path later = scratch.dir("replace/turbo-brains");
    write_text(earlier / "files" / "units" / "RIDGEA.fbi", "earlier");
    write_text(later / "files" / "units" / "RIDGEA.fbi", "later");
    const mutators::MutatorFiles listed[] = {
        mutator("ridge-models", earlier, add_plan("files")),
        mutator("turbo-brains", later, add_plan("files")),
    };
    const mutators::FilesLayer layer = mutators::build_files_layer(listed, kPlain);
    require(layer.failures.empty(), "replace: no failure");
    require(layer.spec.entries.size() == 1, "replace: one file");
    require(layer.notes.size() == 1, "replace: one note");
    if (layer.spec.entries.size() == 1) {
        require(layer.spec.entries[0].owner == "turbo-brains", "replace: later owner");
        require(read_text(layer.spec.entries[0].host) == "later", "replace: later bytes");
    }
    if (layer.notes.size() == 1) {
        require(layer.notes[0].kind == mutators::FileNoteKind::add_replaced, "replace: kind");
        require(layer.notes[0].mutator == "turbo-brains", "replace: winner");
        require(layer.notes[0].other == "ridge-models", "replace: earlier");
        require(fold(layer.notes[0].path) == "units/ridgea.fbi", "replace: path");
    }
    require(layer.spec.label == "mutators ridge-models turbo-brains", "replace: label");
    require(layer.displaced.size() == 1, "replace: one displaced file");
    if (layer.displaced.size() == 1) {
        const mutators::DisplacedFile& file = layer.displaced[0];
        require(file.owner == "ridge-models", "replace: the earlier file is displaced");
        require(fold(file.path) == "units/ridgea.fbi", "replace: displaced path");
        require(file.kind == oa::MutatorSourceKind::loose, "replace: displaced loose file");
        const std::shared_ptr<const std::vector<uint8_t>> bytes = mutators::displaced_bytes(file);
        require(
            bytes != nullptr && std::string(bytes->begin(), bytes->end()) == "earlier",
            "replace: displaced bytes"
        );
    }
}

/// Each file a later mutator replaces is kept in order, from an archive as
/// well as loose, and a mask that hides the path drops them.
///
/// @param scratch the scratch directory
void displaced_files_follow_the_path(const Scratch& scratch) {
    const fs::path first = scratch.dir("displaced/ridge-first");
    const fs::path second = scratch.dir("displaced/ridge-second");
    const fs::path third = scratch.dir("displaced/ridge-third");
    const fs::path masking = scratch.dir("displaced/ridge-mask");
    write_text(first / "files" / "scripts" / "RIDGEA.cob", "first");
    write_text(first / "files" / "scripts" / "RIDGEB.cob", "first-b");
    write_archive(
        second / "files" / "second.ufo",
        {oa::HpiWriteFile{"scripts/RIDGEA.cob", bytes_of("second")}}
    );
    write_text(third / "files" / "scripts" / "RIDGEA.cob", "third");
    write_text(third / "files" / "scripts" / "RIDGEB.cob", "third-b");
    mutators::FilesPlan mask_plan;
    mask_plan.mask.push_back("scripts/RIDGEB.cob");
    const mutators::MutatorFiles listed[] = {
        mutator("ridge-first", first, add_plan("files")),
        mutator("ridge-second", second, add_plan("files")),
        mutator("ridge-third", third, add_plan("files")),
        mutator("ridge-mask", masking, std::move(mask_plan)),
    };
    const mutators::FilesLayer layer = mutators::build_files_layer(listed, kPlain);
    require(layer.failures.empty(), "displaced: no failure");
    require(layer.displaced.size() == 2, "displaced: two files held scripts/RIDGEA.cob first");
    if (layer.displaced.size() != 2)
        return;
    require(layer.displaced[0].owner == "ridge-first", "displaced: the first file first");
    require(layer.displaced[1].owner == "ridge-second", "displaced: the archive file second");
    require(
        layer.displaced[1].kind == oa::MutatorSourceKind::archive &&
            layer.displaced[1].archive != nullptr,
        "displaced: the archive is held"
    );
    const std::shared_ptr<const std::vector<uint8_t>> bytes =
        mutators::displaced_bytes(layer.displaced[1]);
    require(
        bytes != nullptr && std::string(bytes->begin(), bytes->end()) == "second",
        "displaced: the archive file's bytes"
    );
}

/// A mask hides an earlier add, and a later add of a matching path is kept.
///
/// @param scratch the scratch directory
void mask_hides_earlier_add(const Scratch& scratch) {
    const fs::path earlier = scratch.dir("mask/ridge-models");
    const fs::path masking = scratch.dir("mask/turbo-brains");
    const fs::path later = scratch.dir("mask/fresh-brains");
    write_text(earlier / "files" / "ai" / "ridge.txt", "ridge");
    fs::create_directories(masking);
    write_text(later / "files" / "ai" / "turbo.txt", "turbo");
    mutators::FilesPlan mask_plan;
    mask_plan.mask.push_back("ai/*.txt");
    const mutators::MutatorFiles listed[] = {
        mutator("ridge-models", earlier, add_plan("files")),
        mutator("turbo-brains", masking, std::move(mask_plan)),
        mutator("fresh-brains", later, add_plan("files")),
    };
    const mutators::FilesLayer layer = mutators::build_files_layer(listed, kPlain);
    require(layer.failures.empty(), "mask: no failure");
    require(find_entry(layer, "ai/ridge.txt") == nullptr, "mask: the earlier profile is gone");
    const oa::MutatorLayerEntry* turbo = find_entry(layer, "ai/turbo.txt");
    require(turbo != nullptr && turbo->owner == "fresh-brains", "mask: the later profile stays");
    require(layer.spec.masks.size() == 1, "mask: one mask");
    if (!layer.spec.masks.empty()) {
        require(layer.spec.masks[0].pattern == "ai/*.txt", "mask: pattern");
        require(layer.spec.masks[0].owner == "turbo-brains", "mask: owner");
    }
    require(layer.notes.size() == 1, "mask: one note");
    if (!layer.notes.empty()) {
        require(layer.notes[0].kind == mutators::FileNoteKind::add_masked, "mask: kind");
        require(layer.notes[0].mutator == "turbo-brains", "mask: masking mutator");
        require(layer.notes[0].other == "ridge-models", "mask: earlier mutator");
        require(fold(layer.notes[0].path) == "ai/ridge.txt", "mask: path");
    }
}

/// A mutator's second add folder replaces its first with no note.
///
/// @param scratch the scratch directory
void second_folder_replaces_quietly(const Scratch& scratch) {
    const fs::path package = scratch.dir("second/ridge-models");
    write_text(package / "files" / "first" / "units" / "RIDGEA.fbi", "first-fbi");
    write_text(package / "files" / "second" / "units" / "RIDGEA.fbi", "second-fbi");
    mutators::FilesPlan plan;
    plan.add.push_back("files/first");
    plan.add.push_back("files/second");
    const mutators::MutatorFiles listed[] = {mutator("ridge-models", package, std::move(plan))};
    const mutators::FilesLayer layer = mutators::build_files_layer(listed, kPlain);
    require(layer.failures.empty() && layer.notes.empty(), "second: no note and no failure");
    require(layer.displaced.empty(), "second: a mutator displaces none of its own files");
    require(layer.spec.entries.size() == 1, "second: one file");
    if (!layer.spec.entries.empty())
        require(
            read_text(layer.spec.entries[0].host) == "second-fbi", "second: the later folder wins"
        );
}

/// A missing add folder records a note and adds nothing.
///
/// @param scratch the scratch directory
void missing_add_folder(const Scratch& scratch) {
    const fs::path package = scratch.dir("missing/ridge-models");
    fs::create_directories(package);
    const mutators::MutatorFiles listed[] = {
        mutator("ridge-models", package, add_plan("files/ridge")),
    };
    const mutators::FilesLayer layer = mutators::build_files_layer(listed, kPlain);
    require(layer.failures.empty(), "missing: no failure");
    require(layer.spec.entries.empty(), "missing: no file");
    require(layer.spec.archives.empty() && layer.spec.masks.empty(), "missing: nothing else");
    require(layer.notes.size() == 1, "missing: one note");
    if (!layer.notes.empty()) {
        require(layer.notes[0].kind == mutators::FileNoteKind::add_folder_missing, "missing: kind");
        require(layer.notes[0].path == "files/ridge", "missing: folder");
        require(layer.notes[0].mutator == "ridge-models", "missing: mutator");
        require(layer.notes[0].other.empty(), "missing: no other mutator");
    }
    require(layer.spec.label == "mutators ridge-models", "missing: the mutator is kept");
}

/// An archive below the top of the add folder is a loose file, not opened.
///
/// @param scratch the scratch directory
void archive_below_the_top_is_a_file(const Scratch& scratch) {
    const fs::path package = scratch.dir("nested/ridge-models");
    write_text(package / "files" / "objects3d" / "packed.ufo", "not-an-archive");
    write_text(package / "files" / "scripts" / "notes.hpi", "not-an-archive");
    const mutators::MutatorFiles listed[] = {mutator("ridge-models", package, add_plan("files"))};
    const mutators::FilesLayer layer = mutators::build_files_layer(listed, kPlain);
    require(layer.failures.empty(), "nested: a deeper archive is not opened");
    const oa::MutatorLayerEntry* packed = find_entry(layer, "objects3d/packed.ufo");
    const oa::MutatorLayerEntry* notes = find_entry(layer, "scripts/notes.hpi");
    require(
        packed != nullptr && packed->kind == oa::MutatorSourceKind::loose, "nested: ufo is loose"
    );
    require(
        notes != nullptr && notes->kind == oa::MutatorSourceKind::loose, "nested: hpi is loose"
    );
    require(layer.spec.archives.empty(), "nested: nothing was opened");
}

/// A file at the top that is not `.ufo` or `.ccx` is not opened as an archive.
///
/// @param scratch the scratch directory
void other_file_at_the_top_is_not_an_archive(const Scratch& scratch) {
    const fs::path package = scratch.dir("hpi-file/bad");
    write_text(package / "files" / "notes.hpi", "not-an-archive");
    write_text(package / "files" / "units" / "RIDGEB.fbi", "unit");
    mutators::FilesPlan plan = add_plan("files");
    plan.mask.push_back("ai/*.txt");
    expect_isolated_failure(
        scratch,
        "hpi-file",
        mutator("bad-files", package, std::move(plan)),
        "this folder is not one a mutator may change"
    );
}

/// Each refusal drops only that mutator.
///
/// @param scratch the scratch directory
void failures_leave_the_others(const Scratch& scratch) {
    {
        const fs::path package = scratch.dir("fail-gui/bad");
        write_text(package / "files" / "guis" / "RIDGEFAB1.GUI", "gui");
        write_text(package / "files" / "units" / "RIDGEB.fbi", "bad-unit");
        mutators::FilesPlan plan = add_plan("files");
        plan.mask.push_back("ai/*.txt");
        expect_isolated_failure(
            scratch,
            "fail-gui",
            mutator("bad-files", package, std::move(plan)),
            "guis/RIDGEFAB1.GUI"
        );
    }
    {
        const fs::path package = scratch.dir("fail-palette/bad");
        write_text(package / "files" / "palettes" / "palette.pal", "pal");
        write_text(package / "files" / "units" / "RIDGEB.fbi", "bad-unit");
        mutators::FilesPlan plan = add_plan("files");
        plan.mask.push_back("ai/*.txt");
        expect_isolated_failure(
            scratch,
            "fail-palette",
            mutator("bad-files", package, std::move(plan)),
            "palettes/palette.pal"
        );
    }
    {
        const fs::path package = scratch.dir("fail-mask/bad");
        write_text(package / "files" / "units" / "RIDGEB.fbi", "bad-unit");
        mutators::FilesPlan plan = add_plan("files");
        plan.mask.push_back("guis/**");
        plan.mask.push_back("ai/*.txt");
        expect_isolated_failure(
            scratch, "fail-mask", mutator("bad-files", package, std::move(plan)), "guis/**"
        );
    }
    {
        const fs::path package = scratch.dir("fail-parent/bad");
        fs::create_directories(package);
        mutators::FilesPlan plan;
        plan.add.push_back("../outside");
        plan.add.push_back("files");
        plan.mask.push_back("ai/*.txt");
        expect_isolated_failure(
            scratch, "fail-parent", mutator("bad-files", package, std::move(plan)), "../outside"
        );
    }
    {
        const fs::path package = scratch.dir("fail-absolute/bad");
        fs::create_directories(package);
        mutators::FilesPlan plan;
        plan.add.push_back(package.string());
        plan.mask.push_back("ai/*.txt");
        expect_isolated_failure(
            scratch,
            "fail-absolute",
            mutator("bad-files", package, std::move(plan)),
            package.string()
        );
    }
    {
        const fs::path package = scratch.dir("fail-archive/bad");
        write_text(package / "files" / "broken.ufo", "not-an-archive");
        write_text(package / "files" / "units" / "RIDGEB.fbi", "bad-unit");
        mutators::FilesPlan plan = add_plan("files");
        plan.mask.push_back("ai/*.txt");
        expect_isolated_failure(
            scratch, "fail-archive", mutator("bad-files", package, std::move(plan)), "broken.ufo"
        );
    }
    {
        const fs::path package = scratch.dir("fail-count/bad");
        write_text(package / "files" / "units" / "RIDGEB.fbi", "bad-unit");
        mutators::FilesPlan plan = add_plan("files");
        plan.mask.assign(65, "ai/*.txt");
        expect_isolated_failure(
            scratch, "fail-count", mutator("bad-files", package, std::move(plan)), "65"
        );
    }
}

/// A link inside an add folder fails that mutator, where a link can be made.
///
/// @param scratch the scratch directory
void link_inside_add_folder(const Scratch& scratch) {
    const fs::path package = scratch.dir("fail-link/bad");
    const fs::path outside = scratch.dir("fail-link/outside");
    write_text(outside / "secret.fbi", "secret");
    write_text(package / "files" / "units" / "RIDGEB.fbi", "bad-unit");
    const fs::path link = package / "files" / "units" / "linked.fbi";
    std::error_code link_error;
    fs::create_symlink(outside / "secret.fbi", link, link_error);
    std::error_code status_error;
    if (link_error || !fs::is_symlink(fs::symlink_status(link, status_error))) {
        std::fprintf(stderr, "files layer: skipped the link case\n");
        return;
    }
    mutators::FilesPlan plan = add_plan("files");
    plan.mask.push_back("ai/*.txt");
    expect_isolated_failure(
        scratch, "fail-link", mutator("bad-files", package, std::move(plan)), "linked.fbi"
    );
}

/// A build picture is allowed for a known unit or a unit this list adds.
///
/// @param scratch the scratch directory
void build_picture_follows_the_unit(const Scratch& scratch) {
    mutators::PathContext base_unit;
    base_unit.is_unit_name = [](std::string_view name) { return fold(name) == "ridgefab"; };
    const fs::path known = scratch.dir("picture/known");
    write_text(known / "files" / "anims" / "ridgefab1.gaf", "picture");
    const mutators::MutatorFiles known_listed[] = {
        mutator("ridge-models", known, add_plan("files")),
    };
    const mutators::FilesLayer known_layer = mutators::build_files_layer(known_listed, base_unit);
    require(known_layer.failures.empty(), "picture: a base unit allows the picture");
    require(
        find_entry(known_layer, "anims/ridgefab1.gaf") != nullptr, "picture: the picture is kept"
    );

    const fs::path same = scratch.dir("picture/same");
    write_text(same / "files" / "units" / "RIDGEFAB.fbi", "unit");
    write_text(same / "files" / "anims" / "ridgefab1.gaf", "picture");
    const mutators::MutatorFiles same_listed[] = {mutator("ridge-models", same, add_plan("files"))};
    const mutators::FilesLayer same_layer = mutators::build_files_layer(same_listed, kPlain);
    require(same_layer.failures.empty(), "picture: the mutator's own unit allows it");
    require(find_entry(same_layer, "units/RIDGEFAB.fbi") != nullptr, "picture: the unit is kept");
    require(
        find_entry(same_layer, "anims/ridgefab1.gaf") != nullptr, "picture: the picture is kept too"
    );

    const fs::path earlier = scratch.dir("picture/earlier");
    const fs::path later = scratch.dir("picture/later");
    write_text(earlier / "files" / "units" / "RIDGEFAB.fbi", "unit");
    write_text(later / "files" / "anims" / "ridgefab1.gaf", "picture");
    const mutators::MutatorFiles across[] = {
        mutator("ridge-models", earlier, add_plan("files")),
        mutator("turbo-brains", later, add_plan("files")),
    };
    const mutators::FilesLayer across_layer = mutators::build_files_layer(across, kPlain);
    require(across_layer.failures.empty(), "picture: an earlier unit allows the picture");
    require(
        find_entry(across_layer, "anims/ridgefab1.gaf") != nullptr,
        "picture: the later picture stays"
    );

    const fs::path refused = scratch.dir("picture/refused");
    write_text(refused / "files" / "anims" / "ridgefab1.gaf", "picture");
    expect_isolated_failure(
        scratch,
        "picture-refused",
        mutator("bad-files", refused, add_plan("files")),
        "anims/ridgefab1.gaf"
    );

    const fs::path masked = scratch.dir("picture/masked-unit");
    const fs::path masked_picture = scratch.dir("picture/masked-picture");
    write_text(masked / "files" / "units" / "RIDGEFAB.fbi", "unit");
    write_text(masked_picture / "files" / "anims" / "ridgefab1.gaf", "picture");
    mutators::FilesPlan hiding;
    hiding.mask.push_back("units/*.fbi");
    hiding.add.push_back("files");
    const mutators::MutatorFiles hidden[] = {
        mutator("ridge-models", masked, add_plan("files")),
        mutator("bad-files", masked_picture, std::move(hiding)),
    };
    const mutators::FilesLayer hidden_layer = mutators::build_files_layer(hidden, kPlain);
    require(hidden_layer.failures.size() == 1, "picture: hiding the unit refuses the picture");
    require(
        hidden_layer.failures.empty() || hidden_layer.failures[0].mutator == "bad-files",
        "picture: the picture mutator fails"
    );
    require(!spec_names(hidden_layer, "bad-files"), "picture: the failed mask is dropped");
    const oa::MutatorLayerEntry* unit = find_entry(hidden_layer, "units/RIDGEFAB.fbi");
    require(unit != nullptr && unit->owner == "ridge-models", "picture: the unit is restored");
}

/// Mounts a built spec and checks the files a match would see.
///
/// @param scratch the scratch directory
void mounted_layer_serves_and_hides(const Scratch& scratch) {
    const fs::path game = scratch.dir("mount/game");
    const fs::path package = scratch.dir("mount/ridge-models");
    write_text(game / "units" / "RIDGEA.fbi", "base-unit");
    write_text(game / "ai" / "ridge.txt", "base-ai");
    write_text(game / "objects3d" / "ridgea.3do", "base-model");
    write_text(game / "scripts" / "ridgea.cob", "base-script");
    write_text(package / "files" / "units" / "RIDGEA.fbi", "layer-unit");
    write_text(package / "files" / "objects3d" / "ridgea.3do", "layer-model");
    write_archive(
        package / "files" / "extra.ufo", {{"scripts/ridgea.cob", bytes_of("layer-script")}}
    );
    mutators::FilesPlan plan = add_plan("files");
    plan.mask.push_back("ai/*.txt");
    const mutators::MutatorFiles listed[] = {mutator("ridge-models", package, std::move(plan))};
    const mutators::FilesLayer layer = mutators::build_files_layer(listed, kPlain);
    require(layer.failures.empty(), "mount: the layer builds");

    oa::AssetStore store(game);
    std::string error;
    require(store.mount_mutator_layer(layer.spec, &error), "mount: " + error);
    require(store.read("units/RIDGEA.fbi").bytes == bytes_of("layer-unit"), "mount: the unit");
    require(
        store.read("objects3d/ridgea.3do").bytes == bytes_of("layer-model"), "mount: the model"
    );
    const oa::AssetData script = store.read("scripts/ridgea.cob");
    require(script.bytes == bytes_of("layer-script") && script.archived, "mount: the script");
    require(!store.read("units/RIDGEA.fbi").archived, "mount: the loose unit is not archived");
    bool hidden = false;
    try {
        (void)store.read("ai/ridge.txt");
    } catch (const std::runtime_error&) {
        hidden = true;
    }
    require(hidden, "mount: the masked profile cannot be read");
    require(
        !listing_contains(store.list_effective("ai", ".txt"), "ai/ridge.txt"),
        "mount: the masked profile is not listed"
    );
    require(
        listing_contains(store.list_effective("units", ".fbi"), "units/RIDGEA.fbi"),
        "mount: the unit is listed"
    );
}

} // namespace

int main() {
    const Scratch scratch;
    loose_file_wins_over_own_archive(scratch);
    archive_group_and_name_order(scratch);
    underscore_archive_follows_ntfs_order(scratch);
    add_folder_ignores_letter_case(scratch);
    later_mutator_replaces(scratch);
    displaced_files_follow_the_path(scratch);
    mask_hides_earlier_add(scratch);
    second_folder_replaces_quietly(scratch);
    missing_add_folder(scratch);
    archive_below_the_top_is_a_file(scratch);
    other_file_at_the_top_is_not_an_archive(scratch);
    failures_leave_the_others(scratch);
    two_folded_names_refuse_the_folder(scratch);
    link_inside_add_folder(scratch);
    build_picture_follows_the_unit(scratch);
    mounted_layer_serves_and_hides(scratch);
    return oa::test::check_exit_status();
}
