# data/script-host

Lua 5.4.9 for the script host, built as one C translation unit.

## Entry points

`oa-data-script-host-lua` (`oa::data::script_host_lua`). Code that calls Lua
includes `lua.hpp` through that library.

## State

None. A caller keeps the Lua states it creates.

## Invariants

- The sources in `third_party/lua` are unchanged.
- Every string hash uses one constant seed, `0x4F414C55` (the letters OALU),
  so two states walk the same table in the same order wherever they sit.
- `table.sort` does not draw a pivot from the clock. The pivot choice is
  fixed.
- Numbers are read and written with `.`, whatever the machine's locale.
- Hexadecimal numbers are converted by Lua's own code on every platform,
  rather than by the C library's conversion on some of them.
- The build compiles the auxiliary library and the base, string, table and
  utf8 libraries. It leaves out the maths, input and output, operating-system,
  package, debug and coroutine libraries, and the loader that would open
  every library.
- The translation unit is C, with the engine's floating-point options: no
  fused multiply-add and no value-changing optimisation, and Visual Studio's
  strict floating-point mode.

## Tests

`data-script-host-lua` loads a text chunk and checks that the state frees
every allocation, checks the libraries that are present and absent and the
number text they write, checks that three states walk one table in one
order, and refuses a precompiled chunk.

## Limitations

This library is the interpreter only. It has no sandbox, no budget and no
deterministic maths. The standard `math` library is not compiled.
