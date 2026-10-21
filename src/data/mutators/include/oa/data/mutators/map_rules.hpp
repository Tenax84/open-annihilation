// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Rules whose target is maps/*.ota are held out of the rules phase and
// applied to the chosen map, before any maps hook. Contributions combine
// without regard to list order: assignments and metas average, factors
// multiply, and clamps widen.
#pragma once

#include "oa/data/mutation_rules/apply.hpp"
#include "oa/data/mutators/maps.hpp"

#include <span>
#include <string>
#include <vector>

namespace oa::data::mutators {

/// One list of rule records, split by whether the block targets a map OTA.
struct SplitRules {
    /// Blocks whose folder is not `maps`, each mutator keeping source order.
    std::vector<oa::data::mutation_rules::MutatorRules> others;
    /// Blocks whose folder is `maps` and whose extension is `ota`.
    std::vector<oa::data::mutation_rules::MutatorRules> maps;
    /// One sentence per `maps` block whose extension is not `ota`. A
    /// non-empty list means the caller must not apply either half.
    std::vector<std::string> refusals;
};

/// Splits map OTA blocks out of the rules the plan returned.
///
/// A folder equal to `maps`, ignoring ASCII case and reading `\\` as `/`,
/// with extension `ota`, goes to `maps`. The same folder with any other
/// extension is a refusal naming the mutator, the source and the line, and
/// that block is in neither vector. Every other block stays in `others`.
/// A mutator is listed on a side only when it has blocks of that kind, and
/// both halves keep that mutator's name. Sources and blocks stay in order.
///
/// @param rules mutators in list order
/// @return the two halves and any refusals
[[nodiscard]] SplitRules
split_map_rules(std::span<const oa::data::mutation_rules::MutatorRules> rules);

/// Applies map rules to the chosen map's OTA alone.
///
/// The set gains that OTA at `map.ota_path` when the path is set, otherwise
/// at `maps/<key>.ota`, from `map.ota`, when it is not already loaded. The
/// combination does not follow list order. An assignment replaces a factor
/// inside one mutator and resets that mutator's clamps. Across mutators,
/// assignments average, metas average and win over plain numbers, factors
/// multiply after ordering by their exact bits and then their rule text,
/// and clamps widen. A number is written only through the TDF number
/// spelling, and only when it is finite and different.
///
/// The field check uses the maps header limits and refuses every key that
/// is not a key of `[GlobalHeader]`. A header `SurfaceMetal` is not written
/// there: the check refuses it with `copied to every schema's SurfaceMetal`,
/// and a kept number is then written to every schema through the schema
/// metal check. A header `MohoMetal` is refused with `nothing in OA reads
/// MohoMetal; skipped` and written nowhere. Both refusals are
/// `check_refused` notes. Schema writes are `FieldWrite`s whose block is
/// `Schema N`.
///
/// @param maps map-rule mutators, in list order
/// @param map the chosen map; its bytes are the OTA before these rules
/// @param[in,out] documents set that receives the OTA, and the edits
/// @return the writes and the notes
[[nodiscard]] oa::data::mutation_rules::ApplyResult apply_map_rules(
    std::span<const oa::data::mutation_rules::MutatorRules> maps,
    const ChosenMap& map,
    oa::data::mutation_rules::DocumentSet& documents
);

} // namespace oa::data::mutators
