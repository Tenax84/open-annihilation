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
this text.

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
