# TDF documents

`oa::formats::tdf` reads the game's TDF, FBI, GUI and OTA text, can change a
parsed document, and writes it back. One number formatter spells every finite
value the same way on every machine. A document's storage is one bounded
arena (`max_arena_bytes`); nothing an edit stores lives outside it.

## Who reads it

- `src/data/defs` reads every `units/*.FBI` header (`load_unit_header`), drops
  the unavailable types and sorts the rest by unit name
  (`unit_defs_finalize_catalog`), then reads each kept type's `[UNITINFO]`
  into its `UnitDef` (`load_unit_def`). A key that is present takes its text's
  value even with no number in it, and a value that opens with `;` ends at
  once, so the rest of that line runs into the next key's name.
- `oa-data-unit-definitions` fills the typed `UnitDefinition` from a loaded
  `UnitDef` (`unit_definition_from`), resolves movement, footprint and sensor
  metadata (`resolve_runtime_metadata`) and copies weapon target masks
  (`target_category_masks`). Fixed-point fields stay 16.16 integers.
- Scenario (OTA), side, sound, meteor and feature files use the same reader
  and its number getters. `OwnedDocument` holds a parsed document and frees it
  when it goes out of scope. An entry missing its closing `;` runs on to the
  next `;`: the shipped `CORALS.TDF` gives Coral20 no height, and
  `BARRIERS.TDF` gives Barrier13 a shadow sequence name no archive holds.
- A weapon section's body hash is folded into each unit's weapon checksum for
  battle-room unit sync. An OTA `GlobalHeader` body hash is the map's
  multiplayer hash, and that body includes the schema blocks inside the header.

## Entry points

Reading: `parse_text` and `OwnedDocument::parse` build the tree.
`find_value`, `get_int`, `get_double`, `get_fixed` and `get_string` read a
field. `parse_int` and `parse_double` interpret a value's text.

Editing (`oa/formats/tdf/edit.hpp`): `set_value`, `remove_key`, `add_child`
and `remove_child` change a block in the document's arena. `document_edited`
reports whether any block changed. `editable_root` and `editable_child` hand
back a block to change. `property_value_at` is the value beside
`property_key_at`.

Writing (`oa/formats/tdf/write.hpp`): `write_text` spells the document. A
document nobody edited is the comment-stripped text it was parsed from. A
section nobody edited, under one someone did, is written with its parsed body
byte for byte, so its body hash does not move. An edited section is laid out
again, keys in file order.

Numbers (`oa/formats/tdf/number.hpp`): `format_number` spells one finite
value. Callers that write a numeric field pass its text to `set_value`.

## What round-trips

- Section order is the order of `Block::children`.
- Key order is `Property::order`: the key's place in its block. A repeated
  byte-identical key keeps one slot, the last value, at the first
  occurrence's place. A new key takes the next place, after every existing key.
- Each key keeps the spelling it was written with.
- Keys that differ only in case keep separate slots. The newest is what
  `find_value` returns. The writer emits case variants oldest first, so a
  re-read sees the same newest.
- Comments are not kept. They are blanked to spaces before parsing.
- A value is trimmed of spaces, tabs and line breaks at both ends. A value
  that ran past a missing `;` keeps its line breaks and the next key's text,
  and an edited block writes that stored text unchanged.
- `Block::body_hash` hashes the body the block was parsed from. It goes stale
  when the block is edited, until the written text is parsed again. Writing
  an untouched section byte for byte keeps its hash: changing one weapon
  leaves every other weapon section's hash, and so every other unit's weapon
  checksum, as it was. An OTA schema sits inside `GlobalHeader`, so editing a
  schema changes the map's multiplayer hash. The same mutators on every
  machine change it the same way.

## Number rule

`format_number` is the only spelling the engine uses to write a value. It
works on the value's bits with integer arithmetic, so every machine writes
the same text.

1. Zero, of either sign, is `0`.
2. A value that is a whole number of 65536ths (every integer and every 16.16
   value, tested as the binary value times 65536 being a whole number) is
   written as its exact decimal: the whole part and at most 16 places, with
   no rounding. Trailing zeros and a bare point are dropped.
3. Any other finite value is rounded to 15 significant digits, half away from
   zero, on its exact binary value. Trailing zeros and a bare point are dropped.
4. The text is an optional `-`, digits, and an optional point and digits.
   Not-a-number and infinities have no text: the formatter refuses them.

Fifteen significant digits is how the tools of that era spelled a double, and
it writes what decimal arithmetic meant. 1.6 × 1.5 is slightly over 2.4 in
binary and is written `2.4`, which a 16.16 field reads as 157286; scaling the
stored record instead gives 157285. 0.57 × 100 is slightly under 57 and is
written `57`, which an integer field reads as 57. Rule 2 keeps every integer
and every 16.16 value exact, so a value read as 16.16 and written back reads
as the same 16.16 value: 65537/65536 written to 15 significant digits would
come back one 65536th lower. An exponent is never written, because `parse_int`
reads `1e+21` as 1.

This is not the profile number spelling (`canonical_number_text`), which is
for mod profiles and does use an exponent.

## Tests

`tdf-parser` covers the reader. `tdf-writer` covers editing, writing and the
number table, including the 16.16 values `2.4` and `2.399993896484375`.
`installed-tdf-writer`, with game data, writes every installed FBI, weapon,
feature, download, gamedata, GUI and OTA file the store lists and the reader
accepts; it names each file the reader refuses and counts it as skipped.

Build and test this module on its own:

```sh
cmake -S src/formats/tdf -B local/build-tdf-D01
cmake --build local/build-tdf-D01
ctest --test-dir local/build-tdf-D01 --output-on-failure
```

Inside the engine's build, `unit-definitions-data` loads every unit of the
installation `OA_GAME_DIR` names as a match loads them.

## Limitations

- Comments are not kept, so a written file cannot put them back.
- An edited block is laid out again. Sections nobody edited keep the body
  bytes they were parsed from; their surrounding brackets follow the writer's
  layout when a parent had to be rewritten.
- Edits allocate only from the document's arena and fail with
  `out_of_memory` once `max_arena_bytes` is exhausted. The arena does not
  reuse storage an edit replaced, so a long series of edits can exhaust it
  before the text itself is large.
- `write_text` refuses a result longer than `max_input_bytes`, because the
  reader would refuse it too.
