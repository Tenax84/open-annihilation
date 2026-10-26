// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Imports TA: Mutation mutators. A .tam, its rule file and its archives become
// one mutator package: a manifest, the rule file unchanged, archives with
// protected entries left out, and a script that adds those files.
#pragma once

#include "oa/formats/oamut/manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::mutator_import {

/// The most bytes of one .tam. A longer file is refused.
inline constexpr std::size_t most_tam_bytes = std::size_t{64} * 1024;

/// The most bytes of one archive or other companion, uncompressed.
inline constexpr std::size_t most_archive_bytes = std::size_t{64} * 1024 * 1024;

/// The most uncompressed bytes `files_from_zip` reads from one zip.
///
/// Only `.tam`, `.sm` and `.ccx` entries count. A zip whose counted entries
/// are larger than this is refused and nothing is read.
inline constexpr std::size_t most_zip_read_bytes = std::size_t{512} * 1024 * 1024;

/// One legacy file, as it was named and as its bytes were.
struct LegacyFile {
    std::string name;           ///< the file name, or a zip path
    std::vector<uint8_t> bytes; ///< the file's bytes
};

/// One .tam, after its text has been read.
struct Tam {
    std::string title;                 ///< the title line, as written
    std::string description;           ///< the description line, as written
    std::string credit;                ///< the credit taken from the description
    std::vector<std::string> codes;    ///< compatibility codes, in order
    int companion_count{};             ///< how many numbered archives, from 0 to 9
    std::string archive_name;          ///< a hard mutator's archive, when a fifth line names it
    std::vector<std::string> warnings; ///< notes that do not refuse the file
};

/// Reads one .tam.
///
/// The bytes are Windows-1252. Lines end in CR LF or LF, and the last line
/// may have no line end. A file longer than most_tam_bytes is refused, and so
/// is a NUL. An empty line between the description and the compatibility line
/// is skipped and noted. A missing title, a missing compatibility line, or a
/// companion count that is not a single digit from 0 to 9 is a problem.
///
/// @param bytes the file
/// @param[out] tam the fields; left unchanged when the file is refused
/// @param[out] problem what is wrong, when the file is refused
/// @return true when `tam` was filled
[[nodiscard]] bool read_tam(std::span<const uint8_t> bytes, Tam& tam, std::string& problem);

/// Makes a mutator id from a title.
///
/// Latin-1 letters become ASCII, the rest is lower case, and every other run
/// of characters becomes one hyphen. Hyphens at either end are dropped. A
/// result longer than 64 bytes is cut at a hyphen. An empty result is
/// returned as it is; the caller may try the file name.
///
/// @param title the title, as UTF-8
/// @return the id, or an empty string when the title has no letters or digits
[[nodiscard]] std::string id_from_title(std::string_view title);

/// Turns a .tam description into the text the manifest stores.
///
/// A run of k `#` becomes ceil(k/2) line breaks. A run of spaces and tabs
/// that holds a tab becomes one space. Each line is trimmed. The result is
/// at most 2,000 bytes, cut at a character boundary.
///
/// @param description the description line
/// @return the manifest text
[[nodiscard]] std::string description_text(std::string_view description);

/// Reads the credit from a description.
///
/// The last line, when it starts with `created by`, `made by` or `by`,
/// ignoring case and the spaces between the words, and then an optional
/// colon, gives a name of 1 to 128 bytes. Any other line gives an empty
/// string.
///
/// @param description the description, after description_text
/// @return the name, or an empty string when the last line names nobody
[[nodiscard]] std::string credit_of(std::string_view description);

/// What the caller chooses for an import. Empty text means the default.
struct ImportOptions {
    std::string id;                ///< one mutator's id; empty to take it from the title
    std::string version;           ///< the version; empty means 1.0
    std::vector<std::string> tags; ///< tags; empty means gameplay, with a warning
    /// Compatibility codes the caller maps, as code then works-with entry.
    std::vector<std::pair<std::string, std::string>> code_map;
    std::string engine; ///< the engine requirement, as written; required
};

/// One mutator the import produced.
struct Imported {
    oa::formats::oamut::Manifest manifest; ///< the manifest that was written
    /// Package paths and their bytes, beginning with oamut.yaml and mutator.lua.
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
    std::vector<std::string> warnings; ///< notes that did not refuse this mutator
};

/// Every mutator an import produced, and what it refused.
///
/// `warnings` holds notes that belong to no one mutator, such as a file no
/// .tam claims. They do not refuse the import.
struct ImportResult {
    std::vector<Imported> mutators;    ///< the mutators that were imported, in file order
    std::vector<std::string> problems; ///< one line each, for a mutator or a file that was refused
    std::vector<std::string> warnings; ///< notes that belong to the whole run
};

/// Imports every .tam in a set of legacy files.
///
/// Each .tam is paired with the companions its name asks for, in the same
/// directory, with ASCII case ignored. A mutator whose rule file or archive
/// is missing is refused and the others are still imported. A file no .tam
/// claims is named in `warnings`. An archive entry on a protected path is
/// left out of the written archive and named in that mutator's warnings.
///
/// @param files the legacy files
/// @param options the caller's id, version, tags, code map and engine requirement
/// @return the mutators, the refusals and the stray-file notes
[[nodiscard]] ImportResult
import_legacy(std::span<const LegacyFile> files, const ImportOptions& options);

/// Reads a zip into legacy files.
///
/// Names are the names the streamed reader decoded. Folders are skipped. A
/// link or a special file refuses the archive. Only a `.tam`, a `.sm` and a
/// `.ccx` are read. Any other entry is listed, with no bytes, so the import
/// can say it belongs to no mutator. A `.tam` larger than most_tam_bytes, a
/// read entry larger than most_archive_bytes, or read entries together larger
/// than most_zip_read_bytes, refuses the archive.
///
/// @param zip the archive's bytes
/// @param[out] files the entries; left unchanged when the archive is refused
/// @param[out] problem what is wrong, when the archive is refused
/// @return true when `files` was filled
[[nodiscard]] bool
files_from_zip(std::span<const uint8_t> zip, std::vector<LegacyFile>& files, std::string& problem);

} // namespace oa::data::mutator_import
