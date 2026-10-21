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
`oa/data/mutators/choice.hpp`, `oa/data/mutators/clashes.hpp`,
`oa/data/mutators/collections.hpp`, `oa/data/mutators/files_layer.hpp`,
`oa/data/mutators/hacks.hpp`,
`oa/data/mutators/journal.hpp`, `oa/data/mutators/loadout.hpp`,
`oa/data/mutators/map_rules.hpp`, `oa/data/mutators/maps.hpp`,
`oa/data/mutators/menus.hpp`,
`oa/data/mutators/pipeline.hpp`, `oa/data/mutators/rules_lua.hpp`,
`oa/data/mutators/verdicts.hpp` and `oa/data/mutators/weapon_ids.hpp`.

- `check_mutable_path` judges one file path.
- `check_mutable_mask` judges one mask. The pattern's shape is
  `mutator_mask_problem` from `oa/formats/hpi.hpp`. A wildcard is allowed
  only in a folder the table allows as a whole.
- `rule_name` returns the stable lower-case name of a `PathRule`.
- `build_files_layer` turns an ordered list of files plans into one
  `MutatorLayerSpec`. See [Files layer](#files-layer).
- `Menus` is the build menus a hook reads and writes. See [Menus](#menus).
- `read_hacks` reads a plan's `hacks` table, `merge_hacks` keeps the later
  value for each path, and `developer_conflicts` notes where those hacks
  overlap Developer Mode or a mod that leaves the entry off. `hack_faults`
  names the mutators a refused resolution of those hacks fails: each one
  an error names, or every one when no error names any.
- `run_pipeline` runs an ordered list of mutators against an asset store
  and returns each mutator's outcome, the layer it left mounted, the
  `Journal` of the changes kept and the `ConflictReport`. See
  [Pipeline](#pipeline).
- `effective_clash` is the mode in force: the player's choice, else the
  manifest's `clashes`, else the preference `mutators.clash-default`.
  `clash_from_setting` reads that preference. `private_record_name` is
  the private name an added record takes. `records_identical` says two
  records can be shared.
- `list_refusal` is that list's check: more than `max_mutators`, an id
  twice, a requirement an earlier mutator does not meet, or two that
  conflict. Empty when the list may run.
- `Journal::entries_of` returns one mutator's changes, and `describe` writes
  one conflict as a single English line.
- `units:clone(source, name, fields)` and `weapons:clone(source, name, fields)`
  add a copy of one record. A unit clone keeps the source model by writing
  `Objectname` and serves the source script, picture and build pages under
  the new name. A weapon clone asks for the lowest free id from 1 to 255.
  `assign_weapon_ids` gives that id when it is still free. Each mutator is
  numbered in list order: its requested ids, then ids for its other weapons.
  A later request for an id an earlier weapon already holds takes another
  free one.
- `record_file_kind` says whether a path is a unit FBI, a weapon TDF, a
  download TDF, or a whole file. `build_files_layer` keeps every kept
  mutator's copy of a record file in `FilesLayer::records`, and each file
  a later mutator's file replaced in `FilesLayer::displaced`, which
  `displaced_bytes` reads.
- A hook's `ctx.sides` is the side table and the build lists. `ctx.sides[1]`
  is the first side, `ctx.sides.ARM` finds a side by name, ignoring case,
  and `ctx.sides:each` walks the sides in that order. `canbuild`,
  `set_canbuild`, `add_canbuild` and `remove_canbuild` are methods of the
  collection.
- A hook's `ctx.tables` opens a `.tdf` directly in the layout's `gamedata`
  folder, other than `sidedata.tdf`. A bare name such as `los` is that
  file. Variant folders are not opened. Any other request is refused, and
  the message names the request. `ctx.tables["los"]` and
  `ctx.tables["gamedata/los.tdf"]` are the same table. `sections` lists
  section names in file order. Walks of sides and of these tables are
  charged against the hook's instruction allowance.
- `parse_choice` reads one `--mutator` value, `format_choice` writes it
  back, and `resolve_choice` finds the package and types its options. See
  [Choice text](#choice-text).
- `loadout_of` builds a loadout from a resolved list, `applied_loadout`
  keeps the mutators a run applied, and `resolve_loadout` finds the
  installed packages a loadout names. See [Loadouts](#loadouts).

## Choice text

An id holds no `/` and no `\`. Anything else is a path to a package folder.
On every platform, a text that starts with a drive letter, `:` and `/` or
`\`, as `C:\Mods\pack:factor=4` does, keeps that `:` in the path. A target
that holds any other `:` is a path only when it names an existing folder or
file; otherwise the text is refused. Options follow the first `:` after any
drive that is followed by a name and `=`, and every name is an option id
and every value an identifier. The text is not trimmed. An empty text, an
empty name or value, a repeated option and a `,` or `=` inside a value are
refused.

`resolve_choice` looks an id up in the installed listing. A path is the
package folder itself, and `oamut.yaml` is matched without ASCII case. A
symbolic link is not followed. Defaults are filled, and the named options
are written over them. A missing mutator, an option the manifest does not
have and a value of the wrong kind or outside the option's choices are
refused in words that quote the player's text.

`package` is the package file's SHA-256 from the origin record when that
record names 64 hexadecimal digits, and `folder` stays clear. When the
record names no SHA-256, `package` is `folder_digest` of the installed
folder and `folder` is set. A recorded value that is not 64 hexadecimal
digits is refused. A path sets `folder` and keeps `folder_digest` of that
folder.

## State

None kept. `parse_choice` asks whether a target that holds a `:` other than
a drive's names an existing folder or file, and keeps nothing.
`format_choice` reads nothing. `resolve_choice` reads one package's
`oamut.yaml`, or the installed listing of the Mutators folder it is given.
For a path, and for an id whose origin record names no SHA-256, it also
reads the files of that folder that `folder_digest` counts. It
keeps nothing after it returns. `resolve_loadout` reads the installed
listing, each package's manifest and, for a folder entry, those same files
of the installed folder. `loadout_of` and `applied_loadout` read nothing.
The two checks are pure: they do not read files, the asset store or the
preferences. A null `PathContext::layout` means `data_layout()`. An empty
`is_unit_name` knows no unit. `build_files_layer` reads the package
folders it is given while it builds the layer, and it keeps nothing after
it returns. `folder_digest` reads one folder and does not write.
`run_pipeline` keeps nothing between calls:
it reads the packages and the store it is given, and mounts its layer there.

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
- Side fields are read from the language variant of `sidedata.tdf` when
  that file is present, because that is the file the side loader reads.
  Build lists are read from the plain `gamedata/sidedata.tdf`, because
  that loader passes no variant. A change is written back to the file its
  reader opens.
- A build list is written back as `canbuild1` through `canbuildN` with no
  gap. A list the builder did not have is created under the one
  `[CANBUILD]` section.
- A protected game-data file is refused with the allow-list's own message.
  `sidedata` is not opened here: the error is `use ctx.sides`. A table
  request that is not a `.tdf` directly in `gamedata` is refused with the
  request named in the message. `gamedata-<variant>/` is not a table path.

## Tests

`data-mutators-allow-list` (`tests/allow_list_test.cpp`) checks each
allowed and refused form, including mixed case, backslashes and a variant
folder; a layout that renames directories; every invalid path; the masks;
and that every message starts with the path and every rule has its own name.

`data-mutators-clone` (`tests/clone_test.cpp`) runs made-up packages
through `run_pipeline` over a scratch store: a unit's copied definition,
the files the layer serves under the new name, a weapon's free id, and a
clone that a later error rolls back. A loop of clones, a loop of clones
whose source script is 1 MiB, and a `pcall` loop of weapon clones that are
refused, each stop on the work budget, not on the long real-time limit the
cases give it, within 60 s of processor time. A clone whose build page
collides with an existing screen is refused and the layer serves nothing.

`data-mutators-choice` (`tests/choice_test.cpp`) checks the choice text,
including drive paths, which read the same on every platform, the round
trip of `format_choice`, and `resolve_choice` for each option kind, against
an installed mutator and against a package folder, by absolute and by
relative path.

`data-mutators-pipeline` (`tests/pipeline_test.cpp`) runs the made-up
Ridge packages under `tests/data/mutators` through `run_pipeline`: rules in
either order, a later assignment replacing an earlier one, a rule that
reads a hook's write from the mutator above it, a hook that replaces a
rule's value, a lock that holds against a later mutator's rules but not
its hook, hooks in list order, each kind of failure, and a run with a
failing mutator against the run of the mutators that remain, whether its
hook, its rules or its files would have reached the mutators below it, and
with the mutators that require it. It also covers plans whose tables have
metatables, the allowances and the string cap, files added, masked and
hidden with their journal entries, a document that does not parse, a
player's option, the list checks and two identical runs. A hack the
check refuses, with a units hook beside it, fails in the plan phase and
leaves no hook change and no journal entry.
It also runs two
mutators that share `weapons/extra.tdf`, a unit file that replaces a unit,
a weapon body that must not move a base id, download sections matched by
builder and unit, a rule that sees the unit its own file added, and a weapon table
with no id left. It also checks a rule and a hook that try to renumber a
base weapon, a weapon that mutator brought whose rule sets its id, a
request list that overflows in either order, a record file that brings a
removed document back, and the journal entry and note when one record
file replaces another's. It also runs two mutators that bring one weapon
name under Add and under Replace, an identical weapon that is shared, a
renamed unit whose menu button, side list and clone follow the private
name, two download entries that share a section name and name different
units, and the clash mode taken from the player, the manifest and the
setting, in that order. In Add mode a menus hook that names a builder or
a unit its mutator renamed reaches the private record, in one call or
two, beside the game's button for that unit, and its `remove` leaves the
game's buttons; a renamed unit's own script leaves the asked path to the
earlier mutator's patch there, and a renamed unit with no script takes an
earlier patch or the game's file, never a later patch. A Replace clone
serves the source's script and picture under the replaced name, and the
replacement notes name the earlier mutator's clone or file, the game or
the mod.

`data-mutators-clashes` (`tests/clashes_test.cpp`) checks that order on
`effective_clash`, the preference text, the private-name rule and that
two weapon sections differing only in `ID` are the same weapon.

`data-mutators-weapon-ids` (`tests/weapon_ids_test.cpp`) checks
`assign_weapon_ids`: a requested id is kept when it is free, each mutator
takes its free requests and then ids for its other weapons before the next
mutator is numbered, a later request for a taken id takes another free id,
a missing id or an id outside 1 to 255 is not a request, slot 0 is never
given out, a full table refuses the mutator that overflows and writes
nothing, and a base weapon's id text stays. Two mutators that both ask
for ids are numbered in that same list order, so when the later one's
requests are what overflow, that later mutator is the one refused.

`data-mutators-journal` (`tests/journal_test.cpp`) checks the entry order,
`entries_of`, each note conversion and the sentences `describe` writes.
`data-mutators-hacks` (`tests/hacks_test.cpp`) checks reading a hacks table,
the value's normal form, merging two mutators, the Developer Mode notes,
and the values the resolver plays.
`data-mutators-menus` (`tests/menus_test.cpp`, timeout 60 seconds) checks
adding, moving, greying, hiding and removing build buttons, the conflict
notes, and the downloadable mark through `load_download_menu` and
`OA_UNIT_DEF_FLAG_DOWNLOADABLE`. It also runs a greyed page through
`update_build_button_validity`. It checks that a slot an earlier mutator
emptied, by `menus:remove` or by removing the unit, is taken exactly by a
later one, and that the same mutator's menus hook and `ctx.menus` in its
units hook see a slot its units hook freed. A rule's change to a download
entry stays when a later mutator removes another entry of that file, and
a slot the rule freed is free. A unit and a builder a hook cloned can be
put in a menu from the same hook, the same mutator's menus hook and a
later mutator, and a cloned builder's served page keeps its slots taken
and is greyed in place. Removing an entry from the six-section
`marked.tdf`, and from a six-section file whose sixth entry is a unit the
builder does not offer, keeps the sixth section unread. A created file is
journaled as a file added. A file created and then emptied in one mutator
is absent from the journal and the layer, and the next entry uses the
same path. Removing the first entry of `marked.tdf` leaves a placeholder
and a note that names no unit. A loop of refused calls under `pcall` leaves
nothing behind, and a loop of `builds` and one of `remove` stop on the
instruction allowance at a small part of the real-time limit's processor
time. `data-mutators-menus-data` is the same program with `--data`
(timeout 180 seconds): it reads the installed game and checks
`menus:builds("ARMCOM")`, a grey of `ARMALAB` on `ARMCK`, that a
builds-only hook serves nothing, and that a loop of `builds("ARMCOM")`
and one of `remove("ARMSOLAR")` stop on the allowance.

`data-mutators-sides` (`tests/collections_sides_test.cpp`) runs made-up
Ridge packages through `run_pipeline` and checks side fields, build lists,
the language variant against the plain file, the other game-data tables,
a failed mutator and the layer grants. `data-mutators-sides-data` is the
same program with `--data`: it reads every side and every build list of
the installed game and serves no game-data file.

`data-mutators-maps` (`tests/maps_test.cpp`) runs maps hooks over a
made-up Ridge scenario: header and schema writes and their refusals,
placed units and features, `glob` and `pattern`, two mutators on one
field, a write that then errors, and the session loader reading the
served text. It also runs refused writes in a `pcall` loop, loops of
long-key reads, writes, `:set` calls and unit lookups that stop on a
small instruction allowance under a long real-time limit, within 20 s of
processor time, a write count past the limit, and placed
names that an earlier hook cloned or removed. `data-mutators-maps-data`
is the same program with `--data`: it reads every installed map and
writes nothing, then points every map whose schemas name a profile at
another profile. Each rewrite must be served, and each map that loads as
a skirmish session must read its rewrite back through the session loader.

`data-mutators-map-rules` (`tests/map_rules_test.cpp`) applies rule
files that name `maps/*.ota` to one made-up map. It checks the split, the
combination, the schema copy of a header `SurfaceMetal`, a skipped
`MohoMetal`, a maps hook that reads the rules' result, a dry run with no
map, a refused `.tnt` target and a missing `maps` layer.

## Limitations

The checks do not look at a mutator's files and do not apply a verdict.
A build page is not a file a mutator may replace. Buttons are added, moved,
greyed, hidden and removed through the menus hook. An authored page's
button cannot be moved: grey or hide it and add an entry. A download button
whose unit has no `anims/<unit>_gadget.gaf` is added with no picture, and
the run notes that. A unit clone may serve `guis/<new><digits>.gui` when that
path is not already in the store or the layer; every path a clone serves
still goes through `check_mutable_path`, and the new name counts as a unit
for `anims/`. A clone is refused, naming the path, when the script, the
picture, a build page or that page's art already exists. Only a named
unit's build picture and its download-button art are files a mutator may
add under `anims/`, and only as those files. `sounds/` stays refused until
a later ticket shows the game reads it again for every match, or changes
the game so that it does.

A clone does not copy a download entry, and it does not copy
download-linked button art (`anims/<unit>_gadget.gaf`). A download-linked
button for a clone draws with no picture. A weapon clone never takes slot
0, which is the default weapon. A unit or weapon whose name already
exists follows that mutator's clash mode. The player's choice wins, then
the manifest's `clashes`, then the preference `mutators.clash-default`
(`add` or `replace`; any other text, and a missing key, read as
`replace`). `Runtime::apply_match_mutators` reads the preference and
records the mode in force on the match loadout. A guest that resolves
that loadout plays the recorded mode, not its own preference.
Replace puts the new record in place of the old one, by a record file or
by a clone onto that name. The clone keeps the existing file, takes the
source's model, and serves the source's script, build picture, build
pages and page art under that name in place of its own, so the script
always matches the model. A replaced unit's own build pages past the
source's last page are still served. Add shares a record only when
it is the same after this mutator's weapon names have been rewritten, and
stores a different one under a private name: the name, `_`, and the first
free number from 2, shortened so the name stays within 31 characters and
holds no `/`, `\`, `:` or `.`. A unit that names a weapon this mutator
renamed is a different unit, so it is renamed too. The private name is
not shown to the player, and the unit's display name (`Name`) is
unchanged. In Add mode a mutator's own name for a record it renamed means
its private record: `units.NAME` and `weapons.NAME`, a rule's `is("NAME")`
on `unitname` or on a unit or weapon section, a clone's source, that
mutator's own rule writes, a renamed builder's `UNITMENU`, and that
builder's `canbuild` block. A later mutator's use of the original name is
left alone. The renamed unit's own files are served under the private
name, and the unit already in the game keeps its files, including an
earlier mutator's patch of them. A later mutator's patch of them does
not reach the private unit. The paths the
engine looks up by the unit name are `scripts/<NAME>.COB`,
`unitpics/<NAME>.PCX`, `objects3d/<NAME>.3DO` when the model is the unit
name, `anims/<NAME><n>.GAF`, `anims/<NAME>_gadget.GAF` and
`guis/<NAME><page>.GUI`. A replacement note names whose record it was.
A record an earlier mutator brought, by a file or by a clone, is that
mutator's: "ridge-b replaced ridge-a's RGBOT on units/rgbot.fbi". A
record no mutator brought reads "ridge-a replaced RGBOT from the game on
units/rgbot.fbi", or "from the mod" when the run plays a mod and the
mod's folder serves the record's file: the first of the store's folders
when it layers several, and the whole store when it has one. A run plays
a mod when its mod id is neither the base game's nor its game id. A shared
record is journaled, and the match log does not count it as a file added.
A replaced base or mod weapon keeps the id it had. A rule or a hook
that writes `id` on a weapon the game already numbered does not change
it, and the note names the mutator. A write of `id` on a weapon that
mutator brought is that weapon's request. Two mutators never share an
id. Numbering takes each mutator in list order, its requests and then
its other weapons, so an earlier mutator is not refused for a later
mutator's requests. When no id from 1 to 255 is free, the mutator whose
weapon could not be numbered is refused and named, and nothing of that
mutator is served.

A download entry is its builder (`UNITMENU`) and its unit (`UNITNAME`),
not its section name. Two mutators that add the same unit to one
builder's menu replace that button, and the section name already stored
stays. A new button takes a private section name only when the
destination file already has that section. The same name in another
download file is not a clash. The loader reads five sections of a file.
A new button past that cap is written to a new download file. When the
unit is not already downloadable, that file starts with a `PLACEHOLDER`
section whose `UNITMENU` is empty. A file opened for that unit is reused
while it still has fewer than five sections. The unit is the private name
when this mutator renamed it, so the new button does not replace the
original unit's button. The menus hook maps the builder and the unit a
script names in `menus:add`, `move`, `remove`, `grey`, `hide` and
`builds` through the calling mutator's renames, so in Add mode a name it
renamed means its private record: `menus:add("RGLAB", "RGNEW")` adds to
its `RGLAB_2`, `menus:add("RGLAB", "RGBOT")` gives its `RGBOT_2` a button
even when `RGLAB` already offers the game's `RGBOT`, and
`menus:remove("RGBOT")` takes away only `RGBOT_2`'s buttons. Names read
from the documents are never mapped: `menus:builds("RGLAB")` still reports
the game's `RGBOT` as `RGBOT`. These edits do not pass through the hook
write list, so the retarget does not see them, and they keep the
placeholder and the five-section cap. A mutator's renames last for its
own turn. A later mutator's names, and every maps hook's, mean the
records of those names.

A text that does not parse is unread. A unit, weapon or download file
that does not parse is left unread the same way: the path is warned once,
the mutator still applies, and that file is not served. A base weapon
file the pipeline cannot parse is unread too. The game may still load it,
but its ids are not counted as taken, so a mutator weapon could share an
id with one of them.

A maps hook cannot add a placed unit or a placed feature, and it cannot
move a start position or change how many there are. It writes only the
map: a write through a units, weapons, features, sides, tables or menus
collection the script kept from an earlier hook is refused as if the
mutator did not declare that layer. `type`, `missionname`,
`planet` and `numplayers` stay as the map file wrote them. `glob` is `*`
and `?` only, with no character classes and no full regular expressions.
`pattern` is a Lua pattern. A placed unit's `xpos` and `zpos` are whole
numbers from 0 to 32,767, and its `player` is a whole number from 1 to 255.

A mutator cannot add or remove a side. Rectangle sub-sections of a side
are neither shown nor changed. A build list keeps at most 1024 names. When
the written list is longer than 30, the journal notes that the AI keeps
only the profile's `limits.build-list-entries` count (30 by default).

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

A unit FBI, a weapon TDF and a download TDF are record files: one level
under that layout folder, the unit extension or `.tdf`. The spec still
holds one host file per path, so a later mutator wins the path for a
whole-file mount and an `add_replaced` note is still recorded. Each kept
mutator's bytes are also stored in `records`, including the copy a later
mutator replaced in the spec. A mask that hides the path drops those
snapshots. A later folder of the same mutator replaces only its own
snapshot. The pipeline does not mount those spec entries. It reads each
mutator's snapshot into that mutator's step. Every other file stays one
whole file, and the later mutator wins the path. The file it replaced, of
any kind, is kept in `displaced` with its mutator, in the order that
happened, so the files that held one path read from the first to the one
the spec serves. A mask that hides the path drops them, and a mutator
whose later folder replaces its own file displaces nothing.

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
checks the order, the notes, the failures, the displaced files, from a
loose file and from an archive, and a spec mounted on an asset store. A link case is skipped where the system cannot make a link.

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
`units:clone` and `weapons:clone` add a record to that collection at once.
The new unit's file is `units/<name>.<extension>` with `UnitName` set to
the new name. `Objectname` is the source's model, or the source's own
name when the source leaves the model out, so the clone shares that model
instead of shipping another one. The layer serves `scripts/<name>.COB`,
`unitpics/<name>.PCX`, the build pages `guis/<name><n>.GUI` from page 0
and then 1, 2, … until one is missing, and `anims/<name><n>.GAF` for each
page it served, each with the source's bytes. A field in the clone's
table is a typed write, in sorted key order, and a bad field adds nothing.
A weapon clone copies the source section, including `[DAMAGE]`, into
`Weapons/<name>.tdf` (or `Weapons/<name>-<k>.tdf` when that path is taken,
`k` from 2) and writes `ID` to the lowest free id from 1 to 255. That id
is a request. After the last hook the pipeline keeps it when it is still
free, and otherwise gives the weapon another free id. The journal line
names the id the weapon was given. When several
sections share a name, the copy is the first in collection order (name,
then path, then position), which may not be the lowest slot the game fires.
A field named `ID` is refused. The journal records each clone and each
served copy. A unit clone is refused, naming the path, when any path it
would serve already exists. While clash mode is active, a clone onto a
name that exists follows that mode. Replace puts the clone in place of
the existing record and journals a replacement. The existing file stays,
and the unit takes the source's model, script, build picture, build pages
and page art, served under the existing name in place of its own as for a
clone under a new name, and charged the same way. Add stores the clone
under a private name, notes the rename, and charges every candidate
lookup, including the path check, against the hook's allowance. While
clash mode is not active, that clone is still refused with "the new name
already exists". In Add mode, `units.NAME`, `weapons.NAME` and a clone's
source name a record this mutator renamed as the private record. A
clone's work (its name lookups, the bytes it reads and writes and, for a
weapon, each section the id search visits) is charged in one step when
the clone returns, so a refused clone pays too.

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
`set`, `keys` and `fixed`. Any other name is a field. A field whose name
is one of those five is reached only with `get` and `set`. A weapon's
`damage` is its `[DAMAGE]` block, created on the first write into it.
`get("damage")`
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

`record:fixed(key)` reads that field as a fixed value. Text that is present
is read the way `fixed.parse` reads it. A key the record leaves out uses
the loader's default, so an absent `bankscale` is 1 and an absent
`moverate1` is twice `maxvelocity`. A key the schema does not hold, and a
key a feature leaves out, is nil. A weapon's `damage` block has the method
too. Storing a fixed value, by assignment, by `:set` or by a set's
`:set { }`, is checked as the number `count / 65536` and written as the
text `f:text()` gives. A field that is itself named `fixed` stays reachable
with `get` and `set`. Assigning the method name is refused.

`scale` multiplies each field that is present and numeric. An absent
field is left absent. Text that is not a number is left as it is and
noted. `set` and `scale` visit the table's fields in the script host's
`pairs` order, and the records of one field in collection order.

`remove` on a unit marks that unit's document removed and drops every
section in the layout's download folder whose `UNITNAME` or `UNITMENU`
names the unit, ignoring ASCII case. A download file left with no section
is marked removed. When the section removed was the file's first and
another section remains, a note names the unit that file now marks.
When the build menus then leave a placeholder in that first place, the
note says the placeholder holds it and no unit gains a mark.
Pages under `guis/` are left as they are. The menus collection greys those
buttons; `remove` does not. `remove` on a weapon or a
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
name uses an index built once for each document. The build walks that
document's sections once and records each lower-cased name in a map, so
it stays linear however many sections share a name. A section added later
is indexed from where the last build stopped, and a removal drops that
section and shifts the later ones down. It does not build the document
again. Writing `unitname` moves only that unit's entry. A field is
resolved through the section index stored on the handle, not by scanning
the document. Each entry carries its section's handle slot, so a walk or
a lookup costs the same however many records were handed out before it.
`keys()` lists the spellings that are present, ordered with `name_less`.
Nothing in this walk follows Lua
table order, hash order or pointer order.

A walk of a script table, of records or of keys, a scan of a name or a
path, building a document's record index, and the field text a read, a
write or a `where` comparison touches, count against the phase's
instruction allowance through the script host's `charge_work`. A
key-collecting walk costs 4096 units. Each section indexed costs one
unit, and a byte length costs one unit per 32 bytes. A call that stays
inside the allowance returns the same records and raises the same errors.

`writes()` is every successful field write, in the order it happened:
the mutator, the path, the record, the block (`""` or `DAMAGE`), the key
as stored, and the old and new text. A later mutator that writes a field
an earlier mutator wrote in this run adds a `hook_overwrote` note naming
both; the later value is the one kept. `removals()` lists each unit,
weapon, feature, download entry and document that a removal took out.
`notes()` lists a check warning, an overwrite, a download file whose
first entry changed, and a scale that skipped a value that is not a
number, and the build menus' notes: a moved entry, a button with no
picture, and an entry the AI's list cannot hold.
One mutator may journal at most 65536 field writes and 65536
notes. The write tally is one number for that mutator: the units,
weapons, features, sides, tables and menus hooks share it with the maps
phase. Each field a clone's table sets counts on it; the copy itself does
not. Passing the write limit fails that mutator even when the script
catches the error and returns normally. Passing the note limit fails the
call. The messages are "this mutator has changed too many fields" and "this
mutator has made too many notes".

`data-mutators-collections` builds a small Ridge document set and runs
the fixtures in `tests/collections`, including the `fixed_` scripts.
`data-mutators-charge` checks that a loop of collection walks stops on a
small instruction allowance, and that one `where` and one `pairs` over two
Ridge weapons stays inside a bound and spends the same count on two runs.

## Menus

`Menus` is the build menus of one document set. A hook receives it as
`ctx.menus` on every hook, and a plan may return a `menus` function that
is called with the collection and the context after the units hook and the
weapons hook. The menus share the collections' documents, units index and
journal. `begin_mutator` and `end_mutator` bracket one mutator's hooks
inside the collections' own turn, and say whether the manifest declares
the `menus` layer. Writes need that layer. `builds` does not.
`added_files` lists the download files the menus created, each with the
mutator whose request made it; the run journals each one as a file added.

Every name is matched without ASCII case and must be a unit the units
collection holds when the call runs: a unit or a builder a hook cloned is
there, and a unit a hook removed is not.

- `menus:add(builder, unit)`,
  `menus:add(builder, unit, { page = p, slot = s })` and
  `menus:add(builder, unit, { page = "new" })` add a download entry and
  return `{ page = p, slot = s }`, where the entry landed. With no
  place it takes the first free slot. With `"new"` it takes slot 1 of the
  page after the builder's last page. A unit the builder already has a
  button or an entry for is not added twice: the call changes nothing and
  returns that place.
- `menus:move(builder, unit, { page = p, slot = s })` moves that builder's
  entry for the unit and returns `{ page = p, slot = s }`, where it landed.
- `menus:remove(unit)` drops the download entries that name the unit, on
  every builder, and greys its buttons on every builder's pages.
  `menus:remove(builder, unit)` does the same for one builder. Both return
  nothing, and a unit the builder does not offer is not an error.
- `menus:grey(builder, unit)` and `menus:hide(builder, unit)` grey or hide
  each button of that unit on the builder's authored pages, and return
  nothing.
- `menus:builds(builder)` returns a new array of unit names, each once:
  the authored unit buttons that are neither greyed nor hidden, in page
  and button order, then the builder's download entries in page and slot
  order, leaving out a unit the units collection no longer holds.

A call is refused with a Lua error, and changes nothing, when:

- a page is not 1 to 7 ("page must be from 1 to 7") or a slot is not 1 to
  6 ("slot must be from 1 to 6"). `move` takes no `"new"`;
- the builder has no free slot left: "B has no free build slot on pages 1
  to 7";
- `move` names a button of an authored page: "U's button is part of B's
  page p; grey or hide it and add an entry instead";
- `grey` or `hide` names a unit the builder offers only by a download
  entry: "U is a download entry on B; use menus:remove". When the builder
  offers the unit nowhere, `grey`, `hide` and `move` give "B does not
  build U";
- a name is not a unit, "unknown unit X", or not a builder, "unknown
  builder X";
- the mutator writes without declaring the layer: "this mutator does not
  declare the menus layer";
- the arguments are not text or are the wrong number, for example
  "menus:add expects a builder and a unit", or the script reads a method
  the collection does not have, "menus has no X", or assigns one.

A slot is a builder, a page from 1 to 7 and a slot from 1 to 6. The page a
player calls `p` is the file `guis/<unit><p>.GUI` and the download field
`MENU=p+1`. `BUTTON` is the slot minus one. Every slot of an authored page
is taken, because an entry there would replace the page's own button or
an order button. A builder a hook cloned has the pages the clone serves
under its name, so their slots are taken too. An occupied slot is not
overwritten: the later request moves to the next free slot and the report
records "U moved from page p slot s to page p' slot s': taken by <who>".
`<who>` is the mutator that placed the entry there, or the page itself
when an authored page or the mod's own download entry holds the slot.

A slot is decided when the request is made, in list order, not in one
layout step after the last mutator. A later mutator cannot move an earlier
mutator's entry, and an earlier request is not moved into a slot a later
mutator empties: that slot stays free for whoever asks next. Two mutators
that claim one slot, and a slot an earlier mutator emptied, come out the
same as they would in one step at the end.

Every call reads the download files, the units and the build pages as
they stand when it runs. A slot an earlier mutator, the same mutator's
units hook or an earlier call freed is free, and a rule's change to a
download file is what the call sees. An edit changes only the sections
and keys it names, in the shared documents: `add` appends a section to a
file the menus made, `move` writes `MENU` and `BUTTON`, and `remove` takes
out a section. Another mutator's rule or hook writes to the same file
stay. A download entry is the builder and the unit it names; the section
name is only the label written in the file (`MENUENTRY1`, `MENUENTRY2`,
and so on).

A unit stays downloadable exactly when it was before the call, except a
unit a hook removed from the game and a unit whose every download entry
was removed. A new file for a unit that is not downloadable starts with a
`PLACEHOLDER` section whose `UNITMENU` is empty. The loader leaves that
section zeroed, so it marks nobody. The loader reads only the first five
sections of a file, so removing one of those from a longer file puts a
placeholder in its place, and no section past the fifth becomes an entry.
When a file's first section goes, another entry of the same unit in that
file moves first; otherwise a placeholder takes the first place, so no
other unit becomes downloadable. The note on that removal says the
placeholder holds the first place and no unit gains a mark. When that
leaves the unit with entries but no mark, one of its entries moves to
the front of a file again. A file left with no entry is hidden. A file
this mutator created and then emptied is left out of the journal and the
layer, and a later entry for that unit uses the same path. A units
hook's removal drops the removed unit's entries at once; the next menus
call, or the end of the
mutator's hooks, puts the same placeholders where those entries were and
greys the unit's buttons on every builder's pages, journaled to that
mutator.

An authored page stays a file a mutator may not replace. Grey sets
`grayedout=1` and clears bit `0x04` of `commonattribs`, because the build
page's validity pass clears `grayedout` on every button that still has
that bit. Hide sets `active=0`. Neither changes a download entry. A grey
or a hide on a page a clone serves rewrites the copy it serves. A button
with no `anims/<unit>_gadget.gaf` is still added, with a note. An add that
the AI's list cannot hold is still added, with a note that the list is
full.

Each call is charged against the hook's instruction allowance at the
collections' rates: one unit for each document, section, button and entry
it walks, a binary search's depth for each look in the units index, and
the bytes of the names it compares and returns. A callback works into the
menus' own storage and charges before it pushes a result or raises, so no
C++ object is alive across a Lua error. The list of download entries and
the list of builders are rebuilt only when a document, a journaled write
or a removal was added since the last call, or a new mutator's turn
began, so a loop of calls stops on the allowance well inside the
real-time limit.

`data-mutators-menus` is the test named above.

## Loadouts

`loadout.hpp` is the record a game, a recording and a save name their
mutators by, before a match start fills it. A `LoadoutEntry` names one
mutator by id, version and package SHA-256. `clashes` is the mode in
force, `add` or `replace`. `loadout_of` takes the preference's default
and stores that mode: the player's choice, else the manifest, else the
default. `resolve_loadout` copies it onto `ResolvedMutator::clashes`, so
a guest plays the host's mode. The member is empty when the mutator was
named with `--mutator` and the player did not choose. `folder` says the
package was a folder named by path, and then the SHA-256 is that folder's
digest rather than a package file's. `options` is every option of the manifest,
defaults filled by `default_option`, in id byte order. A toggle's value is
`toggle`. A choice keeps the manifest's `ChoiceValue` (a number or a
string). A number option keeps its number and that number's canonical
text.

`canonical_loadout` writes one text through the engine's canonical JSON:
`{"mutators":[...],"oaloadout":1}`, entries in list order. Each entry's
members are `clashes`, `folder`, `id`, `options`, `sha256` and `version`.
`clashes` is `add` or `replace`, the mode in force for that mutator.
`sha256` is
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

`folder_digest` hashes each regular file under the folder that a package
would carry, sorted by its relative path compared bytewise. The path is the
UTF-8 spelling, with `/` between its parts, of the names as the file system
stores them, on every system; there is no Unicode normalisation, so a name
stored composed and the same name stored decomposed give different digests.
Each file is that path, a NUL, its size as eight little-endian bytes, and
the SHA-256 of its bytes. The names left out are the ones `oa-tool mutator
pack` leaves out: `__MACOSX` and `.backup` only when they are the first
part of the path, `.git` and `.oa-origin.yaml` at any depth, and
`.DS_Store` (in any ASCII case) and a name that starts with `._` at any
depth. A folder left out that way is not walked. A name that starts with
`.` and is none of those is still hashed, including `.notes`, `.gitignore`
and `.github`. A symbolic link, anything that is not a regular file or a
folder (a pipe, a socket or a device), or a file that cannot be read
refuses the digest, and the error names the path in UTF-8, even when the
name would otherwise be left out. An empty folder hashes the empty message.

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

`loadout_of` builds this record from a resolved list. Every mutator is
listed, in the list's order, and each option is that mutator's typed
value with its default already filled, stored in id byte order. The
package digest and the folder mark are the ones `resolve_choice` recorded:
an installed id keeps the SHA-256 from its origin record, or, when that
record names none, `folder_digest` of the installed folder with `folder`
set. A package folder named by path keeps `folder_digest` and sets
`folder`. A folder digest that is refused refuses the choice. A recorded
SHA-256 that is not 64 hexadecimal digits refuses the choice. `loadout_of`
reads no file.

`applied_loadout` keeps the entries a pipeline result applied, in the
loadout's order. `resolve_loadout` finds each entry among the installed
mutators by id, with the same version text. A package entry must have the
origin record's SHA-256. A folder entry is accepted when that id's
installed folder has the same folder digest, and refused, naming it as a
package folder, otherwise. The options are typed as `resolve_choice` types
`options_text`. A missing package, another version and another SHA-256
are refused, and the error names the entry.

A playable match that applied at least one mutator keeps that applied
loadout and `mutator_digest`. The layer digest reads each served path's
bytes from the asset store while the layer is mounted. Both are forgotten
when the match releases its mutators, including when the match ends. The
simulation hash the run reports is the profile's sim hash folded with that
digest while it is held, and the profile's hash alone otherwise. Nothing
in this header runs during a match.

`data-mutators-loadout` pins the canonical text, the four digests against
values computed outside the engine, the refusals, the folder and layer
digests, the unchanged simulation hash and the options text. It reads
records back equal (a `1.0` number default, a numeric choice and a string
choice among them), compares records by their text, digests a folder
holding a name outside ASCII, and refuses a pipe where the system has
pipes. It also checks `loadout_of`, `applied_loadout` (applied entries
kept, and failed, excluded and incompatible entries dropped) and
`options_text` read back through `parse_choice` and `resolve_choice`. A
path and an installed folder with no origin record resolve with `folder`
set and that folder's digest. `resolve_loadout` finds those packages, and
refuses a missing id, another version, another SHA-256, a folder digest
that is not the installed folder's, and an origin SHA-256 that is not 64
hexadecimal digits.

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
are the forms a rule file writes. `set`, `lock` and `scale` (a factor, or
a factor and two bounds) take a fixed value as the count divided by
65536. `where` and `:change` check their table when they are called and
keep their own copy of it, so a loop that
reuses one table builds a different block each time, and changing the
table later changes no block already built. Field names in one block are
stored in the order of those names after ASCII lower-casing, with the
raw bytes breaking a tie, so the records do not follow Lua's iteration
order. Two changes to one field are two blocks, because one table cannot
hold the same key twice. Two keys that trim to the same field, such as
`a` and ` a`, are refused, and the message names both spellings. A
`where` or `:change` table costs 64 units plus one unit per entry and one
unit per 32 bytes of each key. Past the entry limit only the entry count
is charged, so the count does not depend on which key is visited first.
The byte scans of names and paths count the same way, through the script
host's `charge_work`. A call that stays inside the allowance builds the
same blocks and raises the same errors. A block's
line is the line the host reports for
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
Ridge document set. `fixed_set` and `fixed_scale` pass fixed values.
`data-mutators-charge` checks that repeating `where` on a 4,000-key table
stops on a small instruction allowance, that a small plan stays inside a
bound and spends the same count on two runs, and that `where` and
`:change` refuse two keys which trim to one field.

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

`run_pipeline` takes an asset store and an ordered list of mutators, and
applies them in one fixed order. The list is checked first: more than 32
mutators, an id listed twice, a requirement that no mutator above it meets,
or two mutators that conflict refuses the whole list, and nothing runs.
Each manifest is then checked against the mod and the engine version. A
mutator that does not fit is incompatible and left out. The rest go through
passes. A pass plans every mutator by calling its `mutate(ctx)` in a
sandbox of its own, builds the files layer and mounts it without the
record files, parses every document the match will load from that store,
then takes each mutator in list order. The mutator applies the weapons
in its own files, then the units, then points those units' weapon fields
at any weapon it renamed, then the download entries. A unit is one FBI,
named by `UnitName` or by the file stem. Each section of a weapon TDF is
a weapon. A download entry is the builder and unit it names. A unit or
weapon whose name already exists follows that mutator's clash mode: the
player's choice, else the manifest's `clashes`, else
`PipelineInput::clash_default`. Replace writes over the existing record,
including a clone onto a name that exists. Add shares a record that
matches after this mutator's weapon names are rewritten, and stores a
different one under a private name, leaving the existing record in place.
A unit that names a renamed weapon is renamed too. A new record is
added. The renamed unit's name-keyed files move with it:
`scripts/<NAME>.COB`, `unitpics/<NAME>.PCX`, `objects3d/<NAME>.3DO` when
the model is the unit name, `anims/<NAME><n>.GAF`,
`anims/<NAME>_gadget.GAF` and `guis/<NAME><page>.GUI`. A file this
mutator brought is served under the private name and leaves the asked
path, so the unit already there keeps what it had: an earlier mutator's
file at that path, which this mutator's had displaced, is served again,
and the note that this mutator won the path is dropped. A file this
mutator did not bring is copied as the path stood at its turn: the latest file a
mutator above it brought, else the game's or the mod's, read beneath the
layer. A later mutator's file at the asked path belongs to the unit
already there and does not reach the private one. In Add mode
this mutator's `units.NAME`, `weapons.NAME`, rule `is("NAME")` selectors,
rule writes, clone sources, `UNITMENU` and `canbuild` builders mean the
private record. A later mutator's use of the original name is left alone.
Other files stay whole files, and the later mutator wins the path.
The mutator then applies its rules and then its hooks, on the documents
the mutators above it left. Its units hook runs, then its weapons hook,
then its menus hook. When the caller names a map, each mutator's maps
entry then runs, in list order, as a transaction of its own, after every
mutator's records, rules and hooks and after the weapons are numbered.
Only a document the pass changed is written back, whole, with the text
writer, at the path the store listed it under. A document a hook removed
is hidden. A scenario is served only when its bytes differ from the map
the caller passed.

Each phase of a mutator's script may run for `PipelineInput::time_limit` of
real time, the script host's `default_time_limit` unless the caller names
another. The plan is read inside the plan phase, with the mutator's
allowances and real-time limit in force, and by raw access only, so no
metamethod of the script runs. The plan, its `files` table, the `mask` and
`add` lists and the `rules` list must be plain tables. One with a metatable
refuses the plan,
because its raw contents need not be what the script meant. A plan key the
engine does not support yet (`sides` and `tables`) refuses the plan
instead of being ignored. A `hacks` table sets match parameters of
entries the registry marks `mutator: true`. Today those are
`limits.units-per-player.default`, `console.game-speed-range.min` and
`console.game-speed-range.max`. The manifest must list the `hacks` layer.
Each value is kept in the registry's normal form. A hack the registry does
not open, a value it refuses, a table it cannot read, or a `hacks` table
whose manifest lacks the layer fails that mutator in the plan phase, as
`Cause::hack_refused`, and the game starts without it.
`PipelineInput::check_hacks` runs after the plans are read and before
files, rules and hooks, so a hack the profile refuses fails the same way
and the pass runs again without that mutator. Nothing of it is mounted.

A pass is a transaction. A plan, or a rule aimed at a folder no mutator may
change, depends on nothing but the mutator itself, so every such failure is
kept. Any later failure can depend on the mutators above it: a mask or a
layer-wide limit in the files layer, or the documents a hook reads. So the
pass keeps only the failure nearest the top of the list, and it stops at the
first hook that fails. No later mutator runs on what a failed mutator left. The
mutator that failed, and every mutator that requires it, is dropped, and the
next pass starts again from the store. A mutator dropped for what it
requires is `excluded`, in the phase `loadout`. Each pass drops at least one
mutator, so a run makes at most one pass more than it has mutators. The
result is the run of the mutators that remain: a mutator that failed leaves
no file, rule, journal entry or note.

The journal keeps every change of the last pass: each rule write, naming
the mutator that made it, each hook write and removal, each file a mutator
added, each record a file added or replaced, and each file of the mod a
mask hides. A replaced record is `added`, and `old_text` names the record
that was already there. A shared record is `added`, with `new_text`
`shared`, and it does not take the record from the mutator that brought
it. A renamed record is `added` under the private name, with `old_text`
the name the mutator wrote. The journal is sorted by the first mutator,
then by phase, so one mutator's rules come before its hooks, then by
path, record, block and field. The conflict report holds the stacking notes, and the paths a
parse left out, one warning each. A text that does not parse is unread. A
record file that does not parse is unread the same way, and it is not
served. Among the notes, `value_replaced` is a value a later mutator's
rule replaced, and also a record a later mutator replaced. A record the
match already had, which no mutator brought, is one note naming that
record and whether the game or the mod served it. A record an earlier
mutator brought, by a file or by a clone, is one note for that collection
and that pair of mutators. A clone in Replace mode onto a record its own
mutator brought is not noted. One field note is kept for each
field name and pair, so a stack that rewrites the same field does not
flood the report. A clone that Add stores under a private name is a
`record_renamed` note, as a renamed record file is. `field_locked`
is a field an earlier mutator locked, so a later mutator's rule left it
alone. A later hook that writes a field names the mutator who last wrote
it, which may be a rule rather than an earlier hook, and then itself. One
such note is kept for each field name and pair, including a hook's set
over every unit. A write over a lock is that one note, naming the locker.
Each of these names both mutators, the earlier first. A script stopped by
an allowance, the real-time limit or the string cap has a cause of its own
(`instruction_budget`, `memory_budget`, `time_limit` or `string_length`),
never `script_error`.

After the last hook, every weapon a mutator brought, by a record file or
by `weapons:clone`, is given an id. The game's weapons and the mod's
weapons keep theirs. A rule or a hook that writes `id` on one of those
weapons does not change it, and the note names the mutator. A write of
`id` on a weapon that mutator brought is a request. A section's `ID=` is
a request when it is a number from 1 to 255. Numbering takes each mutator
in list order. That mutator's requests are kept when the ids are still
free, and its other weapons then take free ids from 1 to 255, by name.
A later mutator's request for an id an earlier weapon already holds takes
another free id. Slot 0 is never given out. The weapons are numbered
before anything is written. When one weapon has no free id, nothing is
written, that mutator's result is `failed` in the phase `integrity` with
cause `weapon_ids`, and the message names the mutator and the weapon. The
run is repeated without that mutator. The written weapon files carry the
final ids. A record file that replaces a record an earlier mutator brought
is journaled, and one note names the two mutators. A document a hook
removed is brought back when a later mutator's file uses that path: a
unit file replaces the document, and a weapon or download file starts
again from an empty document.

The pipeline prints nothing and keeps nothing between calls. The same input
gives the same layer bytes, journal and report. The store must hold no
mutator layer when the call starts. On return the final layer is mounted
when it holds at least one entry or mask, and the caller unmounts it.

## Maps

A mutator's `maps` entry is a function `maps(map, ctx)` or a list of
`where { … }:change { … }` blocks. It runs only once a match has chosen a
map, after every other hook, and only for that match. The setup screens
keep the installed scenario. The rewritten text is served from the mutator
layer at `maps/<key>.ota` when a mutator changed the bytes.

The 3.1c map hash covers the schemas, not only the header and the terrain:
the GlobalHeader hash is taken over the block's whole text, and the
`[Schema N]` blocks sit inside it. A rewritten OTA would therefore hash
differently. What keeps a room together is when the rewrite is served.
The battle room's map check reads the installed file before the match
starts, and the rewrite is served only from the match's start, so the
check and the hash itself are unchanged. Every machine that runs the same
loadout writes the same bytes.

`map.name` is the scenario's `missionname`, or the map key when that key is
absent. `map.file` is the key. `map.planet` is the header's planet.
`map.pack` is the id after the last `@`, or nil for a base map. `map.header`
reads every GlobalHeader key, ignoring case. These keys can be written:

- `minwindspeed`, `maxwindspeed`, `gravity` and `waterdamage`: whole numbers
  from 0 to 32,767;
- `lavaworld`, `waterdoesdamage` and `nosealeveltrigger`: 0 or 1;
- `tidalstrength`, `killmul` and `timemul`: finite numbers from 0 to 1,000.

Any other header key is refused. `map.schemas` is the `[Schema N]` blocks
from 0, with `:each`, `:where`, `:set` and `:scale`. A schema's `index` is
N, and `type` and `players` (its start-position count) are read-only.
`aiprofile` is 1 to 31 letters, digits, `_` or `-`, and `ai/<name>.txt`
must already be in the layered store, including a file this mutator added.
`humanmetal`, `humanenergy`, `computermetal` and `computerenergy` are whole
numbers from 0 to 1,000,000. `surfacemetal` is a whole number from 0 to 255.
`:scale` multiplies a stored number; it cannot scale `aiprofile`.

`schema.units` and `schema.features` are the placed entries, in order. A
unit's `unitname` may name a unit that is in `units` as the mutators above
left it: a unit an earlier hook cloned counts, and one a hook removed does
not. Its `xpos` and `zpos` are whole numbers from 0 to 32,767, and its
`player` is a whole number from 1 to 255. A feature's `featurename` may
name a feature that is in `features` the same way. `:remove()` drops that
one entry. Nothing in the view adds a placed unit, a placed feature or a
schema, and start positions are never shown or written.

In the list form, `where` reads `name`, `file`, `planet`, `pack` and header
keys. A header key in `change` writes the header. A schema key writes every
schema. A key that is neither is refused when the plan is read, and so is
a read-only header or schema key. `glob` matches `*` and `?` only, ASCII
case ignored, at most 256 bytes, with `/` ordinary. `pattern` is a Lua
pattern, matched with the sandbox's `string.find`, so its case matters.
Both are refused in a units rule with "glob() and pattern() match maps
only". The list is applied in order. A later mutator's write wins, and
both writes stay in the journal, with a `same_field_hooked` note for the
pair. A mutator that writes and then errors leaves the scenario as the
mutator above it left it, or unserved when it was the first to write.

A refused write is an error the script can catch with `pcall`, and it
names the field or the rule it broke. Every read and write of the view
counts against the instruction allowance at the collections' rates: 8
units for each field read or written, and one unit per 32 bytes of the
key and of the value. A `:set`, a `:scale` or a list item costs that for
each schema and key it writes, a walk or a `where` one unit for each
element it visits plus the bytes it compares, and a profile or a placed
name the lookup that checks it. A `where`, `:set` or `:scale` table holds
at most 4096 keys. Map writes count on the same tally as that mutator's
other hooks, at most 65536 field writes in all. A further write fails that
mutator with "this mutator has changed too many fields", even when the
script catches the error.

## Rules that target maps

A rule block whose folder is `maps` (ASCII case ignored, `\` read as `/`)
and whose extension is `ota` is taken out of the rules phase. The rules
phase never loads or serves a path under `maps/`. Any other extension in
that folder refuses the plan: rules can change a map's OTA only. The
mutator's manifest must declare the `maps` layer, or the plan is refused
the same way as any other undeclared layer.

Those blocks run at the start of the maps phase, before the first maps
hook, and only on the chosen map's OTA. Needs read that file as it stands.
Across mutators, assignments of one field average, meta values average and
win over plain numbers, factors multiply after ordering by their exact
value bits and then their rule text, and clamps widen to the lowest low
and the highest high. Inside one mutator the last kind of change wins, an
assignment resets that mutator's factors and clamps, and scales multiply
in file order. A number is written with the TDF number spelling when it
is finite and different from the current number. The header limits are
the ones a maps hook uses. A whole-number header key may be left
fractional when the result is finite and from 0 to 32,767. A key that is
not on `[GlobalHeader]` is refused.

A header `SurfaceMetal`, in any case, is not written into `[GlobalHeader]`.
The rules note `copied to every schema's SurfaceMetal`, and a kept number
is copied onto every `[Schema N]` through the schema check (a whole number
from 0 to 255). A value outside that range is refused for each schema, and
the OTA is not served. A removal, or text that is not a number, is not
copied, and a note says so. A factor on a header that has no `SurfaceMetal`
changes nothing. A header `MohoMetal` changes nothing and is noted with
`nothing in OA reads MohoMetal; skipped`.

Whatever the rules change is served, journalled and re-read the way a maps
hook's OTA is, so a later maps hook sees it. Journal rows for a header key
use `map:<key>`, and each schema copy uses `map:<key>/Schema N`. With no
chosen map the rules change nothing and leave one note per mutator,
`applies to the chosen map only`.
