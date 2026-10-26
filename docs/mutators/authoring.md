# Writing a mutator

A mutator is one folder, packed as a `.oamut`. This guide walks that folder
from empty to a file `oa-tool mutator check` accepts, then covers the
choices that keep the same package working on another mod. The rules are
[the .oamut standard](oamut-standard.md). Publishing a package is
[Packages](../registries/packages.md).

## Contents

- [1. A first mutator](#1-a-first-mutator)
- [2. Selecting by properties](#2-selecting-by-properties)
- [3. Options](#3-options)
- [4. Prerequisites and conflicts](#4-prerequisites-and-conflicts)
- [5. Translations](#5-translations)
- [6. A hook that asks across units](#6-a-hook-that-asks-across-units)
- [7. Computing stats with fixed](#7-computing-stats-with-fixed)
- [8. When to prefer a rule](#8-when-to-prefer-a-rule)
- [9. Features, sides and tables](#9-features-sides-and-tables)
- [10. Menus, maps and hacks](#10-menus-maps-and-hacks)
- [11. The Files layer](#11-the-files-layer)
- [12. Clash modes](#12-clash-modes)
- [13. When a mutator fails](#13-when-a-mutator-fails)
- [14. Testing](#14-testing)
- [15. Importing a TA: Mutation mutator](#15-importing-a-ta-mutation-mutator)
- [16. Publishing](#16-publishing)

## 1. A first mutator

Make a folder and put `oamut.yaml` in it. The id is lower-case
kebab-case. The version is quoted. `tags`, `works-with`, `layers` and
`author` are required. Steady Hands fits the made-up mod Ridge and the
base game. It makes mobile builders faster, and the player picks how much.

```yaml
oamut: 1
id: steady-hands
name: Steady Hands
version: "1.0"
author:
  name: A. Builder
description: Builders move a little faster.
tags: [gameplay]
works-with:
  - ridge >= 1
  - ta-3.1c
layers: [units]
options:
  pace:
    type: number
    label: How much faster
    min: 1
    max: 2
    step: 0.5
    default: 1.5
entry: mutator.lua
```

`mutator.lua` sits beside the manifest, not under `files/`, `rules/` or
`lua/`. `mutate` returns the plan. `pace` is read here, and only a unit
that builds and moves is selected. `builder` alone would also write the
factories, and a factory has no `maxvelocity` of its own. The assignment
is [7. Computing stats with fixed](#7-computing-stats-with-fixed).

```lua
function mutate(ctx)
  local pace = fixed(ctx.options.pace)
  return {
    units = function(units, ctx)
      units:where { builder = true, bmcode = 1 }:each(function(u)
        u.maxvelocity = u:fixed("maxvelocity") * pace
      end)
    end,
  }
end
```

From the folder that holds `steady-hands`:

```
oa-tool mutator pack steady-hands
oa-tool mutator check steady-hands-1.0.oamut
```

Pack from outside the package folder. `oa-tool mutator pack .` writes
the `.oamut` inside the folder, and a package may not hold that file.
Running `pack .` again stops with
`oa-tool mutator pack: the output already exists: steady-hands-1.0.oamut`.
Packing the folder from outside while that file is still in it, or
running `pack . --force`, is refused with
`The package holds steady-hands-1.0.oamut, outside the folders a mutator may hold.`
Delete the file and pack from outside.

`mutator pack FOLDER [--out FILE] [--force]` writes
`<id>-<version>.oamut` in the current folder when `--out` is left out.
`--force` replaces an existing file. `--game-dir` is a usage error. A
manifest problem, a Lua source that does not compile, or a rule file that
does not parse is listed with its file and line, and nothing is written.
A folder the installer would refuse is not packed. A folder without
`oamut.yaml` is a usage error.

`mutator check FILE [--json]` exits 0 when the installer would accept the
package, and 1 when it would refuse it. It also refuses a script that
does not compile or a rule file that has an error, which the installer
would still accept. Rule-file warnings are not shown, and they do not
refuse the package. The check compiles Lua and parses rule files. It
does not run them. A file that is not a `.oamut` is a usage error. The
last line is `result: ok` or `result: refused`.

`oa-tool mutator check steady-hands-1.0.oamut` ends with `result: ok`.

## 2. Selecting by properties

Select units by what they are, not by their names. `commander`, `bmcode`,
`canfly` and `builder` are fields the unit file already carries:

| Field | What it selects |
| --- | --- |
| `commander` | The side's commander, when the field is set. |
| `bmcode` | A unit that moves when the field is not 0, and a structure when it is 0. |
| `canfly` | A unit that flies. |
| `builder` | A unit that builds. |

```lua
units:where { canfly = true }:remove()
units:where { builder = true, bmcode = 0 }:each(function(f)
  -- a factory, a lab, or any other structure that builds
end)
```

Another mod, Ridge included, ships its own unit names. A rule that names
one unit from the base game matches nothing there. The note is
`<id> matched nothing`, and it is written only for a rule block. The game
logs notes as `open-annihilation: mutators: <line>` when the match is
prepared. `mutator diff` and `mutator check` do not print notes.

A hook that names a missing unit gets nil. Indexing it is a script error,
and the mutator fails:

```lua
units.RIDGEZIP.maxvelocity = 1
```

`oa-tool mutator diff` then exits 1 with
`oa-tool mutator diff: by-name: attempt to index a nil value (field 'RIDGEZIP')`.
A property still matches the unit that plays that part. Name a unit only
when the mutator is adding that unit.

## 3. Options

`options` is what the player picks before the match. A toggle's value on
`ctx.options` is `true` or `false`. A number, and a choice whose values
are numbers, is a number. A choice whose values are identifiers is text.
An option the player left is the default.

Steady Hands declares `pace`. The script in
[1. A first mutator](#1-a-first-mutator) reads it as `ctx.options.pace`
and multiplies by that count. The manifest's `min`, `max` and `step` are
the only values the player can pick, so the script does not have to test
the range again. A value off the step is refused before the script runs.
`oa-tool mutator diff` exits 2 with
`oa-tool mutator diff: steady-hands: pace: 1.7 does not lie on a step of 0.5 from 1`.

`--mutator ./steady-hands:pace=1 --versus ./steady-hands:pace=2` exits 1
with 57 differences. One of them is
`left only field units/armaca.fbi [unitinfo] maxvelocity: 7.6 -> 7.5999908447265625`.
Pace 1 still rewrites the text, which
[7. Computing stats with fixed](#7-computing-stats-with-fixed) explains.

A choice lists `values`, and `labels` when the words differ from the
values. The standard's table of keys is
[3.4 Options](oamut-standard.md#34-options).

## 4. Prerequisites and conflicts

`requires.mutators` names mutators that must be on, and they apply before
this one, in the order written. `conflicts-with` names mutators this one
refuses to run beside. Both are conditions, the same shape as
`works-with`.

```yaml
requires:
  mutators:
    - long-reach >= 1
conflicts-with:
  - long-shadows
```

The list is checked before anything runs. More than 32 mutators, an id
listed twice, a requirement nothing above it meets, or two mutators that
conflict, refuses the whole list. A mutator that does not fit the mod is
incompatible. The match does not start. What the player sees is
[13. When a mutator fails](#13-when-a-mutator-fails).

## 5. Translations

`strings` maps a language tag to the words the Library shows. `name`,
`description`, `options` and `labels` are the keys a language may hold.
An option id has to be one the manifest defines.

```yaml
strings:
  de:
    name: Ruhige Hände
    description: Die Bauarbeiter sind etwas schneller.
    options:
      pace: Wie viel schneller
```

## 6. A hook that asks across units

Some changes depend on another unit. Empty Yards removes every flying
unit, then removes a structure that builds when nothing it builds is
left. `builds` reads the menus. It does not need the `menus` layer, and
a units hook may read `ctx.menus`.

The manifest is the first mutator's, with this id and name and with no
`options` key:

```yaml
oamut: 1
id: empty-yards
name: Empty Yards
version: "1.0"
author:
  name: A. Builder
description: Structures that build nothing are removed, and so is anything that flies.
tags: [gameplay]
works-with:
  - ridge >= 1
  - ta-3.1c
layers: [units]
entry: mutator.lua
```

The script is:

```lua
function mutate(ctx)
  return {
    units = function(units, ctx)
      units:where { canfly = true }:remove()
      units:where { builder = true, bmcode = 0 }:each(function(f)
        if #ctx.menus:builds(f.unitname) == 0 then f:remove() end
      end)
    end,
  }
end
```

`units:remove` drops the unit and the download sections that name it. At
the end of the mutator's turn the engine also greys the unit's buttons on
every builder's authored pages, so removing a unit does not need the
`menus` layer. `menus:grey`, `menus:hide` and the other menu writes do.
`oa-tool mutator check` ends with `result: ok`. The diff drops 38 units,
hides those unit files and 2 download files, removes 22 menu entries and
greys the buttons.

## 7. Computing stats with fixed

Use `fixed` for any stat a hook computes. Steady Hands does:

```lua
local pace = fixed(ctx.options.pace)
u.maxvelocity = u:fixed("maxvelocity") * pace
```

`fixed` counts 65,536ths. The arithmetic is integer arithmetic, so the
count is the same on every machine, the 32-bit Windows builds included.
A plain Lua float can differ in rare cases, and a room whose machines
disagree does not start.

`u:fixed("maxvelocity")` reads the field the way the game does, including
the loader's default when the key is absent. Assigning the product writes
`f:text()`, the decimal the game stores. That text is not the short
decimal a rule would write. At pace 1.5 a field written `1.6` becomes
`2.3999786376953125`, and a field written `7.6` becomes
`11.3999786376953125`. A factor of 1 rewrites `7.6` as
`7.5999908447265625`, so a script should skip the write when the factor
is 1:

```lua
if ctx.options.pace ~= 1 then
  u.maxvelocity = u:fixed("maxvelocity") * pace
end
```

The package in [1. A first mutator](#1-a-first-mutator) does not skip, so
pace 1 and pace 2 both rewrite every mobile builder.
`oa-tool mutator check steady-hands-1.0.oamut` is the check that
compiles it.

The range, the operations and the text are
[10.9 The fixed library](oamut-standard.md#109-the-fixed-library).

## 8. When to prefer a rule

A rule computes on the field's text. A hook computes on the count.
`scale(1.5)` on a field written `1.6` stores `2.4`. The hook line above
multiplies the counts and stores `2.3999786376953125`.

Prefer a rule when the change is a factor, an assignment or a clamp on
fields the text already expresses, and when the result should be the
decimal TA: Mutation would have written. Prefer a hook when the new
value depends on another unit, on a count the text does not show, or on
a test the rule forms do not express. An option can be read from either:
Steady Rule is Steady Hands with `id: steady-rule`, `name: Steady Rule`
and this script.

```lua
function mutate(ctx)
  return {
    rules = {
      where { builder = true, bmcode = 1 }:change { maxvelocity = scale(ctx.options.pace) },
    },
  }
end
```

`oa-tool mutator diff --game-dir DIR --mutator ./steady-hands --versus ./steady-rule`
exits 1 with 54 differences. Two of them are:

```
left only field units/armacv.fbi [unitinfo] maxvelocity: 1.6 -> 2.3999786376953125
right only field units/armacv.fbi [unitinfo] maxvelocity: 1.6 -> 2.4
```

The left line is the hook. The right line is the rule. The same rule as
a `.sm` file, with the factor written out, is:

```
[*Group*] {
  [Needs] { builder=1,1; bmcode=1,1; }
  [Changes] { maxvelocity=*1.5; }
}
```

A plan names that file with `rules = { sm "rules/pace.sm" }`. The path
is matched letter for letter, including case. A rule needs the layer of
each target. The forms are
[6.1 The Lua forms](oamut-standard.md#61-the-lua-forms).

## 9. Features, sides and tables

`features`, `sides` and `tables` are hooks of their own. Each one needs
its own layer. A mutator that changes only wrecks declares only
`features`. Declaring `units` does not let any hook write the wrecks,
the sides or the tables. Reading them through `ctx` needs no layer.

Soft Wrecks declares `layers: [features, tables]`.

```lua
function mutate(ctx)
  return {
    features = function(features, ctx)
      features:where { category = has("corpses") }:set { blocking = 0 }
    end,
    tables = function(tables, ctx)
      local moves = tables.moveinfo
      if moves == nil then ctx.incompatible("this game has no moveinfo.tdf") end
      moves.CLASS0.maxslope = "20"
    end,
  }
end
```

The diff has 212 lines like
`field features/corpses/arm_corpses.tdf [armaap_dead] blocking: 1 -> 0`
and one line
`field gamedata/moveinfo.tdf [class0] maxslope: 32 -> 20`.

The tables hook opens only `moveinfo`, `los` and `meteor`.
`tables.sound` is refused, and so is any other file in `gamedata`. The
standard's account is [7.3 Tables](oamut-standard.md#73-tables).

Grounded declares `layers: [sides]`. It takes every flying unit off the
computer's build lists in `sidedata.tdf`. A player's build menus are the
`menus` hook's. The sides collection cannot add or remove a side.

```lua
function mutate(ctx)
  return {
    sides = function(sides, ctx)
      ctx.units:where { canfly = true }:each(function(u) sides:remove_canbuild(u.unitname) end)
    end,
  }
end
```

One line is
`field gamedata/sidedata.tdf [canbuild][armaap] canbuild1: ARMACA -> (absent)`.

## 10. Menus, maps and hacks

`menus` is the hook for build menus, and it needs the `menus` layer. No
Wings declares `layers: [menus]`. It takes every flying unit off the
build menus and greys its buttons, and it leaves the units in the game:

```lua
function mutate(ctx)
  return {
    menus = function(menus, ctx)
      ctx.units:where { canfly = true }:each(function(u) menus:remove(u.unitname) end)
    end,
  }
end
```

The diff removes 2 download entries, hides the 2 download files they
leave empty, and greys 32 buttons, for example
`field guis/armaap1.gui [gadget4] grayedout: (absent) -> 1`.
`menus:add`, `menus:move`, `menus:grey` and `menus:hide` are in
[7.4 Menus](oamut-standard.md#74-menus).

`maps` runs once the match has chosen a map, and it needs the `maps`
layer. Rich Start declares `layers: [maps]`:

```lua
function mutate(ctx)
  return {
    maps = function(map, ctx)
      map.schemas:set { humanmetal = 5000, computermetal = 5000 }
    end,
  }
end
```

`oa-tool mutator diff` runs a maps hook only with `--map NAME`. With
`--map "Comet Catcher"` one line is
`field maps/comet catcher.ota [globalheader][schema 0] humanmetal: 1000 -> 5000	by rich-start`.
Without `--map` the report lists nothing, and the Library's dry run does
not run a maps hook either. The keys a maps hook may write are
[7.5 Maps](oamut-standard.md#75-maps).

`hacks` sets, for one match, a parameter of a hack the registry opens to
mutators, and it needs the `hacks` layer. Big Army declares
`layers: [hacks]`:

```lua
function mutate(ctx)
  return { hacks = { ["limits.units-per-player"] = { default = 500 } } }
end
```

The value that plays is clamped to the profile's limits, 20 to 1,500
unless the profile sets others. `mutator diff` does not list hacks, so
its report for Big Army is empty. The parameters a mutator may set are
[8. Hacks](oamut-standard.md#8-hacks).

## 11. The Files layer

Files the match reads again, a model, a script or a new unit, go in the
package's own `files/` folder. The plan returns `files = { add = { "files" } }`,
and the manifest declares the `files` layer. The paths a mutator may
add are the allow-list in
[5.2 The allow-list](oamut-standard.md#52-the-allow-list). A protected
path fails the mutator and refuses the start. Adding `sounds/beep.wav`
exits 1 with
`oa-tool mutator diff: loud-path: loud-path: 'sounds/beep.wav': sounds/beep.wav: this is loaded once when the game starts`.

Add folders are how files vary by option. The plan lists them in order,
and a later folder wins a path both folders hold. An add folder that is
not in the package adds nothing, so `files/<mod id>` is safe for a mod
that ships no files of its own. The missing folder is a note when the
match is prepared. The diff of a package that adds only `files/ridge`
lists no file.

Ridge Plans layers three folders, the way a package with several
related variants does: a folder every variant shares, a folder that a
group of variants shares, then the chosen variant's own folder. It
declares `layers: [files]` and a choice `level` whose values are `easy`,
`hard` and `brutal`, default `easy`. The tree is:

```text
ridge-plans/
  oamut.yaml
  mutator.lua
  files/common/ai/NOTES.TXT
  files/common/ai/RIDGE.TXT
  files/gentle/ai/RIDGE.TXT
  files/fierce/ai/FIERCE.TXT
  files/fierce/ai/RIDGE.TXT
  files/brutal/ai/RIDGE.TXT
```

Each `RIDGE.TXT` is its folder's name and a line feed (`common`,
`gentle`, `fierce` or `brutal`). `NOTES.TXT` is `notes` and a line feed,
and `FIERCE.TXT` is `fierce only` and a line feed.

```lua
function mutate(ctx)
  local level = ctx.options.level
  local tier = "gentle"
  if level ~= "easy" then
    tier = "fierce"
  end
  return { files = { add = { "files/common", "files/" .. tier, "files/" .. level } } }
end
```

There is no `files/easy` or `files/hard`. Those add folders add nothing,
so `easy` plays `gentle` over `common`, and `hard` plays `fierce` over
`common`. The default gives:

```
file added ai/notes.txt 444e0fffbd825e9610ff5b199485707a0c895339ae80c15cc8a8aee41b106fda	by ridge-plans
file added ai/ridge.txt 2521577847baf50d853153a96223fdcca4de821af0f82d1f42ee7af94bbbbc4b	by ridge-plans
```

That `ridge.txt` is the gentle file. The common one lost the path, and
the common `notes.txt` stays. `:level=hard` gives:

```
file added ai/fierce.txt d16e05ce615de21b5f226716d7b731342be4bb91be1b37940f1127a7759bea50	by ridge-plans
file added ai/notes.txt 444e0fffbd825e9610ff5b199485707a0c895339ae80c15cc8a8aee41b106fda	by ridge-plans
file added ai/ridge.txt 3a3afd2a8609c710fce3ca0405465d8b38052c57a876328b18a607b21db78c35	by ridge-plans
```

`:level=brutal` gives the same `fierce.txt` and `notes.txt`, and
`ridge.txt` is the brutal file,
`a8ca9de77d0f0494b237fec1593255d7bfc1a41eab9afff1ca179103ec73f289`.

## 12. Clash modes

A clash is a unit or a weapon whose name is already in the game, the mod
or an earlier mutator. It comes from a record file under `files/`, or
from `clone`. The mode in force is the player's choice, else the
manifest's `clashes`, else the preference `mutators.clash-default`,
which is `replace` when unset. The standard's account is
[3.8 Clashes](oamut-standard.md#38-clashes).

`replace` patches that record. The player still builds the original name,
and the fields are this mutator's. Set it when the mutator means to
change the unit the game already has.

`add` keeps the existing record and stores a different one as `NAME_2`,
then `NAME_3`, and so on. This mutator's own references follow the
private name. A later mutator's use of the original name is left alone.
Set it when the original unit should stay and this package is bringing
another one that happens to share the name.

Second CV declares `layers: [units]` and clones `ARMCV` onto its own
name:

```lua
function mutate(ctx)
  return {
    units = function(units, ctx)
      units:clone("ARMCV", "ARMCV", { maxvelocity = "9" })
    end,
  }
end
```

With `clashes: replace` the diff is
`field units/armcv.fbi [unitinfo] maxvelocity: 1.4 -> 9	by second-cv-replace`.
With `clashes: add` the original file is unchanged and the diff lists
`unit added ARMCV_2	by second-cv-add` and
`file added units/armcv_2.fbi	by second-cv-add`.

## 13. When a mutator fails

`ctx.incompatible(reason)` stops this mutator. The reason is text. Empty
text, and anything that is not text, is stored as
`incompatible with this mod`. The mutator For Ridge calls it unless the
mod is Ridge:

```lua
function mutate(ctx)
  if ctx.mod.id ~= "ridge" then
    ctx.incompatible("written for the Ridge mod")
  end
  return {}
end
```

On the base game, `oa-tool mutator diff` exits 1 with
`oa-tool mutator diff: for-ridge: written for the Ridge mod`.
A `works-with` miss is the same shape:
`oa-tool mutator diff: ridge-only: ridge-only works with ridge, not ta-3.1c`.

A mutator also fails when the plan is refused, a hook raises, a path is
protected, a budget is spent (the instruction allowance, the memory cap,
the real-time limit or the string cap), or the integrity check fails. The player sees
one reason. In a skirmish the start is refused with
`Skirmish start failed: <Name> (<id>): <phase>: <reason>`.
The phase is `loadout`, `manifest`, `plan`, `files`, `rules`, `hooks` or
`integrity`. In a battle room, START is held and the build is marked
failed.

The exception is a refused hack: a problem inside the `hacks` table
fails that mutator alone. The mutators that require it are dropped with
it, and the match starts without them. If `hacks` is not a table, or
the `hacks` table has a metatable, that is an invalid plan and the start is
refused with the others.

`mutator diff` reports a failure on its error stream and exits 1. The
line is `oa-tool mutator diff: <id>: <reason>`. The Library's verdict
lines are in [14. Testing](#14-testing).

## 14. Testing

`oa-tool mutator check` is the first test. It answers whether the
package would install, and whether its scripts compile and its rule
files parse.

`oa-tool mutator diff` answers what the package changes in a game:

```
oa-tool mutator diff --game-dir DIR [--mod-dir DIR] [--map NAME] [--seed N] [--mutator SPEC]... [--versus SPEC]... [--allow FILE] [--dump FILE]
```

`--game-dir` is the game folder. `--mod-dir` is a mod folder mounted
over it. Each `--mutator` is a package folder, with options after the
last colon, in the same text as the game's `--mutator`. An id with no
path is refused. `--seed` is the match seed, and it is 0 when left out.
`--map` also runs each mutator's maps hook for that map. A map the game
does not have is a usage error. A dry run in the Library sets no map, so
a maps entry does not run there.

```
oa-tool mutator diff --game-dir DIR --mutator ./steady-hands:pace=1.5
```

The report lists units dropped or added, files added, hidden or
replaced, every changed field, and build-menu entries added or removed.
A field line looks like `field PATH SECTION KEY: OLD -> NEW`. When the
journal names the mutator that wrote a line, a tab and `by <id>` follow.
A side's build list, and a button the engine greyed for a removed unit,
carry no `by`. The header lines start
with `#`. One line of Steady Hands at the default pace is
`field units/armaca.fbi [unitinfo] maxvelocity: 7.6 -> 11.3999786376953125	by steady-hands`.

`--versus` builds a second loadout the same way and compares the two
reports. A pattern is matched against the printed line, `left only …`
or `right only …`.

```
oa-tool mutator diff --game-dir DIR --mutator ./steady-hands --versus ./steady-rule
```

That comparison exits 1 with 54 differences. The lines are the ones in
[8. When to prefer a rule](#8-when-to-prefer-a-rule).

`--allow FILE` names differences that may remain. It is a usage error
without `--versus`. The file ignores a leading byte-order mark and blank
lines. A `#` line is a reason. Every other line is a whole-line pattern:
`*` is any run, `?` is one character. A blank line ends a block. Each
pattern needs a reason above it in its own block.

```
# The hook multiplies the game's own counts, so its decimals run long.
left only field units/*.fbi [unitinfo] maxvelocity: *

# The rule scales the text, as TA: Mutation did.
right only field units/*.fbi [unitinfo] maxvelocity: *
```

With that file the same comparison exits 0 and prints
`mutator diff: 0 differences, 54 allowed, 0 unused allowances`.

A pattern that matches nothing is printed as
`allowed difference matched nothing: <pattern>` and the comparison
fails. Exit 0 when the report was written, and, for `--versus`, only
when nothing remains and every pattern matched. Exit 1 when a mutator
fails, the folder is not a usable game, or a comparison does not match.
Exit 2 when the arguments are not the command's. `--dump FILE` writes
the mutated file list and its fields, and does not change the report.

The game's own check is the Library. A mutator's rules line reads
`Changes the game: unranked on Core Prime`. After a dry run the Checked
line is one of:

| Verdict | What the Library shows |
| --- | --- |
| compatible | `With {mod}: {summary}` |
| no effect | `With {mod}: no effect` |
| incompatible | `With {mod}: {reason}` |
| failed | `With {mod}: it stopped: {reason}` |
| still running | `Checking with {mod}…` |
| not run yet | `Not checked with {mod}` |

The dry run applies the mutator. `mutator check` does not.

## 15. Importing a TA: Mutation mutator

`oa-tool mutator import` reads a `.tam` or a zip and writes a mutator
folder. It writes a `legacy` record for each original. A record holds
`name`, `prefix`, `compatibility` and `files`. Curating the package
keeps those records. Folding several variants into one package lists
every original as its own record, as the packages under `mutator-packages`
do. The fields and their limits are
[3.6 Legacy](oamut-standard.md#36-legacy).

```yaml
legacy:
  - name: Careful Hands
    prefix: "{}"
    compatibility: ["OTA"]
    files: ["careful.sm"]
  - name: Rapid Hands
    prefix: "{}"
    compatibility: ["OTA", "*"]
    files: ["rapid.sm"]
```

`oa-tool mutator check` of a package that carries that list ends with
`result: ok`.

```
oa-tool mutator import INPUT... --out DIR [--id ID] [--version VERSION] [--tag TAG]... [--works-with CODE=ENTRY]... [--pack] [--force]
```

`OTA` and `TACC` are already mapped to works-with entries. `--tag`
replaces the `gameplay` tag. `--id` and `--version` set the id and the
version, and only when one mutator is named. `--pack` also writes
`DIR/<id>-<version>.oamut`. `--force` replaces an existing folder.
Exit 0 when every mutator was imported, 1 when one was refused or could
not be written, and 2 when the arguments are not the command's. A link
is not followed. `DIR` is not the folder that holds a `.tam`. A zip has
no companions beside it, so `DIR` may be the folder that holds the zip.

A protected file is dropped. The warning is
`left out <archive>: <path> (<rule>)`.

The import is the start of the package, not the finished mutator. Curate
it before publishing:

- Turn variants into `options`, and read the choice in `mutate`. The
  importer does not do that. Keep a `legacy` record for each original.
- Turn rule files into the Lua forms in
  [8. When to prefer a rule](#8-when-to-prefer-a-rule), or leave them as
  `.sm` files named from the plan with `sm`.
- A whole-file copy that only changes numbers is a rule. Keep a file in
  the Files layer for a model, a script or a new unit.
- Leave protected files out. Replacing one fails the mutator and refuses
  the start.

Then pack and check the folder as in
[1. A first mutator](#1-a-first-mutator).

## 16. Publishing

A `.oamut` is a package. Where it installs, how a registry serves it, and
what the catalogue lists are
[Packages](../registries/packages.md) and
[the catalogue](../registries/catalogue.md). The command line that plays
one is [the command line](../command-line.md).
