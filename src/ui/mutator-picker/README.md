# Mutator picker

The model of the mutator picker: which mutators are on, in which order, with which options and clash mode, what each row and the footer say, and the question asked before a prerequisite is switched off. Library `oa-ui-mutator-picker`, headers `include/oa/ui/mutator_picker/`, namespace `oa::ui::mutator_picker`.

It draws nothing and reads no file, clock, preference or network. The host fills `Inputs`. Every word a player reads comes from `text.hpp` through the Library's `TextHooks`. Layout, input and hosting are other modules.

## Entry points

- `open` switches the last set on in its order, with its options. An option the mutator no longer has is dropped. A value it no longer allows, and an option the set does not name, take the default. A mutator that is gone or unavailable is named in `left_out`. A mutator whose prerequisites need each other is named by those ids and not placed. Prerequisites the set does not name are brought in above what needs them. The order is then mended so every prerequisite sits above its dependants. At most `most_on` (32) stay on. `from_last_set` is set when the set was remembered and anything is on. The selection is the first On mutator, or the first mutator by name.
- `refresh` takes new inputs and keeps the On list, its order, its options, the filters, the search and the selection. A mutator the inputs no longer hold is dropped and named in `left_out`. The order is mended only when a prerequisite would sit below a dependant.
- `switch_on` switches a mutator on with its default options and brings the prerequisites it lacks. It refuses when the picker is read-only, when the mutator or a prerequisite is unavailable, when the prerequisites need each other, when the set would hold a conflict, and when the On list would pass 32. Not being installed is no refusal.
- `switch_off` asks, and changes nothing, when an On mutator needs it. `confirm_turn_off` switches it and those dependants off. `cancel_turn_off` clears the question.
- `allowed_places`, `move_to`, `move_up` and `move_down` reorder inside the band below the lowest On prerequisite and above the highest On dependant. `drop` moves, switches on, or switches off, from where the row was dropped.
- `set_option` keeps a choice, a toggle or a number when the value is one the option allows. A number is compared as decimal text. `set_clash` keeps Add or Replace on an On mutator that brings units or weapons.
- `rows`, `filter_count`, `tags_shown`, `footer` and `hint` are what the list draws. `result` is the On list. `ready` is true when every On mutator is installed and none is unavailable or in conflict. `item_text` and `list_text` are the setup's short lines. `turn_off_question` is the question `asking` holds.

## State

`Picker` holds the host's `Inputs`, the On list (`Chosen`: id, option values, and clash mode when the mutator brings units or weapons), the prerequisites brought for a dependant, the names left out of the last set, the filter, the tag, the search, the selection, whether the set was the remembered one, and the turn-off question while it is open. The host may set the filter, the tag and the search. The other fields change through the functions above.

`Inputs::clash_default` is the clash mode a mutator uses when its manifest names none. The host passes the setting `mutators.clash-default`. The model does not read it.

## Invariants

- After every change, every prerequisite in the On list stands above the On mutators that need it. The order is the Library's closure (`prerequisite_closure`, `dependants_of`, `prerequisites_above`).
- A prerequisite is not switched off while a dependant is on until the player confirms.
- Only On mutators hold option values, in the mutator's option order. Switching off and on again starts from the defaults.
- A number outside its range, or off its step, is refused. The comparison is decimal text, not a floating-point value.
- On rows are shown under every filter, tag and search, numbered from 1. Off rows follow, filtered, by name.
- A row shows one line, the first case that holds: unavailable, a conflict, an incompatible or failed verdict, needed by an On mutator, a no-effect verdict, a download, not installed, the options (and the clash choice when it brings units or weapons), or the summary.
- The footer chip is the check in progress, else the first incompatible or failed On verdict, else nothing while any On verdict is unchecked, else the first no-effect verdict, else checked. The summary counts the On mutators and names the first brought prerequisite that is still on with its dependant.
- Read-only refuses every change. Opening still applies the set. Cancelling a question is still allowed, so a question cannot stick.
- No word is looked up except through `TextHooks`.

## Tests

`ui-mutator-picker-model` opens an empty set, a full set, a set that names a removed mutator and a removed option, a value out of range, a set out of order, a set with a prerequisite missing, and a set past the cap. It switches Gatehouse Keep on, refuses Grounded beside Quiet Seas, refuses Ridge Walls with its reason, refuses a cycle, and refuses every change while read-only. It asks before Gatehouse goes off, confirms and cancels. It moves and drops through a chain of three and a diamond. It accepts and refuses each option type, including a number on its step. It checks every row line, the filters' counts, the tags, the footer's chip for each verdict, the hint for each setup, the short texts and the question, and a lookup that translates one line.

## Limitations

Nothing here draws, takes input, downloads, runs a verdict check, or stores the remembered set. The host does. The clash default is an input, not a preference this module reads. `Chosen` carries the clash mode for the loadout; the loadout record itself does not have that field yet. One row line means a higher-priority line hides the options and the clash choice; the On record still holds them. The decimal reader refuses text that is not a decimal, a non-positive step, and a magnitude longer than 4096 digits. Mutators whose prerequisites need each other are refused rather than placed. A prerequisite the inputs do not hold is refused with its id in the reason: the catalogue has no separate sentence for a missing prerequisite. `Needed by` names the first On dependant in the On list. Opening records `came_with` only for a prerequisite the set itself did not name.
