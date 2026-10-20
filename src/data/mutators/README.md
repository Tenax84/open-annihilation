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
`oa::data::mutators`. Header `oa/data/mutators/allow_list.hpp`.

- `check_mutable_path` judges one file path.
- `check_mutable_mask` judges one mask. The pattern's shape is
  `mutator_mask_problem` from `oa/formats/hpi.hpp`. A wildcard is allowed
  only in a folder the table allows as a whole.
- `rule_name` returns the stable lower-case name of a `PathRule`.

## State

None. The two checks are pure. They do not read files, the asset store or
the preferences. A null `PathContext::layout` means `data_layout()`. An
empty `is_unit_name` knows no unit.

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
  `<unit><digits>.gaf` is allowed, and only when `is_unit_name` accepts
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
a named unit's button art is a file a mutator may add, and only as that
one file. `sounds/` stays refused until a later ticket shows the game
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

Still protected:

- `sounds/`. `Runtime::load_all_sounds` runs once, from the `Runtime`
  constructor (`src/app/runtime.cpp`), and not at match start. A mutator
  that changed a sound would leave the game half-changed until the next
  launch.
