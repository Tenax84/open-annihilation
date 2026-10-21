# The OAMUT standard

A mutator package, `oamut.yaml` inside a `.oamut` file, changes one game.
The manifest names the mutator. Its scripts and rule files say how that
game differs from the base game or from a mod. Open Annihilation reads the
manifest without running any code, then runs the scripts in a sandbox
before the match.

This document is the format and its rules as the engine implements them.
The archive a package is shipped in, and the catalogue entry a registry
publishes, are described with the other package kinds. This standard names
what is different for mutators and does not repeat those rules.

## Contents

- [1. Goals and principles](#1-goals-and-principles)
- [2. The package](#2-the-package)
- [3. The manifest](#3-the-manifest)
- [5. The Files layer](#5-the-files-layer)
- [6. Rules](#6-rules)
- [10. The sandbox](#10-the-sandbox)
- [13. Versioning](#13-versioning)

## 1. Goals and principles

A mutator changes one game. The script that describes the change runs in
a sandbox, and it runs before the match. The match loads ordinary files.
No mutator code runs while the match runs.

| Goal | What it means in practice |
| --- | --- |
| Sandboxed script | A mutator reaches the game only through the sandbox. It has no file, network, clock or environment of its own. A script that fails stops that mutator. It does not stop the game. |
| Before the match only | The script runs, and its files and rules are written, before the match loads. During the match the engine reads those files the way it reads any mod. |
| Per game | Every mutator applies to the game being set up. The next game starts from the base game or the mod again. Nothing a mutator writes is kept for a later launch. |
| Open Annihilation only, and unranked | Mutators are for games played in Open Annihilation. A mutated game is not a ranked game. |

The principles that follow:

1. **Text, then the match.** A mutator rewrites the text the match already
   knows how to load. It does not hand the simulation a private record.
2. **One manifest.** `oamut.yaml` is what the packer writes, what the
   installer checks and what the Library lists. A key spelled wrong is
   refused, so it is noticed. A key that belongs in a mod profile is
   refused with its own message.
3. **The same result on every machine.** The sandbox's numbers, the order
   it walks tables, and the text it writes do not depend on the machine.
   A real-time limit can stop a script on a slow machine and let it finish
   on a fast one. Every machine must produce the same result.

## 2. The package

A mutator is one `.oamut` file, or the folder that file unpacks to. The
archive is the zip every package kind uses. The names it may hold, and the
limits on its size, are
[Packages](../registries/packages.md). They are not defined a second time.

The manifest is the strict YAML a mod profile uses
([3.3 Strict YAML rules](../mods/oamod-standard.md#33-strict-yaml-rules)).
One document, one top-level mapping, no anchors and no block scalars.

### 2.1 Folder layout

The top of the package holds `oamut.yaml`. Beside it, a file is one of
these:

| Path | What it is |
| --- | --- |
| `oamut.yaml` | The manifest. Required. |
| The entry script | Lua source. The default name is `mutator.lua`. `entry` names another. |
| `lua/` and a name ending `.lua` | Further Lua source. `require` reads only these files. |
| `rules/` and a name ending `.sm` | Rule files. |
| `files/` and anything under it | Files and archives the Files layer may add. |
| A top-level name ending `.md` or `.txt` | A note for people. The game does not load it. |
| The icon | The PNG named by `icon`, when the manifest names one. |

A file outside that list is refused. The comparison ignores the case of
ASCII letters. An entry script is not placed under `files/`, `rules/` or
`lua/`.

```text
steady-hands/
  oamut.yaml
  mutator.lua
  icon.png
  lua/hands.lua
  rules/economy.sm
  files/units/ridgecom.fbi
  README.md
```

### 2.2 The .oamut file

`oa-tool mutator pack` and `oa-tool mutator check` are the pack and check
commands limited to a mutator. How a package is packed, which files are
stored, the default name, `--out`, `--force`, `--game-dir`, the problem
lines and the report are [Packages](../registries/packages.md). A mutator's
revision in that report is null.

What those commands add for a mutator:

- They compile the entry script and every file under `lua/` whose name
  ends `.lua`. A `.lua` file under `files/` is not compiled. The
  installer's size and precompiled-source checks still cover every
  `.lua` file, as [2.4 Size limits](#24-size-limits) says.
- They parse every file under `rules/` whose name ends `.sm`. A rule file
  larger than 4 MiB is refused before it is parsed. The rule reader
  refuses a text longer than 1 MiB, so a rule file past 1 MiB is refused
  either way. A warning does not refuse the package. An error does.
- Neither command runs a script or applies a rule.
- `mutator check` exits 1 when the installer would refuse the package, and
  also when one of those scripts does not compile or a rule file has an
  error. The installer would accept a package whose only fault is a
  script that does not compile or a rule file that has an error.
- `--out` must name a file ending in `.oamut`. After pack writes the file,
  it reads that file back with the installer and refuses a package the
  installer refuses.

### 2.3 Where it installs

Into `Documents/Open Annihilation/Mutators/<id>`, by the manifest's `id`,
never by the file's name. Installing does not turn the mutator on. The
player turns it on when setting up a game.

Where that folder is on each system, how a catalogue package differs from
a file the player opened, and the questions an install asks, are
[Packages](../registries/packages.md). This standard does not repeat them.
The revision stored for an installed mutator is 0.

### 2.4 Size limits

The limits every package meets, a mutator included, are
[Packages](../registries/packages.md). That covers the manifest's size,
the archive's size, the spare path length, and the name, link, encryption
and compression rules.

A mutator also meets these limits. The installer applies them to every
`.lua` file, including one under `files/`:

| Limit | Value |
| --- | --- |
| One Lua file, the entry included | 1 MiB. An empty file is source. A file whose first byte is `0x1B` is precompiled Lua and is refused. |
| The icon, when named | A PNG, square, at least 1 pixel and at most 256 pixels on a side, and at most 256 KiB. Every icon failure shares one sentence. |
| `requires.engine`, when written | This build's version meets the range. The refusal names the range and this build. |

## 3. The manifest

`oamut` is the integer `1`. A key that is not in this table is refused.
`1.0` and `"1"` are refused. The writer prints the keys in the order of
the table, with two-space indentation, every string in double quotes, and
a short list as a flow sequence. An optional key that was left out is left
out again, and so is `entry` when it is `mutator.lua`.

A profile key is refused with
`<key> belongs in a mod's oamod.yaml; a mutator cannot carry it`. Those
keys are `identity`, `layout`, `limits`, `script-extensions`, `data-keys`,
`hacks`, `settings`, `media`, `packaging`, `supportedGames` and
`requiredExpansions`. `hacks` inside `layers` is a layer name. Any other
unknown key is refused with `<key> is not a key of oamut.yaml`.

`homepage`, `tags` and `requires.engine` follow the shared package-key
rules. Tags are then limited to four. When the shared rules already report
a tag problem, the four-tag limit is not reported again.

| Key | Type | Required | Limits | Meaning |
| --- | --- | --- | --- | --- |
| `oamut` | integer | yes | `1` | The format. |
| `id` | string | yes | 1 to 64 bytes, lower-case kebab-case | The mutator's id. No leading, trailing or doubled hyphen. The catalogue uses the same rule for a package key. |
| `name` | string | yes | 1 to 64 bytes, no control character | The name players see. |
| `version` | string | yes | A mutator version, written in quotes, at most 32 bytes | One to three parts joined by `.`, each a decimal from 0 to 65535 with no leading zero. A plain number, and a plain scalar such as `2.0.1`, are refused. |
| `author` | mapping | yes | `name`, and optionally `email` | Who wrote the mutator. |
| `author.name` | string | yes | 1 to 128 bytes | The author or team. |
| `author.email` | string | no | At most 254 bytes | One `@`, text on both sides, a dot in the part after it, that part not starting or ending with `.`, and no space or control character. |
| `description` | string | no | 1 to 2,000 bytes | A line feed is allowed. No other control character. The first line, cut to 200 bytes on a UTF-8 boundary, is the summary the Library and the catalogue show. |
| `homepage` | string | no | 1 to 256 bytes | An `http` or `https` address, the scheme in either case, with at least one byte after the scheme and no space or control character. |
| `icon` | string | no | A package path ending `.png` | The ending matches in either case. Parts separated by `/`. No part is empty, `.` or `..`. No leading `/` or `\`, and no `\` anywhere. The installer checks the pixels. |
| `tags` | list of strings | yes | 1 to 4 | Each tag is lower-case kebab-case of 1 to 32 bytes. None is repeated. An empty list is refused. The key is required, so the list holds one to four tags. |
| `works-with` | list of strings | yes | 1 to 32 | Conditions, below. No id twice. |
| `requires` | mapping | no | Only `engine` and `mutators` | Anything else in the mapping is refused. |
| `requires.engine` | string | no | At most 64 bytes | An engine requirement, written in quotes. |
| `requires.mutators` | list of strings | no | 1 to 16 | Conditions on mutator ids. The order written is the order those mutators apply in, all before this one. |
| `conflicts-with` | list of strings | no | 1 to 32 | Conditions on mutator ids this mutator refuses to run beside. |
| `layers` | list of strings | yes | 1 to 9, none repeated | The nine names below. |
| `options` | mapping | no | At most 16 option ids | Settings the player picks before a match. |
| `strings` | mapping | no | At most 16 language tags | Translations of the name, the description and the option labels. |
| `entry` | string | no | A package path ending `.lua` | The entry script. The default is `mutator.lua`. It is not under `files/`, `rules/` or `lua/`. |
| `legacy` | list of mappings | no | 1 to 16 records | TA: Mutation mutators this package was imported from, in the order written. A bare mapping is refused. |

A problem names the value's path, such as `options.factor.default`,
`requires.mutators[0]` or `strings.de.options.factor`. Indexes count from
0. The line and column are the value's, 1-based, in bytes.

### 3.1 Versions, ranges and conditions

A missing version part compares as 0, so `2`, `2.0` and `2.0.0` are equal.
The writer still prints as many parts as were written.

A version range is one to four comparisons (`>=`, `>`, `<=`, `<` or `=`),
separated by commas that spaces may surround, at most 64 bytes. It is the
engine requirement's grammar with mutator versions in place of three-part
releases, so `>= 4.0` and `>= 2` are ranges. A range that holds a comma is
quoted in YAML.

`requires.engine` uses three-part releases, `MAJOR.MINOR.PATCH`, each part
a decimal from 0 to 65535 with no leading zero. The same one to four
comparisons apply, and the whole text is at most 64 bytes. A value that
starts with `>` is a block scalar unless it is quoted, and the manifest
reader refuses that before the range is read. Reading the manifest does
not ask whether this Open Annihilation meets the range. The installer and
`oa-tool mutator check` do.

A condition is an id alone, or an id, one or more spaces, and a range. An
id is lower-case kebab-case of 1 to 64 bytes, `*` or `ta-3.1c`. `*` and
`ta-3.1c` take no range. Ids are compared byte for byte. A condition with
no range is met by its id at any version text. A range is met only when
the version text is a mutator version inside the range, so a mod version
such as `5b` misses.

In `works-with`, `*` means any mod and the base game played alone, and it
is the only entry when it appears. `ta-3.1c` means the base game with no
mod. Any other id is a mod.

In `requires.mutators` and `conflicts-with` an id is a mutator id. It is
not this mutator's own id. An id in `conflicts-with` is not an id
`requires.mutators` names. No id is repeated in one list.

### 3.2 Tags

The reader accepts any tag that is lower-case kebab-case of 1 to 32 bytes,
at most four of them. The Library draws an icon, and a translated label,
for these eight ids, matched exactly:

| Id | What the Library files it under |
| --- | --- |
| `statistics` | Statistics |
| `economy` | Economy |
| `commanders` | Commanders |
| `new-units` | New Units |
| `disable` | Disable |
| `gameplay` | Gameplay |
| `ai` | AI |
| `maps` | Maps |

Any other id is shown as it was written, with no icon of its own. When the
manifest names no `icon`, the badge uses one tag: the catalogue listing's
tags when that list has any, and otherwise the installed manifest's tags.
The icon is the first tag's, when that id is one of the eight. An id
outside the eight shows no icon, and a later tag is not tried.

### 3.3 Layers

`layers` names the parts of the game the mutator uses. The names, in the
order the Library shows them, are:

| Name | What the manifest means by it |
| --- | --- |
| `files` | Loose files and archives |
| `units` | Unit definitions |
| `weapons` | Weapon definitions |
| `features` | Wrecks and features |
| `sides` | `gamedata/sidedata.tdf` |
| `tables` | The other `gamedata` tables |
| `menus` | Build menus |
| `maps` | Map settings |
| `hacks` | Engine hacks for one match |

What a hook or a rule may write through each of those names is outside
this text. The files a `files` layer may add are
[5. The Files layer](#5-the-files-layer).

### 3.4 Options

`options` is a mapping of at most 16 entries. The key is the option id: a
Lua name, `[a-z][a-z0-9_]*`, of 1 to 32 bytes. The reader accepts the id
even when it is a Lua keyword, such as `end`. Each option is a mapping.
The keys any option may hold
are `type`, `label`, `default`, `values`, `labels`, `min`, `max` and
`step`. A key its type does not use is refused.

`type` is `toggle`, `choice` or `number`. `label` is 1 to 64 bytes with no
control character. The words a player reads are the label.

| Type | Keys | Rule |
| --- | --- | --- |
| `toggle` | `type`, `label`, `default` | `default` is `true` or `false`. |
| `choice` | `type`, `label`, `default`, `values`, and optionally `labels` | `values` holds 2 to 16 entries, all numbers or all identifiers, none repeated. `default` equals one of them. An identifier is 1 to 32 bytes of ASCII letters, digits, `_`, `-` and `.`. `labels`, when written, lists one label of 1 to 64 bytes for each value, in the same order. |
| `number` | `type`, `label`, `default`, `min`, `max`, `step` | `min`, `max`, `step` and `default` are exact numbers, the decimals the strict YAML reader keeps. `min` is below `max`. `step` is above 0. `(max - min) / step` is a whole number of at most 1,000 steps. `default` lies on a step, including either end. |

### 3.5 Strings

`strings` maps a language tag to a translation. A tag is 2 to 35 bytes: a
run of letters, then hyphen-joined parts of letters and digits, in either
case. Each language is a mapping of optional `name`, `description`,
`options` and `labels`. Any other key is refused.

`name` is 1 to 64 bytes with no control character. `description` follows
the description rule. `options` maps an option id to a label of 1 to 64
bytes with no control character. `labels` maps a choice option's id to a
list of value labels, one of 1 to 64 bytes with no control character for
each value, in the same order as `values`. An option id the manifest does
not define is refused, and so are value labels for an option that is not a
choice.

### 3.6 Legacy

Each record in `legacy` is a mapping of `name`, `prefix`, `compatibility`
and `files`.

| Key | Limits |
| --- | --- |
| `name` | 1 to 128 bytes, no control character. The TA: Mutation name. |
| `prefix` | `"{}"` or `"}{"`. |
| `compatibility` | 1 to 8 codes. Each code is 1 to 8 letters and digits, or `*`. `*` may sit beside other codes. |
| `files` | 1 to 16 file names of 1 to 255 bytes, with no control character and no `/` or `\`. |

`oa-tool mutator import` reads TA: Mutation `.tam` files and zips and
writes this list. Its warnings and its flags are
[the tool](../../tools/oa-tool/README.md) and
[the importer](../../src/data/mutator-import/README.md).

### 3.7 A full example

Steady Hands slows every builder. It fits the made-up mod Ridge and the
base game, and it refuses to run beside Long Shadows.

```yaml
oamut: 1
id: steady-hands
name: Steady Hands
version: "1.2"
author:
  name: A. Builder
  email: builder@example.com
description: "Builders take their time.\nThe second line stays in the description. The first line is the summary."
homepage: "https://example.org/steady-hands"
icon: icon.png
tags: [economy, gameplay]
works-with:
  - ridge >= 1
  - ta-3.1c
requires:
  engine: ">= 0.8.5"
  mutators:
    - long-reach >= 1
conflicts-with:
  - long-shadows
layers: [units, files]
options:
  pace:
    type: choice
    label: How slow
    values: [2, 4]
    default: 2
    labels: [Careful, Deliberate]
  announce:
    type: toggle
    label: Announce each builder
    default: false
  extra:
    type: number
    label: Extra metal
    min: 0
    max: 100
    step: 5
    default: 0
strings:
  de:
    name: Ruhige Hände
    description: "Die Bauarbeiter lassen sich Zeit."
    options:
      pace: Wie langsam
    labels:
      pace: [Vorsichtig, Bedächtig]
entry: mutator.lua
legacy:
  - name: Steady Hands
    prefix: "{}"
    compatibility: ["OTA"]
    files: ["steady.sm"]
```

A plain value such as `ridge >= 1` is read as text. A value is quoted
when it would otherwise be something else: a leading `>` is a block
scalar, so `requires.engine` is quoted; `*` is an alias unless it is
quoted; a range that holds a comma is quoted when it sits in a flow
list. A description that holds a line break is one double-quoted string,
with `\n` for the break. A block scalar (`|` or `>`) is refused. The
summary is `Builders take their time.`

## 5. The Files layer

The Files layer hides files that are already in the game and adds files
from the package. Both happen before the match, and the match then reads
ordinary paths. A mask hides. An add folder contributes.

A plan lists `mask` patterns and `add` folders, both in the order written,
and both optional. The mutators are taken in the player's list order. For
each mutator that is kept, the masks are applied first and then the add
folders. A mask is kept on the layer, and every file an earlier mutator
added that it matches is removed. The note names both mutators. The
mutator's own files are added after its masks, so a mask hides files from
earlier mutators and leaves that mutator's own add in place.

An add folder is a relative path inside the package: no `..`, no drive and
no leading slash, and not a link. Each part is matched to the directory's
entries without ASCII case, so `FILES` and `files` are the same folder on
every machine. Two entries that fold to one name refuse the mutator. A
folder that is not there adds nothing and records that the folder is
missing. That is what a plan written as `files/<mod id>` relies on when
that mod has no files of its own.

Inside a folder that exists:

- Only the top of the folder is searched for archives, and only `.ccx` and
  `.ufo` are opened. An archive further down is an ordinary loose file, as
  is any other kind of file.
- `.ccx` archives are opened before `.ufo` archives. Within each group the
  names follow NTFS order: compared byte by byte after ASCII upper-casing,
  with the raw bytes breaking a tie. The earlier name keeps a path both
  archives hold.
- Loose files are walked after the archives. A loose file keeps a path over
  that folder's archives. Listings are sorted by the folded path, so the
  machine's own directory order never decides a file.
- A path this mutator already added from an earlier folder is replaced, with
  no note. A path an earlier mutator added is replaced whole, except a unit
  file, a weapon file and a download file, which a coming change reads as
  records.

The layer lists paths in mutator order and, within a mutator, in the order
the paths were first added. Each folder's new paths are sorted by the
folded path. Replacing a path does not move it. The archives a kept entry
still reads are listed in the order they were opened, and an archive whose
files were all replaced is omitted. The layer's label is `mutators`
followed by the ids that were kept, separated by spaces.

A mutator that fails leaves nothing behind. Its masks, paths, archives and
notes are dropped, and the mutators after it are built as though it were
not in the list. One mutator failing does not fail the others. Two machines
with the same packages and the same list build the same paths, owners,
archive order, masks and label.

### 5.1 Masks

A mask matches a resource path. ASCII case is ignored, and `\` matches `/`.
`*` matches any run of characters inside one segment. `?` matches one
character inside a segment. A segment that is exactly `**` matches any
number of whole segments, including none. `**` anywhere but as a whole
segment is refused.

A pattern is refused when it is empty, longer than 256 bytes, absolute,
holds `:`, holds a NUL, holds an empty, `.` or `..` segment, has a first
segment that is exactly `.backup` in any case, or holds more than 16
`*` and `?` counted together. A refused pattern matches nothing.

| Limit | Value | What it counts |
| --- | --- | --- |
| Masks in one plan | 64 | `mask` patterns one mutator lists |
| Add folders in one plan | 16 | `add` folders one mutator lists |
| Archives in one add folder | 16 | `.ccx` and `.ufo` files at the top of that folder |
| Loose files under one add folder | 16,384 | Files walked under that folder |
| Nested folders under one add folder | 32 | How deep the walk goes |
| Masks in the whole layer | 2,048 | Every kept mutator's masks |
| Paths in the whole layer | 65,536 | Served and hidden paths together |
| Archives in the whole layer | 512 | Archives a kept entry still reads |
| Bytes held in memory | 256 MiB | Content the layer holds in memory. File bytes read from disk or from an archive are not counted here. |

Crossing a plan or folder limit fails that mutator. Crossing the mask,
path or archive limit of the whole layer fails the mutator that would
cross it. The 256 MiB bound is applied when the layer is mounted, and
only to content held in memory. Crossing it refuses the mount.

### 5.2 The allow-list

Every path an add contributes, and every mask, is judged against one
table. The default is to refuse. A path longer than 4,096 bytes is
over-long. A path of 4,096 bytes is not. An empty path, a path that holds
a NUL, an absolute path, a path that holds `:`, and a path with an empty,
`.` or `..` segment, are refused.

Names are compared without ASCII case. `/` and `\` both separate segments.
A first segment `<folder>-<variant>` follows that folder's row, for every
folder in the table. The seven directory names a mod may rename come from
the active layout. In the base game they are `units`, `Weapons`,
`gamedata`, `ai`, `guis`, `unitpics` and `download`. A renamed directory
replaces the base name, and the base name is then an unknown folder.

A folder the table allows as a whole may use a wildcard. A folder the
table allows file by file must name one file in the mask: a wildcard there
is refused. A mask with no wildcard is judged as that one path.

| Path | Verdict | Why |
| --- | --- | --- |
| `units/` | allowed, the whole tree | Read again for every match. The layout's units folder. |
| `Weapons/` | allowed, the whole tree | Read again for every match. The layout's weapons folder. |
| `ai/` | allowed, the whole tree | Read again for every match. |
| `unitpics/` | allowed, the whole tree | Read again for every match. |
| `download/` | allowed, the whole tree | Read again for every match. |
| `features/` | allowed, the whole tree | Read again for every match. |
| `objects3d/` | allowed, the whole tree | Read again for every match. |
| `scripts/` | allowed, the whole tree | Read again for every match. |
| `gamedata/moveinfo.tdf` | allowed | Read again for every match. |
| `gamedata/sidedata.tdf` | allowed | Read again for every match. |
| `gamedata/los.tdf` | allowed | Read again for every match. |
| `gamedata/meteor.tdf` | allowed | Read again for every match. |
| `gamedata/sound.tdf` | read once | Loaded once when the game starts. |
| Any other file under `gamedata/` | not confirmed | Not yet shown to be read again for every match. A mask here names one file. |
| `guis/` | menus only | Build pages and screens change only through a mutator's menus. |
| `maps/` | maps only | Maps change only through a mutator's maps hook. |
| `sounds/` | read once | Loaded once when the game starts. |
| `music/` | read once | Loaded once when the game starts. |
| `palettes/` | read once | Loaded once when the game starts. |
| `fonts/` | read once | Loaded once when the game starts. |
| `bitmaps/` | read once | Loaded once when the game starts. |
| `camps/` | read once | Loaded once when the game starts. |
| `textures/` | not confirmed | Not yet shown to be read again for every match. |
| `anims/<unit><digits>.gaf` | allowed | One level under `anims/`, when the unit name is a unit this game already has, or a unit this mutator or an earlier one still adds. Read again for every match. |
| `anims/<unit>_gadget.gaf` | allowed | The same, for a unit's download-button art. The suffix matches without regard to case. |
| Any other file under `anims/` | interface art | Interface art, which changes with the screens. A mask here names one file. |
| Anything else, including a file at the top | unknown folder | This folder is not one a mutator may change. |

The rule names are `allowed`, `invalid-path`, `menus-only`, `maps-only`,
`read-once`, `not-confirmed`, `interface-art` and `unknown-folder`.

A path whose rule is anything other than `allowed` fails that mutator
when the layer is built. The failure is an English message. It names the
mutator and the mask, folder, archive or file, and it includes the
allow-list's own message. The message starts with the path.

`oa-tool mutator import` does not fail the import for a protected path
inside a TA: Mutation archive. An archive with no such entry is copied
unchanged. An entry on a protected path is left out, and the warning is
`left out <archive>: <path> (<rule>)`. The archive is written again from
the entries that remain. An archive with nothing left is not written.

### 5.3 Tracing lookups

`--trace-lookups FILE` writes every game file and listing the run looks
up, one a line. For a path the mutator layer serves or hides it writes a
second line, `path`, a tab, then `mutator <id>` or
`hidden by mutator <id>`. The option is
[the command line](../command-line.md).

## 6. Rules

A rule selects records and writes their fields. The same records are
written two ways: a Lua block, and a `.sm` file. The two spellings compare
equal when they are the same rules. Mutators apply one after another, in
list order. Each one is applied to the data the mutators above it left.
Within one mutator, its rules apply in file order. How one mutator's
result meets the next is outside this text: the design's Stacking section
is the account of that order, and the detailed rules are written when R06
and V12 land, in O01.

A plan's rules list is a sequence of finished blocks and `sm` paths.
Consecutive blocks become one source. Each `sm` path is its own source.
The bytes come from the package. A path that is absolute, has a drive, or
has a `..` segment is outside the package and is not read. A file that
cannot be read, or that the rule parser reports an error in, refuses the
plan. A warning is kept with the source. A path is read and parsed once,
however many times the list names it.

The blocks of every source together stay within 4,096 blocks, and the plan
holds at most 256 sources. A statement, a name, a field and a quoted text
stay within the rule file's own limits, below. Passing a limit refuses
the plan.

`where` and `:change` copy the table they are given. A loop that reuses
one table builds a different block each time, and changing the table later
changes no block already built. Field names in one block are stored in the
order of those names after ASCII lower-casing, with the raw bytes breaking
a tie. Two changes to one field are two blocks, because one table cannot
hold the same key twice. An unfinished block refuses the plan.

### 6.1 The Lua forms

The engine publishes `where`, `all`, `named`, `on`, `set`, `scale`,
`lock`, `remove`, `append`, `sm` and `empty` for the changes, and `range`,
`is`, `has`, `absent` and `not_` for the needs. `not` is a Lua keyword, so
the need is `not_`.

`on` takes a folder and an extension together. It stores both in lower
case, turns `\` into `/`, trims spaces, and strips one leading `.` from
the extension. `on` by itself selects every record of that target. The
default target, when a block never calls `on`, is the units folder with
the `fbi` extension. `:change` finishes a block. A second `:change` is an
error. A later `where`, `all` or `named` on a block that already has a
selector is refused. Calling `on` on a block that already has a selector
keeps the line of the call that started the block and replaces the folder
and the extension.

| Lua | Rule file |
| --- | --- |
| `where { field = n }` | `field=n,n` in `[Needs]` |
| `where { field = true }` | `field=1,1` |
| `where { field = false }` | `field=!1,1` |
| `where { field = "text" }` | `field="text"` |
| `where { field = range(lo, hi) }` | `field=lo,hi` |
| `where { field = is("text") }` | `field="text"` |
| `where { field = has("text") }` | `field="text"*` |
| `where { field = has("") }` | `field=""*` |
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

`empty` assigns empty text, the record a rule file writes with `field=;`.
`""`, `set("")` and `remove()` remove the field.

### 6.2 A field the record leaves out

When the field is written, a range holds when the text's number is inside
the bounds, inclusive. Text that is not a number reads as zero, the way
the game reads it. An equality holds when the text matches without ASCII
case. A substring holds when the text contains the needle without ASCII
case. An empty needle is contained.

When the field is not written, `has("")` and `absent()` do not read a
default. `has("")` holds exactly when the key is in the file.
`absent()` holds exactly when the key is not. In the rule file those two
are `field=""*` and `field=!""*`. Every other need is read against the
value the loader uses for that key. For a unit, a weapon, or a movement
class that value is the field's default. A key the schema does not hold,
and any other record, reads 0 for a range and empty text for an equality
or a substring. The defaults that are not zero or empty are listed in
[the field schema](../../src/data/field-schema/README.md#defaults-that-are-not-zero-or-empty).

A negated need holds exactly when the positive one does not.

### 6.3 The .sm file

A rule file is text of at most 1 MiB. A longer text is refused before it
is read. Keywords, section names and `<AllDepth2>` match without ASCII
case. A `//` outside quotes runs to the end of the line. `/*` is not a
comment, and it is an error. A line ends at LF, CRLF or a lone CR.
Whitespace is free around `=`, around braces and at the start of a line.
A number is `-` optionally, then digits with an optional fraction, or a
point and digits. An exponent, or a leading `+`, is not a number. The
number is then read by the same reader the game uses for the value.

```ebnf
file        = { directive | block | stray_brace } ;
directive   = ( "folder" | "extension" ) "=" text [ ";" ] ;
block       = "[" header "]" "{" body "}" ;
header      = "*all*" | "*group*" | name ;
body        = { change }                         (* *all* and a named record *)
          | { needs | changes } ;                (* a group, either order *)
needs       = "[Needs]" "{" { need } "}" ;
changes     = "[Changes]" "{" { change } "}" ;
need        = field "=" [ "!" ] ( number "," number
                                | string [ "*" ]
                                | '""' ) [ ";" ] ;
change      = ( field | "<AllDepth2>" ) "=" value [ ";" ] ;
value       = number
          | "*" number [ "(" number "," number ")" ]
          | "^" number
          | string
          | string "+"
          | bare ;
bare        = (* empty, or words with their inner spaces *) ;
string      = '"' { byte - '"' } '"' ;
stray_brace = "}" ;                                  (* lenient, outside a block *)
```

`folder` and `extension` apply to every block after them, and each may be
set more than once. `\` in either becomes `/`, and both are stored in
lower case. The extension is a filename suffix with no leading dot, so
`sidedata.tdf` names one file. Until a file sets them, the target is
`units` and `fbi`. A directive inside a block is an error.

`<AllDepth2>` writes every key of every block directly inside the record,
each key its own field. A weapon's `[DAMAGE]` entries are reached that
way. Keys of the record itself are left alone. A record that has no such
keys still matched. Two spellings of one key inside a block, differing
only in ASCII case, are one field: the reader keeps the newest, and the
change is applied once.

A group's needs are all required. There is no or between them. A group
with no needs matches every record of its target. A statement outside
`[Needs]` and `[Changes]` in a group is a `stray_text` error. The
statement is dropped, and the error refuses the plan and
`oa-tool mutator check`.

### 6.4 Lenient forms

Each of these is kept, and a warning names it. The rest of the file is
still read.

- A statement or directive may end at the end of the line, or at the `}`
  that closes it, with no `;`. Several statements may share a line when a
  `;` separates them, and the next header may follow a `}` on that same
  line.
- The `{` of a block may be on a later line than its header.
- A header whose body holds `[Needs]` or `[Changes]` is a group even when
  the header is not `*Group*`.
- A number written in quotes is a text change. The bytes inside the quotes
  are kept. An empty quoted value with no `+` removes the field.
- Spaces inside an unquoted value, such as `*7 (2,15)`, are ignored.
- A need whose low bound is above its high bound is kept. It matches
  nothing, or everything when it is negated.
- A group with no `[Needs]` is kept. It changes every record of its target.
- A second `[Needs]` or `[Changes]` in one group is merged.
- A leading byte-order mark is skipped.
- A change whose value is a bare word or words is a text assignment. The
  words are trimmed at both ends and their inner spaces stay. A value that
  starts like a number, a scale, a clamp, a meta value or a quote keeps
  the stricter reading, including a malformed one.
- A change with nothing after `=` assigns empty text. That is the Lua
  value `empty`. It is distinct from `""`, which removes the field.
- A need written `""` or `!""` is equality with empty text, negated when
  it is written `!""`. `""*` and `!""*` stay the presence tests
  `has("")` and `absent()`.
- A `}` outside any block is ignored, including one after a middle block
  and one after the last block. A `}` that ends a block still ends it.

A bad statement is dropped, and reading continues. A block whose header or
braces are broken is dropped. A `[` inside `[Needs]` or `[Changes]` is an
unknown section: a section cannot hold a section. That section's braces
are skipped.

### 6.5 Limits and diagnostics

| Limit | Value |
| --- | --- |
| The file | 1 MiB |
| Blocks kept | 4,096 |
| Statements in one block | 4,096 |
| One name, field, quoted text or bare text | 1,024 bytes |
| Diagnostics stored | 1,000. The last one stored is the notice that collection stopped. |

The twelve lenient codes are warnings. A warning keeps the statement or
the block. Every other code is an error. An error drops the statement, or
the block when its braces or header are broken, and it refuses the plan
and `oa-tool mutator check`. `too_many_diagnostics` is an error, so a file
with more than 1,000 findings is refused even when each earlier finding
is a warning.

| Code | Severity | Meaning |
| --- | --- | --- |
| `missing_semicolon` | warning | a kept statement was not closed with `;` |
| `group_without_asterisks` | warning | a header without `*Group*` holds a group body |
| `quoted_number` | warning | a change's quoted text is a number |
| `spaces_in_value` | warning | spaces inside an unquoted value were ignored |
| `empty_range` | warning | a need's low bound is above its high bound |
| `group_without_needs` | warning | a group has no needs, so it matches every record |
| `repeated_section` | warning | a second needs or changes section was merged |
| `byte_order_mark` | warning | a leading byte-order mark was skipped |
| `unquoted_text` | warning | bare words read as a text assignment |
| `empty_value` | warning | a change with nothing after `=`, read as empty text |
| `empty_text_need` | warning | `""` or `!""` in a need, equality with empty text |
| `stray_closing_brace` | warning | a `}` outside any block, ignored |
| `unknown_section` | error | a section other than needs or changes, or a section inside a section |
| `unclosed_block` | error | a header or a brace was never closed |
| `unexpected_brace` | error | a brace where a block did not call for one |
| `directive_in_block` | error | `folder` or `extension` inside a block |
| `bad_number` | error | a number the grammar does not allow |
| `bad_need` | error | a need that matches no need form |
| `bad_change` | error | a change that matches no change form |
| `need_form_in_changes` | error | a need written where a change belongs |
| `change_form_in_needs` | error | a change written where a need belongs |
| `clamp_low_above_high` | error | a scale's low clamp is above its high clamp |
| `unterminated_string` | error | a quote was never closed |
| `block_comment` | error | `/*`, which is not part of the language |
| `stray_text` | error | text that is not a directive, a block or a statement, or a statement outside `[Needs]` and `[Changes]` in a group |
| `too_large` | error | the text is longer than 1 MiB |
| `too_many_blocks` | error | more than 4,096 blocks |
| `too_many_statements` | error | more than 4,096 statements in one block |
| `text_too_long` | error | a name, field or quoted text is longer than 1,024 bytes |
| `too_many_diagnostics` | error | the list stopped at 1,000 diagnostics |

The tool reads a rule file up to 4 MiB and then this reader refuses a text
longer than 1 MiB, so either bound refuses the file.

### 6.6 How a number is written

A number is written only with the TDF number spelling, and only when it is
finite and different from the field's current number. The current text
counts as that number when, after spaces or tabs, it has an optional sign
and then a digit. A leading point is not a number. A factor does not
treat a word as zero.

### 6.7 Which records a rule reaches

A target is a folder and a filename suffix. The suffix matches the end of
the file name, without ASCII case, so `sidedata.tdf` reaches that file and
`tdf` reaches every file of that ending. The folder is not recursive:
`features/corpses` reaches `features/corpses/a.tdf` and not a file in a
folder below it.

A folder whose first segment is one of the seven base directory names
(`units`, `weapons`, `gamedata`, `ai`, `guis`, `unitpics`, `download`)
means the active layout's folder of that kind. The suffix `fbi` means the
layout's unit extension. Any other folder and suffix are kept as the rule
wrote them. Until a block sets a target, the target is the units folder
and the unit extension.

Unit files hold one record, the first `[UNITINFO]` section. A named block
matches `UnitName` without ASCII case, and the record is reported under
that name when the key is present, otherwise under the section name. Every
other folder treats each top-level section as a record, and a named block
matches the section name without ASCII case.

The layout's `gamedata/moveinfo.tdf` is read as movement classes. Units
and weapons win over that path when the same file could be read both ways.
A weapon file's `[DAMAGE]` block is reached by `<AllDepth2>`, not as a
record of its own.

## 10. The sandbox

A mutator script runs as Lua 5.4.9 in one sandbox. The sandbox owns one
state. Two sandboxes share no modules, no globals and no budgets. The
mutator profile gives that state the default instruction allowance and
the default memory cap, and it adds no globals of its own.

A script has no file, no clock, no environment, no process and no
network. Every chunk is loaded as text. A failure comes back as a value
that names the package, the phase, the chunk and the line:
`<package>: <phase>: <chunk>:<line>: <message>`. The chunk and the line
are left out when the chunk is unknown. A failed phase sticks. Later
phases return that same error and run nothing.

`pcall` and `xpcall` cannot keep a spent phase alive. Once the
instruction allowance is spent, the real-time limit is past, or the
sandbox has failed, `xpcall` returns the error and does not run the
handler the script passed. An instruction error, a real-time error or a
memory error is reported even when the script's own `pcall` caught it.

The host's own account of these rules, including the measurement behind
the time limit, is
[the script host](../../src/data/script-host/README.md).

### 10.1 Libraries

The libraries a script can call are the base library (`_G`), `string`,
`table`, `utf8` and `math`. `io`, `os`, `debug`, `package` and
`coroutine` are not opened.

These names are nil: `load`, `loadfile`, `dofile`, `collectgarbage`,
`string.dump`, `string.pack`, `string.unpack` and `string.packsize`.
Pack sizes and alignment differ between the machines the engine builds
for, so a script cannot pack bytes itself.

`setmetatable` installs a copy of the metatable. The copy keeps every
field the caller's table had at that moment, including `__index`, so a
methods table that `__index` points at still sees fields added later. A
later write to the caller's own table does not change the copy.
`getmetatable` returns the copy, or the `__metatable` field when the
caller set one. Making the copy counts 64 instruction units, and one
unit per field copied.

A metatable that already holds `__gc` or `__mode` is refused:
`setmetatable: a metatable with __gc is refused` and
`setmetatable: a metatable with __mode is refused`. A protected
metatable is refused with `cannot change a protected metatable`.
`rawset` of `__gc` or `__mode` is refused on every table, with
`rawset: the key __gc is refused` and `rawset: the key __mode is refused`.
A later write of `__mode` on the copy is refused with
`the key __mode is refused`. A script cannot make a weak table. A later
write of `__gc` on the copy is not refused.

`print` writes nothing to a file. The host keeps at most 256 lines and
16,384 bytes of that text. A line is cut only when it is longer than
16,384 bytes, and then to 16,384. A line that does not fit in the bytes
still left is dropped whole, and that drop closes `print`. Every later
line is dropped too. Kept lines may be dropped so the note
`1 line left out` or `<n> lines left out` fits.

### 10.2 Loading code

The entry script and every module are source. An empty file is source.
A file whose first byte is `0x1B` is precompiled and is refused before
the loader sees it, with
`<path> is precompiled Lua; packages hold Lua source only`.
A file longer than 1,048,576 bytes is refused with
`<path> is <n> bytes of source; the most is 1048576`.

Packing and checking load a chunk in a throwaway state and do not run
it. A chunk that would fail or print when run is still accepted there.
Precompiled bytes and a file past the source cap are refused first.

### 10.3 require

`require` takes one name of at most 64 bytes: lower-case letters,
digits and `_`, in parts joined by dots, with no empty part. `util.grid`
reads `lua/util/grid.lua` through the package reader. The path is matched
byte for byte, so a file system that folds case cannot change which file
loads. A module that has already been loaded in this state is returned
again and is not read again.

| What went wrong | Message |
| --- | --- |
| The name is not a module name | `require: "<name>" is not a module name: lower-case letters, digits and _ in parts joined by dots`. A name longer than 96 bytes is shown as its first 96. |
| The package has no such file | `require: the package holds no lua/<parts>.lua` |
| The file is longer than 1,048,576 bytes | `lua/<parts>.lua is <n> bytes of source; the most is 1048576` |
| The file is precompiled | `lua/<parts>.lua is precompiled Lua; packages hold Lua source only` |
| Modules are nested more than 32 deep | `require: modules are nested more than 32 deep` |
| The file could not be read | `require: the package file could not be read` |

A cycle names the chain, for example
`require: lua/a.lua needs lua/b.lua, which needs lua/a.lua`.
The first module is joined with ` needs `, and
each later one with `, which needs `. `require` counts one instruction
unit for every frame it walks, on every call.

### 10.4 Budgets

Each phase starts again from the full instruction allowance and the full
real-time limit. The memory cap is the Lua allocation of the state, and
it is not handed out again for the next phase. Text the host has copied
out, including the lines `print` kept, is not part of that allocation.

| Budget | Value |
| --- | --- |
| Instructions in one phase | 50,000,000 |
| Memory | 64 MiB (67,108,864 bytes) |
| Real time in one phase | 10,000 milliseconds (10 seconds) |
| One string | 65,536 bytes. `string.rep` uses the same cap, separators included. |
| One source file | 1,048,576 bytes |
| A `require` name | 64 bytes |
| `require` nesting | 32 modules loading in one chain |
| Lines `print` keeps | 256 |
| Bytes `print` keeps | 16,384 |
| One error's stack listing | 2,048 bytes |

The instruction count is read every 1,000 instructions. A short phase
can report 0. Passing the allowance stops the phase with
`the script ran past its budget of <n> instructions`.
Passing the memory cap stops
it with `the script used more than its <n> MiB of memory`. For the
default cap that number is 64.

### 10.5 Charged work

Library work that would otherwise run in C is counted against the same
instruction allowance. A unit counts one-for-one against that allowance.
A comparison that costs 2 units is about the time of one or two
instructions of the interpreter. A new string longer than 40 bytes
counts one unit per 32 bytes of its length, rounded up. A string of 40
bytes or fewer is not counted: making one allocates only when no live
copy exists, which depends on when the collector last ran, and the
instruction or call that makes it bounds that work. The header in front
of the bytes is not counted. Freeing a string does not give the units
back. The same script spends the same count on every build and every run.

Pattern matching counts its own steps. A step is one entry into the
matcher, one character a `*`, `+` or `-` repetition tries, or one
character a `%b` pair scans. `string.find`, `string.match`,
`string.gmatch` and `string.gsub` stop with the instruction-budget error
when that count reaches the allowance, and a `pcall` around the call
does not let the phase continue. Within the allowance they return the
same values, of the same types, and raise the same errors, as Lua
5.4.9. A call on a string value, such as `("x"):find`, uses the same
functions.

| Call | Units |
| --- | --- |
| `string.byte` | One per byte returned. |
| `utf8.codepoint` | One per code point returned. |
| `utf8.char` | One per code point. |
| `utf8.codes` | One per 32 bytes the iterator walks, continuation bytes included, on every call, including the call that finds the end. |
| `utf8.len`, `utf8.offset` | One per 32 bytes of the interval they walk. |
| A plain `string.find` | One per 32 bytes each scan and each comparison examines. |
| `require` | One per frame walked, on every call. |
| `table.move`, `table.insert`, `table.remove`, `table.concat`, `table.pack`, `table.unpack` | One per element moved or visited. |
| `table.sort` | Every comparison. Two strings cost one unit per byte of the two lengths, and at least 2. Any other comparison costs 2. A comparison function written in C is counted too. |
| `next` | 4,096 per call. |
| `pairs` | 4,096 for the walk that collects the keys, and each ordering of two keys costs the same as a `table.sort` comparison. |
| `rawequal` of two strings | One per 32 bytes of the two lengths. |
| `tonumber` of a string | One per 32 bytes of that text. |
| `string.format` | One per format byte, 2 per value it converts, and one per byte of each text `%s` or `%q` copies. |
| `math.max`, `math.min` | One comparison, as `table.sort` counts it, for each value after the first. |
| `warn` | 2 per argument. |
| `setmetatable` | 64, plus one per field copied. |

A table call whose range is longer than the default memory cap could
hold, one element per stored value and its tag, is refused before it
loops. The message is
`<function>: a range of <n> elements is past the most of <limit>`.

`next`, `rawequal`, `tonumber`, `warn`, `string.byte`, `string.format`,
the charged `table` and `utf8` calls and `utf8.char` are copies of Lua
5.4.9's with the count inside. The script calls them directly, so an
error names the function and starts with the script's chunk and line.
A method call such as `("%d"):format(x)` numbers its arguments as Lua
does.

Comparing two strings with `<` or `==` is one instruction of the
interpreter, and these charges do not see it. Equality of two 65,536-byte
strings takes about 1.3 microseconds on the machine where this was
measured. Ordering two such strings of ordinary bytes takes about 3.3
microseconds. Ordering two strings of that length filled with zero bytes
takes about 480 microseconds. `table.sort` and `pairs` charge that
comparison by length, so a sort of those zero-filled strings stops
inside the default allowance. A bare `<` of two zero-filled strings does
not. The real-time limit is what stops a phase that hides its work in a
step the charges do not see.

### 10.6 Strings

No string may be longer than 65,536 bytes. The cap is applied in the
allocator, because the interpreter has no switch that stops a longer
string. Each state measures the header in front of the bytes once, from
one 200-byte string, before the libraries open, so the cap compares the
string's length. A longer string ends the phase with
`the script made a string of <n> bytes; the most is 65536`,
at the line that made it.

`string.rep` refuses earlier with
`string.rep would make a string of <n> bytes; the most is 65536`.
`table.concat` refuses with
`table.concat would make a string of more than 65536 bytes`.
`string.gsub` refuses with
`string.gsub would make a string of more than 65536 bytes`
when it can see the size coming. A `pcall` that
catches the refusal does not let the phase continue. A value the host
pushes counts as a string too, so text longer than 65,536 bytes fails
the script that receives it. A string literal that long fails the chunk
that contains it. `print` cuts a long value instead of refusing it.

### 10.7 Text, order and numbers

`tostring` of a float is the same characters on every machine. A NaN of
either sign is `nan`, the infinities are `inf` and `-inf`, and every
finite value is `%.14g`, with `.0` added to an integral value. Joining a
float to a string uses that same text.

A table, a function or any other value without `__tostring` prints as
its kind, a colon and `hidden`, for example `table: hidden`. It does not
print where the value sits. `__tostring` is used when the metatable has
one, and `__name` is used as the kind when it has no `__tostring`.

`string.format` refuses `%p`, because it would show where a value sits,
with `string.format: %p would show a memory address`. Lua 5.4.9 has no
`%F`. `%a` and `%A`, `%e` and `%E`, `%f`, and `%g` and `%G` are refused
when the number is a NaN or an infinity, with
`string.format: cannot write nan or an infinity with %<letter>; test the number first, or use tostring`.
A finite number,
`%q`, `%s`, `%d`, `%i`, `%x` and `%c` are unchanged. `%q` of an
infinity is `1e9999` or `-1e9999`, and `%q` of a NaN is `(0/0)`.

A sandbox opens only when the C library's collation, numeric and
character-type locales are each `C` or `POSIX`. Any other setting is
refused at open, naming the category and the locale, for example
`the C library's numeric locale is de_DE.UTF-8; scripts need C`. String
comparison and `table.sort` on strings then follow byte order, number
text is read and written with `.`, and `upper`, `lower` and the pattern
classes such as `%a` treat a byte past 127 the same way on every
machine. Hexadecimal numbers use Lua's own conversion.

Every string hash uses one constant seed, `0x4F414C55`. `table.sort`
does not draw a pivot from the clock. Each basic floating-point
operation is rounded as written. A Visual Studio build uses `/fp:strict`.
Every other build, including a MinGW build, uses `-fno-fast-math` and
`-ffp-contract=off`. There is no fused multiply-add and no
value-changing optimisation.

`pairs` visits number keys in ascending order, then string keys in byte
order, then `false` and then `true`. The key list is taken when `pairs`
is called. A value changed before the walk reaches it is the value the
walk sees, and a key whose value became nil is skipped. Keys added
during the walk are not visited. A table whose keys include a table, a
function or any other type with no fixed order raises
`pairs: this table has keys of type <type>, which have no fixed order; keep such values in a sequence and walk it with ipairs`.
A table with a `__pairs`
metamethod keeps that metamethod. `ipairs` is unchanged. `next` returns
the key the interpreter would, so `next(t) == nil` is still the
emptiness test. The hash seed makes the same history of a table walk
the same way on every machine, and `next` follows that order. A key
that is a table has no order two machines share, and `pairs` refuses a
table that holds such a key. A mutator that lets the last write in a
loop win, or that joins text in loop order, uses `pairs` or a sequence.

`math` is Lua 5.4's library, with `pi`, `huge`, `maxinteger` and
`mininteger`. The compatibility names `cosh`, `sinh`, `tanh`, `pow`,
`frexp`, `ldexp` and `log10` are absent. `sin`, `cos`, `tan`, `asin`,
`acos`, `atan`, `exp` and `log` (every base), and the `^` operator, are
computed by the engine, so a script gets the same bits on every machine.
Sine, cosine and tangent are rounded once to a double. An angle at or
above 2,147,483,648 in magnitude, infinity included, is refused:
`sin: the angle must be below 2147483648 in magnitude`, and the same
sentence for `cos` and `tan`. A NaN comes back as a NaN. `log` of a
power of two, and of `10^k` for k from 0 to 22, is the exact integer.
An exponent of 2 is an IEEE multiply, and an exponent of 0.5 is an IEEE
square root. An integer exponent of magnitude at most 256 is repeated
squaring. A larger integer exponent is `exp(y · log x)`, and a negative
base keeps the sign an odd integer calls for. `sqrt`, `floor`, `ceil`,
`fmod` and the four basic operations are the IEEE results.

`math.random` is Lua 5.4's generator. It starts from
`OpenOptions::random_seed`, which defaults to 0. Nothing opens a mutator
sandbox with the game's seed. `math.randomseed()` with no argument
returns to `OpenOptions::random_seed`. It does not read a clock. Zero is
a defined sequence. A call with one integer, or two, sets a new seed
from those integers.

### 10.8 The time limit

Each phase stops when it has run longer than its real-time limit. The
default is 10 seconds. A steady clock is read when the phase starts and
checked in the instruction hook, every 100 instructions, and in every
library charge. The hook counts the allowance on every tenth of those
calls, so the count still steps by 1,000 instructions. There is no
thread and no signal.

The default message is
`<phase> ran for more than 10 seconds; its work is too large or never ends`.
A limit of one second says `second`. A
limit that is not a whole number of seconds names that length in
milliseconds, and one millisecond says `millisecond`. A `pcall` or
`xpcall` cannot keep the phase going. A phase that finishes inside the
limit has the same results, the same charges and the same messages as
it would with no clock. The limit only stops the script. A result that
comes back is unchanged.

The check lands at the next hook or the next charge, so a phase can run
past the limit by the work of 100 instructions, or of one charge. Most
instructions take well under a microsecond. The slowest step found is
an assignment that makes a table rebuild itself: 10 to 30 milliseconds
of processor time for a table of a million keys, about the largest
whose rebuild fits the default memory cap. A loop can do that every 4
instructions, 25 times between two reads of the clock, so it can run a
quarter to three quarters of a second of processor time past the limit,
and longer in real time on a machine busy with other work. On the
machine where this was measured (10 cores, load averages of 20 to 80),
loops that rebuild such a table stopped 0.01 to 0.9 seconds after a 2
second limit, and in three runs at the busiest 2.4 to 2.9 seconds after
it. A loop of 64 KiB zero-filled comparisons stopped within 60
milliseconds of a 300 millisecond limit, and a tight loop within the
same millisecond.

A script near the limit can pass on one machine and fail on another.
Every machine must produce the same result.

## 13. Versioning

`oamut` is the format version. The only value the reader accepts is the
integer `1`, written `oamut: 1`. `1.0` and `"1"` are refused, and so is
any other number, with `oamut must be 1`. A package of another format
is not read with the keys of this one left out.

`version` is the mutator's own version, not the format. Its grammar,
and the grammar of a condition's range, are
[3.1 Versions, ranges and conditions](#31-versions-ranges-and-conditions).

`requires.engine` is optional. It is a string of at most 64 bytes, one
to four comparisons of a three-part release `MAJOR.MINOR.PATCH`. Each
part is a decimal from 0 to 65535 with no leading zero. The comparisons
are `>=`, `>`, `<=`, `<` and `=`, separated by commas that spaces may
surround. Every comparison has to hold. A value that starts with `>`
is a block scalar unless it is quoted, and the manifest reader refuses
that before the range is read, so the value is written in quotes:

```yaml
requires:
  engine: ">= 0.8.0"
```

Reading the manifest does not ask whether this build meets the range.
Opening the package does, which is what the installer does and what
`oa-tool mutator pack` and `oa-tool mutator check` do. The build's
version is the three-part release that build was made with.

When the range is read and this build misses it, the player sees
`needs Open Annihilation <range>; this is Open Annihilation <build>`.
The range is one phrase for each comparison, joined with `, `:

| Comparison | Phrase |
| --- | --- |
| `>= 0.8.0` | `0.8.0 or later` |
| `> 0.8.0` | `later than 0.8.0` |
| `<= 0.9.0` | `0.9.0 or earlier` |
| `< 0.9.0` | `before 0.9.0` |
| `= 0.8.0` | `exactly 0.8.0` |

A stored requirement the range reader cannot read is refused with
`Its engine requirement cannot be read.`
