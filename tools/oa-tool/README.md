# oa-tool

`oa-tool` reads archives and images from a game directory, and prints or
writes what a script asks for. Mod authors and translators run it by hand.
The checks under `tools/` run `list` and `extract` and match their output.
`pack` writes a mod or language package. `check` runs the installer's own
checks on a package and reports the facts a catalogue lists. `catalogue`
makes a publisher key and signs or checks a catalogue. `registry` makes an
empty signed registry and copies one into a folder.

## How to run it

From a build tree, `build/oa-tool` is the program.

- No arguments prints the command list on standard error and exits 2.
- `oa-tool --version` prints the game's version line (the same line as
  `open-annihilation --version`) on standard output and exits 0. It reads
  no file.
- `oa-tool help`, `oa-tool --help` and `oa-tool -h` print that list on
  standard output and exit 0.
- `oa-tool help COMMAND` and `oa-tool COMMAND --help` (or `-h` before any
  `--`) print that command's usage line and explanation on standard output
  and exit 0. `oa-tool help GROUP SUBCOMMAND` does the same for a
  subcommand.
- A group with no subcommand prints its own list on standard error and
  exits 2.

Options, for a command that has any, may be written before, between or
after the positional arguments. `--name value` and `--name=value` are the
same. `--` ends options. A command takes its options through
`parse_arguments` in `arguments.hpp`.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | The command did what was asked. |
| 1 | The command ran and could not do it, or the answer is no. `Failure`, and any other exception, prints `oa-tool <command>: <message>` on standard error. |
| 2 | The invocation is not one the tool can run: no command, an unknown command, or the wrong number of arguments. The line says what is wrong and names `oa-tool help`. |

## Commands

| Command | What it does |
|---|---|
| `list ARCHIVE` | Prints each file in an HPI archive as `path`, a tab and its size in bytes. |
| `extract ARCHIVE ENTRY OUTPUT` | Writes one archive entry to OUTPUT and prints nothing. |
| `asset-extract ROOT ENTRY OUTPUT [ARCHIVE...]` | Writes ENTRY from loose files under ROOT, or from the named archives, and prints where it was read. A loose file wins. |
| `preview ARCHIVE PCX_ENTRY OUTPUT.ppm\|OUTPUT.png` | Decodes a PCX image from an archive as a PPM file, or a PNG file when the path ends in `.png`, and prints its size. |
| `decode-pcx INPUT.pcx OUTPUT.ppm\|OUTPUT.png` | Decodes a PCX file the same way. |
| `pack FOLDER [--out FILE] [--force] [--game-dir DIR]` | Packs FOLDER as a `.oamod`, a `.oalang`, a `.oamap` or a `.oamut`. The manifest is first, then every other file in byte order of its path. A file whose extension, without case, is png, jpg, jpeg, gif, ogg, mp3, zip, gz, bz2, xz, 7z, oamod, oalang or oamap, and an empty file, is stored; every other file is deflated. The same files give the same bytes. `--game-dir` is the game's data; map packs need the game's palette for previews. `--force` replaces an existing file. A map's `players` is the largest whole number in its OTA `numplayers` when every number is from 2 to 10, and otherwise the most start positions of a Network schema. The folder's `oamap.yaml` may leave `maps` out, or set a map's `name`, `description` or `players`. `size`, `preview` and `files` are computed. A mutator's default name is `<id>-<version>.oamut`. Its Lua is compiled and its rule files are parsed before the file is written, and neither is run. `--game-dir` with a mutator is a usage error. |
| `check FILE [--game-dir DIR] [--mod FOLDER[=KEY]]... [--accept-unimplemented-hacks] [--json]` | Runs the checks an install runs before it writes, and reports the facts a catalogue lists. A map pack needs `--game-dir` and may name each further target with `--mod`. `--json` prints one JSON object, and a map pack adds `maps`. A mutator adds `summary`, `description`, `homepage`, `tags`, `author`, `requires`, `works_with`, `conflicts_with`, `layers`, `options`, `icon`, `entry`, `scripts`, `rules` and `legacy`, and its `revision` is null. Without `--json` each fact is a `key: value` line, a map pack then prints one line per map and target, and the last line is `result: ok` or `result: refused`. Exit 0 when the package would be installed, 1 when it would be refused or a map does not fit `ta-3.1c`. A mod that does not fit leaves that exit code unchanged. |
| `mutator pack FOLDER [--out FILE] [--force]` | Packs a folder whose top holds `oamut.yaml`, the same way `pack` does. A folder without `oamut.yaml` is a usage error. |
| `mutator check FILE [--json]` | Checks a `.oamut` and reports the same facts as `check`. A file of another kind is a usage error. |
| `catalogue keygen --id ID --out FILE [--passphrase-file FILE]` | Makes a new Ed25519 key, seals its seed with a passphrase and writes FILE. The file is created only when that name is new, and on macOS and Linux its mode is 0600. The passphrase is read from the terminal with echo off, twice, and has to be at least 12 bytes. `--passphrase-file` reads the file's first line instead. Prints `key:`, `public: ed25519:` and `fingerprint:`. |
| `catalogue public KEY_FILE` | Prints the same three lines. The passphrase is not used. |
| `catalogue sign --key KEY_FILE CATALOGUE [--out SIG] [--passphrase-file FILE]` | Signs CATALOGUE's exact bytes and writes CATALOGUE.sig, or SIG. The line is the one a catalogue check reads: `ed25519`, the key id and the signature. A catalogue this build would refuse is refused before the key is opened. The written pair is checked before the command reports success. Prints `signed <file> with <id>`. |
| `catalogue verify CATALOGUE [--sig SIG] (--descriptor FILE \| --registry ID --key ed25519:KEY [--key ...])` | Checks CATALOGUE against its signature, with the descriptor's id and keys, or with ID and the given keys. Each `--key` is read as the key the signature names, and the first is the one that is trusted. Prints `good signature by <id>` and the verdict, and exits 0 when the catalogue can be used. A refused catalogue prints the verdict and exits 1. |
| `registry init FOLDER --id ID --name NAME --base-url URL --key KEY_FILE [--passphrase-file FILE] [--homepage URL]` | Makes a direct registry in FOLDER: `registry.yaml`, an empty catalogue signed for 30 days, its signature, and `v1/p/` and `v1/i/`. FOLDER must be missing or empty. `--base-url` is the http address the registry is served at, with no trailing slash. Prints the descriptor's path and the key's fingerprint. |
| `registry mirror URL FOLDER [--key ed25519:<base64>]` | Copies the registry at URL into FOLDER, laid out as on the host that served it. The signature is checked, a sequence lower than the one FOLDER holds stops the copy, and each package and picture is checked by SHA-256. A `.part` is resumed with a Range request. A tokens registry is copied through its download API. The catalogue and its signature are written last, byte for byte. |
| `interface-words --json` | Prints a JSON array of every English text this build passes to the interface catalogue, sorted, with each text once. The texts are the lists the modules that look them up export. |

## Entry points

- `run_tool` in `command.hpp` turns an argument list into one command. The
  program's `main` passes it the process arguments and the standard streams.
  The overload that takes a table runs that table instead, which is how a
  test drives a group the tool does not ship.
- `commands` returns the table in `commands.cpp`.
- `parse_arguments` splits options from positional arguments.
- `read_file`, `write_file` and `write_image` in `files.hpp` move bytes and
  images. Each throws `Failure` when it cannot.

## State

A run reads the archives, loose files and images named on its command line
and writes the output file a command names. It keeps nothing after it exits.
`registry mirror` writes the folder it is given. When that registry asks for
an install id, the id is made for the run and kept only in memory: it is not
written and not printed. Commands print only through the `Output` they are
given, so a test can capture every line.

## Invariants

- `list`, `extract`, `asset-extract`, `preview` and `decode-pcx` print the
  same bytes on a successful run as they did before the command table, and
  that run exits 0.
- `pack` writes the manifest first and the other files in byte order of
  their paths, stored or deflated by the rule in its help, and the same
  files give the same bytes.
- `check` accepts a package exactly when an install would, and reports
  `sim_hash` and `full_hash` from the profile resolved without the packaged
  settings. A missing fact is left out. `revision` is null when a language
  pack has no packaging revision, and for every mutator. A mutator's Lua is
  compiled and its rule files are parsed, and neither is run.
- The exit code is 0, 1 or 2.
- `catalogue sign` writes the signature with the catalogue check's own
  line, and reports success only after that check accepts the written pair.
- `catalogue keygen` creates the key file only when the name is new. On
  macOS and Linux the file's mode is 0600. The passphrase is not printed.
- Help lists commands in the table's order, and a group's subcommands under
  the group.
- `registry init` writes a descriptor the registry reader accepts and a
  catalogue the catalogue check accepts, and reports success only after that
  check's verdict is accepted.
- `registry mirror` copies the catalogue and its signature byte for byte,
  after the packages and pictures are in place. The sequence check is the
  catalogue check's own. A file is kept only after its SHA-256 matches.
- A repeating option keeps every value, in the order given. An option that
  does not repeat, and every flag, is refused the second time.
- `interface-words --json` prints one JSON array of the texts the modules
  export for the interface catalogue. The array is sorted and holds each
  text once. Any other arguments exit 2.

## Adding a command

1. Write its `run` function in a `.cpp` file under `tools/oa-tool`, and
   declare that function in `command.hpp` with the others from the same file.
2. Add its row to the table in `commands.cpp`: the name, the usage arguments,
   a one-line summary, a paragraph of help, how many arguments it takes and
   the function. A group leaves the function null and names its subcommands.
   A command whose options make the argument count vary sets the upper bound
   to `any_count` and checks the arguments itself with `parse_arguments`.
   Declare a repeating option with `repeats`; the parser already keeps every
   value.
3. Add the `.cpp` file to `oa-tool-commands` in `tools/oa-tool/CMakeLists.txt`.
4. Cover the command from `tools/oa-tool/tests/oa_tool_test.cpp`, or from
   the command's own test when it builds packages. The tests run commands
   in-process through `run_tool`, with streams of their own. `pack` and
   `check` are covered by `pack_test.cpp` and `check_test.cpp`.

## Tests

`tools-oa-tool` (`oa-tool-test`) builds an archive in memory, runs every
archive command, checks help and the usage failures, checks `parse_arguments`,
and dispatches a group through `run_tool`'s table overload.

`tools-oa-tool-pack` (`oa-tool-pack-test`) packs a made-up mod and the pseudo
language pack, checks the order and the methods, packs again after the
files' times change, and checks the refusals. Its argument is the pseudo
pack's folder.

`tools-oa-tool-pack-map` (`oa-tool-pack-map-test`) packs two maps into a
`.oamap`, checks the index, the preview and the refusals, and leaves
`pack-map-source` and `pack-map-game` in its working folder. It also checks
that `numplayers` sets `players`, that a missing or malformed `numplayers`
falls back to the start positions, that a folder's `name`, `description`
and `players` replace the computed values, and that an unknown stem is
refused. Two packs of one folder have the same bytes.

`tools-oa-tool-check` (`oa-tool-check-test`) checks a made-up mod package,
its refusals, and a language manifest read on its own. Its argument is the
pseudo pack's `language.yaml`.

`tools-oa-tool-mutator` (`oa-tool-mutator-test`) packs a made-up mutator,
checks the entry order, the methods and the JSON facts, and checks the
refusals from `pack` and from `check`. `mutator pack` and `mutator check`
are the same commands limited to a mutator.

`tools-oa-tool-check-map` (`oa-tool-check-map-test`) checks a made-up map
pack against a stand-in game and a made-up mod. `tools-oa-tool-check-map-data`
runs the same program with `--data` against the installed game.

`tools-oa-tool-catalogue` (`oa-tool-catalogue-test`) makes one publisher
key with the real parameters, then signs and checks a catalogue with that
key, with `--registry` and `--key` and with a descriptor. A changed
catalogue, another key, another registry and a signature that names
another key are refused. A short passphrase, an existing key file and a
catalogue the check would refuse fail before the key is opened.

`tools-oa-tool-registry` (`oa-tool-registry-test`) makes one publisher key,
then an empty registry, and mirrors registries served by the test HTTP
fixture on 127.0.0.1. It covers a resumed download, a file that fails its
SHA-256 check, a signature and a sequence the catalogue check refuses, and a
tokens registry whose key requests carry reason `mirror`.

`tools-oa-tool-interface-words` runs the built `oa-tool interface-words
--json` and fails when a lookup wrapper in the sources is handed a text
that array does not hold. The comparison reads
`tools/oa-tool/tests/interface_words_scan.py`.
Each module that looks a text up exports it in its `interface_words` list;
the command only joins those lists.

`tools-oa-tool-interface-catalogue` reads `languages/interface.tdf` into
the interface catalogue and checks that German, French, Spanish and
Italian each cover that same list, that a German lookup returns German,
and that placeholders, numbers and product names are kept.

## Limitations

The archive commands take no options. `pack` takes `--out`, `--force` and,
for a map pack, `--game-dir`. `check` takes `--game-dir`, `--mod`,
`--accept-unimplemented-hacks` and `--json`. A map pack needs `--game-dir`.
Each `--mod` is one target after the base game, and exit 1 is reserved for
a map that does not fit `ta-3.1c`. `catalogue keygen` does not replace a
key file. On macOS and Linux that file's mode is 0600. The signature
`catalogue sign` writes is the one line a catalogue check reads. A
passphrase comes from the terminal, with echo off, or from
`--passphrase-file`. One key is stored in one file, and this command does
not rotate it. `registry init` makes a direct registry and signs its empty
catalogue for 30 days. `registry mirror` copies the origin's files and does
not sign anything, and it posts no install result. A mirror folder is a copy
of the registry. It is served as a direct registry: the files and signatures
stand on their own, and the origin's key API stays the origin's policy. The
macOS package does not ship `oa-tool`. `interface-words` takes only
`--json`.
