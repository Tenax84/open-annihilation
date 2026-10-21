# data/mutators

The module the mutation pipeline grows in. A mutator may change a match's
files only where the game reads those files again at the start of every
match. This part of the module is that allow-list. `check_mutable_path`
and `check_mutable_mask` accept those paths and refuse every other path
with a rule and an English message that names the path. The messages are
for the log, `oa-tool` and the dry-run report. A player sees a translated
sentence chosen from the rule, which is not this module's to write.

## Entry points

Target `oa-data-mutators`, alias `oa::data::mutators`, namespace
`oa::data::mutators`. Headers `oa/data/mutators/allow_list.hpp`,
`oa/data/mutators/collections.hpp`, `oa/data/mutators/files_layer.hpp`,
`oa/data/mutators/journal.hpp`, `oa/data/mutators/loadout.hpp`,
`oa/data/mutators/pipeline.hpp`, `oa/data/mutators/rules_lua.hpp` and
`oa/data/mutators/verdicts.hpp`.

- `check_mutable_path` judges one file path.
- `check_mutable_mask` judges one mask. The pattern's shape is
  `mutator_mask_problem` from `oa/formats/hpi.hpp`. A wildcard is allowed
  only in a folder the table allows as a whole.
- `rule_name` returns the stable lower-case name of a `PathRule`.
- `build_files_layer` turns an ordered list of files plans into one
  `MutatorLayerSpec`. See [Files layer](#files-layer).

## State

None kept. The two checks are pure: they do not read files, the asset store
or the preferences. A null `PathContext::layout` means `data_layout()`. An
empty `is_unit_name` knows no unit. `build_files_layer` reads the package
folders it is given while it builds the layer, and it keeps nothing after
it returns. `folder_digest` reads one folder and does not write. The other
loadout functions read nothing.

## Invariants

- The default is to refuse. A folder the table does not name is
  `unknown_folder`, including a file at the top of the tree.
- One table in `src/allow_list.cpp` decides every folder. A path moves to
  the allowed side by editing one row of that table.
- The seven directory names a mod may rename come from `DataLayout`:
  `units`, `Weapons`, `gamedata`, `ai`, `guis`, `unitpics` and `download`
  in the base game. Names are compared without ASCII case. A variant
  folder `<directory>-<variant>` follows its base folder. A renamed
  directory replaces the base name, which is then an unknown folder.
- `features/`, `objects3d/` and `scripts/` are allowed as wholes, as are
  the layout's `units`, `Weapons`, `ai`, `unitpics` and `download`
  folders. Under `gamedata/` only `moveinfo.tdf`, `sidedata.tdf`,
  `los.tdf` and `meteor.tdf` are allowed. Under `anims/` only
  `<unit><digits>.gaf` and `<unit>_gadget.gaf` are allowed, one level down
  (a variant folder follows `anims/`), and only when `is_unit_name` accepts
  that unit. `guis/` is refused as a whole: build pages and screens
  change only through a mutator's menus. `maps/` changes only through a
  mutator's maps hook. `sounds/` is read once per run.

## Tests

`data-mutators-allow-list` (`tests/allow_list_test.cpp`) checks each
allowed and refused form, including mixed case, backslashes and a variant
folder; a layout that renames directories; every invalid path; the masks;
and that every message starts with the path and every rule has its own name.

## Limitations

The checks do not look at a mutator's files and do not apply a verdict.
Build pages stay refused, because they change through the menus hook; only
a named unit's build picture and its download-button art are files a
mutator may add under `anims/`, and only as those files. `sounds/` stays
refused until a later ticket shows the game
reads it again for every match, or changes the game so that it does.

## Moving a path to the allowed side

A path moves only when a ticket of its own does all three of these:

1. Name the function that reads the path at every match start, and show
   that nothing keeps those bytes from one match to the next.
2. Show two matches of one run reading different bytes after the layer
   changes. A `--trace-lookups` log that lists the path under both match
   starts is the minimum.
3. Change one row of the table, its test case, and the design's table.

This ticket moved two paths onto the allowed side:

- `gamedata/los.tdf`. `load_gamedata_tables` reads it while
  `Runtime::bootstrap_match` prepares each match
  (`src/app/runtime_skirmish_start.cpp`). The match's line-of-sight
  tables come from that read.
- `gamedata/meteor.tdf`. `Runtime::reset_meteors` reads it for the meteor
  defaults (the file name `kMeteorDefaults` in
  `src/app/runtime_saveload.cpp`), and `bootstrap_match` calls
  `reset_meteors` for every match.

`anims/<unit>_gadget.gaf` moved onto the allowed side the same way, for a
unit `is_unit_name` accepts. The path is one level under `anims/` (a
variant folder such as `Anims-German/` follows it), and the suffix
`_gadget.gaf` matches without regard to case.

- `Runtime::bind_gadget_gaf_art` (`src/app/runtime_order_panel.cpp:249`)
  appends `anims/<name>_gadget.gaf` for each button whose picture comes
  from a GAF and is not already in the panel. `Runtime::load_match_hud_layout`
  (`src/app/runtime_match_menus.cpp:1565`) calls it (`:1635`) for every
  panel a match shows, and a download page calls it again when it links
  buttons (the redraw on `Runtime::order_panel_loader`,
  `runtime_order_panel.cpp:350`).
- `Runtime::append_gaf_file` (`src/app/runtime_gaf.cpp:284`) reads those
  bytes from the store each time. A path it could not read is remembered
  only for that match: `Runtime::teardown_match`
  (`src/app/runtime_loading.cpp:243`) drops the panel (`:297`) and clears
  the set (`:299`). A GAF that fails to parse is still logged once in the
  run. A file the store does not hold stays silent.
- `--check-gadget-art-per-match` (`native-gadget-art-per-match`) is the
  evidence: three matches of one run. The lab's download page links a
  button with no picture, then draws the picture after the mod folder
  gains the button's GAF (the bytes of the lab's build-page art, read
  through the store), then shows no picture once that file is gone.

## Files layer

`build_files_layer` takes the mutators in the player's list order. Each one
has an id, the installed package folder and a `FilesPlan`: `mask` patterns
and `add` folders, both in the order written, and both optional. It returns
the `MutatorLayerSpec` the asset store mounts, notes for the change journal
and one failure for each mutator it left out. It does not mount the layer
and it does not read the asset store.

For each mutator that is kept, the masks are applied first and then the add
folders, in the order listed. A mask is stored on the spec and every file an
earlier mutator added that it matches is removed, with an `add_masked` note
naming both mutators. The mutator's own files are added after its masks, so
a mask does not hide that mutator's own add.

An add folder is a relative path inside the package: no `..`, no drive, no
leading slash, and not a link. Each segment is matched to the directory's
entries without ASCII case, so `FILES` and `files` are the same folder on
every machine. Two entries that fold to one name refuse the mutator. A
folder that is not there adds nothing and records an `add_folder_missing`
note, which is what a plan written as `files/<id>` relies on when that mod
has no files of its own. A folder that exists is read as follows.

- Only the top of the folder is searched for archives, and only `.ccx` and
  `.ufo` are opened. An archive further down is an ordinary loose file, as
  is any other kind of file.
- `.ccx` archives are opened before `.ufo` archives. Within each group the
  names follow NTFS order: compared byte by byte after ASCII upper-casing,
  with the raw bytes breaking a tie. The earlier name keeps a path both
  archives hold.
- Loose files are walked after the archives. A loose file keeps a path over
  that folder's archives. Listings are sorted by the folded path, so the
  machine's own directory order never decides a file or the order of the
  spec.
- A path this mutator already added from an earlier folder is replaced with
  no note. A path an earlier mutator added is replaced with an
  `add_replaced` note naming the later mutator as the one that won.
- Every path that remains is judged with `check_mutable_path`, and every
  mask with `check_mutable_mask`. A unit's build picture under `anims/`, and
  its download-button art `anims/<unit>_gadget.gaf`, are allowed when the
  mod already names that unit, or when this mutator or an earlier one still
  adds that unit's file under the layout's units folder.

The spec lists entries in mutator order and, within a mutator, in the order
the paths were first added. Each folder's new paths are sorted by the folded
path. Replacing a path does not move it. `spec.archives` holds every archive
a kept entry still reads, in the order the archives were opened, and omits
an archive whose files were all replaced. `spec.label` is `mutators`
followed by the ids that were kept, separated by spaces.

A mutator that fails leaves nothing behind. Its masks, entries, archives and
the notes about it are dropped, and the mutators after it are built as
though it were not in the list. The failure names the mutator and the mask,
folder, archive or file, and includes the allow-list's own message when
that is what refused it. One mutator failing does not fail the others.

Limits, each of which fails the mutator that would cross it:

| Limit | Value | What it counts |
| --- | --- | --- |
| `files_plan_mask_limit` | 64 | mask patterns in one plan |
| `files_plan_add_limit` | 16 | add folders in one plan |
| `add_folder_archive_limit` | 16 | `.ccx` and `.ufo` files at the top of one add folder |
| `add_folder_file_limit` | 16384 | loose files walked under one add folder |
| `add_folder_depth_limit` | 32 | nested folders under one add folder |
| `MutatorLayerPatternLimit` | 2048 | masks in the whole layer |
| `MutatorLayerEntryLimit` | 65536 | paths in the whole layer |
| `MutatorLayerArchiveLimit` | 512 | archives a kept entry still reads |

The table is deterministic. Every listing is sorted by the folded path
before it is used, archive groups have a fixed order, and nothing in the
result follows the order a directory happened to return. Two machines with
the same packages and the same list build the same paths, owners, kinds,
archive order, masks and label. That is the table the loadout digest hashes.

`data-mutators-files-layer` builds made-up packages in a scratch folder and
checks the order, the notes, the failures and a spec mounted on an asset
store. A link case is skipped where the system cannot make a link.

Still protected:

- `sounds/`. `Runtime::load_all_sounds` runs once, from the `Runtime`
  constructor (`src/app/runtime.cpp`), and not at match start. A mutator
  that changed a sound would leave the game half-changed until the next
  launch.

## Collections

`Collections` is the units, weapons and features a hook reads and writes
on the pipeline's shared documents. The object must outlive every Lua
state it is pushed into, and the document set must outlive the object.
`push_units`, `push_weapons` and `push_features` push one collection each.
`set_context_collections` sets those collections on a context table as
`units`, `weapons` and `features`. `begin_mutator` names whose turn it is
and which layers that mutator may change. `end_mutator` ends the turn.

`install_selection_forms` publishes `range`, `is`, `has`, `absent` and
`not_`. `need_from_lua` reads one need: a number `n` is `range(n, n)`,
`true` is `range(1, 1)`, `false` is `not_(range(1, 1))`, a string is `is`
of that string, and `absent()` is `not_(has(""))`. `not_` of a `not_` is
an error. Hooks and rules share this reading.

A collection is indexed by record name, ignoring ASCII case. A missing
name is nil. `#` is how many records it holds. `where { field = form, ... }`
returns the records that meet every need, judged on the records as they
stand, so a hook sees earlier writes. A field the record leaves out is
read as the value the loader uses: a missing flag or number is 0, missing
text is empty, and a missing `bankscale` is 1. `absent()` asks whether the
key is written, not what the default would be. A set has `each`, `set`,
`scale`, `remove`, a length and `pairs`. A record has `remove`, `get`,
`set` and `keys`. Any other name is a field. A field whose name is one of
those four is reached only with `get` and `set`. A weapon's `damage` is
its `[DAMAGE]` block, created on the first write into it. `get("damage")`
reads a field of that name and does not open the block.

A read returns the schema's integer, number, boolean or string, and nil
when the key is absent. An unknown field, and every feature field, comes
back as a string. A write of a known unit or weapon field is checked
first. A refusal is a Lua error that names the collection, the record,
the field and the reason, and the script line travels with it. A warning
is stored and noted. A number is stored only through the TDF number
spelling, and a value that is not finite is an error. A boolean is stored
as `1` or `0`. A string is stored as given. Nil removes the key. Feature
fields skip the schema and are still stored as text, numbers included.
A field name loses spaces, tabs and line breaks at either end before the
check, the read and the write. The journal keeps the spelling already
stored, or the trimmed name when the key is new. A write that would store
the text already there changes nothing: the document, the journal and the
notes stay as they are.

`scale` multiplies each field that is present and numeric. An absent
field is left absent. Text that is not a number is left as it is and
noted. `set` and `scale` visit the table's fields in the script host's
`pairs` order, and the records of one field in collection order.

`remove` on a unit marks that unit's document removed and drops every
section in the layout's download folder whose `UNITNAME` or `UNITMENU`
names the unit, ignoring ASCII case. A download file left with no section
is marked removed. When the section removed was the file's first and
another section remains, a note names the unit that file now marks.
Pages under `guis/` are left as they are. `remove` on a weapon or a
feature drops that section, and marks the document removed when it was
the last section. A handle remembers the section it was given. The name
it keeps is only for messages. Removing that section, or its document,
makes a later read or write raise "<name> was removed". Removing a
different section that shares the name, or writing `unitname`, leaves
the handle on its own section. The collection no longer returns a
removed record by name.

Writing or removing units needs the `units` layer, weapons the `weapons`
layer, and features the `features` layer. Reading is allowed either way,
including from another collection on the context. With no mutator begun,
every write and every removal is refused.

Records are walked in one order: the name compared byte by byte after
ASCII lower-casing, then the document path the same way, then the ordinal
of that name within its document. `pairs` and `each` use that order, on a
collection and on a set. `each` and `pairs` on a collection skip a record
removed since that walk began. A set keeps the records `where` selected,
so a later edit does not add a record to it, and it still yields a member
removed after `where`. Using that member raises. Looking a record up by
name uses an index. A removal builds it again; writing `unitname` moves
only that unit's entry. Each entry carries its section's handle slot, so
a walk or a lookup costs the same however many records were handed out
before it.
`keys()` lists the spellings that are present, ordered with `name_less`.
Nothing in this walk follows Lua
table order, hash order or pointer order.

`writes()` is every successful field write, in the order it happened:
the mutator, the path, the record, the block (`""` or `DAMAGE`), the key
as stored, and the old and new text. A later mutator that writes a field
an earlier mutator wrote in this run adds a `hook_overwrote` note naming
both; the later value is the one kept. `removals()` lists each unit,
weapon, feature, download entry and document that a removal took out.
`notes()` lists a check warning, an overwrite, a download file whose
first entry changed, and a scale that skipped a value that is not a
number. One mutator may journal at most 65536 field writes and 65536
notes on one collections object. Passing either limit fails that mutator.
The messages are "this mutator has changed too many fields" and "this
mutator has made too many notes".

`data-mutators-collections` builds a small Ridge document set and runs
the fixtures in `tests/collections`.

## Loadouts

`loadout.hpp` is the record a game, a recording and a save name their
mutators by, before a match start fills it. A `LoadoutEntry` names one
mutator by id, version and package SHA-256. `folder` says the package was
a folder named by path, and then the SHA-256 is that folder's digest
rather than a package file's. `options` is every option of the manifest,
defaults filled by `default_option`, in id byte order. A toggle's value is
`toggle`. A choice keeps the manifest's `ChoiceValue` (a number or a
string). A number option keeps its number and that number's canonical
text.

`canonical_loadout` writes one text through the engine's canonical JSON:
`{"mutators":[...],"oaloadout":1}`, entries in list order. Each entry's
members are `folder`, `id`, `options`, `sha256` and `version`. `sha256` is
64 lower-case hex digits. Options are members of one object, so their
order in the record does not change the text and none is left out. A
toggle is a boolean, a number is a number, and a choice is a number or a
string as the manifest types it. `parse_loadout` accepts only that text.
The limits are `loadout_most_entries` (32) and `loadout_text_most_bytes`
(64 KiB). A wrong `oaloadout`, an unknown key, upper-case hex, a 33rd
mutator, 64 KiB plus one byte, trailing text and any spacing or key order
the writer would not produce are refused, and the error names what.
A JSON number reads back as a number option. A numeric choice is written
as a JSON number too, so it reads back as a number option with that
number; the manifest tells the two apart, and this text does not repeat
the manifest.

Records compare equal exactly when their canonical texts are equal, so a
record read back with `parse_loadout` equals the one written. Two options
are equal when their ids are equal and their values are the same JSON
value: toggles by `toggle`, string choices by their text, and anything
holding a number (a number option or a numeric choice) by the number's
canonical text. So a default written `1.0` in the manifest equals the `1`
read back, and a numeric choice equals the number option it reads back as.
An entry's options compare as members of one object, whatever order they
are listed in.

`loadout_digest` is the SHA-256 of the canonical text.

`folder_digest` hashes each regular file under the folder, sorted by its
relative path compared bytewise. The path is the UTF-8 spelling, with `/`
between its parts, of the names as the file system stores them, on every
system; there is no Unicode normalisation, so a name stored composed and
the same name stored decomposed give different digests. Each file is that
path, a NUL, its size as eight little-endian bytes, and the SHA-256 of its
bytes. A symbolic link, anything that is not a regular file or a folder (a
pipe, a socket or a device), or a file that cannot be read refuses the
digest, and the error names the path in UTF-8. An empty folder hashes the
empty message.

`layer_digest` hashes `oa-mutator-layer-1` and a line feed, then each path
the layer answers for, sorted by its lower-case `/` form. The message
carries that form, a NUL, `F` and the SHA-256 of the bytes the layer
serves for the path as stored, or `H` for a hidden entry. The served bytes
are not read for a hidden entry. Then each mask pattern, lower-cased with
`/` separators and sorted: `M`, the pattern and a NUL. Owners, archives,
the label and the table's order are not hashed.

`mutator_digest` hashes `oa-mutators-1` and a line feed, the canonical
loadout and the layer digest. `fold_sim_hash` returns the simulation hash
unchanged when no mutator digest is given. Otherwise it hashes
`oa-sim-hash-mutators-1` and a line feed, the simulation hash and the
mutator digest.

`option_text` writes one option's value on its own: a toggle is `true`
or `false`, anything holding a number is the number's canonical text (the
digits the canonical loadout writes), and a string choice is its text.
`options_text` joins `name=value` pairs with `,`, names in byte order,
each value `option_text` of the option. No options write nothing. This is
the text that follows `id:` on `--mutator`.

The match start fills this record from the mutators that play and folds
the mutator digest into the simulation hash. Nothing in this header runs
during a match.

`data-mutators-loadout` pins the canonical text, the four digests against
values computed outside the engine, the refusals, the folder and layer
digests, the unchanged simulation hash and the options text. It reads
records back equal (a `1.0` number default, a numeric choice and a string
choice among them), compares records by their text, digests a folder
holding a name outside ASCII, and refuses a pipe where the system has
pipes.

## Rules in Lua

`install_rule_forms` publishes the rule forms in a sandbox, and
`read_plan_rules` reads a plan's rules list into the records the mutation
pass applies. A list is a sequence of finished blocks and `sm` paths.
Consecutive blocks become one source, named with the entry the caller
gives. Each `sm` path is its own source, named with that path after `\`
is read as `/`. The bytes come only from the reader the caller passes.
A path that is absolute, has a drive, or has a `..` segment is outside
the package, and the reader is not called. A file that cannot be read,
or that the rule parser reports an error in, refuses the plan. The
message names the path, and a parse error names the line as well.
Warnings stay on the result and the source is still kept.

Needs are the same forms the collections use (`need_from_lua`). Changes
are the forms a rule file writes. `where` and `:change` check their
table when they are called and keep their own copy of it, so a loop that
reuses one table builds a different block each time, and changing the
table later changes no block already built. Field names in one block are
stored in the order of those names after ASCII lower-casing, with the
raw bytes breaking a tie, so the records do not follow Lua's iteration
order. Two changes to one field are two blocks, because one table cannot
hold the same key twice. A block's line is the line the host reports for
the `where`, `all` or `named` call, or for `on` when `on` is what starts
the block. Calling `on` on a block that already has a selector keeps
that line and only replaces the folder and extension. A later `where`,
`all` or `named` on a block that already has a selector is refused, and
the message names the line of that call. An unfinished block refuses the
plan, and the message names the call that started it and that call's
line.

The blocks of every source together stay within the rule file's block
limit, and the plan holds at most `max_plan_sources` sources. Passing
either limit refuses the plan. A path is read and parsed once, however
many times the list names it. The statement and text limits are the
rule file's limits. `empty` assigns empty text, the record a rule file
writes with `field=;`. `""`, `set("")` and `remove()` remove the field.

`on` takes a folder and an extension together. It stores both in lower
case, turns `\` into `/`, trims spaces, and strips one leading `.` from
the extension. `on` by itself selects every record of that target. The
default target, when a block never calls `on`, is the units folder with
the `fbi` extension. `:change` finishes a block, and a second `:change`
is an error. An unfinished block in the list refuses the plan.

| Lua | Rule file |
|---|---|
| `where { field = n }` | `field=n,n` in `[Needs]` |
| `where { field = true }` | `field=1,1` |
| `where { field = false }` | `field=!1,1` |
| `where { field = "text" }` | `field="text"` |
| `where { field = range(lo, hi) }` | `field=lo,hi` |
| `where { field = is("text") }` | `field="text"` |
| `where { field = has("text") }` | `field="text"*` |
| `where { field = absent() }` | `field=!""*` |
| `where { field = not_(...) }` | the same need with a leading `!` |
| `where { ... }:change { ... }` | `[*Group*]` with `[Needs]` and `[Changes]` |
| `all():change { ... }` | `[*All*]` |
| `named("NAME"):change { ... }` | `[NAME]` |
| `on(folder, extension):change { ... }` | `folder=` and `extension=`, then `[*All*]` |
| `on(folder, extension):all():change { ... }` | the same folder and extension, then `[*All*]` |
| `on(folder, extension):named("NAME"):change { ... }` | the same folder and extension, then `[NAME]` |
| `on(folder, extension):where { ... }:change { ... }` | the same folder and extension, then `[*Group*]` |
| `change { field = n }` or `set(n)` | `field=n` |
| `change { field = "text" }` or `set("text")` | `field="text"` |
| `change { field = "" }` or `set("")` or `remove()` | `field=""` |
| `change { field = empty }` | `field=;` |
| `change { field = true }` | `field=1` |
| `change { field = false }` | `field=0` |
| `change { field = scale(k) }` | `field=*k` |
| `change { field = scale(k, lo, hi) }` | `field=*k(lo,hi)` |
| `change { field = lock(n) }` | `field=^n` |
| `change { field = append("text") }` | `field="text"+` |
| `change { ["<AllDepth2>"] = scale(k) }` | `<AllDepth2>=*k`, in any ASCII case |
| `sm "path"` | that rule file, as its own source |

`data-mutators-rules-lua` checks each Lua fixture against the rule file
beside it, both as records and as the writes those records make on one
Ridge document set.

## Verdicts

A dry run's answer for one list of mutators against one mod is a
`Verdict`: `compatible`, `no_effect`, `incompatible` or `failed`. The
file and the log spell `no_effect` as `no-effect`. A compatible verdict
carries `ChangeCounts`, twelve counts per collection (units, weapons and
features changed, added and removed, then menu entries, files added and
files hidden). `any` says whether one of them is above zero. `incompatible`
and `failed` carry a reason; the other two do not. `mod_label` is the
mod's name and version, empty for the base game. `one_line_reason` turns
a reason or a label into one line of at most 200 bytes, with the ends
trimmed again when the cut lands on a space.

A verdict is remembered under a `VerdictKey`: the loadout digest of the
whole checked list (prerequisites included), the mod key, and the engine
version text. `RememberedVerdict` pairs that key with a `VerdictRecord`.

The file is named `mutator-verdicts.tsv`. Each line is UTF-8, ends with
a line feed, and is tab-separated: `1`, the loadout digest as 64
lower-case hex digits, the mod key likewise, the engine version
(printable ASCII, no space, 1 to 64 bytes), the verdict word, the twelve
counts as unsigned decimals without leading zeros joined by `,`, the mod
label, the reason. The label and the reason are written through
`one_line_reason`. A line is dropped when it has another field count,
another form than `1`, a digest that is not 64 lower-case hex digits, a
bad engine version, an unknown verdict word, a count that is not a
decimal within `uint32_t`, a reason on a compatible or no-effect verdict,
more than 2048 bytes, a carriage return, or bytes that are not UTF-8.
When two lines share a key, the later record wins and keeps the earlier
line's place. `read_verdicts` then keeps at most the newest 1024 of those
verdicts; the oldest past that are dropped. The file is at most 4 MiB. A
longer span reads as empty. `remember_verdict` drops every excess oldest
verdict in one step.

`read_verdicts` and `write_verdicts` are the codec. `remember_verdict`
replaces a key and moves it to the newest place. `find_verdict` looks a
key up. The dry run fills the records. The Library and the picker show
them. Nothing here decides which outcome is which verdict, and nothing
here opens the file.

## Pipeline

`run_pipeline` takes an asset store and an ordered list of mutators and
applies them in one fixed order: each `mutate(ctx)` for its plan, then the
files layer, then every document the match will load, then one rules pass,
then each mutator's hooks in list order. A hook is a transaction. A mutator
that fails is dropped, with every mutator that requires it, and the run is
repeated, so a failure leaves no file, rule or note. Only a document the
run changed is written back. The result names each mutator's outcome and
holds the journal and the conflict report. The store must hold no mutator
layer when the call starts. On return the final layer is mounted when it
holds at least one entry or mask, and the caller unmounts it.

## Entry points

- `run_pipeline` runs one loadout and returns the outcomes, the layer, the
  journal and the conflict report. It prints nothing.
- `Journal` records the changes that were kept, in phase order, and
  `entries_of` returns one mutator's entries.
- `ConflictReport` holds the stacking notes and the paths a parse left out.
  `describe` writes one note as a single English line.

## State

None kept between calls. The pipeline mounts the layer on the store it is
given and keeps nothing after it returns.

## Invariants

- The same input produces the same layer bytes, journal and report.
- A mutator that fails leaves no trace: the result equals the run without
  it and without every mutator that requires it.
- A document that did not change is never written. A changed document is
  written with the text writer, at the path the store listed.
- Nothing of a mutator runs after `run_pipeline` returns. The scripts stay
  inside the sandbox they were given.

## Tests

`data-mutators-pipeline` (`tests/pipeline_test.cpp`) runs the made-up Ridge
packages under `tests/data/mutators`: rules, hooks, a failed mutator, the
budgets, files added and hidden, and the list checks. `data-mutators-journal`
checks the entry order, `entries_of`, each note conversion and the sentences
`describe` writes.

## Limitations

The plan keys `menus`, `sides`, `tables`, `maps` and `hacks` are refused
until their tickets add them: menus with D06, sides and tables with D04,
maps with D07 and hacks with D08. The integrity check after the last hook
arrives with V04. Until then a plan that names one of those keys fails
that mutator instead of ignoring the key.
