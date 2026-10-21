# Mutator picker

The model of the mutator picker: which mutators are on, in which order, with which options and clash mode, what each row and the footer say, and the question asked before a prerequisite is switched off. Library `oa-ui-mutator-picker`, headers `include/oa/ui/mutator_picker/`, namespace `oa::ui::mutator_picker`.

It draws nothing and reads no file, clock, preference or network. The host fills `Inputs`. Every word a player reads comes from `text.hpp` through the Library's `TextHooks`. Layout, input and hosting are other modules.

## Entry points

- `open` switches the last set on in its order, with its options. An option the mutator no longer has is dropped. A value it no longer allows, and an option the set does not name, take the default. A mutator that is gone is named in `left_out` by its id. The order is mended so every prerequisite sits above its dependants, then the list is read from the top: a mutator that is locked beside those kept above it (see the rows) is left out and named in `left_out`, so of two that conflict the later one goes, and one that needs a mutator left out goes too. A kept mutator gets the prerequisites it lacks above it. At most `most_on` (32) stay on. `from_last_set` is set when the set was remembered and anything is on. The selection is the first On mutator, or the first mutator by name.
- `refresh` takes new inputs and keeps the On list, its order, its options, the filters, the search and the selection, then settles the list as `open` does. A mutator the inputs no longer hold is dropped and named by its id. A mutator whose prerequisite went or no longer meets its range, whose needs became a loop, that became unavailable or that now conflicts with one above it goes off with its On dependants, each named in `left_out`; its row says why. A prerequisite a mutator now needs is brought in above it when it can be had. A question about a mutator that went off is dropped.
- `switch_on` switches a mutator on with its default options and brings the prerequisites it lacks. It refuses when the picker is read-only, when its row is locked (with the row's line), and when the On list would pass 32. Not being installed is no refusal.
- `switch_off` asks, and changes nothing, when an On mutator needs it. `confirm_turn_off` switches it off with every On mutator that needs it at that moment, including one switched on after the question was asked. `cancel_turn_off` clears the question.
- `allowed_places`, `move_to`, `move_up` and `move_down` reorder inside the band below the lowest On prerequisite and above the highest On dependant. `drop` moves, switches on, or switches off, from where the row was dropped.
- `set_option` keeps a choice, a toggle or a number when the value is one the option allows. Choices and toggles are compared as identifiers. A number is read with the manifest's number grammar (`oa::formats::oamod::parse_number`) and kept in its canonical text (`canonical_number_text`), so `1e1` is kept as `10` and `+4` or `00.5` is refused. `number_values` lists the values a number offers, its stops from the minimum to the maximum in canonical text, for the slider and the arrows that step it. `set_clash` keeps Add or Replace on an On mutator that brings units or weapons.
- `rows`, `filter_count`, `tags_shown`, `footer` and `hint` are what the list draws. `result` is the On list. `ready` is true when every On mutator is installed, none is locked, and each has its prerequisites on above it. `item_text` and `list_text` are the setup's short lines. `turn_off_question` is the question `asking` holds, naming the dependants on now.

### Locked rows

One check decides whether a mutator can be on beside the On list; `rows`, `switch_on`, `open`, `refresh` and `ready` all use it. It reads the mutator's prerequisite closure (`prerequisite_closure`) and locks the row, in the danger tone, with the first reason that holds:

- the mutator is unavailable: "Not compatible with this mod: {reason}";
- its closure is a loop: "Not compatible with this mod: These mutators need each other: {ids}", the Library's `blocked_cycle`, naming the ids in the loop;
- a prerequisite is not among the inputs, or its version is outside the range its dependant asks for (`oa::formats::oamut::condition_met`): "Not compatible with this mod: Needs {name} {range}, which no registry offers", the Library's `blocked_prerequisite`, or "Needs {name}, which no registry offers" when no range is asked;
- a prerequisite is unavailable: its reason, as for the mutator;
- the mutator, or a prerequisite it would bring, conflicts with an On mutator ("Cannot run beside {name}", naming the On one), or the two conflict with each other. A conflict between two mutators already on is not checked against a third.

## State

`Picker` holds the host's `Inputs`, the On list (`Chosen`: id, option values, and clash mode when the mutator brings units or weapons), the prerequisites brought for a dependant, the names left out of the last set, the filter, the tag, the search, the selection, whether the set was the remembered one, and the turn-off question while it is open. The host may set the filter, the tag and the search. The other fields change through the functions above.

`Inputs::clash_default` is the clash mode a mutator uses when its manifest names none. The host passes the setting `mutators.clash-default`. The model does not read it.

## Invariants

- After every change, every prerequisite in the On list stands above the On mutators that need it. The order is the Library's closure (`prerequisite_closure`, `dependants_of`, `prerequisites_above`).
- After every change, no On mutator is locked: each has its whole closure on above it, in range and available, with no loop and no conflict. `ready` then needs only every On mutator installed.
- A prerequisite is not switched off while a dependant is on until the player confirms.
- Only On mutators hold option values, in the mutator's option order. Switching off and on again starts from the defaults.
- A number outside its range, or off its step, is refused. Numbers are read and compared with the manifest's grammar, exactly, never as floating-point values, and kept in canonical text.
- On rows are shown under every filter, tag and search, numbered from 1. Off rows follow, filtered, by name.
- A row shows one line, the first case that holds: locked (above), an incompatible or failed verdict, needed by an On mutator, a no-effect verdict, a download, not installed, the options (and the clash choice when it brings units or weapons), or the summary.
- The footer chip is the check in progress, else the first incompatible or failed On verdict, else nothing while any On verdict is unchecked, else the first no-effect verdict, else checked. The summary counts the On mutators and names the first brought prerequisite that is still on with its dependant.
- Read-only refuses every change. Opening still applies the set. Cancelling a question is still allowed, so a question cannot stick.
- No word is looked up except through `TextHooks`.

## Tests

`ui-mutator-picker-model` opens an empty set, a full set, a set that names a removed mutator and a removed option, a value out of range, a set out of order, a set with a prerequisite missing, a set holding two mutators that conflict, and a set past the cap. It switches Gatehouse Keep on, refuses Grounded beside Quiet Seas, refuses Ridge Walls with its reason, refuses a loop, and refuses every change while read-only. It locks and refuses a mutator whose prerequisite is missing, out of its range (Gatehouse 3.9 against `>= 4.0`), unavailable, or conflicts with an On mutator, and lets an unrelated mutator on beside a conflicting pair. It refreshes after a prerequisite goes, after its version falls below the range, after needs become a loop, after a new need that can or cannot be had, after a new conflict and after a mutator becomes unavailable. It asks before Gatehouse goes off, confirms and cancels, and confirms after a dependant came on while the question was open. It moves and drops through a chain of three and a diamond. It accepts and refuses each option type, lists a number's values (including stops the grammar cannot write) and keeps numbers in canonical spelling. It checks every row line and the clash mode on a row, the filters' counts, the tags, the footer's chip for each verdict, the hint for each setup, the short texts and the question, and a lookup that translates one line. Random walks of every call, with questions left open across other calls and refreshes that change the inputs, check after each call that the order holds, that the set is playable, that `ready` is true exactly when it is also installed, and that each Off row's lock matches what `switch_on` does.

## Limitations

Nothing here draws, takes input, downloads, runs a verdict check, or stores the remembered set. The host does. The clash default is an input, not a preference this module reads. `Chosen` carries the clash mode for the loadout; the loadout record itself does not have that field yet. One row line means a higher-priority line hides the options and the clash choice; the On record still holds them. The model sees one version of each mutator, the one the host gives; a prerequisite whose given version is out of range is locked even when another registry might offer one in range. A number option whose minimum, maximum or step does not read, whose step is not above zero, or whose range holds more than 1,000 steps offers no values and keeps its default; a stop that needs more significant digits than the number grammar holds is not offered. Mutators whose prerequisites need each other are refused rather than placed. `Needed by` names the first On dependant in the On list. Opening records `came_with` only for a prerequisite the set itself did not name. A read-only picker settles the host's set as any other, so a mutator the guest's inputs lock is left out of the guest's view.
