# data/mutator-library

`oa-data-mutator-library` lists the mutators installed in the player's
Mutators folder. The Library, the pipeline and the battle room share one
description of what is installed: each mutator's manifest, and the
SHA-256, registry, catalogue id and release its origin record holds when
that record reads.

## Entry points

`oa/data/mutator_library/library.hpp` (namespace `oa::data::mutator_library`):

- `mutators_folder_name` is `Mutators`, the folder of the player's own
  folder that holds installed mutators.
- `list_installed` reads that folder. A missing folder is an empty listing.
  Each real directory is read for `oamut.yaml`, at most 256 KiB, with P01's
  reader, and for `.oa-origin.yaml`, at most 4,096 bytes, with the strict
  YAML reader. The result is sorted by id.
- `find` returns the listed mutator of an id, or null.

A folder with no readable manifest, or a manifest with problems, is left
out. `problems` names it: `oamut.yaml: it cannot be read`, `oamut.yaml: it
is larger than 256 KiB`, or P01's `describe` line after the folder's name.
Two folders with one id keep the folder whose name is the id, or else the
first by name, and `problems` names the other.

## State

None. Callers pass a folder and get a `Listing` back. Nothing here is
installed for the rest of the process.

## Invariants

- A folder whose name starts with `.` is the installer's own (staging, old,
  discard, `.backup`) and is skipped. Nothing inside a mutator is walked,
  so a `.backup` is never a second mutator.
- A symbolic link is not followed. A plain file at the top of Mutators is
  neither listed nor opened.
- `oamut.yaml` and `.oa-origin.yaml` are the only files opened, matched
  without ASCII case. A script is never read.
- More than 256 KiB of a manifest, or more than 4,096 bytes of an origin
  record, is not given to the YAML reader.
- An origin record that does not read leaves the mutator listed and its
  origin facts empty. A file origin keeps the SHA-256 only.

## Tests

`data-mutator-library` lists a scratch Mutators folder: three good
mutators, a broken manifest, a folder with none, an oversized manifest, the
installer's staging folder, a `.backup` holding another id, a symbolic
link where the system makes one, two folders with one id, a plain file left
untouched, a catalogue origin, a file origin, a damaged record and an
oversized record, the order and the problems, `find`, and a missing folder.

## Limitations

Whether a mutator is on for a match is not known here. The manifest is not
checked against this build's engine, and no script is parsed or run. The
origin record's registry id and key order are the installer's rules; a
record that is strict YAML of the known keys is read, and any other record
contributes no facts.
