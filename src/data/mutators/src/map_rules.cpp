// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Map rules stay out of the rules phase. Needs read the chosen map as it
// stands. Each mutator's changes fold into one contribution per field, the
// contributions combine without regard to list order, and the writes happen
// last. A header SurfaceMetal is copied onto every schema instead of the
// header. A header MohoMetal is skipped.
#include "oa/data/mutators/map_rules.hpp"

#include "oa/data/defs/layout.hpp"
#include "oa/formats/tdf/edit.hpp"
#include "oa/formats/tdf/number.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
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

enum class TextOp : uint8_t { none, assign, append, remove };

struct Contrib {
    bool used{};
    bool numeric{};
    bool has_meta{};
    double meta{};
    bool has_assignment{};
    double assignment{};
    bool has_factor{};
    double factor{1};
    std::string factor_tie;
    bool has_clamp{};
    double clamp_low{};
    double clamp_high{};
    TextOp text_op{};
    std::string text;
    std::string source;
    uint32_t line{};
};

struct FieldKey {
    std::size_t doc_index{};
    uint32_t section_index{};
    bool inner{};
    uint32_t child_index{};
    std::string block_fold;
    std::string key_fold;

    bool operator<(const FieldKey& other) const {
        if (doc_index != other.doc_index)
            return doc_index < other.doc_index;
        if (section_index != other.section_index)
            return section_index < other.section_index;
        if (inner != other.inner)
            return !inner && other.inner;
        if (child_index != other.child_index)
            return child_index < other.child_index;
        if (block_fold != other.block_fold)
            return block_fold < other.block_fold;
        return key_fold < other.key_fold;
    }
};

struct Slot {
    bool ready{};
    std::size_t doc_index{};
    uint32_t section_index{};
    bool inner{};
    uint32_t child_index{};
    rules::TargetKind kind{};
    std::string path;
    std::string record;
    std::string block;
    std::string key;
    std::vector<Contrib> contribs;
};

struct Planned {
    std::size_t doc_index{};
    uint32_t section_index{};
    bool inner{};
    uint32_t child_index{};
    rules::TargetKind kind{};
    std::string path;
    std::string record;
    std::string block;
    std::string key;
    bool removal{};
    std::optional<std::string> old_text;
    std::optional<std::string> new_text;
    std::vector<std::string> mutators;
};

struct RecordHit {
    uint32_t section_index{};
    std::string record;
};

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

std::string fold_copy(std::string_view text) {
    std::string out;
    out.resize(text.size());
    for (std::size_t offset = 0; offset < text.size(); ++offset)
        out[offset] = fold_ascii(text[offset]);
    return out;
}

bool less_text(std::string_view left, std::string_view right) {
    const std::size_t shared = left.size() < right.size() ? left.size() : right.size();
    for (std::size_t offset = 0; offset < shared; ++offset) {
        const char left_fold = fold_ascii(left[offset]);
        const char right_fold = fold_ascii(right[offset]);
        if (left_fold < right_fold)
            return true;
        if (left_fold > right_fold)
            return false;
    }
    if (left.size() != right.size())
        return left.size() < right.size();
    return left < right;
}

bool starts_as_number(const char* text) {
    if (text == nullptr)
        return false;
    const auto* cursor = reinterpret_cast<const unsigned char*>(text);
    while (*cursor == ' ' || *cursor == '\t')
        ++cursor;
    if (*cursor == '+' || *cursor == '-')
        ++cursor;
    return *cursor >= '0' && *cursor <= '9';
}

bool maps_folder(std::string_view folder) {
    return same_path(folder, "maps");
}

void clear_number(Contrib& contrib) {
    contrib.numeric = false;
    contrib.has_meta = false;
    contrib.meta = 0;
    contrib.has_assignment = false;
    contrib.assignment = 0;
    contrib.has_factor = false;
    contrib.factor = 1;
    contrib.factor_tie.clear();
    contrib.has_clamp = false;
    contrib.clamp_low = 0;
    contrib.clamp_high = 0;
}

bool apply_number(Contrib& contrib, const sm::Change& change, const std::string& source_name) {
    if (change.kind == sm::ChangeKind::meta) {
        clear_number(contrib);
        contrib.numeric = true;
        contrib.has_meta = true;
        contrib.meta = change.value;
        return true;
    }
    if (contrib.has_meta)
        return false;
    if (change.kind == sm::ChangeKind::assign) {
        contrib.has_assignment = true;
        contrib.assignment = change.value;
        contrib.has_factor = false;
        contrib.factor = 1;
        contrib.factor_tie.clear();
        contrib.has_clamp = false;
        contrib.clamp_low = 0;
        contrib.clamp_high = 0;
        return true;
    }
    std::string piece = source_name;
    piece.push_back(':');
    piece += std::to_string(change.line);
    if (!contrib.has_factor) {
        contrib.has_factor = true;
        contrib.factor = change.value;
        contrib.factor_tie = std::move(piece);
    } else {
        contrib.factor *= change.value;
        contrib.factor_tie.push_back('\n');
        contrib.factor_tie += piece;
    }
    if (change.kind == sm::ChangeKind::scale_clamp) {
        if (!contrib.has_clamp) {
            contrib.has_clamp = true;
            contrib.clamp_low = change.low;
            contrib.clamp_high = change.high;
        } else {
            if (change.low < contrib.clamp_low)
                contrib.clamp_low = change.low;
            if (change.high > contrib.clamp_high)
                contrib.clamp_high = change.high;
        }
    }
    return true;
}

void apply_text(Contrib& contrib, const sm::Change& change) {
    if (change.kind == sm::ChangeKind::assign_text) {
        contrib.text_op = TextOp::assign;
        contrib.text = change.text;
        return;
    }
    if (change.kind == sm::ChangeKind::remove) {
        contrib.text_op = TextOp::remove;
        contrib.text.clear();
        return;
    }
    if (contrib.text_op == TextOp::remove) {
        contrib.text_op = TextOp::assign;
        contrib.text = change.text;
        return;
    }
    if (contrib.text_op == TextOp::assign) {
        contrib.text += change.text;
        return;
    }
    contrib.text_op = TextOp::append;
    contrib.text += change.text;
}

void fold_one(Contrib& contrib, const sm::Change& change, const std::string& source_name) {
    const bool as_text = change.kind == sm::ChangeKind::assign_text ||
                         change.kind == sm::ChangeKind::remove ||
                         change.kind == sm::ChangeKind::append;
    bool took = false;
    if (as_text) {
        clear_number(contrib);
        apply_text(contrib, change);
        took = true;
    } else {
        if (contrib.text_op != TextOp::none) {
            contrib.text_op = TextOp::none;
            contrib.text.clear();
        }
        contrib.numeric = true;
        took = apply_number(contrib, change, source_name);
    }
    contrib.used = true;
    if (took) {
        contrib.source = source_name;
        contrib.line = change.line;
    }
}

const tdf::Block*
section_at(rules::DocumentSet& documents, std::size_t doc_index, uint32_t section_index) {
    return tdf::child_at(documents.at(doc_index).document.root(), section_index);
}

std::vector<RecordHit> records_of(const rules::LoadedDocument& loaded) {
    std::vector<RecordHit> hits;
    const tdf::Block* root = loaded.document.root();
    const uint32_t sections = tdf::child_count(root);
    for (uint32_t section_index = 0; section_index < sections; ++section_index) {
        const tdf::Block* section = tdf::child_at(root, section_index);
        if (section == nullptr || section->name == nullptr)
            continue;
        RecordHit hit;
        hit.section_index = section_index;
        hit.record = section->name;
        hits.push_back(std::move(hit));
    }
    return hits;
}

bool selected(
    rules::DocumentSet& documents,
    std::size_t doc_index,
    const RecordHit& hit,
    const sm::Block& rule,
    const defs::DataLayout& layout
) {
    if (rule.selector == sm::Selector::all)
        return true;
    if (rule.selector == sm::Selector::name)
        return same_text(hit.record, rule.name);
    const tdf::Block* section = section_at(documents, doc_index, hit.section_index);
    for (const sm::Need& need : rule.needs) {
        if (!rules::need_met(
                section, rules::TargetKind::other, documents.at(doc_index).path, layout, need
            ))
            return false;
    }
    return true;
}

const char* property_spelling(const tdf::Block* target, std::string_view key_text) {
    if (target == nullptr)
        return nullptr;
    const uint32_t keys = tdf::property_count(target);
    for (uint32_t key_index = 0; key_index < keys; ++key_index) {
        const char* got = tdf::property_key_at(target, static_cast<int32_t>(key_index));
        if (got != nullptr && same_text(got, key_text))
            return got;
    }
    return nullptr;
}

const char* current_of(rules::DocumentSet& documents, const Slot& slot) {
    const tdf::Block* section = section_at(documents, slot.doc_index, slot.section_index);
    if (section == nullptr)
        return nullptr;
    const tdf::Block* target = section;
    if (slot.inner) {
        target = tdf::child_at(section, slot.child_index);
        if (target == nullptr)
            return nullptr;
    }
    return tdf::find_value(target, slot.key.c_str());
}

bool slot_before(const Slot* left, const Slot* right) {
    if (less_text(left->path, right->path))
        return true;
    if (less_text(right->path, left->path))
        return false;
    if (less_text(left->record, right->record))
        return true;
    if (less_text(right->record, left->record))
        return false;
    if (less_text(left->block, right->block))
        return true;
    if (less_text(right->block, left->block))
        return false;
    return less_text(left->key, right->key);
}

bool write_before(const rules::FieldWrite& left, const rules::FieldWrite& right) {
    if (less_text(left.path, right.path))
        return true;
    if (less_text(right.path, left.path))
        return false;
    if (less_text(left.record, right.record))
        return true;
    if (less_text(right.record, left.record))
        return false;
    if (less_text(left.block, right.block))
        return true;
    if (less_text(right.block, left.block))
        return false;
    return less_text(left.key, right.key);
}

bool note_before(const rules::RuleNote& left, const rules::RuleNote& right) {
    if (less_text(left.path, right.path))
        return true;
    if (less_text(right.path, left.path))
        return false;
    if (less_text(left.record, right.record))
        return true;
    if (less_text(right.record, left.record))
        return false;
    if (less_text(left.key, right.key))
        return true;
    if (less_text(right.key, left.key))
        return false;
    if (left.kind != right.kind)
        return static_cast<uint8_t>(left.kind) < static_cast<uint8_t>(right.kind);
    if (left.line != right.line)
        return left.line < right.line;
    if (less_text(left.source, right.source))
        return true;
    if (less_text(right.source, left.source))
        return false;
    return less_text(left.message, right.message);
}

rules::RuleNote field_note(
    rules::NoteKind kind,
    std::vector<std::string> mutator_names,
    const Slot& slot,
    std::string message
) {
    rules::RuleNote note;
    note.kind = kind;
    note.mutators = std::move(mutator_names);
    note.path = slot.path;
    note.record = slot.record;
    note.key = slot.key;
    note.message = std::move(message);
    return note;
}

void fill_one_source(
    rules::RuleNote& note,
    const std::vector<Contrib>& contribs,
    const std::vector<std::size_t>& involved
) {
    if (involved.size() != 1)
        return;
    const Contrib& contrib = contribs[involved[0]];
    note.source = contrib.source;
    note.line = contrib.line;
}

std::vector<std::string>
names_of(const std::vector<std::size_t>& involved, std::span<const rules::MutatorRules> mutators) {
    std::vector<std::string> names;
    names.reserve(involved.size());
    for (const std::size_t mutator_index : involved)
        names.push_back(mutators[mutator_index].mutator);
    return names;
}

struct FactorTerm {
    double value{};
    uint64_t bits{};
    std::string rule_text;
};

bool factor_before(const FactorTerm& left, const FactorTerm& right) {
    if (left.bits != right.bits)
        return left.bits < right.bits;
    return left.rule_text < right.rule_text;
}

void combine_slot(
    const Slot& slot,
    const char* current,
    std::span<const rules::MutatorRules> mutators,
    std::vector<Planned>& planned,
    std::vector<rules::RuleNote>& notes
) {
    std::vector<std::size_t> metas;
    std::vector<std::size_t> plain;
    std::vector<std::size_t> text_ones;
    std::vector<std::size_t> appends;
    std::vector<std::size_t> non_appends;
    for (std::size_t mutator_index = 0; mutator_index < slot.contribs.size(); ++mutator_index) {
        const Contrib& contrib = slot.contribs[mutator_index];
        if (!contrib.used)
            continue;
        if (contrib.numeric && contrib.has_meta) {
            metas.push_back(mutator_index);
            continue;
        }
        if (contrib.numeric &&
            (contrib.has_assignment || contrib.has_factor || contrib.has_clamp)) {
            plain.push_back(mutator_index);
            continue;
        }
        if (contrib.text_op == TextOp::none)
            continue;
        text_ones.push_back(mutator_index);
        if (contrib.text_op == TextOp::append)
            appends.push_back(mutator_index);
        else
            non_appends.push_back(mutator_index);
    }

    const bool number_wins = !metas.empty() || !plain.empty();
    if (number_wins && !text_ones.empty()) {
        rules::RuleNote dropped = field_note(
            rules::NoteKind::not_a_number,
            names_of(text_ones, mutators),
            slot,
            "a number change dropped the text changes"
        );
        fill_one_source(dropped, slot.contribs, text_ones);
        notes.push_back(std::move(dropped));
    }

    auto old_text = [&]() {
        std::optional<std::string> previous;
        if (current != nullptr)
            previous = std::string(current);
        return previous;
    };

    if (!metas.empty()) {
        double sum = 0;
        for (const std::size_t mutator_index : metas)
            sum += slot.contribs[mutator_index].meta;
        const double result_number = sum / static_cast<double>(metas.size());
        if (!std::isfinite(result_number)) {
            notes.push_back(field_note(
                rules::NoteKind::not_finite,
                names_of(metas, mutators),
                slot,
                "the combined value is not finite"
            ));
            return;
        }
        const std::optional<std::string> spelled = tdf::format_number(result_number);
        if (!spelled.has_value()) {
            notes.push_back(field_note(
                rules::NoteKind::not_finite,
                names_of(metas, mutators),
                slot,
                "the combined value is not finite"
            ));
            return;
        }
        if (current != nullptr && starts_as_number(current) &&
            tdf::parse_double(current) == result_number)
            return;
        Planned write;
        write.doc_index = slot.doc_index;
        write.section_index = slot.section_index;
        write.inner = slot.inner;
        write.child_index = slot.child_index;
        write.kind = slot.kind;
        write.path = slot.path;
        write.record = slot.record;
        write.block = slot.block;
        write.key = slot.key;
        write.old_text = old_text();
        write.new_text = *spelled;
        write.mutators = names_of(metas, mutators);
        planned.push_back(std::move(write));
        return;
    }

    if (!plain.empty()) {
        bool creates = false;
        for (const std::size_t mutator_index : plain) {
            if (slot.contribs[mutator_index].has_assignment)
                creates = true;
        }
        if (!creates && current == nullptr) {
            rules::RuleNote absent = field_note(
                rules::NoteKind::factor_on_absent_field,
                names_of(plain, mutators),
                slot,
                "a factor did nothing because the field is absent"
            );
            fill_one_source(absent, slot.contribs, plain);
            notes.push_back(std::move(absent));
            return;
        }
        if (!creates && !starts_as_number(current)) {
            rules::RuleNote skipped = field_note(
                rules::NoteKind::not_a_number,
                names_of(plain, mutators),
                slot,
                "the field text is not a number"
            );
            fill_one_source(skipped, slot.contribs, plain);
            notes.push_back(std::move(skipped));
            return;
        }
        double base = 0;
        uint32_t assign_total = 0;
        if (creates) {
            double sum = 0;
            for (const std::size_t mutator_index : plain) {
                const Contrib& contrib = slot.contribs[mutator_index];
                if (!contrib.has_assignment)
                    continue;
                sum += contrib.assignment;
                ++assign_total;
            }
            base = sum / static_cast<double>(assign_total);
        } else {
            base = tdf::parse_double(current);
        }
        double product = 1;
        bool any_clamp = false;
        double bound_low = 0;
        double bound_high = 0;
        std::vector<std::size_t> writers;
        std::vector<FactorTerm> factors;
        for (const std::size_t mutator_index : plain) {
            const Contrib& contrib = slot.contribs[mutator_index];
            if (contrib.has_factor) {
                FactorTerm term;
                term.value = contrib.factor;
                term.bits = std::bit_cast<uint64_t>(contrib.factor);
                term.rule_text = contrib.factor_tie;
                factors.push_back(std::move(term));
            }
            if (contrib.has_clamp) {
                if (!any_clamp) {
                    any_clamp = true;
                    bound_low = contrib.clamp_low;
                    bound_high = contrib.clamp_high;
                } else {
                    if (contrib.clamp_low < bound_low)
                        bound_low = contrib.clamp_low;
                    if (contrib.clamp_high > bound_high)
                        bound_high = contrib.clamp_high;
                }
            }
            if (contrib.has_assignment || contrib.has_factor || contrib.has_clamp)
                writers.push_back(mutator_index);
        }
        std::stable_sort(factors.begin(), factors.end(), factor_before);
        for (const FactorTerm& term : factors)
            product *= term.value;
        double result_number = base * product;
        if (!std::isfinite(result_number)) {
            notes.push_back(field_note(
                rules::NoteKind::not_finite,
                names_of(writers, mutators),
                slot,
                "the combined value is not finite"
            ));
            return;
        }
        if (any_clamp) {
            if (result_number < bound_low)
                result_number = bound_low;
            else if (result_number > bound_high)
                result_number = bound_high;
        }
        if (!std::isfinite(result_number)) {
            notes.push_back(field_note(
                rules::NoteKind::not_finite,
                names_of(writers, mutators),
                slot,
                "the combined value is not finite"
            ));
            return;
        }
        const std::optional<std::string> spelled = tdf::format_number(result_number);
        if (!spelled.has_value()) {
            notes.push_back(field_note(
                rules::NoteKind::not_finite,
                names_of(writers, mutators),
                slot,
                "the combined value is not finite"
            ));
            return;
        }
        if (current != nullptr && starts_as_number(current) &&
            tdf::parse_double(current) == result_number)
            return;
        Planned write;
        write.doc_index = slot.doc_index;
        write.section_index = slot.section_index;
        write.inner = slot.inner;
        write.child_index = slot.child_index;
        write.kind = slot.kind;
        write.path = slot.path;
        write.record = slot.record;
        write.block = slot.block;
        write.key = slot.key;
        write.old_text = old_text();
        write.new_text = *spelled;
        write.mutators = names_of(writers, mutators);
        planned.push_back(std::move(write));
        return;
    }

    if (appends.empty() && non_appends.size() > 1) {
        rules::RuleNote cancelled = field_note(
            rules::NoteKind::not_a_number,
            names_of(non_appends, mutators),
            slot,
            "text assignments cancel, so the field is unchanged"
        );
        fill_one_source(cancelled, slot.contribs, non_appends);
        notes.push_back(std::move(cancelled));
        return;
    }
    if (!appends.empty() && !non_appends.empty()) {
        rules::RuleNote lost = field_note(
            rules::NoteKind::not_a_number,
            names_of(non_appends, mutators),
            slot,
            "an assignment or removal lost to an append"
        );
        fill_one_source(lost, slot.contribs, non_appends);
        notes.push_back(std::move(lost));
    }

    if (!appends.empty()) {
        std::string built = current != nullptr ? std::string(current) : std::string{};
        for (const std::size_t mutator_index : appends)
            built += slot.contribs[mutator_index].text;
        if (current != nullptr && built == current)
            return;
        if (current == nullptr && built.empty())
            return;
        Planned write;
        write.doc_index = slot.doc_index;
        write.section_index = slot.section_index;
        write.inner = slot.inner;
        write.child_index = slot.child_index;
        write.kind = slot.kind;
        write.path = slot.path;
        write.record = slot.record;
        write.block = slot.block;
        write.key = slot.key;
        write.old_text = old_text();
        write.new_text = std::move(built);
        write.mutators = names_of(appends, mutators);
        planned.push_back(std::move(write));
        return;
    }

    if (non_appends.size() != 1)
        return;
    const std::size_t mutator_index = non_appends[0];
    const Contrib& contrib = slot.contribs[mutator_index];
    if (contrib.text_op == TextOp::remove) {
        if (current == nullptr)
            return;
        Planned write;
        write.doc_index = slot.doc_index;
        write.section_index = slot.section_index;
        write.inner = slot.inner;
        write.child_index = slot.child_index;
        write.kind = slot.kind;
        write.path = slot.path;
        write.record = slot.record;
        write.block = slot.block;
        write.key = slot.key;
        write.removal = true;
        write.old_text = old_text();
        write.mutators = names_of(non_appends, mutators);
        planned.push_back(std::move(write));
        return;
    }
    if (current != nullptr && contrib.text == current)
        return;
    Planned write;
    write.doc_index = slot.doc_index;
    write.section_index = slot.section_index;
    write.inner = slot.inner;
    write.child_index = slot.child_index;
    write.kind = slot.kind;
    write.path = slot.path;
    write.record = slot.record;
    write.block = slot.block;
    write.key = slot.key;
    write.old_text = old_text();
    write.new_text = contrib.text;
    write.mutators = names_of(non_appends, mutators);
    planned.push_back(std::move(write));
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

void commit_write(
    rules::DocumentSet& documents,
    Planned& write,
    const rules::ApplyOptions& options,
    rules::ApplyResult& result
) {
    std::optional<std::string> proposed;
    if (!write.removal && write.new_text.has_value())
        proposed = write.new_text;
    if (options.check) {
        rules::FieldRef ref;
        ref.kind = write.kind;
        ref.path = write.path;
        ref.record = write.record;
        ref.block = write.block;
        ref.key = write.key;
        const rules::CheckResult verdict = options.check(ref, proposed);
        if (verdict.outcome == rules::CheckOutcome::refuse) {
            rules::RuleNote refused;
            refused.kind = rules::NoteKind::check_refused;
            refused.mutators = write.mutators;
            refused.path = write.path;
            refused.record = write.record;
            refused.key = write.key;
            refused.message = verdict.reason;
            result.notes.push_back(std::move(refused));
            return;
        }
        if (verdict.outcome == rules::CheckOutcome::warn) {
            rules::RuleNote caution;
            caution.kind = rules::NoteKind::check_warned;
            caution.mutators = write.mutators;
            caution.path = write.path;
            caution.record = write.record;
            caution.key = write.key;
            caution.message = verdict.reason;
            result.notes.push_back(std::move(caution));
        }
    }
    tdf::Document* document = documents.at(write.doc_index).document.get();
    tdf::Block* root = tdf::editable_root(document);
    tdf::Block* section = tdf::editable_child(root, write.section_index);
    tdf::Block* target = section;
    if (write.inner)
        target = section != nullptr ? tdf::editable_child(section, write.child_index) : nullptr;
    if (target == nullptr || (!write.removal && !write.new_text.has_value())) {
        rules::RuleNote refused;
        refused.kind = rules::NoteKind::check_refused;
        refused.mutators = write.mutators;
        refused.path = write.path;
        refused.record = write.record;
        refused.key = write.key;
        refused.message = "the record is no longer in the document";
        result.notes.push_back(std::move(refused));
        return;
    }
    const tdf::EditStatus status =
        write.removal ? tdf::remove_key(document, target, write.key)
                      : tdf::set_value(document, target, write.key, *write.new_text);
    if (status != tdf::EditStatus::ok) {
        rules::RuleNote refused;
        refused.kind = rules::NoteKind::check_refused;
        refused.mutators = write.mutators;
        refused.path = write.path;
        refused.record = write.record;
        refused.key = write.key;
        refused.message = tdf::edit_status_message(status);
        result.notes.push_back(std::move(refused));
        return;
    }
    rules::FieldWrite done;
    done.mutators = write.mutators;
    done.path = write.path;
    done.record = write.record;
    done.block = write.block;
    done.key = write.key;
    done.old_text = std::move(write.old_text);
    done.new_text = write.removal ? std::nullopt : write.new_text;
    result.writes.push_back(std::move(done));
}

bool fully_numeric_text(const std::string& text) {
    if (text.empty() || text.find('\0') != std::string::npos)
        return false;
    double number = 0;
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
    number = tdf::parse_double(text.c_str());
    return std::isfinite(number);
}

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
    std::map<FieldKey, Slot> slots;
    MetalHold metal;

    auto ensure = [&](const FieldKey& key,
                      std::size_t doc_index,
                      const RecordHit& hit,
                      std::string block_spell,
                      std::string key_spell) -> Slot& {
        Slot& slot = slots[key];
        if (!slot.ready) {
            slot.ready = true;
            slot.doc_index = doc_index;
            slot.section_index = hit.section_index;
            slot.inner = key.inner;
            slot.child_index = key.child_index;
            slot.kind = rules::TargetKind::other;
            slot.path = documents.at(doc_index).path;
            slot.record = hit.record;
            slot.block = std::move(block_spell);
            slot.key = std::move(key_spell);
            slot.contribs.resize(maps.size());
        }
        return slot;
    };

    auto fold_field = [&](std::size_t mutator_index,
                          const std::string& source_name,
                          std::size_t doc_index,
                          const RecordHit& hit,
                          bool inner,
                          uint32_t child_index,
                          std::string block_spell,
                          const tdf::Block* target,
                          const sm::Change& change) {
        FieldKey key;
        key.doc_index = doc_index;
        key.section_index = hit.section_index;
        key.inner = inner;
        key.child_index = inner ? child_index : 0;
        key.block_fold = fold_copy(block_spell);
        key.key_fold = fold_copy(change.field);
        std::string key_spell = change.field;
        const char* existing = property_spelling(target, change.field);
        const bool created = existing == nullptr;
        if (!created)
            key_spell = existing;
        Slot& slot = ensure(key, doc_index, hit, std::move(block_spell), std::move(key_spell));
        if (created && change.field < slot.key)
            slot.key = change.field;
        if (mutator_index < slot.contribs.size())
            fold_one(slot.contribs[mutator_index], change, source_name);
    };

    for (std::size_t mutator_index = 0; mutator_index < maps.size(); ++mutator_index) {
        const rules::MutatorRules& one = maps[mutator_index];
        for (const rules::RuleSource& source : one.sources) {
            for (const sm::Block& rule : source.rules.blocks) {
                const std::vector<std::size_t> docs =
                    documents.documents_in(rule.target.folder, rule.target.extension, false);
                bool matched = false;
                for (const std::size_t doc_index : docs) {
                    const std::vector<RecordHit> hits = records_of(documents.at(doc_index));
                    for (const RecordHit& hit : hits) {
                        if (!selected(documents, doc_index, hit, rule, layout))
                            continue;
                        matched = true;
                        const tdf::Block* section =
                            section_at(documents, doc_index, hit.section_index);
                        for (const sm::Change& change : rule.changes) {
                            if (change.all_depth2) {
                                const uint32_t children = tdf::child_count(section);
                                for (uint32_t child_index = 0; child_index < children;
                                     ++child_index) {
                                    const tdf::Block* inner = tdf::child_at(section, child_index);
                                    if (inner == nullptr)
                                        continue;
                                    const std::string block_spell =
                                        inner->name != nullptr ? inner->name : "";
                                    const uint32_t key_total = tdf::property_count(inner);
                                    std::vector<std::string_view> taken_keys;
                                    taken_keys.reserve(key_total);
                                    for (uint32_t key_index = 0; key_index < key_total;
                                         ++key_index) {
                                        const char* key_text = tdf::property_key_at(
                                            inner, static_cast<int32_t>(key_index)
                                        );
                                        if (key_text == nullptr)
                                            continue;
                                        bool repeated = false;
                                        for (const std::string_view earlier : taken_keys) {
                                            if (same_text(earlier, key_text)) {
                                                repeated = true;
                                                break;
                                            }
                                        }
                                        if (repeated)
                                            continue;
                                        taken_keys.emplace_back(key_text);
                                        sm::Change one_change = change;
                                        one_change.all_depth2 = false;
                                        one_change.field = key_text;
                                        fold_field(
                                            mutator_index,
                                            source.name,
                                            doc_index,
                                            hit,
                                            true,
                                            child_index,
                                            block_spell,
                                            inner,
                                            one_change
                                        );
                                    }
                                }
                                continue;
                            }
                            fold_field(
                                mutator_index,
                                source.name,
                                doc_index,
                                hit,
                                false,
                                0,
                                {},
                                section,
                                change
                            );
                        }
                    }
                }
                if (!matched) {
                    rules::RuleNote missed;
                    missed.kind = rules::NoteKind::matched_nothing;
                    missed.mutators.push_back(one.mutator);
                    missed.source = source.name;
                    missed.line = rule.line;
                    missed.message = "the block matched no record";
                    result.notes.push_back(std::move(missed));
                }
            }
        }
    }

    std::vector<Slot*> ordered;
    ordered.reserve(slots.size());
    for (auto& entry : slots)
        ordered.push_back(&entry.second);
    std::stable_sort(ordered.begin(), ordered.end(), slot_before);

    std::vector<Planned> waiting;
    for (const Slot* slot : ordered)
        combine_slot(*slot, current_of(documents, *slot), maps, waiting, result.notes);

    rules::ApplyOptions options;
    options.layout = &layout;
    options.check = [&metal](const rules::FieldRef& field, const std::optional<std::string>& text) {
        return check_map_field(field, text, metal);
    };
    for (Planned& write : waiting)
        commit_write(documents, write, options, result);
    if (metal.seen)
        copy_surface_metal(documents, result, metal);

    std::stable_sort(result.writes.begin(), result.writes.end(), write_before);
    std::stable_sort(result.notes.begin(), result.notes.end(), note_before);
    return result;
}

} // namespace oa::data::mutators
