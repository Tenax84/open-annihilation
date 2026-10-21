// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutator picker's model: opening a set, prerequisites and their order,
// conflicts, options, rows, the footer, the hint and the turn-off question.
// The mutators are made up.
#include "oa/test/check.hpp"
#include "oa/ui/library/prerequisites.hpp"
#include "oa/ui/mutator_picker/model.hpp"
#include "oa/ui/mutator_picker/text.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace picker = oa::ui::mutator_picker;
namespace lib = oa::ui::library;

/// A made-up mutator that is installed and can be switched on.
///
/// @param id the id
/// @param name the name a player sees
/// @return the mutator
picker::Mutator made(std::string id, std::string name) {
    picker::Mutator mutator;
    mutator.id = std::move(id);
    mutator.name = std::move(name);
    mutator.installed = true;
    return mutator;
}

/// Records that `mutator` needs `id`.
///
/// @param[in,out] mutator the mutator
/// @param id the prerequisite
void need(picker::Mutator& mutator, std::string id) {
    lib::Prerequisite prerequisite;
    prerequisite.id = std::move(id);
    mutator.needs.push_back(std::move(prerequisite));
}

/// The prerequisite graph of the picker's inputs.
///
/// @param model the picker
/// @return the graph
std::vector<lib::Need> graph_of(const picker::Picker& model) {
    std::vector<lib::Need> graph;
    for (const picker::Mutator& mutator : model.inputs.mutators) {
        lib::Need record;
        record.id = mutator.id;
        for (const lib::Prerequisite& prerequisite : mutator.needs)
            record.needs.push_back(prerequisite.id);
        graph.push_back(std::move(record));
    }
    return graph;
}

/// The On ids, top to bottom.
///
/// @param model the picker
/// @return the ids
std::vector<std::string> ids_of(const picker::Picker& model) {
    std::vector<std::string> ids;
    for (const picker::Chosen& chosen : model.on)
        ids.push_back(chosen.id);
    return ids;
}

/// Checks the On ids.
///
/// @param model the picker
/// @param expected the ids, top to bottom
void check_ids(const picker::Picker& model, const std::vector<std::string>& expected) {
    const std::vector<std::string> ids = ids_of(model);
    OA_CHECK(ids.size() == expected.size());
    if (ids.size() != expected.size())
        return;
    for (std::size_t index = 0; index < ids.size(); ++index)
        OA_CHECK(ids[index] == expected[index]);
}

/// The row with `id`, or null.
///
/// @param listed the rows
/// @param id the mutator
/// @return the row
const picker::Row* row_of(const std::vector<picker::Row>& listed, std::string_view id) {
    for (const picker::Row& row : listed)
        if (row.id == id)
            return &row;
    return nullptr;
}

/// A choice option.
///
/// @param id the option id
/// @param label the label
/// @param values the values
/// @param captions the captions, in the same order
/// @param fallback the default
/// @return the option
picker::OptionSpec choice_option(
    std::string id,
    std::string label,
    std::vector<std::string> values,
    std::vector<std::string> captions,
    std::string fallback
) {
    picker::OptionSpec spec;
    spec.id = std::move(id);
    spec.type = picker::OptionSpec::Type::choice;
    spec.label = std::move(label);
    spec.values = std::move(values);
    spec.captions = std::move(captions);
    spec.fallback = std::move(fallback);
    return spec;
}

/// A toggle. The off value is first.
///
/// @param id the option id
/// @param label the label
/// @param off the off value
/// @param on the on value
/// @return the option
picker::OptionSpec
toggle_option(std::string id, std::string label, std::string off, std::string on) {
    picker::OptionSpec spec;
    spec.id = std::move(id);
    spec.type = picker::OptionSpec::Type::toggle;
    spec.label = std::move(label);
    spec.values = {std::move(off), std::move(on)};
    spec.fallback = spec.values.front();
    return spec;
}

/// A number option.
///
/// @param id the option id
/// @param label the label
/// @param least the low end
/// @param most the high end
/// @param step the step
/// @param fallback the default
/// @return the option
picker::OptionSpec number_option(
    std::string id,
    std::string label,
    std::string least,
    std::string most,
    std::string step,
    std::string fallback
) {
    picker::OptionSpec spec;
    spec.id = std::move(id);
    spec.type = picker::OptionSpec::Type::number;
    spec.label = std::move(label);
    spec.least = std::move(least);
    spec.most = std::move(most);
    spec.step = std::move(step);
    spec.fallback = std::move(fallback);
    return spec;
}

picker::Chosen chosen_id(std::string id) {
    picker::Chosen chosen;
    chosen.id = std::move(id);
    return chosen;
}

/// An empty remembered set switches nothing on and shows no last-set hint.
void open_empty() {
    picker::Inputs inputs;
    inputs.remembered = true;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(made("fast-builders", "Fast Builders"));
    inputs.mutators.push_back(made("ridge-walls", "Ridge Walls"));
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(model.on.empty());
    OA_CHECK(!model.from_last_set);
    OA_CHECK(picker::hint(model).empty());
    OA_CHECK(picker::ready(model));
    const picker::Footer foot = picker::footer(model);
    OA_CHECK(foot.summary == "None on");
    OA_CHECK(foot.chip.empty());
    OA_CHECK(foot.chip_mark == lib::Mark::none);
    OA_CHECK(model.selected && *model.selected == "fast-builders");
}

/// A full set keeps its order and its allowed option values.
void open_full_set() {
    picker::Mutator builders = made("fast-builders", "Fast Builders");
    builders.options.push_back(
        choice_option("build-speed", "Build speed", {"1", "4", "8"}, {"1×", "4×", "8×"}, "1")
    );
    picker::Inputs inputs;
    inputs.remembered = true;
    inputs.mod_name = "No Mod";
    inputs.setup = picker::Setup::skirmish;
    inputs.mutators.push_back(builders);
    picker::Chosen chosen = chosen_id("fast-builders");
    chosen.options.emplace_back("build-speed", "4");
    inputs.last_set.push_back(std::move(chosen));
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(model.from_last_set);
    check_ids(model, {"fast-builders"});
    OA_CHECK(model.on[0].options.size() == 1);
    OA_CHECK(model.on[0].options[0].second == "4");
    OA_CHECK(
        picker::hint(model) ==
        "Your last skirmish set, ready to use. CANCEL plays without mutators."
    );
    OA_CHECK(model.selected && *model.selected == "fast-builders");
}

/// A removed mutator is named, a removed option is dropped, and a value the
/// mutator no longer allows takes the default.
void open_drops_what_went() {
    picker::Mutator builders = made("fast-builders", "Fast Builders");
    builders.options.push_back(
        choice_option("build-speed", "Build speed", {"1", "4"}, {"1×", "4×"}, "1")
    );
    builders.options.push_back(number_option("income", "Income", "0", "10", "0.5", "1"));
    picker::Inputs inputs;
    inputs.remembered = true;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(builders);
    picker::Chosen gone = chosen_id("not-in-the-list");
    picker::Chosen chosen = chosen_id("fast-builders");
    chosen.options.emplace_back("removed-option", "yes");
    chosen.options.emplace_back("build-speed", "nope");
    chosen.options.emplace_back("income", "99");
    inputs.last_set.push_back(std::move(gone));
    inputs.last_set.push_back(std::move(chosen));
    picker::Picker model;
    picker::open(model, std::move(inputs));
    check_ids(model, {"fast-builders"});
    OA_CHECK(model.left_out.size() == 1);
    OA_CHECK(model.left_out[0] == "not-in-the-list");
    OA_CHECK(model.on[0].options.size() == 2);
    OA_CHECK(model.on[0].options[0].first == "build-speed");
    OA_CHECK(model.on[0].options[0].second == "1");
    OA_CHECK(model.on[0].options[1].first == "income");
    OA_CHECK(model.on[0].options[1].second == "1");
    OA_CHECK(picker::hint(model).find("not-in-the-list") != std::string::npos);
}

/// A set stored with a dependant above its prerequisite is mended, and the
/// prerequisite is not recorded as brought: the set already named it.
void open_mends_order() {
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(keep);
    inputs.last_set.push_back(chosen_id("gatehouse-keep"));
    inputs.last_set.push_back(chosen_id("gatehouse"));
    picker::Picker model;
    picker::open(model, std::move(inputs));
    check_ids(model, {"gatehouse", "gatehouse-keep"});
    OA_CHECK(model.came_with.empty());
    const std::vector<lib::Need> graph = graph_of(model);
    const std::vector<std::string> ids = ids_of(model);
    OA_CHECK(oa::ui::library::prerequisites_above(graph, ids));
}

/// A prerequisite the set does not name is brought above what needs it.
void open_brings_missing() {
    picker::Mutator grounded = made("grounded", "Grounded");
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    need(gatehouse, "grounded");
    picker::Mutator walls = made("ridge-walls", "Ridge Walls");
    need(walls, "grounded");
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    need(keep, "ridge-walls");
    picker::Inputs inputs;
    inputs.remembered = true;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(grounded);
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(walls);
    inputs.mutators.push_back(keep);
    inputs.last_set.push_back(chosen_id("gatehouse-keep"));
    picker::Picker model;
    picker::open(model, std::move(inputs));
    check_ids(model, {"grounded", "gatehouse", "ridge-walls", "gatehouse-keep"});
    OA_CHECK(model.came_with.size() == 3);
    OA_CHECK(model.came_with[0].prerequisite == "grounded");
    OA_CHECK(model.came_with[0].dependant == "gatehouse-keep");
    OA_CHECK(model.came_with[1].prerequisite == "gatehouse");
    OA_CHECK(model.came_with[2].prerequisite == "ridge-walls");
    const std::vector<lib::Need> graph = graph_of(model);
    const std::vector<std::string> ids = ids_of(model);
    OA_CHECK(oa::ui::library::prerequisites_above(graph, ids));
    const picker::Footer foot = picker::footer(model);
    OA_CHECK(foot.summary == "4 on · Grounded came with Gatehouse Keep");
}

/// Past the cap, the end of the list is left out.
void open_trims_the_cap() {
    picker::Inputs inputs;
    inputs.remembered = true;
    inputs.mod_name = "No Mod";
    for (int index = 0; index < 33; ++index) {
        const std::string id = std::string("m") + (index < 10 ? "0" : "") + std::to_string(index);
        inputs.mutators.push_back(made(id, id));
        inputs.last_set.push_back(chosen_id(id));
    }
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(model.on.size() == picker::most_on);
    OA_CHECK(model.on.front().id == "m00");
    OA_CHECK(model.on.back().id == "m31");
    OA_CHECK(model.left_out.size() == 1);
    OA_CHECK(model.left_out[0] == "m32");
}

/// Switching Gatehouse Keep on brings Gatehouse above it.
void switch_on_brings() {
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(keep);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    const picker::SwitchResult outcome = picker::switch_on(model, "gatehouse-keep");
    OA_CHECK(outcome.changed);
    OA_CHECK(outcome.refused.empty());
    OA_CHECK(outcome.brought.size() == 1);
    OA_CHECK(outcome.brought[0] == "gatehouse");
    check_ids(model, {"gatehouse", "gatehouse-keep"});
    OA_CHECK(model.came_with.size() == 1);
    OA_CHECK(model.came_with[0].prerequisite == "gatehouse");
    OA_CHECK(model.came_with[0].dependant == "gatehouse-keep");
    const std::vector<lib::Need> graph = graph_of(model);
    const std::vector<std::string> ids = ids_of(model);
    OA_CHECK(oa::ui::library::prerequisites_above(graph, ids));
}

/// Grounded is refused while Quiet Seas, which conflicts with it, is on.
void switch_on_conflict() {
    picker::Mutator grounded = made("grounded", "Grounded");
    picker::Mutator seas = made("quiet-seas", "Quiet Seas");
    seas.conflicts.push_back("grounded");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(grounded);
    inputs.mutators.push_back(seas);
    picker::Picker model;
    picker::open(model, inputs);
    OA_CHECK(picker::switch_on(model, "quiet-seas").changed);
    const std::vector<std::string> before = ids_of(model);
    const picker::SwitchResult outcome = picker::switch_on(model, "grounded");
    OA_CHECK(!outcome.changed);
    OA_CHECK(outcome.refused.find("Quiet Seas") != std::string::npos);
    check_ids(model, before);
    OA_CHECK(picker::ready(model));
    picker::open(model, inputs);
    inputs.last_set.push_back(chosen_id("quiet-seas"));
    inputs.last_set.push_back(chosen_id("grounded"));
    picker::open(model, inputs);
    check_ids(model, {"quiet-seas", "grounded"});
    OA_CHECK(!picker::ready(model));
    const std::vector<picker::Row> listed = picker::rows(model);
    const picker::Row* seas_row = row_of(listed, "quiet-seas");
    const picker::Row* grounded_row = row_of(listed, "grounded");
    OA_CHECK(seas_row != nullptr && seas_row->locked && seas_row->tone == picker::Tone::bad);
    OA_CHECK(seas_row->line == "Cannot run beside Grounded");
    OA_CHECK(grounded_row != nullptr && grounded_row->locked);
    OA_CHECK(grounded_row->line == "Cannot run beside Quiet Seas");
}

/// An unavailable mutator is refused with its reason.
void switch_on_unavailable() {
    picker::Mutator walls = made("ridge-walls", "Ridge Walls");
    walls.unavailable = "needs Ridge's weapons";
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(walls);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    const picker::SwitchResult outcome = picker::switch_on(model, "ridge-walls");
    OA_CHECK(!outcome.changed);
    OA_CHECK(outcome.refused.find("needs Ridge's weapons") != std::string::npos);
    OA_CHECK(model.on.empty());
}

/// Two mutators that need each other are refused, and not placed.
void switch_on_cycle() {
    picker::Mutator left = made("loop-a", "Loop A");
    picker::Mutator right = made("loop-b", "Loop B");
    need(left, "loop-b");
    need(right, "loop-a");
    picker::Inputs inputs;
    inputs.remembered = true;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(left);
    inputs.mutators.push_back(right);
    picker::Picker model;
    picker::open(model, inputs);
    const picker::SwitchResult outcome = picker::switch_on(model, "loop-a");
    OA_CHECK(!outcome.changed);
    OA_CHECK(model.on.empty());
    OA_CHECK(outcome.refused.find("loop-a") != std::string::npos);
    OA_CHECK(outcome.refused.find("loop-b") != std::string::npos);
    inputs.last_set.push_back(chosen_id("loop-a"));
    picker::open(model, std::move(inputs));
    OA_CHECK(model.on.empty());
    OA_CHECK(model.left_out.size() == 2);
}

/// A read-only picker still opens on the set, and then refuses every change.
void read_only_refuses() {
    picker::Mutator builders = made("fast-builders", "Fast Builders");
    builders.options.push_back(
        choice_option("build-speed", "Build speed", {"1", "4"}, {"1×", "4×"}, "1")
    );
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    picker::Inputs inputs;
    inputs.read_only = true;
    inputs.remembered = true;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(builders);
    inputs.mutators.push_back(gatehouse);
    inputs.last_set.push_back(chosen_id("fast-builders"));
    picker::Picker model;
    picker::open(model, inputs);
    check_ids(model, {"fast-builders"});
    OA_CHECK(picker::hint(model) == "The host's mutators. Only the host can change them.");
    const picker::SwitchResult switched = picker::switch_on(model, "gatehouse");
    OA_CHECK(!switched.changed);
    OA_CHECK(!switched.refused.empty());
    OA_CHECK(!picker::switch_off(model, "fast-builders"));
    OA_CHECK(!picker::move_to(model, "fast-builders", 0));
    OA_CHECK(!picker::drop(model, "gatehouse", picker::Drop{true, 0}));
    OA_CHECK(!picker::set_option(model, "fast-builders", "build-speed", "4"));
    OA_CHECK(model.on[0].options[0].second == "1");
    check_ids(model, {"fast-builders"});

    inputs.read_only = false;
    picker::open(model, inputs);
    OA_CHECK(picker::switch_off(model, "fast-builders") == std::nullopt);
    OA_CHECK(model.on.empty());
    picker::open(model, inputs);
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    model.inputs.mutators.push_back(keep);
    OA_CHECK(picker::switch_on(model, "gatehouse").changed);
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").changed);
    OA_CHECK(picker::switch_off(model, "gatehouse"));
    model.inputs.read_only = true;
    picker::confirm_turn_off(model);
    OA_CHECK(model.asking);
    OA_CHECK(model.on.size() == 3);
    picker::cancel_turn_off(model);
    OA_CHECK(!model.asking);
    OA_CHECK(model.on.size() == 3);
}

/// Switching a prerequisite off asks, and nothing changes until the answer.
void turn_off_asks() {
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(keep);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").changed);
    const std::optional<picker::TurnOff> asked = picker::switch_off(model, "gatehouse");
    OA_CHECK(asked);
    OA_CHECK(model.asking);
    OA_CHECK(asked->id == "gatehouse");
    OA_CHECK(asked->dependants.size() == 1);
    OA_CHECK(asked->dependants[0] == "gatehouse-keep");
    check_ids(model, {"gatehouse", "gatehouse-keep"});
    picker::cancel_turn_off(model);
    OA_CHECK(!model.asking);
    check_ids(model, {"gatehouse", "gatehouse-keep"});
    OA_CHECK(!model.came_with.empty());
    OA_CHECK(picker::switch_off(model, "gatehouse"));
    picker::confirm_turn_off(model);
    OA_CHECK(!model.asking);
    OA_CHECK(model.on.empty());
    OA_CHECK(model.came_with.empty());
}

/// A chain of three can be moved only inside each mutator's band.
void chain_moves() {
    picker::Mutator grounded = made("grounded", "Grounded");
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    need(gatehouse, "grounded");
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(grounded);
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(keep);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").changed);
    check_ids(model, {"grounded", "gatehouse", "gatehouse-keep"});
    OA_CHECK(!picker::move_up(model, "grounded"));
    OA_CHECK(!picker::move_down(model, "grounded"));
    OA_CHECK(!picker::move_up(model, "gatehouse"));
    OA_CHECK(!picker::move_down(model, "gatehouse"));
    OA_CHECK(!picker::move_up(model, "gatehouse-keep"));
    OA_CHECK(!picker::move_down(model, "gatehouse-keep"));
    const picker::Band band = picker::allowed_places(model, "gatehouse");
    OA_CHECK(band.first == 1);
    OA_CHECK(band.last == 1);
    OA_CHECK(picker::allowed_places(model, "fast-missing").first == 0);
    for (std::size_t place = 0; place <= model.on.size(); ++place) {
        std::ignore = picker::move_to(model, "gatehouse", place);
        std::ignore = picker::drop(model, "gatehouse", picker::Drop{true, place});
        const std::vector<lib::Need> graph = graph_of(model);
        const std::vector<std::string> ids = ids_of(model);
        OA_CHECK(oa::ui::library::prerequisites_above(graph, ids));
    }
    check_ids(model, {"grounded", "gatehouse", "gatehouse-keep"});
    OA_CHECK(picker::drop(model, "gatehouse", picker::Drop{false, 0}));
    OA_CHECK(model.asking);
    picker::cancel_turn_off(model);
    OA_CHECK(!picker::drop(model, "gatehouse-keep", picker::Drop{false, 0}));
    OA_CHECK(!model.asking);
    check_ids(model, {"grounded", "gatehouse"});
    OA_CHECK(!picker::drop(model, "gatehouse-keep", picker::Drop{true, 0}));
    check_ids(model, {"grounded", "gatehouse", "gatehouse-keep"});
    const std::vector<lib::Need> graph = graph_of(model);
    const std::vector<std::string> ids = ids_of(model);
    OA_CHECK(oa::ui::library::prerequisites_above(graph, ids));
}

/// A diamond lets the two middle mutators swap, and nothing else.
void diamond_moves() {
    picker::Mutator grounded = made("grounded", "Grounded");
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    need(gatehouse, "grounded");
    picker::Mutator walls = made("ridge-walls", "Ridge Walls");
    need(walls, "grounded");
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    need(keep, "ridge-walls");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(grounded);
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(walls);
    inputs.mutators.push_back(keep);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").changed);
    check_ids(model, {"grounded", "gatehouse", "ridge-walls", "gatehouse-keep"});
    OA_CHECK(picker::move_down(model, "gatehouse"));
    check_ids(model, {"grounded", "ridge-walls", "gatehouse", "gatehouse-keep"});
    OA_CHECK(!picker::move_down(model, "gatehouse"));
    OA_CHECK(picker::move_up(model, "gatehouse"));
    check_ids(model, {"grounded", "gatehouse", "ridge-walls", "gatehouse-keep"});
    OA_CHECK(!picker::move_up(model, "grounded"));
    OA_CHECK(!picker::move_down(model, "gatehouse-keep"));
    for (std::size_t place = 0; place <= model.on.size(); ++place) {
        std::ignore = picker::move_to(model, "ridge-walls", place);
        std::ignore = picker::drop(model, "gatehouse", picker::Drop{true, place});
        const std::vector<lib::Need> graph = graph_of(model);
        const std::vector<std::string> ids = ids_of(model);
        OA_CHECK(oa::ui::library::prerequisites_above(graph, ids));
    }
    const std::vector<lib::Need> graph = graph_of(model);
    const std::vector<std::string> ids = ids_of(model);
    OA_CHECK(oa::ui::library::prerequisites_above(graph, ids));
}

/// Mutators with no prerequisites move up and down until the ends.
void free_moves() {
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(made("fast-builders", "Fast Builders"));
    inputs.mutators.push_back(made("gatehouse", "Gatehouse"));
    inputs.mutators.push_back(made("ridge-walls", "Ridge Walls"));
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::switch_on(model, "fast-builders").changed);
    OA_CHECK(picker::switch_on(model, "gatehouse").changed);
    OA_CHECK(picker::switch_on(model, "ridge-walls").changed);
    OA_CHECK(!picker::move_up(model, "fast-builders"));
    OA_CHECK(!picker::move_down(model, "ridge-walls"));
    OA_CHECK(picker::move_up(model, "gatehouse"));
    check_ids(model, {"gatehouse", "fast-builders", "ridge-walls"});
    OA_CHECK(picker::move_down(model, "fast-builders"));
    check_ids(model, {"gatehouse", "ridge-walls", "fast-builders"});
}

/// Each option type accepts a legal value and refuses the rest.
/// Switching off and on again restores the defaults.
void options_accept_and_refuse() {
    picker::Mutator builders = made("fast-builders", "Fast Builders");
    builders.options.push_back(toggle_option("aircraft", "Aircraft", "off", "on"));
    builders.options.push_back(
        choice_option("build-speed", "Build speed", {"1", "4", "8"}, {"1×", "4×", "8×"}, "1")
    );
    builders.options.push_back(number_option("income", "Income", "0", "10", "0.5", "1"));
    builders.brings = true;
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.clash_default = picker::Clash::replace;
    inputs.mutators.push_back(std::move(builders));
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::switch_on(model, "fast-builders").changed);
    OA_CHECK(model.on[0].options[0].second == "off");
    OA_CHECK(model.on[0].options[1].second == "1");
    OA_CHECK(model.on[0].options[2].second == "1");
    OA_CHECK(model.on[0].clash && *model.on[0].clash == picker::Clash::replace);
    OA_CHECK(picker::set_option(model, "fast-builders", "aircraft", "on"));
    OA_CHECK(picker::set_option(model, "fast-builders", "aircraft", "on"));
    OA_CHECK(picker::set_option(model, "fast-builders", "build-speed", "4"));
    OA_CHECK(picker::set_option(model, "fast-builders", "income", "4"));
    OA_CHECK(picker::set_option(model, "fast-builders", "income", "0"));
    OA_CHECK(picker::set_option(model, "fast-builders", "income", "0.5"));
    OA_CHECK(picker::set_option(model, "fast-builders", "income", "10"));
    OA_CHECK(picker::set_option(model, "fast-builders", "income", "+4"));
    OA_CHECK(picker::set_option(model, "fast-builders", "income", "00.5"));
    OA_CHECK(picker::set_option(model, "fast-builders", "income", "1e1"));
    OA_CHECK(!picker::set_option(model, "fast-builders", "income", "0.3"));
    OA_CHECK(!picker::set_option(model, "fast-builders", "income", "10.1"));
    OA_CHECK(!picker::set_option(model, "fast-builders", "income", "-1"));
    OA_CHECK(!picker::set_option(model, "fast-builders", "income", "4×"));
    OA_CHECK(!picker::set_option(model, "fast-builders", "build-speed", "2"));
    OA_CHECK(!picker::set_option(model, "fast-builders", "aircraft", "maybe"));
    OA_CHECK(!picker::set_option(model, "fast-builders", "missing", "1"));
    OA_CHECK(model.on[0].options[1].second == "4");
    OA_CHECK(model.on[0].options[2].second == "1e1");
    OA_CHECK(picker::set_clash(model, "fast-builders", picker::Clash::add));
    OA_CHECK(model.on[0].clash && *model.on[0].clash == picker::Clash::add);
    OA_CHECK(!picker::switch_off(model, "fast-builders"));
    OA_CHECK(picker::switch_on(model, "fast-builders").changed);
    OA_CHECK(model.on[0].options[0].second == "off");
    OA_CHECK(model.on[0].options[1].second == "1");
    OA_CHECK(model.on[0].options[2].second == "1");
    OA_CHECK(model.on[0].clash && *model.on[0].clash == picker::Clash::replace);

    picker::Mutator fine = made("ridge-walls", "Ridge Walls");
    fine.options.push_back(number_option("gap", "Gap", "0.1", "1", "0.2", "0.1"));
    model.inputs.mutators.push_back(std::move(fine));
    OA_CHECK(picker::switch_on(model, "ridge-walls").changed);
    OA_CHECK(picker::set_option(model, "ridge-walls", "gap", "0.5"));
    OA_CHECK(!picker::set_option(model, "ridge-walls", "gap", "0.4"));
    OA_CHECK(!picker::set_option(model, "not-on", "gap", "0.5"));
    OA_CHECK(!picker::set_clash(model, "ridge-walls", picker::Clash::add));
}

/// The thirty-third mutator is refused, prerequisites included.
void switch_on_cap() {
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    for (int index = 0; index < 33; ++index) {
        const std::string id = std::string("m") + (index < 10 ? "0" : "") + std::to_string(index);
        inputs.mutators.push_back(made(id, id));
    }
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "m00");
    inputs.mutators.push_back(keep);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    for (int index = 0; index < 32; ++index) {
        const std::string id = std::string("m") + (index < 10 ? "0" : "") + std::to_string(index);
        OA_CHECK(picker::switch_on(model, id).changed);
    }
    const picker::SwitchResult extra = picker::switch_on(model, "m32");
    OA_CHECK(!extra.changed);
    OA_CHECK(extra.refused.find("32") != std::string::npos);
    OA_CHECK(model.on.size() == 32);
}

/// Every row line in the order the model chooses them.
void row_lines() {
    picker::Mutator builders = made("fast-builders", "Fast Builders");
    builders.summary = "Builds faster";
    builders.options.push_back(
        choice_option("build-speed", "Build speed", {"1", "4"}, {"1×", "4×"}, "1")
    );
    builders.tags = {"economy"};
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    gatehouse.summary = "A gate";
    gatehouse.tags = {"gameplay", "custom-tag"};
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    keep.summary = "The keep";
    picker::Mutator walls = made("ridge-walls", "Ridge Walls");
    walls.installed = false;
    walls.unavailable = "needs Ridge's weapons";
    walls.summary = "Walls";
    picker::Mutator queued = made("queued-walls", "Queued Walls");
    queued.installed = false;
    queued.summary = "Waiting walls";
    lib::QueueItem item;
    item.phase = lib::QueueItem::Phase::waiting;
    item.name = "Queued Walls";
    queued.queued = item;
    picker::Mutator plain = made("quiet-field", "Quiet Field");
    plain.installed = false;
    plain.summary = "A field";
    plain.author = "Ridge";
    picker::Mutator bad = made("bad-fit", "Bad Fit");
    bad.summary = "Does not fit";
    bad.verdict.state = lib::Verdict::State::incompatible;
    bad.verdict.reason = "the mod has no spot for it";
    picker::Mutator failed = made("stopped-fit", "Stopped Fit");
    failed.summary = "The check stopped";
    failed.verdict.state = lib::Verdict::State::failed;
    failed.verdict.reason = "the check stopped";
    picker::Mutator no_change = made("idle-fit", "Idle Fit");
    no_change.summary = "Changes nothing";
    no_change.verdict.state = lib::Verdict::State::no_effect;
    picker::Mutator seas = made("quiet-seas", "Quiet Seas");
    seas.summary = "Seas";
    seas.conflicts.push_back("grounded");
    seas.verdict.state = lib::Verdict::State::compatible;

    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(builders);
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(keep);
    inputs.mutators.push_back(walls);
    inputs.mutators.push_back(queued);
    inputs.mutators.push_back(plain);
    inputs.mutators.push_back(bad);
    inputs.mutators.push_back(failed);
    inputs.mutators.push_back(no_change);
    inputs.mutators.push_back(seas);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::switch_on(model, "fast-builders").changed);
    OA_CHECK(picker::set_option(model, "fast-builders", "build-speed", "4"));
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").changed);
    OA_CHECK(picker::switch_on(model, "bad-fit").changed);
    OA_CHECK(picker::switch_on(model, "stopped-fit").changed);
    OA_CHECK(picker::switch_on(model, "idle-fit").changed);
    OA_CHECK(picker::switch_on(model, "quiet-seas").changed);

    const std::vector<picker::Row> listed = picker::rows(model);
    OA_CHECK(listed.size() >= 6);
    const picker::Row* builders_row = row_of(listed, "fast-builders");
    OA_CHECK(builders_row != nullptr && builders_row->on && builders_row->number == 1);
    OA_CHECK(builders_row->line == "Build speed: 4×");
    OA_CHECK(builders_row->tone == picker::Tone::hint);
    OA_CHECK(!builders_row->locked && !builders_row->to_download);

    const picker::Row* gatehouse_row = row_of(listed, "gatehouse");
    OA_CHECK(gatehouse_row != nullptr && gatehouse_row->on);
    OA_CHECK(gatehouse_row->line == "Needed by Gatehouse Keep");
    OA_CHECK(gatehouse_row->tone == picker::Tone::warn);
    OA_CHECK(gatehouse_row->number == 2);

    const picker::Row* bad_row = row_of(listed, "bad-fit");
    OA_CHECK(bad_row != nullptr && bad_row->tone == picker::Tone::bad && !bad_row->locked);
    OA_CHECK(bad_row->line == "Not compatible with this mod: the mod has no spot for it");
    const picker::Row* failed_row = row_of(listed, "stopped-fit");
    OA_CHECK(failed_row != nullptr && failed_row->tone == picker::Tone::bad && !failed_row->locked);
    OA_CHECK(failed_row->line == "Stopped with this mod: the check stopped");
    const picker::Row* idle_row = row_of(listed, "idle-fit");
    OA_CHECK(idle_row != nullptr && idle_row->tone == picker::Tone::warn);
    OA_CHECK(idle_row->line == "No effect with this mod");

    const picker::Row* walls_row = row_of(listed, "ridge-walls");
    OA_CHECK(walls_row != nullptr && !walls_row->on && walls_row->number == 0);
    OA_CHECK(walls_row->locked && walls_row->tone == picker::Tone::bad);
    OA_CHECK(walls_row->line == "Not compatible with this mod: needs Ridge's weapons");
    OA_CHECK(!walls_row->to_download);

    const picker::Row* queued_row = row_of(listed, "queued-walls");
    OA_CHECK(queued_row != nullptr && queued_row->tone == picker::Tone::hint);
    OA_CHECK(queued_row->line == "Waiting to download");
    OA_CHECK(!queued_row->to_download);
    const picker::Row* field_row = row_of(listed, "quiet-field");
    OA_CHECK(
        field_row != nullptr && field_row->to_download && field_row->tone == picker::Tone::hint
    );
    OA_CHECK(field_row->line == "Not installed: switching it on downloads it");

    std::size_t previous = 0;
    bool seen_off = false;
    for (const picker::Row& row : listed) {
        if (row.on) {
            OA_CHECK(!seen_off);
            OA_CHECK(row.number == previous + 1);
            previous = row.number;
        } else {
            seen_off = true;
            OA_CHECK(row.number == 0);
        }
    }

    model.filter = picker::Filter::installed;
    const std::vector<picker::Row> installed = picker::rows(model);
    OA_CHECK(row_of(installed, "fast-builders") != nullptr);
    OA_CHECK(row_of(installed, "quiet-seas") != nullptr);
    OA_CHECK(row_of(installed, "quiet-field") == nullptr);
    model.filter = picker::Filter::downloads;
    model.query = "ridge";
    model.tag = "economy";
    const std::vector<picker::Row> hidden = picker::rows(model);
    OA_CHECK(row_of(hidden, "fast-builders") != nullptr);
    OA_CHECK(row_of(hidden, "gatehouse") != nullptr);
    OA_CHECK(row_of(hidden, "quiet-field") == nullptr);

    model.filter = picker::Filter::all;
    model.tag.clear();
    model.query = "ridge field";
    OA_CHECK(row_of(picker::rows(model), "quiet-field") != nullptr);
    model.query = "no-such-word";
    OA_CHECK(row_of(picker::rows(model), "quiet-field") == nullptr);
    model.query.clear();

    const std::size_t all = picker::filter_count(model, picker::Filter::all);
    const std::size_t installed_count = picker::filter_count(model, picker::Filter::installed);
    const std::size_t downloads = picker::filter_count(model, picker::Filter::downloads);
    model.tag = "economy";
    model.query = "ridge";
    OA_CHECK(picker::filter_count(model, picker::Filter::all) == all);
    OA_CHECK(picker::filter_count(model, picker::Filter::installed) == installed_count);
    OA_CHECK(picker::filter_count(model, picker::Filter::downloads) == downloads);
    OA_CHECK(all == model.inputs.mutators.size());
    OA_CHECK(downloads == 3);
    model.tag.clear();
    model.query.clear();
    model.filter = picker::Filter::all;
    const std::vector<std::string> tags = picker::tags_shown(model);
    OA_CHECK(tags.size() == 3);
    OA_CHECK(tags[0] == "economy");
    OA_CHECK(tags[1] == "gameplay");
    OA_CHECK(tags[2] == "custom-tag");
}

/// The footer's chip follows the On verdicts, and the summary names the first brought.
void footer_cases() {
    picker::Mutator first = made("fast-builders", "Fast Builders");
    picker::Mutator second = made("gatehouse", "Gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(first);
    inputs.mutators.push_back(second);
    picker::Picker model;
    picker::open(model, inputs);
    OA_CHECK(picker::switch_on(model, "fast-builders").changed);
    OA_CHECK(picker::switch_on(model, "gatehouse").changed);
    picker::Footer foot = picker::footer(model);
    OA_CHECK(foot.chip.empty());
    OA_CHECK(foot.summary == "2 on");

    model.inputs.mutators[0].verdict.state = lib::Verdict::State::checking;
    model.inputs.mutators[1].verdict.state = lib::Verdict::State::incompatible;
    foot = picker::footer(model);
    OA_CHECK(foot.chip == "Checking with No Mod…");
    OA_CHECK(foot.chip_mark == lib::Mark::none);

    model.inputs.mutators[0].verdict.state = lib::Verdict::State::unchecked;
    model.inputs.mutators[1].verdict.state = lib::Verdict::State::compatible;
    foot = picker::footer(model);
    OA_CHECK(foot.chip.empty());

    model.inputs.mutators[0].verdict.state = lib::Verdict::State::incompatible;
    model.inputs.mutators[0].verdict.reason = "no";
    model.inputs.mutators[1].verdict.state = lib::Verdict::State::failed;
    foot = picker::footer(model);
    OA_CHECK(foot.chip == "Not compatible with No Mod: Fast Builders");
    OA_CHECK(foot.chip_mark == lib::Mark::bad);

    model.inputs.mutators[0].verdict.state = lib::Verdict::State::failed;
    model.inputs.mutators[1].verdict.state = lib::Verdict::State::incompatible;
    foot = picker::footer(model);
    OA_CHECK(foot.chip == "Fast Builders stopped with No Mod");
    OA_CHECK(foot.chip_mark == lib::Mark::bad);

    model.inputs.mutators[0].verdict.state = lib::Verdict::State::no_effect;
    model.inputs.mutators[1].verdict.state = lib::Verdict::State::compatible;
    foot = picker::footer(model);
    OA_CHECK(foot.chip == "No effect with No Mod: Fast Builders");
    OA_CHECK(foot.chip_mark == lib::Mark::warn);

    model.inputs.mutators[0].verdict.state = lib::Verdict::State::compatible;
    model.inputs.mutators[1].verdict.state = lib::Verdict::State::compatible;
    foot = picker::footer(model);
    OA_CHECK(foot.chip == "Checked with No Mod ✓");
    OA_CHECK(foot.chip_mark == lib::Mark::good);
}

/// The hint for each setup, the campaign's two sentences, and a guest's view.
void hint_cases() {
    picker::Inputs inputs;
    inputs.remembered = true;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(made("fast-builders", "Fast Builders"));
    inputs.last_set.push_back(chosen_id("fast-builders"));
    inputs.last_set.push_back(chosen_id("missing-one"));
    picker::Picker model;

    inputs.setup = picker::Setup::skirmish;
    picker::open(model, inputs);
    OA_CHECK(picker::hint(model).find("last skirmish set") != std::string::npos);
    OA_CHECK(picker::hint(model).find("missing-one") != std::string::npos);

    inputs.setup = picker::Setup::multiplayer;
    picker::open(model, inputs);
    OA_CHECK(picker::hint(model).find("last multiplayer set") != std::string::npos);

    inputs.setup = picker::Setup::campaign;
    picker::open(model, inputs);
    const std::string campaign = picker::hint(model);
    OA_CHECK(campaign.find("last campaign set") != std::string::npos);
    OA_CHECK(campaign.find("every mission of this campaign") != std::string::npos);
    const std::size_t first = campaign.find("last campaign set");
    const std::size_t second = campaign.find("every mission");
    OA_CHECK(first != std::string::npos && second != std::string::npos && first < second);

    inputs.remembered = false;
    inputs.last_set.clear();
    inputs.setup = picker::Setup::campaign;
    picker::open(model, inputs);
    OA_CHECK(
        picker::hint(model) ==
        "The set holds for every mission of this campaign, and is saved with it."
    );

    inputs.read_only = true;
    inputs.remembered = true;
    inputs.last_set.push_back(chosen_id("fast-builders"));
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::hint(model) == "The host's mutators. Only the host can change them.");
}

/// The short texts a setup's line is written from.
void short_texts() {
    picker::Mutator builders = made("fast-builders", "Fast Builders");
    builders.options.push_back(
        choice_option("build-speed", "Build speed", {"1", "4"}, {"1×", "4×"}, "1")
    );
    builders.options.push_back(toggle_option("aircraft", "Aircraft", "off", "on"));
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(builders);
    inputs.mutators.push_back(gatehouse);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::switch_on(model, "fast-builders").changed);
    OA_CHECK(picker::set_option(model, "fast-builders", "build-speed", "4"));
    OA_CHECK(picker::set_option(model, "fast-builders", "aircraft", "on"));
    OA_CHECK(picker::switch_on(model, "gatehouse").changed);
    OA_CHECK(picker::item_text(model, model.on[0]) == "Fast Builders 4×");
    const std::vector<picker::Chosen> chosen = picker::result(model);
    OA_CHECK(picker::list_text(model, chosen) == "Fast Builders 4× · Gatehouse");
    OA_CHECK(picker::ready(model));
}

/// The question names the dependants, and its buttons are Cancel and Turn off.
void question_words() {
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    picker::Mutator walls = made("ridge-walls", "Ridge Walls");
    need(walls, "gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(keep);
    inputs.mutators.push_back(walls);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    const oa::ui::kit::Question idle = picker::turn_off_question(model);
    OA_CHECK(idle.title.empty());
    OA_CHECK(idle.buttons.empty());
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").changed);
    OA_CHECK(picker::switch_off(model, "gatehouse"));
    const oa::ui::kit::Question one = picker::turn_off_question(model);
    OA_CHECK(one.title == "Turn off Gatehouse?");
    OA_CHECK(one.paragraphs.size() == 2);
    OA_CHECK(one.paragraphs[0].text == "Gatehouse Keep turns off too: it needs Gatehouse.");
    OA_CHECK(one.paragraphs[1].text.find("Gatehouse Keep") != std::string::npos);
    OA_CHECK(one.paragraphs[1].text.find("comes back with it") != std::string::npos);
    OA_CHECK(one.buttons.size() == 2);
    OA_CHECK(one.buttons[0].caption == "CANCEL");
    OA_CHECK(!one.buttons[0].accent);
    OA_CHECK(one.buttons[0].id == "cancel");
    OA_CHECK(one.buttons[1].caption == "TURN OFF");
    OA_CHECK(one.buttons[1].accent);
    OA_CHECK(one.buttons[1].id == "turn-off");
    OA_CHECK(one.marked == 0);
    OA_CHECK(one.cancel_button == 0);
    OA_CHECK(one.primary_button == 1);
    picker::cancel_turn_off(model);

    OA_CHECK(picker::switch_on(model, "ridge-walls").changed);
    OA_CHECK(picker::switch_off(model, "gatehouse"));
    const oa::ui::kit::Question many = picker::turn_off_question(model);
    OA_CHECK(many.paragraphs[0].text.find("turn off too") != std::string::npos);
    OA_CHECK(many.paragraphs[0].text.find("Gatehouse Keep") != std::string::npos);
    OA_CHECK(many.paragraphs[0].text.find("Ridge Walls") != std::string::npos);
    OA_CHECK(many.paragraphs[1].text.find("They all stay installed") != std::string::npos);
}

/// A lookup replaces the English pattern before its places are filled.
void translated_row() {
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(keep);
    inputs.text.shown = [](void*, std::string_view english) -> std::string_view {
        if (english == picker::words::line_needed_by)
            return "Requis par {name}";
        return english;
    };
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").changed);
    const std::vector<picker::Row> shown_rows = picker::rows(model);
    const picker::Row* row = row_of(shown_rows, "gatehouse");
    OA_CHECK(row != nullptr);
    OA_CHECK(row != nullptr && row->line == "Requis par Gatehouse Keep");
}

/// Refresh keeps the order, the options and the filters, and drops a mutator
/// the new inputs do not hold.
void refresh_keeps() {
    picker::Mutator builders = made("fast-builders", "Fast Builders");
    builders.options.push_back(
        choice_option("build-speed", "Build speed", {"1", "4"}, {"1×", "4×"}, "1")
    );
    builders.verdict.state = lib::Verdict::State::unchecked;
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(builders);
    inputs.mutators.push_back(gatehouse);
    picker::Picker model;
    picker::open(model, inputs);
    OA_CHECK(picker::switch_on(model, "gatehouse").changed);
    OA_CHECK(picker::switch_on(model, "fast-builders").changed);
    OA_CHECK(picker::set_option(model, "fast-builders", "build-speed", "4"));
    model.filter = picker::Filter::installed;
    model.query = "fast";
    model.selected = "fast-builders";
    inputs.mutators[0].verdict.state = lib::Verdict::State::compatible;
    inputs.mutators[1].verdict.state = lib::Verdict::State::compatible;
    picker::refresh(model, inputs);
    check_ids(model, {"gatehouse", "fast-builders"});
    OA_CHECK(model.on[1].options[0].second == "4");
    OA_CHECK(model.filter == picker::Filter::installed);
    OA_CHECK(model.query == "fast");
    OA_CHECK(model.selected && *model.selected == "fast-builders");
    OA_CHECK(picker::footer(model).chip == "Checked with No Mod ✓");

    inputs.mutators.pop_back();
    picker::refresh(model, std::move(inputs));
    check_ids(model, {"fast-builders"});
    OA_CHECK(model.left_out.size() == 1);
    OA_CHECK(model.left_out[0] == "gatehouse");
}

/// Not being installed does not refuse a switch, and an empty On list is ready.
void not_installed_switches_on() {
    picker::Mutator builders = made("fast-builders", "Fast Builders");
    builders.installed = false;
    picker::Inputs inputs;
    inputs.mod_name = "Ridge";
    inputs.mutators.push_back(std::move(builders));
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::ready(model));
    const picker::SwitchResult outcome = picker::switch_on(model, "fast-builders");
    OA_CHECK(outcome.changed);
    OA_CHECK(outcome.refused.empty());
    OA_CHECK(!picker::ready(model));
}

} // namespace

int main() {
    open_empty();
    open_full_set();
    open_drops_what_went();
    open_mends_order();
    open_brings_missing();
    open_trims_the_cap();
    switch_on_brings();
    switch_on_conflict();
    switch_on_unavailable();
    switch_on_cycle();
    read_only_refuses();
    turn_off_asks();
    chain_moves();
    diamond_moves();
    free_moves();
    options_accept_and_refuse();
    switch_on_cap();
    row_lines();
    footer_cases();
    hint_cases();
    short_texts();
    question_words();
    translated_row();
    refresh_keeps();
    not_installed_switches_on();
    return oa::test::check_exit_status();
}
