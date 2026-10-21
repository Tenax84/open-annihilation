# Mutation rules

`oa-data-mutation-rules` holds the parsed documents of one pipeline run and
applies every mutator's rule records to them in one pass. Needs choose the
records. Each mutator's changes to one field become one contribution. The
contributions combine into one write per field. The result does not depend
on the order of the mutators, except text appends, which follow the list.

Every write names the mutators that produced it and the old and new text.
A clamp another mutator widened, text changes that cancelled, a factor on a
field that is not there, a value that is not a number or not finite, and a
block that matched nothing are noted.

## Entry points

`oa/data/mutation_rules/documents.hpp` and `apply.hpp`, namespace
`oa::data::mutation_rules`.

- `DocumentSet` parses and holds the documents. `add` refuses a duplicate
  path, ignoring ASCII case and slash direction, and a text that does not
  parse. `documents_in` lists a folder. `changed` reports a document that
  was edited, added or removed.
- `targets_of` lists each distinct target the rules name, in the order first
  seen, so a later pass knows which folders to load. The targets are the
  folders the rules wrote, not the layout's renamed folders.
- `need_met` reports whether one need holds for a record as it stands. The
  three-argument form is what a later pass selects with: a unit, a weapon,
  or no schema. The five-argument form also treats the layout's
  `gamedata/moveinfo.tdf` as a movement class. Units and weapons win over
  that path.
- `apply_rules` runs the pass. A null layout uses the layout in force. The
  field check, when set, sees each new text or removal before it is written.
  This module does not know the typed field checks; a later pass passes them
  in.

## Targets and records

A target is a folder and a filename suffix. The suffix matches the end of
the file name, without ASCII case, so `sidedata.tdf` reaches that file and
`tdf` reaches every file of that ending. The folder is not recursive:
`features/corpses` reaches `features/corpses/a.tdf` and not a file in a
folder below it, and not `features/rocks`.

A folder whose first segment is one of the seven base directory names
(`units`, `weapons`, `gamedata`, `ai`, `guis`, `unitpics`, `download`) means
the active layout's folder of that kind. The suffix `fbi` means the layout's
unit extension. Any other folder and suffix are kept as the rule wrote them.

Unit files hold one record, the first `[UNITINFO]` section. A named block
matches `UnitName` without ASCII case, and the record is reported under that
name when the key is present, otherwise under the section name. Every other
folder treats each top-level section as a record, and a named block matches
the section name without ASCII case.

A group matches a record when every need holds. A group with no needs
matches every record of its target. There is no or between needs.

`<AllDepth2>` writes every key of every block directly inside the record,
each key its own field. A weapon's `[DAMAGE]` entries are reached that way.
Keys of the record itself are left alone. A record that has no such keys
still matched; it is not "no effect". Two spellings of one key inside a
block, differing only in ASCII case, are one field: the reader keeps the
newest, and the change is applied once.

## Combination

Within one mutator, sources in plan order and blocks and changes in file
order, the last change of a field decides its kind. An assignment resets
that mutator's factors and clamps. Factors then multiply onto whatever is
current. Clamps join, taking the lower low and the higher high. A meta
value replaces the numeric state and survives later numeric changes; a later
meta value replaces the earlier one. Text assignments replace, appends
extend, and a removal clears. An append after a removal becomes an
assignment of the appended text, and an append after an assignment extends
that assignment, so the mutator still fully determines the text. A change of
the other kind discards what came before. That discard inside one mutator
is not noted; the notes below are about mutators combined.

Across mutators the algebra is:

- If any contribution is a meta value, the result is the mean of the meta
  values and every other change, numeric or text, is dropped. Only the
  mutators that supplied a meta value are listed on the write.
- Otherwise the base is the mean of the assignments, or the field's current
  value when there is none, times the product of the factors, then clamped
  to the lowest low and the highest high of every clamp. Sums run in mutator
  list order. Each mutator multiplies its own factors in file order. Those
  products then multiply in a fixed order: the exact bits of the factor,
  smaller first, and, when the bits match, the rule text of the scales (the
  source name and line of each one, in file order). A mean is the sum
  divided by the count. The clamp is applied to the combined product, not
  to each mutator before the product.
- A factor on a field that is absent, and that no assignment creates, does
  nothing. An assignment or a meta value creates the field, and other
  mutators' factors then apply to that base.
- Text changes apply only when no numeric change touches the field. Appends
  add in list order, including when the field is absent. An assignment or a
  removal takes effect only when it is the only text change. Two
  assignments, two removals, or one of each, cancel, and the field stays as
  it was. Appends alongside an assignment or a removal still append; the
  assignment or removal is noted and does not write. A mutator that
  contributed nothing to the value actually written is not listed on it.

A number is written only with the TDF number spelling, and only when it is
finite and different from the field's current number. The current text has
to start, after spaces or tabs, with a sign and a digit before it counts as
that number: a factor does not treat a word as zero. A result that is not
finite is not written, and a clamp is not allowed to pull an infinite result
back into range. A combined value that is written outside the clamp one
mutator gave on its own is noted, naming that mutator, its range, and the
range that was used.

## Needs on fields left out

A need is decided on the record as the pass found it, before any write, so
one rule's change cannot decide which records another rule selects.

When the field is written, a range need holds when the text's number is
inside the bounds, inclusive. Text that is not a number reads as zero, the
way the game reads it. An equality need holds when the text matches without
ASCII case. A substring need holds when the text contains the needle without
ASCII case. An empty needle is contained.

When the field is not written, a presence test does not read a default.
`Field=""*` holds exactly when the key is in the file, and `Field=!""*`
exactly when it is not. Every other need is read against the value the
loader uses for that key. For a unit, a weapon, or a movement class that
value is `default_value`. A key the schema does not hold, and any other
record, reads 0 for a range and empty text for an equality or a substring.
The defaults that are not zero or empty, including `bankscale`, `moverate1`,
the standing orders, the category keys and a weapon's `range`, are listed
in the field schema README under "Defaults that are not zero or empty".
This module does not copy them.

A negated need holds exactly when the positive one does not.

This is the maintainer's answer 4 of 2026-10-20. It differs from TA:
Mutation's documentation, which says a range, an equality or a substring
need fails when the tag does not exist. On the 3.1c data the difference
changes two results, both intended. WG-Fortress's energy-use band now
matches ARMUWFUS, which writes no EnergyUse, so that unit is given
EnergyUse=0 with the other units in the band. CloakMod's metal-cost bands
now catch ARMFMKR and CORFMKR, which write no BuildCostMetal and so read 0,
the lowest band.

## State

The pass edits the `DocumentSet` it is given and no other store. Paths are
kept with `/` separators, in the order the documents were added, and a
removed document stays in the set so its index does not move. This module
does not add or remove documents and does not set those flags. It only
changes keys inside the trees, through the TDF editor. A document no write
touched is left unmarked, including when a computed number matches the
current value and is therefore not written.

## Invariants

- Needs read the records from before the pass. Later blocks of the same
  mutator do too.
- One field is written at most once. The mutators on that write are the ones
  whose contribution is in the value, in list order.
- Numeric results do not depend on mutator order. Stacked factors multiply
  in the fixed bit order above, so twenty factors on one field spell the
  same number in every list order. Appends do depend on order, and follow the
  list. A key the record does not hold is written with the spelling that
  sorts first by raw bytes among the changes that name it, so the file's
  bytes do not depend on list order either. `EnergyStorage` precedes
  `energystorage`.
- A value that is not finite is never written. Numbers become text only
  through the TDF number spelling.
- Two text assignments, or two removals, never pick a silent winner.

## Tests

`oa-data-mutation-rules-test` builds small documents and rule texts. It
checks the written text, the writes and the notes for stacked factors
(including twenty-four factors in several list orders, and three scales
inside one mutator kept in file order), meta
values and the fields they create, a widened clamp, text cancellation and
append order, the within-mutator rule, needs on fields a record leaves out
(including a movement class and a key no schema holds), targets and folders,
a layout that renames `units`, the spelling of a key a record does not
hold, a depth change applied once when a block has two case variants of a
key, and the notes for a block that matched
nothing, a field that is not a number, a result that is not finite, and a
check that warns or refuses.

## Limitations

Needs in one group are all required. There is no or between them.

When two mutators both assign a field, or both remove it, neither wins and
the field stays. Two removals of the same unit therefore leave that unit in
the game. The pass notes that cancellation every time it happens. Appends
follow the mutator list, so that one result does depend on order.

Whether a target folder may be written at all is decided by a later pass.
This module will edit any document it was given.
