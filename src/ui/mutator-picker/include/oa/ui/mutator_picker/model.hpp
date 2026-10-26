// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutator picker's model: which mutators are on, in which order, with
// which options and clash mode, what a row and the footer say, and the
// question asked before a prerequisite is switched off. It draws nothing
// and reads no file, clock, preference or network. The host fills the inputs.
// Every word comes from text.hpp through the Library's TextHooks. The order
// is the Library's prerequisite closure: a prerequisite stays above every
// mutator that needs it.
#pragma once

#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/library/library.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::mutator_picker {

/// Which setup opened the picker. Each one remembers its own last set.
enum class Setup : uint8_t {
    skirmish,    ///< a skirmish
    multiplayer, ///< a battle room
    campaign,    ///< a campaign, one set for every mission
};

/// How a mutator that brings units or weapons treats a name that already exists.
///
/// Add keeps the existing record and adds this mutator's alongside it.
/// Replace uses this mutator's record. The manifest's `clashes` chooses
/// when it names one; otherwise the host passes the setting
/// `mutators.clash-default` as `Inputs::clash_default`. The model does not
/// read that setting itself.
enum class Clash : uint8_t {
    add,     ///< add alongside a record that already exists
    replace, ///< replace a record that already exists
};

/// One option a mutator offers, as the host read it.
///
/// A choice lists `values` and their `captions` in the same order. A toggle
/// lists its two values, off first, and may list a caption for each. A
/// number's range and step are numbers as the manifest writes them, read
/// with the manifest's number grammar. `fallback` is the default, in the
/// same form as a value.
struct OptionSpec {
    /// What kind of value the option holds.
    enum class Type : uint8_t {
        toggle, ///< one of two values, off first
        choice, ///< one of `values`
        number, ///< a decimal in range, on `step`
    };

    std::string id{};                    ///< the option's id
    Type type{Type::toggle};             ///< toggle, choice or number
    std::string label{};                 ///< the words the row shows
    std::vector<std::string> values{};   ///< a choice's values, or a toggle's two
    std::vector<std::string> captions{}; ///< the caption of each value, in order
    std::string least{};                 ///< a number's minimum, as written
    std::string most{};                  ///< a number's maximum, as written
    std::string step{};                  ///< a number's step, as written
    std::string fallback{};              ///< the default value, as written
};

/// One mutator the picker can show. The host fills every field.
struct Mutator {
    std::string id{};   ///< the mutator's id
    std::string name{}; ///< the name a player sees; the id when empty
    /// The version as written. A dependant's version range is checked against it.
    std::string version{};
    std::string author{};            ///< who wrote it; empty when unnamed
    std::string summary{};           ///< the short line under the name
    std::string description{};       ///< the longer text; the search reads it
    std::vector<std::string> tags{}; ///< catalogue tag ids, in the order written
    /// The mutators it needs, in the order they apply.
    std::vector<library::Prerequisite> needs{};
    /// Mutator ids it cannot run beside.
    std::vector<std::string> conflicts{};
    /// The layers it uses, in the order written.
    std::vector<std::string> layers{};
    /// The settings a player picks, in the order written.
    std::vector<OptionSpec> options{};
    bool installed{}; ///< false when switching it on downloads it
    /// The catalogue entry to get it from. Empty when no registry offers it.
    library::EntryId listing{};
    /// The badge picture. The host keeps the pixels until the next refresh.
    const library::Badge* badge{};
    /// The verdict for the options it is shown with. Unchecked shows no line.
    library::Verdict verdict{};
    /// Why it cannot be switched on here: the engine, or works-with. Empty
    /// when it can.
    std::string unavailable{};
    /// The mods it works with, already in words. The host writes it.
    std::string works_with_text{};
    /// Its download, when one is queued. Empty when none is.
    std::optional<library::QueueItem> queued{};
    /// True when it brings units or weapons, so the row offers a clash mode.
    bool brings{};
    /// The manifest's clash mode. Empty means `Inputs::clash_default`.
    std::optional<Clash> clashes{};
};

/// One mutator that is on: its id, every option's value, and its clash mode
/// when it brings units or weapons.
struct Chosen {
    std::string id{}; ///< the mutator's id
    /// Each option's id and its value, in the mutator's option order. A
    /// number is kept in its canonical text, as the manifest writes numbers.
    std::vector<std::pair<std::string, std::string>> options{};
    /// The clash mode in force. Empty when the mutator brings no units or weapons.
    std::optional<Clash> clash{};
};

/// What the host gives the picker.
struct Inputs {
    Setup setup{Setup::skirmish};    ///< which setup opened it
    std::string mod_name{};          ///< the mod being played, as a player reads it
    std::vector<Mutator> mutators{}; ///< every mutator the picker can show
    /// The set the picker opens with: the setup's chosen set when one is
    /// chosen, otherwise its remembered last set.
    std::vector<Chosen> last_set{};
    /// True when `last_set` is the remembered last set, not a set already chosen.
    bool remembered{};
    /// True when a guest is looking: only the host can change the set.
    bool read_only{};
    /// How texts are looked up. Empty shows the English words.
    library::TextHooks text{};
    /// The clash mode a mutator uses when its manifest names none.
    Clash clash_default{Clash::replace};
};

/// Which Off mutators the lower list keeps. On mutators show under every filter.
enum class Filter : uint8_t {
    all,       ///< every mutator
    installed, ///< those installed
    downloads, ///< those not installed
};

/// The most mutators that can be on at once, prerequisites included.
inline constexpr std::size_t most_on = 32;

/// A prerequisite the picker switched on because a dependant needed it.
struct CameWith {
    std::string prerequisite{}; ///< the mutator that was brought
    std::string dependant{};    ///< the mutator the player switched on
};

/// The question waiting for an answer: a prerequisite, and the On mutators
/// that would go off with it.
struct TurnOff {
    std::string id{};                      ///< the prerequisite
    std::vector<std::string> dependants{}; ///< the On mutators that need it, top to bottom
};

/// The picker's state. `open` fills it. The host may set `filter`, `tag` and
/// `query`; the other fields change through the functions below.
struct Picker {
    Inputs inputs{};                   ///< the host's inputs
    std::vector<Chosen> on{};          ///< the On mutators, top to bottom
    std::vector<CameWith> came_with{}; ///< prerequisites brought for a dependant
    /// Mutators the picker left out or switched off itself: by name, or by
    /// id when the inputs no longer hold them.
    std::vector<std::string> left_out{};
    Filter filter{Filter::all};            ///< which Off mutators show
    std::string tag{};                     ///< the tag filter; empty shows every tag
    std::string query{};                   ///< the search; empty matches everything
    std::optional<std::string> selected{}; ///< the selected mutator's id
    /// True when the picker opened on a remembered set and switched something on.
    bool from_last_set{};
    /// Set while the player is being asked before a prerequisite goes off.
    std::optional<TurnOff> asking{};
};

/// What switching a mutator on did.
struct SwitchResult {
    bool changed{};                     ///< true when the On list changed
    std::vector<std::string> brought{}; ///< prerequisites switched on with it, in closure order
    std::string refused{};              ///< why it was not switched on; empty when it was
};

/// The places a mutator may occupy in the On list, inclusive.
struct Band {
    std::size_t first{}; ///< the highest place: below its lowest On prerequisite
    std::size_t last{};  ///< the lowest place: above its highest On dependant
};

/// Where a dragged row was dropped.
struct Drop {
    bool into_on{};      ///< true when the drop is in the On list
    std::size_t place{}; ///< the place in the On list; ignored in the Off list
};

/// How a row's line is drawn.
enum class Tone : uint8_t {
    hint, ///< an ordinary line
    warn, ///< amber: needed by another, or no effect
    bad,  ///< a mutator that cannot be on
};

/// One row, in the order the list draws them.
struct Row {
    std::string id{};      ///< the mutator's id
    bool on{};             ///< true in the On list
    std::size_t number{};  ///< the On number from 1; 0 in the Off list
    std::string line{};    ///< the line under the name
    Tone tone{Tone::hint}; ///< how `line` is drawn
    bool locked{};         ///< true when it cannot be switched on
    bool to_download{};    ///< true when it is not installed and nothing is queued for it
};

/// The footer's chip and the sentence beside it.
struct Footer {
    std::string chip{};                           ///< the chip; empty when none is shown
    library::Mark chip_mark{library::Mark::none}; ///< how the chip is marked
    std::string summary{};                        ///< the count, and the first prerequisite brought
};

/// The question's CANCEL, the button the keys mark and Escape answers.
inline constexpr std::string_view cancel_button_id = "cancel";
/// The question's TURN OFF, the primary button.
inline constexpr std::string_view turn_off_button_id = "turn-off";

/// Opens the picker on `inputs`.
///
/// Switches the last set on in its order, with its options. An option the
/// mutator no longer has is dropped. A value it no longer allows, and an
/// option the set does not name, take the default. A mutator that is gone
/// is named in `left_out` by its id. The order is then mended so every
/// prerequisite sits above its dependants, moving each one above the first
/// dependant that precedes it. The list is then read from the top: a
/// mutator that cannot be on beside those kept above it (the rules `rows`
/// locks a row by) is left out and named in `left_out`, so of two mutators
/// that conflict the later one is left out, and a mutator that needs one
/// left out is left out too. One that can be on is kept after the
/// prerequisites it lacks, which are brought in with their defaults and
/// recorded in `came_with`. At most `most_on` stay on; the rest are named
/// in `left_out`. `from_last_set` is set when the set was remembered and
/// anything is on. The selection is the first On mutator, or the first
/// mutator by name when none are on.
///
/// @param[in,out] picker the picker
/// @param inputs the host's inputs
void open(Picker& picker, Inputs inputs);

/// Replaces the inputs and keeps the On list, its order, its options, the
/// filters, the search and the selection, as far as the new inputs allow.
///
/// A mutator the new inputs do not hold is dropped from the On list and
/// named in `left_out` by its id. An option that disappeared is dropped,
/// and a value that is no longer allowed takes the default. The list is
/// then mended and read from the top as `open` reads it: a mutator whose
/// prerequisite went, or no longer meets its range, whose needs became a
/// loop, that became unavailable or that now conflicts with one above it is
/// switched off with its On dependants, each named in `left_out`, and its
/// row says why. A prerequisite a mutator now needs is brought in above it
/// when it can be. Afterwards every prerequisite stands above what needs
/// it. A question about a mutator that went off is dropped. A selection
/// the inputs no longer hold becomes the first On mutator, or the first
/// mutator by name.
///
/// @param[in,out] picker the picker
/// @param inputs the host's new inputs
void refresh(Picker& picker, Inputs inputs);

/// Switches `id` on, with its default options, and brings the prerequisites
/// it lacks to just above what needs them.
///
/// Refuses, and changes nothing, when the picker is read-only, when its row
/// is locked (`rows` gives the reason, and the refusal is the row's line),
/// and when the On list would pass `most_on`. A row is locked when the
/// mutator or one of its prerequisites is unavailable, when its
/// prerequisites need each other, when a prerequisite is not among the
/// inputs or its version is outside the range asked for, and when it or a
/// prerequisite it would bring conflicts with an On mutator or with each
/// other. Conflicts between two mutators already on do not refuse it. Not
/// being installed is no refusal. `place` is where it sits in the On list,
/// or the end when none is given, clamped below its On prerequisites and
/// above its On dependants. A mutator that is already on is left where it is.
///
/// @param[in,out] picker the picker
/// @param id the mutator's id
/// @param place where it sits; the end when none
/// @return whether the On list changed, the prerequisites brought, and why it was refused
[[nodiscard]] SwitchResult
switch_on(Picker& picker, std::string_view id, std::optional<std::size_t> place = {});

/// Switches `id` off, or asks first when an On mutator needs it.
///
/// When On mutators need it, sets `asking` and changes nothing. Otherwise
/// switches it off and drops the `came_with` records that name it. A
/// read-only picker and a mutator that is not on change nothing.
///
/// @param[in,out] picker the picker
/// @param id the mutator's id
/// @return the question, when one was asked
[[nodiscard]] std::optional<TurnOff> switch_off(Picker& picker, std::string_view id);

/// Switches off the mutator `asking` names, and every On mutator that needs
/// it now.
///
/// The dependants are worked out again when this is called, so a dependant
/// switched on after the question was asked goes off too. Clears `asking`.
/// A read-only picker and a picker that is not asking change nothing.
///
/// @param[in,out] picker the picker
void confirm_turn_off(Picker& picker);

/// Clears `asking` and leaves the On list as it is.
///
/// @param[in,out] picker the picker
void cancel_turn_off(Picker& picker);

/// Returns the places `id` may occupy in the On list.
///
/// The first place is below its lowest On prerequisite. The last is above
/// its highest On dependant. A mutator that is not on reports a band of one
/// place at the top.
///
/// @param picker the picker
/// @param id the mutator's id
/// @return the inclusive band
[[nodiscard]] Band allowed_places(const Picker& picker, std::string_view id);

/// Moves `id` to `place` in the On list, clamped to `allowed_places`.
///
/// @param[in,out] picker the picker
/// @param id the mutator's id
/// @param place the place it should occupy
/// @return false when nothing moves: read-only, not on, or already there
[[nodiscard]] bool move_to(Picker& picker, std::string_view id, std::size_t place);

/// Moves `id` one place up, within its band.
///
/// @param[in,out] picker the picker
/// @param id the mutator's id
/// @return false when it is already at the top of its band, or it cannot move
[[nodiscard]] bool move_up(Picker& picker, std::string_view id);

/// Moves `id` one place down, within its band.
///
/// @param[in,out] picker the picker
/// @param id the mutator's id
/// @return false when it is already at the bottom of its band, or it cannot move
[[nodiscard]] bool move_down(Picker& picker, std::string_view id);

/// Drops `id` at `where`.
///
/// An On row dropped in the On list moves, clamped to its band. An Off row
/// dropped in the On list switches on at that place. An On row dropped in
/// the Off list switches off, and may ask. A refused switch changes nothing.
/// A read-only picker changes nothing.
///
/// @param[in,out] picker the picker
/// @param id the mutator's id
/// @param where the list and the place
/// @return the question, when switching off asked one
[[nodiscard]] std::optional<TurnOff> drop(Picker& picker, std::string_view id, Drop where);

/// Sets one option of an On mutator.
///
/// A choice's value must be one of its values, compared as an identifier. A
/// toggle's must be one of its two. A number is read with the manifest's
/// number grammar and must be one of `number_values`; it is kept in its
/// canonical text, so `1e1` is kept as `10`. Anything else is refused, and
/// so is a mutator that is not on, an option it does not have, and a
/// read-only picker. Switching off and on again starts from the defaults;
/// this does not.
///
/// @param[in,out] picker the picker
/// @param id the mutator's id
/// @param option the option's id
/// @param value the value, as written
/// @return true when the value is kept
[[nodiscard]] bool
set_option(Picker& picker, std::string_view id, std::string_view option, std::string_view value);

/// Returns the values a number option offers, lowest first, in canonical text.
///
/// The minimum, then each step up to the maximum: at most
/// `formats::oamut::most_number_steps` + 1 values. A value the number
/// grammar cannot write exactly (more significant digits than it holds) is
/// not offered. Empty for a toggle or a choice, and for a number whose
/// minimum, maximum or step cannot be read, whose step is not above zero,
/// whose minimum is above its maximum, or whose range holds more steps than
/// a manifest may.
///
/// @param spec the option
/// @return the values, such as 0, 0.5, 1 … 10
[[nodiscard]] std::vector<std::string> number_values(const OptionSpec& spec);

/// Sets the clash mode of an On mutator that brings units or weapons.
///
/// A mutator that brings none, a mutator that is not on, and a read-only
/// picker are refused.
///
/// @param[in,out] picker the picker
/// @param id the mutator's id
/// @param mode add or replace
/// @return true when the mode is kept
[[nodiscard]] bool set_clash(Picker& picker, std::string_view id, Clash mode);

/// Returns the rows: every On mutator in order, numbered from 1, then the
/// Off mutators the filter, the tag and the search keep, by name.
///
/// A row's line is the first that holds: locked (why it cannot be on beside
/// the On list, as `switch_on` refuses it), an incompatible or failed
/// verdict, needed by an On mutator, a no-effect verdict, a download, not
/// installed (an Off row), the options (and the clash choice when it brings
/// units or weapons), or the summary. `to_download` is set when the mutator
/// is not installed and nothing is queued, whether the row is On or Off,
/// unless a line above the not-installed line already returned.
///
/// @param picker the picker
/// @return the rows, On first
[[nodiscard]] std::vector<Row> rows(const Picker& picker);

/// Counts the mutators a filter keeps, before the tag and the search.
///
/// All counts every mutator, installed those that are, and downloads those
/// that are not. On and Off count the same.
///
/// @param picker the picker
/// @param filter the filter
/// @return how many mutators it keeps
[[nodiscard]] std::size_t filter_count(const Picker& picker, Filter filter);

/// Counts the mutators the current filter keeps that carry a tag, before the
/// tag and the search.
///
/// On and Off count the same. It counts inputs, not the rows the tag and
/// the search then leave.
///
/// @param picker the picker
/// @param tag the catalogue id
/// @return how many
[[nodiscard]] std::size_t tag_count(const Picker& picker, std::string_view tag);

/// The option line an Off mutator shows, from each option's default.
///
/// The same shape as an On row's option line. When the mutator brings units
/// or weapons, the clash mode is the one `open` would keep before a player
/// chooses: the manifest's, or the inputs' default.
///
/// @param picker the picker
/// @param id the mutator's id
/// @return the line; empty when the inputs do not hold it, or it has nothing to show
[[nodiscard]] std::string default_options_text(const Picker& picker, std::string_view id);

/// Lists the tags of the mutators the filter keeps, before the tag and the search.
///
/// The eight catalogue ids come first, in the kit's order. Any other id
/// follows, in byte order. A tag no such mutator has is left out.
///
/// @param picker the picker
/// @return the tag ids
[[nodiscard]] std::vector<std::string> tags_shown(const Picker& picker);

/// Returns the footer's chip and summary.
///
/// No chip when none are on. Otherwise the chip is the check in progress
/// while any On verdict is checking, else the first incompatible or failed
/// On verdict, else no chip while any On verdict is unchecked, else the
/// first no-effect verdict, else the checked chip. The summary is the count
/// and the first brought prerequisite that is still on with its dependant,
/// or none when nothing is on.
///
/// @param picker the picker
/// @return the chip and the summary
[[nodiscard]] Footer footer(const Picker& picker);

/// Returns the hint under the filters.
///
/// Read-only shows that only the host can change the set. Otherwise a
/// remembered set that switched something on shows that setup's last-set
/// hint, the campaign adds its sentence after that hint, and mutators left
/// out are named. Empty when none of those hold.
///
/// @param picker the picker
/// @return the hint; empty when there is nothing to say
[[nodiscard]] std::string hint(const Picker& picker);

/// Returns the On list in order, each with every option's value.
///
/// @param picker the picker
/// @return the set a game would start with
[[nodiscard]] std::vector<Chosen> result(const Picker& picker);

/// Tells whether the On list can start a game.
///
/// True when every On mutator is installed, none is locked beside the On
/// list (unavailable, a loop, a prerequisite missing or out of range, a
/// conflict), and every prerequisite of each is on above it. An empty
/// list can: the game plays without mutators.
///
/// @param picker the picker
/// @return true when the set can start
[[nodiscard]] bool ready(const Picker& picker);

/// Writes one mutator as the setup's line shows it.
///
/// The name, then each choice or number option's caption, with a space
/// between. A toggle is not written. A missing mutator is its id.
///
/// @param picker the picker
/// @param chosen the mutator
/// @return the line, such as "Fast Builders 4×"
[[nodiscard]] std::string item_text(const Picker& picker, const Chosen& chosen);

/// Joins `item_text` of each mutator with a middle dot.
///
/// @param picker the picker
/// @param chosen the mutators, in order
/// @return the line; empty when `chosen` is empty
[[nodiscard]] std::string list_text(const Picker& picker, std::span<const Chosen> chosen);

/// Makes the question `asking` holds.
///
/// The title names the prerequisite. The paragraphs name the dependants
/// that turn off with it (those On now, as `confirm_turn_off` finds them),
/// then that they stay installed. CANCEL is marked and is the cancel
/// button. TURN OFF is the primary button. An empty question is returned
/// when the picker is not asking.
///
/// @param picker the picker
/// @return the question
[[nodiscard]] kit::Question turn_off_question(const Picker& picker);

} // namespace oa::ui::mutator_picker
