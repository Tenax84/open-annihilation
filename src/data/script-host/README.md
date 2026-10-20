# data/script-host

`oa-data-script-host` runs one package's Lua in a sandbox. A script can use a
small set of libraries, `require` only the package's own `lua/` files, and
stop at an instruction allowance or a memory cap. Nothing it does reaches a
file, a clock, the environment or a network, and a failure comes back as a
value that names the package, the phase, the chunk and the line.

The interpreter underneath is Lua 5.4.9, built as the C library
`oa-data-script-host-lua`. Code that pushes values into a state includes
`lua.hpp` through that library. The sandbox header does not include it.

## Entry points

`oa/data/script_host/script_host.hpp` (namespace `oa::data::script_host`):

- `mutator_profile` is the profile a mutator runs under: the default
  instruction allowance and memory cap, and no extra globals. `Sandbox::open`
  takes that profile (or another), the package id and a `SourceHooks` reader.
  The reader returns one package file by its exact path, or false when the
  package has no such file. It matches the path byte for byte.
- `run_host`, or the profile's `install`, registers host functions, userdata
  metatables and globals before any script runs. Both run inside the
  protected region. `install` runs once, during open. `run_host` runs the
  caller's work on a later phase and calls no script.
- `run_file` loads a package path as a text chunk and runs it as one phase.
  `call` runs the function `push` placed on the stack, then `read` sees how
  many results came back. Each phase has a fresh instruction allowance.
- A false return fills a `ScriptError`. `describe` prints
  `<package>: <phase>: <chunk>:<line>: <message>`, and leaves the chunk and
  the line out when the chunk is unknown.
- `compile_check` loads a chunk in a throwaway state and does not run it.

## State

Each sandbox owns one Lua state and the lines `print` kept. Two sandboxes
share nothing: no module cache, no globals, no budgets. Moving a sandbox
moves that state; the moved-from object has no state.

## Invariants

- A script cannot reach a file, a clock, the environment, a process or a
  network. `load`, `loadfile`, `dofile`, `collectgarbage` and `string.dump`
  are nil. `io`, `os`, `debug`, `package`, `coroutine` and `math` are absent.
  Every chunk is loaded as text. Bytes that start with the precompiled mark,
  and a file past the source cap, are refused before the loader sees them.
- `require` accepts only lower-case names, digits and `_`, in parts joined by
  dots, and reads `lua/<parts>.lua` through the caller's reader. The match is
  exact, so a file system that folds case cannot change which file loads.
- `open`, `run_file`, `call`, `run_host` and `compile_check` do their Lua work
  inside one protected call. A failure marks the sandbox failed, and every
  later phase returns that same error and runs nothing.
- A count hook that leaves through an error does not restore the flag that
  allows the next hook. The hook turns that flag back on. Once a raise is
  already under way it drops the script's message handler, so a handler that
  loops cannot keep the phase alive. An instruction error or a memory error
  is reported even when the script's own `pcall` caught it.
- No C++ object with a destructor is alive across a Lua raise. `install`,
  `push`, `read` and the `run_host` work may raise and must not throw.
- The instruction allowance is per phase and is not a clock. The memory cap
  is the Lua allocation only. Passing either one sticks for that phase.
- The sources in `third_party/lua` are unchanged. Every string hash uses one
  constant seed, `0x4F414C55` (the letters OALU), so two states walk the same
  table in the same order wherever they sit. `table.sort` does not draw a
  pivot from the clock. Numbers are read and written with `.`, whatever the
  machine's locale. Hexadecimal numbers are converted by Lua's own code on
  every platform. The interpreter is one C translation unit, with the
  engine's floating-point options: no fused multiply-add and no
  value-changing optimisation, and Visual Studio's strict floating-point
  mode.

## Profiles

A profile is a name, an instruction allowance, a memory cap and an optional
`install`. One interpreter serves every profile. `mutator_profile` is the
default. A later profile, such as a computer player's with a per-tick
allowance, adds its own functions from `install` without a second interpreter.
Each sandbox still has its own state.

## Tests

`data-script-host-lua` loads a text chunk and checks that the state frees
every allocation, checks the libraries that are present and absent and the
number text they write, checks that three states walk one table in one
order, and refuses a precompiled chunk.

`data-script-host-sandbox` uses a made-up package. It calls a function the
chunk defined, checks the libraries a script can and cannot see, the
`string.rep` cap, the refused metatable keys, `print`, `require`, errors
with chunk and line, a failed sandbox, `compile_check`, `run_host`, and two
profiles in one process.

`data-script-host-budgets` uses a small test allowance and one run at the
mutator default. A plain loop, a loop inside `pcall`, and an `xpcall` whose
handler loops all stop on the instruction allowance. Two phases that each
use part of the allowance both pass. Keeping strings stops on the memory
cap, including inside `pcall`. A required module that loops forever is
reported at that module's chunk.

## Limitations

Work done inside a C function, such as pattern matching, is not counted
against the allowance. `math` and text that must come out the same on every
platform are not part of this library yet. A `__gc` or `__mode` entry added
to a metatable after `setmetatable` is not caught. `instructions_used`
counts in steps of the check interval, so a short phase can report zero. A
`require` whose error a script catches can leave that module marked loading
until the next phase.
