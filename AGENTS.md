# open-annihilation

A portable engine that plays Total Annihilation 3.1c from the player's own
installed game data, on macOS, modern Windows and Linux: campaigns, skirmish,
computer players, simulation, unit scripts, rendering, sound and save/load,
matching the 3.1c game's behaviour and its save files. Intro, menus and first
gameplay are checkpoints, not completion.

Network play is part of the engine: the multiplayer screens, the network
session and match, and the playback of recorded games, always built into the
game and plugged in through the hook table in
`src/app/include/oa/app/extension.hpp`. Other extension libraries may add
optional features through the same table; engine code never refers to an
extension that is not part of the engine.

Keep `./run.sh` as the user's entry point for inspecting actual progress. It
must build current source and launch the current native application, stop on
a failed build instead of falling back to stale binaries, and follow the main
frontend/game executable as integration advances. State current limitations
plainly; do not substitute another executable or a mock game.

Keep game assets and local captures out of Git.

## Rules

[docs/development/conventions.md](docs/development/conventions.md) holds the rules every change
follows, with their reasons and the directories they cover: compatibility
with 3.1c save files, game data and simulation results; describing behaviour,
never derivation; naming every field by what it holds, and constants; the language rules of
each layer; seams; `///` documentation; comments; tests; the extension
boundary and `run.sh`. [docs/development/testing.md](docs/development/testing.md) explains running
and writing tests. Read both before changing code: they bind agents exactly
as they bind people, and this file adds only how agents work.

## How agents work

- Work in your own worktree and branch. Agents own disjoint files: edit only
  the files your task gives you, coordinate interface changes with the
  integrator, and never commit on behalf of another agent. Do not push or
  rewrite shared branches unless asked.
- Delegate independent tasks when useful.
- Automated checks never open a window: run the game (`open-annihilation`,
  built by the `oa-game` target) headless, or with
  `SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy`.
- Write commit messages as [CONTRIBUTING.md](CONTRIBUTING.md) describes; they
  follow the same wording rules as the sources.
- Tests are switched off in this fork: never run ctest, never configure with
  `-DBUILD_TESTING=ON`, and never build or run test targets or the
  `tools/check_*.py` scripts, whatever the documents below say. Before
  reporting completion, build the game (`oa-game`) and say that no tests ran.
