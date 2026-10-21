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
   on a fast one. The loadout digest refuses a match whose machines
   disagree.

## 2. The package

A mutator is one `.oamut` file, or the folder that file unpacks to. The
archive is the zip every package kind uses. The names it may hold, and the
limits on its size, are that zip's. They are named below where a mutator
meets them. They are not defined a second time.

The manifest is the strict YAML a mod profile uses
([3.3 Strict YAML rules](../mods/oamod-standard.md#33-strict-yaml-rules)).
One document, one top-level mapping, no anchors and no block scalars.
`oamut.yaml` is at most 256 KiB.

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

`oa-tool mutator pack FOLDER` packs a folder whose top holds `oamut.yaml`.
`oa-tool pack FOLDER` does the same when it finds that manifest. The
manifest is the first entry. Every other file follows in byte order of its
path. A file whose extension, without regard to case, is `png`, `jpg`,
`jpeg`, `gif`, `ogg`, `mp3`, `zip`, `gz`, `bz2`, `xz`, `7z`, `oamod`,
`oalang` or `oamap`, and an empty file, is stored. Every other file is
deflated. The same files give the same bytes. The default name is
`<id>-<version>.oamut`, written in the current folder. `--out FILE` names
another file. `--force` replaces a file that is already there.
`--game-dir` with a mutator is a usage error.

Before the file is written, the manifest is read, every Lua file is
compiled, and every rule file is parsed. Nothing is run, and no rule is
applied. A problem is printed and the file is not written. A manifest
problem reads `oamut.yaml:<line>:<column>: <key>: <message>`. A script or
a rule problem reads `<path>:<line>: <message>`.

`oa-tool mutator check FILE` checks a `.oamut` and reports the facts a
catalogue lists. `oa-tool check FILE` does the same for a file it
recognises as a mutator. `--json` prints one object. Without it, each fact
is a `key: value` line and the last line is `result: ok` or
`result: refused`. The command exits 0 when an install would accept the
package, and 1 when an install would refuse it. A file of another kind is
a usage error for `mutator check`. The facts, and the way they are
spelled, are [Packages](../registries/packages.md). A mutator's revision
in that report is null: a mutator package has no packaging revision.

### 2.3 Where it installs

Into `Documents/Open Annihilation/Mutators/<id>`, by the manifest's `id`,
never by the file's name. Installing does not turn the mutator on. The
player turns it on when setting up a game.

Where that folder is on each system, how a catalogue package differs from
a file the player opened, and the questions an install asks, are
[Packages](../registries/packages.md). This standard does not repeat them.
The revision stored for an installed mutator is 0.

### 2.4 Size limits

These limits are the ones the installer applies to every package, a
mutator included. Crossing one refuses the package before anything is
written.

| Limit | Value |
| --- | --- |
| Manifest | 256 KiB |
| Folders, including folders a path only passes through | 16,384 |
| Unpacked size | 4 GiB |
| Unpacked size against the file's own size | Above 64 MiB unpacked, and more than 200 times the package file, is refused |
| Free space | The unpacked files, and 64 MiB more |
| One path part | 1 to 255 bytes |

A name that cannot be unpacked on every system is refused: UTF-8, no
control character, no `< > : " | ? * \`, not ending in a dot or a space,
and not a name Windows keeps for a device. Two names that differ only in
the case of their ASCII letters are refused, and so is a file and a folder
of one name. A link, a special file, an encrypted entry, and an entry
packed otherwise than stored or deflated, are refused. An id that names a
device on Windows is refused. An unpacked path longer than the system
opens, less 16 characters, is refused.

A mutator also meets these limits:

| Limit | Value |
| --- | --- |
| One Lua file, the entry included | 1 MiB. An empty file is source. A file whose first byte is `0x1B` is precompiled Lua and is refused. |
| The icon, when named | A PNG, square, at least 1 pixel and at most 256 pixels on a side, and at most 256 KiB. Every icon failure shares one sentence. |
| `requires.engine`, when written | This build's version meets the range. The refusal names the range and this build. |

`oa-tool mutator check` and `oa-tool mutator pack` add two checks the
installer does not. Each Lua file must compile. Each rule file must parse,
and a rule file larger than 4 MiB is refused before it is parsed. The rule
reader itself refuses a text longer than 1 MiB, so a rule file past 1 MiB
is refused either way. A warning in a rule file does not refuse the
package. An error does. Neither command runs a script or applies a rule.

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
| `author.email` | string | no | At most 254 bytes | One `@`, text on both sides, a dot in the part after it, and no space or control character. |
| `description` | string | no | 1 to 2,000 bytes | A line feed is allowed. No other control character. The first line, cut to 200 bytes on a UTF-8 boundary, is the summary the Library and the catalogue show. |
| `homepage` | string | no | 1 to 256 bytes | An `http` or `https` address, the scheme in either case, with at least one byte after the scheme and no space or control character. |
| `icon` | string | no | A package path ending `.png` | Parts separated by `/`. No part is empty, `.` or `..`. No leading `/` or `\`, and no `\` anywhere. The installer checks the pixels. |
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
| `new-units` | New units |
| `disable` | Units turned off |
| `gameplay` | Gameplay |
| `ai` | Computer players |
| `maps` | Maps |

Any other id is shown as it was written, with no icon of its own. When the
manifest names no `icon`, the Library draws the icon of the first of these
ids the mutator carries.

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
Lua name, `[a-z][a-z0-9_]*`, of 1 to 32 bytes, so a script reads
`ctx.options.<id>`. Each option is a mapping. The keys any option may hold
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

`name` is 1 to 64 bytes. `description` follows the description rule.
`options` maps an option id to a label of 1 to 64 bytes. `labels` maps a
choice option's id to a list of value labels, one of 1 to 64 bytes for
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
| `compatibility` | 1 to 8 codes of 1 to 8 letters and digits, or the one entry `*`. |
| `files` | 1 to 16 file names of 1 to 255 bytes, with no `/` or `\`. |

`oa-tool mutator import` reads TA: Mutation `.tam` files and zips and
writes this list. Its warnings and its flags are
[Packages](../registries/packages.md) and the tool's own help.

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
  - "ridge >= 1"
  - ta-3.1c
requires:
  engine: ">= 0.8.5"
  mutators:
    - "long-reach >= 1"
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

A range is quoted, as `"ridge >= 1"` is, because a plain `>` starts a
block scalar, which the strict reader refuses. A description that holds a
line break is one double-quoted string, with `\n` for the break. A block
scalar (`|` or `>`) is refused. The summary is `Builders take their time.`

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
  no note. A path an earlier mutator added is replaced, and the note names
  the later mutator as the one that won.

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
holds `:`, holds a NUL, holds an empty, `.` or `..` segment, starts with
`.backup`, or holds more than 16 `*` and `?` counted together. A refused
pattern matches nothing.

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
| Bytes held in memory | 256 MiB | The content the layer keeps in memory, taken together, when the layer is mounted |

Crossing a plan or folder limit fails that mutator. Crossing the mask,
path or archive limit of the whole layer fails the mutator that would
cross it. The 256 MiB bound is applied when the layer is mounted.

### 5.2 The allow-list

Every path an add contributes, and every mask, is judged against one
table. The default is to refuse. A path of 4,096 bytes or more is
over-long. An empty path, a path that holds a NUL, an absolute path, a
path that holds `:`, and a path with an empty, `.` or `..` segment, are
refused.

Names are compared without ASCII case. `/` and `\` both separate segments.
A variant folder `<directory>-<variant>` follows its base folder. The seven
directory names a mod may rename come from the active layout. In the base
game they are `units`, `Weapons`, `gamedata`, `ai`, `guis`, `unitpics` and
`download`. A renamed directory replaces the base name, and the base name
is then an unknown folder.

A folder the table allows as a whole may use a wildcard. A folder the
table allows file by file must name one file in the mask: a wildcard there
is refused. A mask with no wildcard is judged as that one path.

| Path | Verdict | Why |
| --- | --- | --- |
| `units/`, and a variant | allowed, the whole tree | Read again for every match. The layout's units folder. |
| `Weapons/`, and a variant | allowed, the whole tree | Read again for every match. The layout's weapons folder. |
| `ai/`, and a variant | allowed, the whole tree | Read again for every match. |
| `unitpics/`, and a variant | allowed, the whole tree | Read again for every match. |
| `download/`, and a variant | allowed, the whole tree | Read again for every match. |
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
when the layer is built. The failure names the mutator and the mask,
folder, archive or file, and it includes the allow-list's own message.
The message starts with the path. The player sees a translated sentence
chosen from the rule.

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
equal when they are the same rules. Several mutators' rules are combined
in one pass. The numbers do not depend on the order of the mutators.
Text that is appended does: appends follow the list.

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

A need is decided on the record as the pass found it, before any write, so
one rule's change cannot decide which records another rule selects.

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
string      = '"' { byte - '"' } '"' ;
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
with no needs matches every record of its target. A group whose body holds
a statement outside `[Needs]` and `[Changes]` drops that statement.

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

Warnings keep the statement or the block. Errors drop the statement, or
the block when its braces or header are broken.

| Code | Meaning |
| --- | --- |
| `missing_semicolon` | a kept statement was not closed with `;` |
| `group_without_asterisks` | a header without `*Group*` holds a group body |
| `quoted_number` | a change's quoted text is a number |
| `spaces_in_value` | spaces inside an unquoted value were ignored |
| `empty_range` | a need's low bound is above its high bound |
| `group_without_needs` | a group has no needs, so it matches every record |
| `repeated_section` | a second needs or changes section was merged |
| `byte_order_mark` | a leading byte-order mark was skipped |
| `unquoted_text` | bare words read as a text assignment |
| `empty_value` | a change with nothing after `=`, read as empty text |
| `empty_text_need` | `""` or `!""` in a need, equality with empty text |
| `stray_closing_brace` | a `}` outside any block, ignored |
| `unknown_section` | a section other than needs or changes, or a section inside a section |
| `unclosed_block` | a header or a brace was never closed |
| `unexpected_brace` | a brace where a block did not call for one |
| `directive_in_block` | `folder` or `extension` inside a block |
| `bad_number` | a number the grammar does not allow |
| `bad_need` | a need that matches no need form |
| `bad_change` | a change that matches no change form |
| `need_form_in_changes` | a need written where a change belongs |
| `change_form_in_needs` | a change written where a need belongs |
| `clamp_low_above_high` | a scale's low clamp is above its high clamp |
| `unterminated_string` | a quote was never closed |
| `block_comment` | `/*`, which is not part of the language |
| `stray_text` | text that is not a directive, a block or a statement |
| `too_large` | the text is longer than 1 MiB |
| `too_many_blocks` | more than 4,096 blocks |
| `too_many_statements` | more than 4,096 statements in one block |
| `text_too_long` | a name, field or quoted text is longer than 1,024 bytes |
| `too_many_diagnostics` | the list stopped at 1,000 diagnostics |

`oa-tool mutator check` refuses the package when a diagnostic is an error.
A warning does not refuse it. The tool reads a rule file up to 4 MiB and
then this reader refuses a text longer than 1 MiB, so either bound refuses
the file.

### 6.6 How changes combine

Within one mutator, sources in plan order and blocks and changes in file
order, the last change of a field decides its kind. An assignment resets
that mutator's factors and clamps. Factors then multiply onto whatever is
current. Clamps join, taking the lower low and the higher high. A meta
value, written `lock` or `^`, replaces the numeric state and survives
later numeric changes; a later meta value replaces the earlier one. Text
assignments replace, appends extend, and a removal clears. An append after
a removal becomes an assignment of the appended text, and an append after
an assignment extends that assignment. A change of the other kind discards
what came before. That discard inside one mutator is not noted.

Across mutators:

- When any contribution is a meta value, the result is the mean of the
  meta values. Every other change, numeric or text, is dropped. Only the
  mutators that supplied a meta value are listed on the write.
- Otherwise the base is the mean of the assignments, or the field's
  current value when there is none. That base is multiplied by the product
  of the factors, then clamped to the lowest low and the highest high of
  every clamp. Sums run in mutator list order. Each mutator multiplies its
  own factors in file order. Those products then multiply in a fixed
  order: the exact bits of the factor, smaller first, and, when the bits
  match, the rule text of the scales (the source name and line of each
  one, in file order). A mean is the sum divided by the count. The clamp
  is applied to the combined product, not to each mutator before the
  product.
- A factor on a field that is absent, and that no assignment creates,
  does nothing. An assignment or a meta value creates the field, and other
  mutators' factors then apply to that base.
- Text changes apply only when no numeric change touches the field.
  Appends add in list order, including when the field is absent. An
  assignment or a removal takes effect only when it is the only text
  change. Two assignments, two removals, or one of each, cancel, and the
  field stays as it was. Appends alongside an assignment or a removal
  still append; the assignment or removal is noted and does not write. A
  mutator that contributed nothing to the value actually written is not
  listed on it.

A number is written only with the TDF number spelling, and only when it is
finite and different from the field's current number. The current text has
to start, after spaces or tabs, with a sign and a digit before it counts
as that number: a factor does not treat a word as zero. A result that is
not finite is not written, and a clamp is not allowed to pull an infinite
result back into range. A combined value that is written outside the clamp
one mutator gave on its own is noted, naming that mutator, its range, and
the range that was used.

One field is written at most once. A key the record does not hold is
written with the spelling that sorts first by raw bytes among the changes
that name it, so `EnergyStorage` precedes `energystorage` and the file's
bytes do not depend on list order.

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
