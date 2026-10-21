# Mutators

A mutator is a small package that changes one game on top of the base game
or a mod. It can hide and add files, and it can change the unit, weapon and
feature data the match reads. Each mutator is a sandboxed Lua script. The
engine writes the results as ordinary text in a layer above everything else.
The match then loads as it would for any mod, and no mutator code runs
during it.

A mutator applies to one game. It does not change the next game, and it
does not ask the player to start the game again. Screens, fonts, palettes,
music and the other files the game reads once in a run stay as they were:
a mutator that tries to replace one of those paths fails, and the rest of
its files are left out of that game. The paths a mutator may change are the
allow-list in [the .oamut standard](oamut-standard.md#5-the-files-layer).

The format is [the .oamut standard](oamut-standard.md). Where a package
installs, and how a registry serves it, are
[Packages](../registries/packages.md). The catalogue entry is
[the catalogue](../registries/catalogue.md). The game's command line,
including `--trace-lookups`, is [the command line](../command-line.md).
