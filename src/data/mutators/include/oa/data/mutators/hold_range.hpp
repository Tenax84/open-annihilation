// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Holds a stacked unit, weapon, movement-class or feature value at the end
// of the range the game reads it in. One pass, on the documents about to be
// published, so every package meets the same limit. Only a value the run
// changed is held: a stock or mod value loads as it always has.
#pragma once

#include "oa/data/defs/layout.hpp"
#include "oa/data/mutation_rules/documents.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace oa::data::mutators {

/// One value rewritten so the loader stores the end of its field.
struct HeldField {
    /// Document path, '/' separated.
    std::string path;
    /// Unit name, weapon section, movement class, feature section or damage section.
    std::string record;
    /// Block inside the record. Empty for the record's own keys.
    std::string block;
    /// Field name, lower case.
    std::string field;
    /// The text the mutators left, which the loader would wrap.
    std::string from_text;
    /// The text now stored, which the loader reads as the range's end.
    std::string to_text;
};

/// Gives a document's text as the game reads it without this run's rules
/// and hooks, or nothing when the run added the path.
using BaseText = std::function<std::optional<std::string>(const std::string& path)>;

/// Rewrites narrow fields the run changed.
///
/// A field is narrow when the loader keeps it in 8, 16 or 32 bits, in a
/// 16.16 value, or in a count of ticks, including a weapon speed or turn
/// rate it scales before storing. Each is held at the range the game reads
/// it in, which is often the unsigned form of what the loader stores, or
/// narrower: `waterline` and the transport sizes are 0 to 255, a unit's
/// `turnrate` and `workertime` and a weapon's `burst`, `sprayangle`, turn
/// rate and tick counts 0 to 65535, a weapon's `range` and `coverage` 0 to
/// 32767 (the module README lists every one). Three more reach the game
/// narrower than their record: `maxdamage` is the unit's 16-bit health (1
/// to 32767), a `[DAMAGE]` entry for a unit name reaches a hit as signed 16
/// bits (-32768 to 32767), `default` is read unsigned before that (0 to
/// 32767), and `selfdestructcountdown` keeps 3 bits (0 to 7). A feature file
/// is held at the range the game reads each field in: metal and energy 0
/// to 65535, and metal 0 to 255 on an indestructible feature that cannot
/// be reclaimed; damage (hit points) 0 to 65535; height, spread chance,
/// reproduce and reproduce area 0 to 255; spark time 0 to 257 ticks, the
/// most whose spread countdown fits its byte; and each footprint at most
/// 256 cells. A value inside the range is left as written. A value past
/// either end is replaced with text the loader reads as that end.
///
/// Documents the run did not change are not opened. In a changed document,
/// a value is held only when it differs from the base text's number for the
/// same section and key: a stock or mod value the run left alone loads as
/// it always has, wrapped or not. A section or document the run added is
/// all the run's own.
///
/// @param documents the pipeline's parsed documents
/// @param layout the mod's directory names and unit extension
/// @param base the base text of a path; empty holds every narrow value of a
///        changed document
/// @return one record for each value that was held, in document order
[[nodiscard]] std::vector<HeldField> hold_narrow_fields(
    oa::data::mutation_rules::DocumentSet& documents,
    const oa::data::defs::DataLayout& layout,
    const BaseText& base
);

/// Reads the whole number an integer field's text holds, before any width.
///
/// It is the number rules and `scale` work on, and the one a script reads, so
/// a stack is worked out on the numbers written and held once, at the end.
/// The digits after a sign and any leading space, as the loader reads them,
/// saturated at the ends of 64 bits rather than wrapped at 32.
///
/// @param text the field's text, or null
/// @return the number, or nothing when the text starts with no digits
[[nodiscard]] std::optional<int64_t> written_whole(const char* text);

} // namespace oa::data::mutators
