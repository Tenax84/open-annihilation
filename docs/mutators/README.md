# Mutators

A mutator is a small package that changes one game on top of the base game
or a mod. It can hide and add files, and it can change the unit, weapon and
feature data, the sides, the `gamedata` tables the match reads again, the
build menus, the map settings and the hacks a mutator may set. Each mutator
is a sandboxed Lua script. The engine writes the results as ordinary text
in a layer above everything else. The match then loads as it would for any
mod, and no mutator code runs during it.

A stacked number is held at the end of the range the game reads its
field in. Turn rate and worker time (at most 65535), health (`maxdamage`,
at most 32767), waterline (at most 255), weapon range (at most 32767),
damage and the other narrow unit, weapon and movement fields stay inside
that range, and so do a feature's metal, energy, damage, height, spread,
regrowth, spark time and footprint, so a long stack does not wrap into a
unit that cannot turn, a weapon that reaches nothing or a metal spot worth
less than before. The dry run notes each held value once. Only a number
the mutators changed is held: a value that already fits, and a stock or
mod value no mutator changed, load as they always have.

A mutator applies to one game. It does not change the next game, and it
does not ask the player to start the game again. Screens, fonts, palettes,
music and the other files the game reads once in a run stay as they were:
a mutator that tries to replace one of those paths fails. A failed mutator
refuses the start, unless the only failure is a refused hack. The paths a
mutator may change are the allow-list in
[the .oamut standard](oamut-standard.md#5-the-files-layer).

The format is [the .oamut standard](oamut-standard.md). Writing one, from
an empty folder to a checked package, is
[the authoring guide](authoring.md). Where a package installs, and how a
registry serves it, are [Packages](../registries/packages.md). The
catalogue entry is [the catalogue](../registries/catalogue.md). The game's
command line, including `--trace-lookups`, is
[the command line](../command-line.md).
