// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-mutator-layer: a match with no mutators, one that changes the
// commanders, the same match again, and one mutator that does not apply.
// The command line names the two packages.

#include "oa/app/runtime.hpp"

#include "oa/data/mutators/choice.hpp"
#include "oa/ui/frontend_dialogs.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("mutator layer check: " + what);
}

/// Tells whether a line names ridge-commander-boom and then its own error.
///
/// The error is the script's `boom`, after the id, so a failure that stops
/// before the write does not pass.
///
/// @param text the status or the thrown message
/// @return true when both are there, in that order
bool names_commander_boom(const std::string& text) {
    const std::string id = "ridge-commander-boom";
    const std::size_t at = text.find(id);
    return at != std::string::npos && text.find("boom", at + id.size()) != std::string::npos;
}

} // namespace

void Runtime::check_mutator_layer() {
    // The span is into the chosen list. Copy it before that list is replaced.
    const auto chosen_now = chosen_match_mutators();
    const std::vector<oa::data::mutators::ResolvedMutator> supplied(
        chosen_now.begin(), chosen_now.end()
    );
    std::optional<oa::data::mutators::ResolvedMutator> tough;
    std::optional<oa::data::mutators::ResolvedMutator> boom;
    for (const oa::data::mutators::ResolvedMutator& one : supplied) {
        if (one.manifest.id == "ridge-tough")
            tough = one;
        else if (one.manifest.id == "ridge-commander-boom")
            boom = one;
    }
    if (!tough.has_value() || !boom.has_value())
        fail("the command line must name ridge-tough and ridge-commander-boom");

    // The two commanders' MaxDamage, lower one first.
    const auto commander_health = [this] {
        int32_t health[2] = {};
        int count = 0;
        if (!match_)
            fail("no match is running");
        const std::size_t slots = match_->world().slots.size();
        for (std::size_t slot = 1; slot < slots && slot <= 0xFFFFU; ++slot) {
            const auto* definition = definition_for(static_cast<uint16_t>(slot));
            if (definition == nullptr || !definition->commander)
                continue;
            if (count >= 2)
                fail("more than two commanders");
            health[count] = definition->max_damage;
            ++count;
        }
        if (count != 2)
            fail("the match placed " + std::to_string(count) + " commanders");
        if (health[0] > health[1])
            std::swap(health[0], health[1]);
        return std::pair<int32_t, int32_t>{health[0], health[1]};
    };

    const auto back_to_menu = [this] {
        if (match_)
            leave_match();
        load(Screen::main_menu);
    };

    choose_match_mutators({});
    start_benchmark_skirmish_as_chosen();
    const std::pair<int32_t, int32_t> stock = commander_health();
    if (stock.first == 4000 || stock.second == 4000)
        fail("the commanders already hold 4000 before a mutator");
    back_to_menu();
    if (assets_.mutator_layer_mounted())
        fail("a match with no mutators left a layer mounted");

    choose_match_mutators({*tough});
    start_benchmark_skirmish_as_chosen();
    const std::pair<int32_t, int32_t> toughed = commander_health();
    if (toughed.first != 4000 || toughed.second != 4000)
        fail(
            "ridge-tough left the commanders at " + std::to_string(toughed.first) + " and " +
            std::to_string(toughed.second)
        );
    if (!assets_.mutator_layer_mounted())
        fail("ridge-tough left no mutator layer mounted");
    back_to_menu();
    if (assets_.mutator_layer_mounted())
        fail("the layer stayed mounted after the match");

    choose_match_mutators({});
    start_benchmark_skirmish_as_chosen();
    const std::pair<int32_t, int32_t> restored = commander_health();
    if (restored != stock)
        fail(
            "the commanders did not return to " + std::to_string(stock.first) + " and " +
            std::to_string(stock.second)
        );
    back_to_menu();

    choose_match_mutators({*boom});
    std::string thrown;
    bool entered = false;
    try {
        start_benchmark_skirmish_as_chosen();
        entered = true;
    } catch (const std::exception& error) {
        thrown = error.what();
    }
    if (entered || (!names_commander_boom(status_) && !names_commander_boom(thrown)))
        fail("ridge-commander-boom did not stop the start (" + thrown + "; " + status_ + ")");
    if (assets_.mutator_layer_mounted())
        fail("ridge-commander-boom left a mutator layer mounted");
    while (oa::ui::frontend_dialogs::dialog_count() != 0)
        oa::ui::frontend_dialogs::close_dialog();
    std::cout << "mutator layer check: passed\n";
}

} // namespace oa::app
