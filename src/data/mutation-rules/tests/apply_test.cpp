// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A factor reads a field the way the TDF reader reads a number, including a
// value that starts with a point. A lone point, with or without a sign, stays
// as text.

#include "oa/data/mutation_rules/apply.hpp"
#include "oa/formats/soft_mutator.hpp"
#include "oa/formats/tdf.hpp"
#include "oa/test/check.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace rules = oa::data::mutation_rules;
namespace tdf = oa::formats::tdf;
namespace sm = oa::formats::soft_mutator;

rules::MutatorRules rules_of(std::string_view text) {
    rules::MutatorRules parsed;
    parsed.mutator = "ridge-point";
    rules::RuleSource source;
    source.name = "rules/legacy.sm";
    const sm::ParseResult result = sm::parse_rules(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size())
    );
    OA_CHECK(!result.has_errors());
    source.rules = result.rules;
    parsed.sources.push_back(std::move(source));
    return parsed;
}

const char* weapon_value(rules::DocumentSet& docs, std::string_view key) {
    rules::LoadedDocument* loaded = docs.find("weapons/ridge.tdf");
    if (loaded == nullptr)
        return nullptr;
    const tdf::Block* section = tdf::find_child(loaded->document.root(), "RIDGE_LASER");
    if (section == nullptr)
        return nullptr;
    const std::string key_text(key);
    return tdf::find_value(section, key_text.c_str());
}

int notes_of(const rules::ApplyResult& result, rules::NoteKind kind) {
    int found = 0;
    for (const rules::RuleNote& note : result.notes) {
        if (note.kind == kind)
            ++found;
    }
    return found;
}

void expect_reload(std::string_view before, std::string_view factor, std::string_view after) {
    rules::DocumentSet docs;
    const std::string text = "[RIDGE_LASER]\n{\nreloadtime=" + std::string(before) + ";\n}\n";
    std::string error;
    OA_CHECK(docs.add("weapons/ridge.tdf", text, &error));
    OA_CHECK(error.empty());
    const std::string rules_text = "folder=weapons;\nextension=tdf;\n[*All*]\n{\nreloadtime=*" +
                                   std::string(factor) + ";\n}\n";
    rules::ApplyState state;
    const rules::MutatorRules parsed = rules_of(rules_text);
    const rules::ApplyResult result = rules::apply_step(parsed, docs, state, {});
    const char* got = weapon_value(docs, "reloadtime");
    OA_CHECK(got != nullptr && std::string_view(got) == after);
    if (before == after) {
        OA_CHECK(result.writes.empty());
        OA_CHECK(notes_of(result, rules::NoteKind::not_a_number) == 1);
    } else {
        OA_CHECK(result.writes.size() == 1);
        OA_CHECK(notes_of(result, rules::NoteKind::not_a_number) == 0);
    }
}

void leading_point_is_a_number() {
    expect_reload(".8", "2", "1.6");
    expect_reload("-.5", "0.5", "-0.25");
    expect_reload("+.25", "2", "0.5");
    expect_reload(".", "2", ".");
    expect_reload("-.", "0.5", "-.");
    expect_reload("+.", "2", "+.");
}

} // namespace

int main() {
    leading_point_is_a_number();
    return 0;
}
