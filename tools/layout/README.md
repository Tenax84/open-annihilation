# The layout pass

One scripted pass that lays the engine out by layer and gives every module
one name: `src/<group>/<module>`, include paths `oa/<group>/<module>...`,
namespaces `oa::<group>::<module>` and targets `oa-<group>-<module>` with an
`oa::<group>::<module>` alias. It runs once, after every other change, and
can be re-run on a fresh checkout until it is.

| File | Holds |
|---|---|
| [layout.txt](layout.txt) | The table: every module's new home, the files that move on their own, every header's new include path, every target's and namespace's new name, the renamed identifiers, the known layer exceptions and the files left as they are, each judgement call with its reason |
| [patches.txt](patches.txt) | The hand-written edits, matched exactly (build files, scripts, baselines and prose the mechanical rewrites cannot reach) |
| [files](files) | The build files of the modules the pass creates, and the layout check's CMake module |
| [layout.py](layout.py) | The pass: `--check`, `--apply`, `--propose`, `--map`, `--self-test` |
| [cxx.py](cxx.py), [cmake_edit.py](cmake_edit.py) | Its C++ and CMake rewriting |
| [selftest.py](selftest.py) | A small tree in today's layout and the text the pass must turn it into |

The layer order, and the check that holds the tree to it afterwards, are in
[tools/check_layout.py](../check_layout.py); the pass registers it as the
`engine-layout` test and writes its baseline of known exceptions.

## Running it

From a clean checkout (every tree committed), with the projects that build
on the engine checked out below it or named with `--tree`:

```sh
python3 tools/layout/layout.py --self-test
python3 tools/layout/layout.py --check
python3 tools/layout/layout.py --apply
```

`--check` computes the whole pass in memory and prints what it would do, or
every row, header, namespace, library, module directory and patch that no
longer fits the tree, every header that moves under `include/` but would
still include a file left outside it, and every script, build file or data
file that would still name a file or directory the pass removes. `--apply`
moves the files with `git mv`, rewrites the engine and the projects that
build on it, and stages everything; it changes nothing when a problem is
found. A project's directory that `--leave DIR` names is never
rewritten. A
second `--apply` on a laid-out tree reports that the tree is laid out
already and changes nothing.

When the tree has changed since the table was written:

- `--propose` prints the rows the naming rule gives for new headers,
  namespaces and targets; append them to `layout.txt` with a reason.
  `--check` fails while any of them is missing. A header outside its
  module's `include/` (the app's headers, included by bare name) takes a
  file row that moves it there; an include row cannot move it, and
  `--check` refuses one that has no effect.
- A hunk whose text moved, or a file `--check` says still names a removed
  path: `--check --dump DIR` writes every file the pass changes or adds, at
  its new path, and the hunk is rewritten from the dumped file.
- The hunks for a project that builds on the engine (an extension that
  follows the extension table's version, a library that links the app's
  headers) are kept with that project, outside the engine's tree, and passed
  with `--patches FILE`; their paths are relative to the engine checkout.
- `tools/style-baseline.json`: each module the pass splits hands the new
  half its share of the counts. After an `--apply`,
  `python3 tools/check_style.py --report` gives each half's counts; write
  them into the hunks so that the totals stay the same.

## Proving a pass

After `--apply` and a commit of every tree, each of these must pass as it
does before the pass:

- The engine: configure with the game (`OA_GAME_DIR`), build, and run the
  whole ctest, which then includes
  `engine-layout`, `engine-layout-selftest` and `layout-pass-selftest`,
  and with Clang `doc-check`.
- The engine with the recorder extensions, as CI builds it: configure with
  `-DOA_RECORD_EXTENSION_HOOKS=ON`, build, and run the whole
  ctest (`extension-hooks-options` and `extension-hooks-game` read the
  table's header).
- Every test's command line against the one before the pass (`ctest
  --show-only=json-v1` of both builds): the same tests, the same arguments,
  apart from renamed targets and moved paths.
- Each project that builds on the engine, configured with
  `-DOA_ENGINE_DIR` at the laid-out tree, built and tested.
- The Windows harness (`tools/test_windows.sh`).

## Known limits

- Module READMEs of split modules still describe both halves, and
  `src/formats/tdf/README.md` describes the unit definitions; that is left
  for a documentation pass.
- The sources of `oa-tool` and of the `oa-platform` probe move to `tools/`,
  where the layout check, which reads `src/` alone, no longer looks at
  them, nor does any other check that reads `src/` alone.
- The public API of `formats/hpi` stays in namespace `oa` until
  `assets.hpp` is split (see the `namespace oa` row).
- Other `*Offline*` names (`OfflineInputs`, `OfflineServices`,
  `effects_offline`) keep their names.
