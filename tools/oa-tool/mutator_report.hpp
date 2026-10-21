// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutated-data report `mutator diff` prints, and the comparison of two
// reports. The command mounts the game and runs the pipeline; this file only
// reads a store and turns two dumps into lines.
#pragma once

#include "oa/formats/hpi.hpp"

#include <compare>
#include <iosfwd>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace oa::tool::mutator_report {

/// One build-menu entry the download files name, once the builder and the
/// unit are both available. The members are the text the report prints, so
/// the set orders entries by those strings.
struct MenuEntry {
    std::string builder; ///< the builder's unit name
    std::string page;    ///< the MENU value, as written
    std::string button;  ///< the BUTTON value, as written
    std::string unit;    ///< the built unit's name

    /// Orders entries by builder, page, button, then unit, byte by byte.
    ///
    /// @param other the entry to compare
    /// @return the order of the two entries
    [[nodiscard]] auto operator<=>(const MenuEntry& other) const = default;
};

/// One file the store serves.
struct DumpedFile {
    /// Where the bytes come from: an archive's file name, `loose`, or
    /// `mutator <id>`.
    std::string source;
    /// SHA-256 of the bytes, as lower-case hexadecimal.
    std::string sha256;
    /// True when the file is FBI, TDF, GUI or OTA text that parsed.
    bool parsed{false};
    /// Parsed fields. The key is the lower-case section path, a space and
    /// the lower-case field name: `[unitinfo] workertime`. A section whose
    /// name repeats among its siblings is `[name@N]` from its second
    /// occurrence on: `[gadget2@2] grayedout`.
    std::map<std::string, std::string> fields;
};

/// One store, as the report compares it.
///
/// Paths are lower case with `/` separators. `dropped_units` names a unit
/// the game would leave out of the catalog, and the reason is `hidden`,
/// `copyright` or `version`.
struct Dump {
    std::map<std::string, DumpedFile> files;          ///< effective files, by path
    std::set<std::string> hidden;                     ///< paths a mutator layer hides
    std::map<std::string, std::string> dropped_units; ///< unit name to the reason
    std::set<MenuEntry> menus;                        ///< effective download entries
};

/// The mutators behind each changed path and field.
///
/// The key is the lower-case path, a line feed and the field the dump
/// uses: the lower-case section path, a space and the key, as in
/// `[unitinfo] workertime`. The field is empty for a whole file. A menu
/// line is stored under the path `menu` and the unit's name. Ids are the
/// manifest ids, in loadout order, each once.
class Attribution {
  public:

    /// Records one mutator for a path and a field.
    ///
    /// A repeated id is kept once, in the place it first had. Callers add
    /// ids in loadout order.
    ///
    /// @param path document path
    /// @param field the dump's field key, or empty for a whole file; a menu
    ///        line uses the unit name
    /// @param id the mutator's manifest id
    void add(std::string_view path, std::string_view field, std::string_view id);

    /// Returns the ids recorded for a path and a field.
    ///
    /// @param path document path
    /// @param field the dump's field key, or empty for a whole file
    /// @return the ids, or null when none were recorded
    [[nodiscard]] const std::vector<std::string>*
    ids(std::string_view path, std::string_view field) const;

  private:

    std::map<std::string, std::vector<std::string>> by_;
};

/// Patterns from an allowed-differences file, in file order.
struct AllowList {
    std::vector<std::string> patterns; ///< whole-line patterns
};

/// What remains after two reports are folded and matched to an allow list.
struct Comparison {
    std::vector<std::string> left_only;       ///< `left only` lines, sorted
    std::vector<std::string> right_only;      ///< `right only` lines, sorted
    std::vector<std::string> allowed;         ///< difference lines a pattern matched
    std::vector<std::string> unused_patterns; ///< patterns that matched nothing
};

/// Reads one store into a dump.
///
/// Effective files keep their source label and SHA-256. FBI, TDF, GUI and
/// OTA files that parse keep their fields. Hidden paths are the files a
/// mutator layer left out of that listing. A unit is dropped when its FBI
/// is hidden, or when `load_unit_header` refuses it (`copyright`) or leaves
/// it out of the catalog (`version`), including a file that call cannot
/// read. A download entry is kept when its builder and its unit are both
/// available.
///
/// An observer is set for the listing and cleared before return, which
/// clears an observer the caller had installed. When `unchanged` is set, a
/// path the mutator layer does not serve and does not hide keeps that
/// dump's entry, so the game's own bytes are hashed once.
///
/// @param store the mounted game, with any mutator layer already applied
/// @param unchanged an earlier dump of the same store; null hashes every file
/// @return the dump
[[nodiscard]] Dump dump_store(oa::AssetStore& store, const Dump* unchanged = nullptr);

/// Writes the change lines from a before-dump to an after-dump.
///
/// The lines are the report's change lines, without the header, sorted by
/// their bytes. A journal id is added after a tab as `by <id>[,<id>...]`.
///
/// @param before the store before the loadout
/// @param after the store after the loadout
/// @param attribution the mutators behind each write; may be empty
/// @return the lines, in byte order
[[nodiscard]] std::vector<std::string>
report_lines(const Dump& before, const Dump& after, const Attribution& attribution);

/// Folds two reports together and matches the remainder to an allow list.
///
/// The lines compared are the text before a tab. Download file and field
/// lines are dropped. For a unit both sides drop, the reason is removed
/// from the `unit dropped` line and that unit's FBI lines, menu lines and
/// every field of a GUI gadget whose `[COMMON] name` or `name` is that unit
/// are dropped. A pattern matches a whole
/// `left only` or `right only` line. `*` is any run of characters and `?`
/// is one character.
///
/// @param left the first loadout's change lines
/// @param right the second loadout's change lines
/// @param left_after the first loadout's after-dump
/// @param right_after the second loadout's after-dump
/// @param allowed the patterns that may remain
/// @return the remaining lines, the matches and the unused patterns
[[nodiscard]] Comparison compare(
    const std::vector<std::string>& left,
    const std::vector<std::string>& right,
    const Dump& left_after,
    const Dump& right_after,
    const AllowList& allowed
);

/// Reads an allowed-differences file.
///
/// A blank line is ignored and ends a block. A line that starts with `#`
/// is a reason. Every other line is a pattern, and it needs a reason
/// earlier in its block. The text is not trimmed.
///
/// @param text the file's bytes as text
/// @param[out] error the line and the reason when a pattern has none; may be null
/// @return the patterns, or nothing when a pattern has no reason
[[nodiscard]] std::optional<AllowList> parse_allowed(std::string_view text, std::string* error);

/// Writes a dump as `mutator diff --dump` writes it.
///
/// One line per effective path, `path`, a tab, the source, a tab and the
/// SHA-256, then one indented line per field, a tab, the field key, ` = `
/// and the value. Hidden paths are not written. The stream is left as the
/// caller set it.
///
/// @param out the stream to write
/// @param dump the dump
void write_dump(std::ostream& out, const Dump& dump);

} // namespace oa::tool::mutator_report
