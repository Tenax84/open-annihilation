// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Map rules stay out of the rules phase. Each mutator's blocks run in file
// order through the rules step, and each mutator runs in list order. A
// header SurfaceMetal is copied onto every schema after that mutator's
// step. A header MohoMetal is skipped.
#include "oa/data/mutators/map_rules.hpp"

#include "oa/data/defs/layout.hpp"
#include "oa/formats/tdf/edit.hpp"
#include "oa/formats/tdf/number.hpp"

#include <cmath>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::mutators {
namespace rules = oa::data::mutation_rules;

namespace {

namespace tdf = oa::formats::tdf;
namespace sm = oa::formats::soft_mutator;
namespace defs = oa::data::defs;

constexpr const char* k_copied = "copied to every schema's SurfaceMetal";
constexpr const char* k_moho = "nothing in OA reads MohoMetal; skipped";
constexpr const char* k_header_only = "a rule can change only the map's header";
constexpr const char* k_unread = "the chosen map could not be read";
constexpr const char* k_ota_only = "rules can change a map's OTA only";

/// The header SurfaceMetal one step kept, to copy after that step.
struct MetalHold {
    bool seen{};
    std::optional<std::string> text;
};

char fold_ascii(char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return byte;
}

char fold_path_byte(char byte) {
    if (byte == '\\')
        return '/';
    return fold_ascii(byte);
}

bool same_text(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t offset = 0; offset < left.size(); ++offset) {
        if (fold_ascii(left[offset]) != fold_ascii(right[offset]))
            return false;
    }
    return true;
}

bool same_path(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t offset = 0; offset < left.size(); ++offset) {
        if (fold_path_byte(left[offset]) != fold_path_byte(right[offset]))
            return false;
    }
    return true;
}

bool maps_folder(std::string_view folder) {
    return same_path(folder, "maps");
}

rules::CheckResult check_map_field(
    const rules::FieldRef& field, const std::optional<std::string>& text, MetalHold& metal
) {
    rules::CheckResult verdict;
    if (!field.block.empty() || !same_text(field.record, "GlobalHeader")) {
        verdict.outcome = rules::CheckOutcome::refuse;
        verdict.reason = k_header_only;
        return verdict;
    }
    if (same_text(field.key, "SurfaceMetal")) {
        metal.seen = true;
        metal.text = text;
        verdict.outcome = rules::CheckOutcome::refuse;
        verdict.reason = k_copied;
        return verdict;
    }
    if (same_text(field.key, "MohoMetal")) {
        verdict.outcome = rules::CheckOutcome::refuse;
        verdict.reason = k_moho;
        return verdict;
    }
    const std::string reason = map_header_refusal(field.key, text);
    if (!reason.empty()) {
        verdict.outcome = rules::CheckOutcome::refuse;
        verdict.reason = reason;
        return verdict;
    }
    verdict.outcome = rules::CheckOutcome::ok;
    return verdict;
}

bool fully_numeric_text(const std::string& text) {
    if (text.empty() || text.find('\0') != std::string::npos)
        return false;
    const char* cursor = text.c_str();
    if (*cursor == '+' || *cursor == '-')
        ++cursor;
    bool digit = false;
    bool dot = false;
    for (; *cursor != '\0'; ++cursor) {
        if (*cursor >= '0' && *cursor <= '9') {
            digit = true;
            continue;
        }
        if (*cursor == '.' && !dot) {
            dot = true;
            continue;
        }
        return false;
    }
    if (!digit)
        return false;
    const double number = tdf::parse_double(text.c_str());
    return std::isfinite(number);
}

/// Copies the SurfaceMetal this step kept onto every schema.
///
/// The header write was refused, so the header stays without the key. A
/// value outside 0 to 255 is refused for each schema and none of them is
/// written. A removal or a text that is not a number is not copied.
void copy_surface_metal(
    rules::DocumentSet& documents, rules::ApplyResult& result, const MetalHold& metal
) {
    std::vector<std::string> mutators;
    std::string path;
    std::string record;
    std::string key;
    bool found = false;
    for (const rules::RuleNote& note : result.notes) {
        if (note.kind != rules::NoteKind::check_refused || note.message != k_copied)
            continue;
        mutators = note.mutators;
        path = note.path;
        record = note.record;
        key = note.key;
        found = true;
    }
    if (!found)
        return;
    auto extra = [&](rules::NoteKind kind, std::string note_record, std::string message) {
        rules::RuleNote note;
        note.kind = kind;
        note.mutators = mutators;
        note.path = path;
        note.record = std::move(note_record);
        note.key = key;
        note.message = std::move(message);
        result.notes.push_back(std::move(note));
    };
    if (!metal.text.has_value()) {
        extra(
            rules::NoteKind::check_refused,
            record,
            "SurfaceMetal was removed, so it was not copied onto the schemas"
        );
        return;
    }
    if (!fully_numeric_text(*metal.text)) {
        extra(
            rules::NoteKind::not_a_number,
            record,
            "SurfaceMetal is not a number, so it was not copied onto the schemas"
        );
        return;
    }
    if (documents.size() == 0)
        return;
    tdf::Document* document = documents.at(0).document.get();
    const int schemas = map_schema_count(document);
    const std::string refusal = map_schema_metal_refusal(metal.text);
    if (!refusal.empty()) {
        for (int number = 0; number < schemas; ++number) {
            const std::string label = map_schema_label(number);
            extra(rules::NoteKind::check_refused, label, label + ": " + refusal);
        }
        return;
    }
    for (int number = 0; number < schemas; ++number) {
        tdf::Block* schema = map_schema_block(document, number);
        const std::string label = map_schema_label(number);
        if (schema == nullptr) {
            extra(rules::NoteKind::check_refused, label, "the chosen map could not be written");
            return;
        }
        const char* existing = tdf::find_value(schema, "SurfaceMetal");
        if (existing != nullptr && *metal.text == existing)
            continue;
        std::optional<std::string> previous;
        if (existing != nullptr)
            previous = std::string(existing);
        if (!write_map_field(document, schema, "SurfaceMetal", *metal.text)) {
            extra(rules::NoteKind::check_refused, label, "the chosen map could not be written");
            return;
        }
        rules::FieldWrite done;
        done.mutators = mutators;
        done.path = path;
        done.record = record;
        done.block = label;
        done.key = "SurfaceMetal";
        done.old_text = std::move(previous);
        done.new_text = metal.text;
        result.writes.push_back(std::move(done));
    }
}

std::string ota_path_of(const ChosenMap& map) {
    if (!map.ota_path.empty())
        return map.ota_path;
    return "maps/" + map.key + ".ota";
}

} // namespace

SplitRules split_map_rules(std::span<const rules::MutatorRules> incoming) {
    SplitRules split;
    for (const rules::MutatorRules& one : incoming) {
        rules::MutatorRules other;
        rules::MutatorRules maps;
        other.mutator = one.mutator;
        maps.mutator = one.mutator;
        for (const rules::RuleSource& source : one.sources) {
            rules::RuleSource other_source;
            rules::RuleSource map_source;
            other_source.name = source.name;
            map_source.name = source.name;
            for (const sm::Block& block : source.rules.blocks) {
                if (!maps_folder(block.target.folder)) {
                    other_source.rules.blocks.push_back(block);
                    continue;
                }
                if (!same_text(block.target.extension, "ota")) {
                    std::string refusal = one.mutator;
                    refusal += ": ";
                    refusal += source.name;
                    refusal += ":";
                    refusal += std::to_string(block.line);
                    refusal += ": ";
                    refusal += k_ota_only;
                    split.refusals.push_back(std::move(refusal));
                    continue;
                }
                map_source.rules.blocks.push_back(block);
            }
            if (!other_source.rules.blocks.empty())
                other.sources.push_back(std::move(other_source));
            if (!map_source.rules.blocks.empty())
                maps.sources.push_back(std::move(map_source));
        }
        if (!other.sources.empty())
            split.others.push_back(std::move(other));
        if (!maps.sources.empty())
            split.maps.push_back(std::move(maps));
    }
    return split;
}

rules::ApplyResult apply_map_rules(
    std::span<const rules::MutatorRules> maps, const ChosenMap& map, rules::DocumentSet& documents
) {
    rules::ApplyResult result;
    const std::string path = ota_path_of(map);
    if (documents.find(path) == nullptr) {
        std::string text;
        if (map.ota != nullptr)
            text.assign(reinterpret_cast<const char*>(map.ota->data()), map.ota->size());
        std::string error;
        if (text.empty() || !documents.add(std::move(path), text, &error)) {
            rules::RuleNote unread;
            unread.kind = rules::NoteKind::check_refused;
            for (const rules::MutatorRules& one : maps)
                unread.mutators.push_back(one.mutator);
            unread.message = k_unread;
            result.notes.push_back(std::move(unread));
            return result;
        }
    }
    const defs::DataLayout& layout = defs::data_layout();
    rules::ApplyState state;
    rules::ApplyOptions options;
    options.layout = &layout;
    MetalHold metal;
    options.check = [&metal](const rules::FieldRef& field, const std::optional<std::string>& text) {
        return check_map_field(field, text, metal);
    };
    for (const rules::MutatorRules& one : maps) {
        metal.seen = false;
        metal.text.reset();
        rules::ApplyResult step = rules::apply_step(one, documents, state, options);
        result.writes.insert(
            result.writes.end(),
            std::make_move_iterator(step.writes.begin()),
            std::make_move_iterator(step.writes.end())
        );
        result.notes.insert(
            result.notes.end(),
            std::make_move_iterator(step.notes.begin()),
            std::make_move_iterator(step.notes.end())
        );
        if (metal.seen)
            copy_surface_metal(documents, result, metal);
    }
    return result;
}

} // namespace oa::data::mutators
