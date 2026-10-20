# data/mutator-import

Turns TA: Mutation mutators into mutator packages. A `.tam`, its rule file
and its archives become `oamut.yaml`, `rules/<id>.sm`, archives under
`files/` and a `mutator.lua` that adds those files. The rule file is copied
unchanged. An archive with no protected entry is copied unchanged. An
archive entry on a protected path is left out and named in a warning.

`oa-tool mutator import` is the command that reads the files. This library
sees only names and bytes.

## Entry points

Target `oa-data-mutator-import`, alias `oa::data::mutator_import`, namespace
`oa::data::mutator_import`. Header
`oa/data/mutator_import/legacy.hpp`.

- `read_tam` reads one `.tam`.
- `id_from_title` makes an id from a title.
- `description_text` turns a description line into the manifest's text.
- `credit_of` reads the credit from that text.
- `import_legacy` pairs every `.tam` with its companions and builds the
  packages.
- `files_from_zip` reads a zip into those names and bytes. Names are the
  names the streamed zip reader decoded. Only a `.tam`, a `.sm` and a `.ccx`
  are read. Any other entry is listed with no bytes. The read entries
  together may be at most 512 MiB uncompressed.

## State

None kept. Each call reads the bytes it is given and returns the packages.
Nothing is written.

## Invariants

- A `.tam` is Windows-1252, at most 64 KiB. A NUL, a missing title, a
  missing compatibility line, or a companion count that is not a digit from
  0 to 9 refuses that file. An empty line before the compatibility line is
  skipped and noted.
- Companions are matched by the last path component, in the same directory,
  with ASCII case ignored. A missing rule file or archive refuses that
  mutator. The others are still imported.
- A file no `.tam` claims is named in `ImportResult::warnings`. Those notes
  do not refuse the import.
- Every manifest is written with `write_manifest` and read back. A manifest
  that does not read is refused. Every `mutator.lua` must compile.
- The id is lower-case kebab-case of 1 to 64 bytes. A title that makes an
  empty id uses the file name's stem. Two mutators in one run that share an
  id keep the first and give the next `-2`, `-3`, with a warning.

## What a .tam becomes

| Line | Manifest |
|---|---|
| Title | `name`, with spaces and `*` trimmed, cut to 64 bytes. `legacy.name` is the title as written, cut to 128 bytes when it is longer. The id comes from the title. |
| Description | `description`, with each run of k `#` becoming ceil(k/2) line breaks, a run of spaces and tabs that holds a tab becoming one space, and each line trimmed. The credit stays in the text. Longer than 2,000 bytes is cut, with a warning. |
| Credit | The description's last line, when it starts with `created by`, `made by` or `by`, ignoring case and the spaces between the words, then an optional colon. That name is `author.name`, or `unknown` when there is none. |
| Compatibility | `legacy.compatibility`, as written. `works-with` is below. |
| Companion count | Which numbered archives belong to the mutator. It is not a manifest field. |
| Fifth line | A hard mutator's archive name, when the line is set. A soft mutator ignores it and says so. |

A soft name starts with `{}` and needs `{}<stem>.sm`, plus `{}<stem>(1).ccx`
through `(n).ccx` when the count is n. A hard name starts with `}{`. Its
archive is the fifth line, or `}{<stem>.ccx` when that line is empty, plus
the numbered archives when the count is not 0. `legacy.prefix` is `{}` or
`}{`. `legacy.files` names the original files, including an archive that
was not written.

`version` is `1.0` unless the caller sets one. `tags` is `gameplay`, with
a warning, unless the caller sets tags. `requires.engine` is the caller's
text. `format` is 1 and the entry script is `mutator.lua`.

## Compatibility codes

`*` gives `works-with: ["*"]`. `OTA` gives `ta-3.1c`. `TACC` gives
`ta-3.1c` as well, with a warning that Core Contingency's units are only
there when it is on. The same mod is not listed twice. Any other code is
mapped only when the caller gives a code and a works-with entry. An
unmapped code stays in `legacy.compatibility` and is named in a warning.
When no code maps to a mod, `works-with` is `["*"]`, with a warning.

## Layers

A rule file's layers are the folders its blocks reach, in the manifest's
order: `units` by default, `weapons` for the weapons folder, `features`
for `features` (including `features/corpses`), `maps` for `maps`, `sides`
for `gamedata` when the extension is `sidedata.tdf` or a suffix of it, and
`tables` for any other `gamedata` file and for any other folder. A rule
file that does not parse refuses that mutator, with the parser's line.

`files` is included when an archive is written. A mutator with no blocks
uses `units`. A mutator left with neither a rule file nor an archive still
imports, with `layers: [files]` and a warning that it changes nothing.

## The script

`mutator.lua` returns the plan. `files` is present when that layer is
used. `rules` is present when a rule file was written. The title is
escaped in the comment.

```lua
-- Made by oa-tool mutator import from the TA: Mutation mutator "<title>".
-- Its rule file is in rules/ and its archives in files/; files OA protects are left out.
function mutate(ctx)
  return {
    files = { add = { "files" } },
    rules = { sm "rules/<id>.sm" },
  }
end
```

## Protected files

Every entry of every archive is checked with `check_mutable_path` under
the base layout. `is_unit_name` is true only for a unit whose file this
import holds, so no game data is read. An archive with no protected entry
is copied into `files/` byte for byte. An archive with one is written
again, holding every other entry with its path and bytes unchanged. An
archive left with no entry is not written. Each entry left out is a
warning naming the archive, the path and the rule.

One written archive is `files/<id>.ccx`. Several are `files/<id>-1.ccx`,
`files/<id>-2.ccx`, in the order the `.tam` named them, after the ones
left empty are dropped.

## Tests

`data-mutator-import` (`tests/import_test.cpp`) reads made-up `.tam`
texts, builds packages from them, and reads a zip whose names are code
page 437. The command's own test is `tools-oa-tool-mutator-import`.

## Limitations

Tags and versions are defaults: `gameplay` and `1.0`, unless the caller
sets them. Codes other than `*`, `OTA` and `TACC` need the caller's
mapping. Nothing is curated: a whole mod in the form of a hard mutator is
imported like any other. The import does not install the package, and it
does not run it. A protected path is omitted rather than kept. An archive
with nothing left is not written. A zip whose `.tam`, `.sm` and `.ccx`
entries are together larger than 512 MiB uncompressed is refused. Other
entries in the zip are named and not read.
