// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Every word the mutator picker shows, in English. A pattern's places are
// written {name}; a translation keeps them and may move them. The texts are
// looked up whole through the Library's TextHooks before their places are
// filled, so a translator finds every one of them here.
#pragma once

#include <string_view>

namespace oa::ui::mutator_picker {

namespace words {

/// The amber line on a mutator an On mutator needs.
inline constexpr std::string_view line_needed_by = "Needed by {name}";
/// An Off mutator that is not installed. Switching it on downloads it.
inline constexpr std::string_view line_not_installed =
    "Not installed: switching it on downloads it";
/// Why a mutator cannot be switched on here, or an incompatible verdict.
inline constexpr std::string_view line_not_compatible = "Not compatible with this mod: {reason}";
/// A verdict whose check did not finish.
inline constexpr std::string_view line_failed = "Stopped with this mod: {reason}";
/// A verdict that applies and changes nothing.
inline constexpr std::string_view line_no_effect = "No effect with this mod";
/// A mutator that cannot be on beside one that already is. `{name}` is the other.
inline constexpr std::string_view line_conflicts = "Cannot run beside {name}";
/// Switching on would pass most_on, counting the prerequisites it brings.
inline constexpr std::string_view refused_cap = "At most {count} mutators can be on";
/// A mutator whose prerequisites need each other. `{names}` are the ids in the loop.
inline constexpr std::string_view line_cycle = "These mutators need each other: {names}";
/// One option on a row, the Library's Options fact form.
inline constexpr std::string_view option_line = "{label}: {value}";
/// The clash choice on a row that brings units or weapons.
inline constexpr std::string_view clash_label = "Units and weapons";
/// The clash choice's Add value.
inline constexpr std::string_view clash_add = "Add";
/// The clash choice's Replace value.
inline constexpr std::string_view clash_replace = "Replace";
/// The footer's count of mutators that are on.
inline constexpr std::string_view summary_on = "{count} on";
/// The footer's summary when none are on.
inline constexpr std::string_view summary_none = "None on";
/// The first prerequisite still on that was brought with a dependant.
inline constexpr std::string_view summary_came_with = "{name} came with {dependant}";
/// The footer's chip when every On mutator was checked and changes the mod.
inline constexpr std::string_view verdict_checked = "Checked with {mod} ✓";
/// The footer's chip while any On mutator is being checked.
inline constexpr std::string_view verdict_checking = "Checking with {mod}…";
/// The footer's chip for the first On mutator that changes nothing.
inline constexpr std::string_view verdict_no_effect = "No effect with {mod}: {name}";
/// The footer's chip for the first On mutator that cannot run with the mod.
inline constexpr std::string_view verdict_not_compatible = "Not compatible with {mod}: {name}";
/// The footer's chip for the first On mutator whose check did not finish.
inline constexpr std::string_view verdict_failed = "{name} stopped with {mod}";
/// The hint when the picker opened on the remembered skirmish set.
inline constexpr std::string_view hint_last_skirmish =
    "Your last skirmish set, ready to use. CANCEL plays without mutators.";
/// The hint when the picker opened on the remembered multiplayer set.
inline constexpr std::string_view hint_last_multiplayer =
    "Your last multiplayer set, ready to use. CANCEL plays without mutators.";
/// The hint when the picker opened on the remembered campaign set.
inline constexpr std::string_view hint_last_campaign =
    "Your last campaign set, ready to use. CANCEL plays without mutators.";
/// The campaign's second sentence, after the last-set hint when that shows.
inline constexpr std::string_view hint_campaign =
    "The set holds for every mission of this campaign, and is saved with it.";
/// Mutators the last set named that were not switched on.
inline constexpr std::string_view hint_left_out = "Left out of your last set: {names}";
/// The hint while a guest looks at the host's set.
inline constexpr std::string_view hint_view = "The host's mutators. Only the host can change them.";
/// The question asked before a prerequisite is switched off.
inline constexpr std::string_view turn_off_title = "Turn off {name}?";
/// The question's first paragraph when one On mutator needs it.
inline constexpr std::string_view turn_off_one = "{names} turns off too: it needs {name}.";
/// The question's first paragraph when more than one On mutator needs it.
inline constexpr std::string_view turn_off_many = "{names} turn off too: they need {name}.";
/// The question's second paragraph for one dependant.
inline constexpr std::string_view turn_off_kept_one =
    "Both stay installed. Turn {dependant} on again and {name} comes back with it.";
/// The question's second paragraph for more than one dependant.
inline constexpr std::string_view turn_off_kept_many =
    "They all stay installed. Turn one on again and {name} comes back with it.";
/// The question's button that leaves the set as it is.
inline constexpr std::string_view cancel = "CANCEL";
/// The question's button that switches the prerequisite and its dependants off.
inline constexpr std::string_view turn_off = "TURN OFF";

} // namespace words

} // namespace oa::ui::mutator_picker
