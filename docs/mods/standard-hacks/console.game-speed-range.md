# Game Speed Range

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Hack id | `console.game-speed-range` |
| Area | Console (`console`) |
| Scope | sim: part of the profile hash; every machine must agree |
| Runs on | Every machine in the game. |
| Parameters | 3 |
| Status | implemented |
<!-- END GENERATED: facts -->

## Description

Sets the slowest and fastest game speed players can choose, and lets the host lock the speed to a narrower range for one game. The default allows speed 0, at which the game stands still without being paused; 3.1c allows 1 to 20 and has no lock. A mutator may set `min` and `max` for one game.

## Configuration example

```yaml
hacks:
  console.game-speed-range: true     # speeds 0 to 20; the host may lock them
```

```yaml
hacks:
  console.game-speed-range:
    min: 5                           # no slower than half speed
    max: 15                          # no faster than one and a half times normal
    syncon: false                    # no speed lock
```

## Details

### Behaviour

Game speed is a number of steps from 0 to 20, where 10 is normal (30 simulation ticks a second). Every way of setting it is held to the range `min` to `max`, narrowed by the host's speed lock while one is on:

- The `+` key raises the speed one step while it is below the range's fastest, and the `-` key lowers it one step while it is above the range's slowest.
- The GAME slider in the options menu sets the speed, clamped to the range.
- A speed change received from another player is clamped to the receiving machine's range.
- A recorded speed change in a replay paces playback only when it lies within the range.

Clamping applies the fastest end first and then the slowest. The message log announces the speed that results ("Game Speed Normal" at 10, otherwise the offset from 10, such as "-10" at 0).

At speed 0 the simulation runs at no speed: nothing moves, but the game is not paused: the pause flag is not set. A player reaches it with `-` from speed 1, or another machine sends it.

`min` and `max` are adjustable per match: the host may set them for one game, within 0 to 20 and with `min` no greater than `max`.

### Speed lock

With `syncon` on, the host can lock the speed for one game by typing a chat line:

- `.syncon <low> <high>` allows only the speeds `low + 10` to `high + 10`. Both numbers are typed relative to normal speed, so `.syncon 0 5` allows 10 to 15 and `.syncon -10 10` allows the whole range. Each end is held within `min` to `max`; a `high` below `low` is raised to it, and a missing number counts as 0.
- `.syncoff` removes the lock, and `min` to `max` applies again.

While the lock is on, the keys and the slider keep to it as they keep to the range: `+` and `-` stop at its ends and the slider is clamped to it. When the lock takes effect and the game speed lies outside it, the speed is set to the nearer end, and the message log announces it. The lock lasts until `.syncoff` or the end of the game; each new game starts without one.

In a single-player game the player is the host: their own `.syncon` and `.syncoff` lines set and remove the lock.

With `syncon` off, `.syncon` and `.syncoff` are ordinary chat lines.

### 3.1c baseline

Speed runs from 1 to 20. A request for 0 or less becomes 1. There is no speed lock.

### Network games

Every machine clamps the speeds it sets and the speeds it receives to its own range. All machines must agree on the range and on `syncon`; the hack is part of the profile hash.

The speed lock needs the game recorder's protocol (see Interactions) as well as `syncon`. Every machine reads every chat line for `.syncon` and `.syncoff`, and takes them only from the host; another player's are ordinary chat. The host can lock the speed in the battle room, where its machine answers "Speed locked between <low> and <high>" or "Speed unlocked", or during the game. A lock set in the battle room carries into the game. The host's options that its recorder sends to each joining recorder carry the lock too; without `syncon` a machine takes the other options but not the lock.

While the lock is on, a machine does not make a local speed change outside it at all, and ignores a received one outside it. When the lock takes effect during the game, each machine whose speed lies outside it sets the nearer end and sends that speed to every player, unless its player only watches.

### Interactions

- [`recorder.ta-demo-recorder`](recorder.ta-demo-recorder.md) carries the `.syncon` and `.syncoff` speed lock in network games.
- [`network.recorder-session-commands`](network.recorder-session-commands.md) covers the recorder's other host commands.

### Implementation notes

- The range is `oa::sim::speed::range_of` in `src/sim/speed/include/oa/sim/speed.hpp` (module `src/sim/speed`), which reads `MatchRules::console.game_speed_range` (`minimum`, `maximum`); `set_speed`, `raise_speed` and `lower_speed` in `src/sim/speed/src/speed.cpp` clamp to it. `sim::speed::locked` narrows a range by a lock, and `sim::speed::read_lock_line` reads `.syncon` and `.syncoff` from a typed line.
- The application takes the range from `Runtime::game_speed_range` in `src/app/runtime_hotkeys.cpp` for the keys, from `src/app/runtime_match_menus.cpp` for the options slider and from `src/app/netgame/runtime_demo.cpp` for replays. It narrows the range by the lock that `Runtime::lock_game_speed` sets and `Runtime::unlock_game_speed` lifts; the lock is cleared as each match starts and ends. `Runtime::read_speed_lock_line`, called from `Runtime::submit_chat_line` in `src/app/runtime_console.cpp`, carries out a single-player game's `.syncon` and `.syncoff`.
- For network play, `wire_rules_of` in `src/app/netgame/wire_rules_binding.cpp` copies the range into `WireRules::speed_min` and `speed_max` and `syncon` into `WireRules::speed_lock`. `net_match_speed_range` in `src/netgame/match/src/net_match.cpp` narrows the range by the recorder session's lock (`recorder_speed_range` in `src/netgame/wire/recorder_session.cpp`), and `net_match_set_speed` and the received-speed path keep to it. The match reads the host's lines in `recorder_chat_line` and, through `speed_lock_applied`, sets a speed outside a new lock and calls `NetMatchHooks::speed_lock_changed`, which network play (`src/app/netgame/runtime_net.cpp`) passes to `Runtime::lock_game_speed`. The battle room reads the lines in `recorder_chat_line` and the host's options in `recorder_lobby_record` (`src/ui/frontend-multiplayer/src/battleroom.cpp`).
- Tested by ctest `sim-speed` (`src/sim/speed/tests/speed_test.cpp`: speed 0, a narrow range, crossing ends, the lock's range and reading its lines), `net-match` (`src/netgame/match/tests/net_match_test.cpp`: each machine clamps a received speed to its own range; the host's `.syncon` narrows every machine's range and sets and sends a speed outside it, another player's is chat, `.syncoff` lifts it, and without `syncon` both are chat), `net-wire-rules` (`src/netgame/tests/wire_rules_test.cpp`: `.syncon` and `.syncoff`, and the lock within the rules' range), `netgame-wire-rules` (`src/app/netgame/tests/wire_rules_test.cpp`: the profile's range and `syncon` reach the wire rules), `ui-multiplayer-lobby` (`src/ui/frontend-multiplayer/tests/lobby_test.cpp`: the battle room's lock, and no lock without `syncon`) and `data-mod-profile` (per-match `min` overrides and their bounds).
- Not tested end to end: a single-player game's `.syncon` line through `Runtime::submit_chat_line`; its parts (`read_lock_line`, `locked`) are tested in `sim-speed`.

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Hack id `console.game-speed-range`, written under the profile's `hacks` block.

| Write | Means |
| --- | --- |
| `console.game-speed-range: true` | On, every parameter at its default. |
| `console.game-speed-range: {min: 0}` | On, the parameters named set and the rest at their defaults. |
| `console.game-speed-range: {preset: baseline}` | On, starting from a preset; parameters named beside `preset` replace its values. |
| `console.game-speed-range: false` | Off, the same as leaving it out: 3.1c behaviour. |

### Parameters

| Parameter | Type | Unit | Allowed | Length | Baseline (3.1c) | Default | Adjustable | Scope | Overridden by |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `min` | `int` | speed steps (10 = normal) | `0` to `20` | - | `1` | `0` | `match` | `sim` | - |
| `max` | `int` | speed steps (10 = normal) | `0` to `20` | - | `20` | `20` | `match` | `sim` | - |
| `syncon` | `bool` | - | `true`, `false` | - | `false` | `true` | `fixed` | `sim` | - |

Adjustable: `fixed`: only the profile sets it; `match`: as `install`, and the host may also change it for one game.

### Presets

| Preset | Values |
| --- | --- |
| `baseline` | `{min: 1, max: 20, syncon: false}` |

`baseline` sets every parameter to its 3.1c value: the hack is on and plays as 3.1c.

### Shorthand

This hack takes no shorthand: write `true`, `false` or a parameter map.

### Constraints

After resolution the values must keep:

- `min <= max`

### Every parameter at its default

```yaml
hacks:
  console.game-speed-range:
    min: 0
    max: 20
    syncon: true
```
<!-- END GENERATED: schema -->
