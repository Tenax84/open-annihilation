# Mutation rules

`oa-data-mutation-rules` holds the parsed documents of one pipeline run and
applies one mutator's rule records to them as a step. The caller runs
mutators one after another and passes the same step state each time, so a
later mutator reads the documents the earlier one left. Needs choose the
records. Each change is written before the next change is read.

Every write names the mutator that produced it and the old and new text.
A value a later mutator replaced, a locked field a later mutator left alone,
a factor on a field that is not there, a value that is not a number or not
finite, and a block that matched nothing are noted.

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
- `apply_step` applies one mutator. It reads the documents and the step
  state, writes the documents its changes reach, and adds lock marks and
  last-writer marks to that state. A null layout uses the layout in force.
  The field check, when set, sees each new text or removal before it is
  written. This module does not know the typed field checks; a later pass
  passes them in. The caller keeps one `ApplyState` for the whole run and
  passes it to each step. A step does not clear a mark an earlier step made.
  The writes come sorted by path, record, block and key, compared without
  ASCII case, so the writes of one field stay in the order they were applied
  whichever spelling of the key each change used.
  The pipeline calls `apply_step` once per mutator and runs that mutator's
  hooks before the next mutator's step.

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

There is no combining step. Rules are applied in sequence.

One mutator's sources run in plan order. Each source's blocks and changes
run in file order. A block's needs read the records as the rules before that
block left them, and each change is written before the next change is read.
A later block of the same mutator sees an earlier block's write. The caller
runs the next mutator as another step on those documents.

A factor multiplies the current number. An assignment replaces it. A clamp
bounds only the result of its own scale; it does not bound a later scale.
A text change or a removal acts on the current text. An append extends the
current text, or the empty text when the field is absent. A later rule sees
the text the earlier rule wrote.

A lock sets the value and marks the field for the rest of the run. A later
mutator's change to a locked field is not written. The step notes that, and
the note names the mutator that locked the field and the mutator that tried
to change it. A later change of the mutator that locked the field still
applies, and the lock mark stays for the mutators after it.

A later assignment, text change, removal or lock that replaces a value an
earlier mutator wrote is noted, naming both mutators. A factor or an append
builds on the current value and is not that note. The first write, over the
documents as they were loaded, is not a replacement. A removal of a field
that is already absent does nothing and is not noted.

A factor on a field that is absent does nothing, and the step notes it. A
factor on text that is not a number is noted and does not write. A number
is written only with the TDF number spelling, and only when it is finite and
different from the field's current number. The current text has to start,
after spaces or tabs, with an optional sign and then a digit, or a point and
at least one digit (`.8`, `-.5`, `+.25`), before it counts as that number.
A lone `.` or `-.` is not a number, and a factor does not treat a word as
zero. A result that is not finite
is not written, and a clamp is not allowed to pull that result back into
range. A lock of a number the field already holds writes nothing and still
marks the field.

A key the record does not hold is written with the spelling of the change
that creates it. A later change of that field keeps the spelling the reader
already has. Two spellings of one key that differ only in ASCII case are one
field.

The order of the mutators matters, and so does the order of the changes.

## Needs on fields left out

A block's needs read the record as the rules before that block left it. A
field an earlier block wrote is read as that write left it. A field this
block has not written yet is read as it stands.

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

A step edits the `DocumentSet` it is given and the `ApplyState` the caller
passes, and no other store. `ApplyState::locks` maps each field a lock
marked to the mutator that locked it, and `ApplyState::writers` maps each
field to the mutator that last wrote it, so a later step can name both
mutators on a replacement. Both maps are keyed by `FieldId`, so finding a
field does not read every mark. The caller starts from an empty state and
passes the same value to each step. A step adds marks and does not clear one
an earlier step made.

Paths are kept with `/` separators, in the order the documents were added,
and a removed document stays in the set so its index does not move. This
module does not add or remove documents and does not set those flags. It
only changes keys inside the trees, through the TDF editor. A document no
write touched is left unmarked, including when a computed number matches the
current value and is therefore not written.

A `FieldId` names a field by its folded path, record, block and key. ASCII
capitals are folded, and a path treats `\` as `/`. A change folds its field's
names once. Two sections of the same name in one file share a lock.

## Invariants

- A block's needs read the records as the rules before that block left them.
  Later blocks, and later mutators, see those writes.
- Two changes apply one after the other. A factor multiplies the current
  number, an assignment replaces it, and a clamp bounds only that scale.
  The same two changes in two steps leave the same text.
- A lock marks the field after it sets the value. A later mutator does not
  write a locked field, and the note names both mutators.
- A value that is not finite is never written. A clamp does not pull such a
  result back into range. Numbers become text only through the TDF number
  spelling.
- The spelling of a key a record does not hold is the spelling of the change
  that creates it. List order decides which change creates it.
- The writes of one field are listed in the order they were applied.

## Tests

`oa-data-mutation-rules-test` and `oa-data-mutation-rules-apply-test` build
small documents and rule texts. They check the written text, the writes and
the notes for factors stacked in both list orders, three scales kept in file
order, storage factors, a later
assignment replacing an earlier one, two removals, a later text assignment,
two bands in one mutator, a clamp that bounds only its own scale, a lock
held against a later scale and a later set, two clamps applied one after
another, text appends, the within-mutator sequence, needs that see a field
an earlier block changed, needs on fields a record leaves out (including a
movement class and a key no schema holds), targets and folders, a layout
that renames `units`, the spelling of a key a record does not hold, the
writes of one field listed in applied order when a change creates it again
under another spelling, a depth change applied once when a block has two
case variants of a key, and the notes for a block that matched nothing, a
field that is not a number, a result that is not finite (including a clamp
that does not rescue it), a check that warns or refuses, and a factor on a
value that starts with a point (`.8` doubled is `1.6`, `-.5` halved is
`-0.25`, and a lone `.` or `-.` is left as it was).

## Limitations

Needs in one group are all required. There is no or between them.

A lock is one mark for a folded path, record, block and key. Two sections
of the same name in one file share that mark.

Whether a target folder may be written at all is decided by a later pass.
This module will edit any document it was given.
