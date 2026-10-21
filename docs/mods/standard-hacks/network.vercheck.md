# Version Check

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Hack id | `network.vercheck` |
| Area | Network (`network`) |
| Scope | sim: part of the profile hash; every machine must agree |
| Runs on | Every machine in the game. |
| Parameters | 1 |
| Status | implemented |
<!-- END GENERATED: facts -->

## Description

Each player's machine checks that every other human player runs the
same program with the same rules and map, and reports in chat how many
players did not match. Nothing is blocked: the check only reports.

## Configuration example

```yaml
hacks:
  network.vercheck: true                  # one challenge at the start
```

```yaml
hacks:
  network.vercheck: periodic-challenge    # challenge again every second until tick 3600
```

## Details

### Behaviour

Each machine challenges every other human player still playing. Players
it has rejected and watchers are skipped. A challenge is 32 random bytes,
sent only to that player over the private chat channel.

The challenged machine sends two answers back. Each is an HMAC-SHA-256
keyed with the challenge:

- one over a digest of its program, and
- one over a digest of its game data: the hash the match plays and the
  match's map name. While the match holds a mutator digest, that digest
  is folded into the hash the match plays (the profile's sim hash, or
  the plain baseline without a profile). Without a mutator digest the
  bytes stay that hash and the map.

The challenger computes the answers it expects from its own digests and
notes for each player whether both answers arrived and matched.

When challenges go out depends on `revision`:

| `revision` | Challenges |
| --- | --- |
| `off` | None. The machine answers nothing and reports nothing. |
| `single-challenge` | At tick 180. |
| `periodic-challenge` | At tick 180 and every 30 ticks after it, up to tick 3600. Until tick 450, a player who has already answered in full is not challenged again. From tick 450, everyone is challenged at each period. |

At tick 600, each machine counts the challenged players whose answers
were missing or different. When there are any, the machine shows a
notice and sends a chat line to everyone:
`<name> reports VerCheck issues with <n> other players`. Nothing more
happens. No player is dropped or blocked.

A report request on the channel asks every machine to say what program
it runs. The requests are ops 3 to 7, sent by the recorder's report
commands such as `.exereport` and `.crcreport`. A machine with the
recorder answers in chat with `*** <name> uses <program>`.

Wire layout of the messages, inside the private channel's 65-byte chat
record (sub-id `0x2B`):

| Op | Meaning | Payload |
| --- | --- | --- |
| `0x01` | challenge | 32 random bytes |
| `0x02` | accepted | two u32; read and ignored |
| `0x03`-`0x07` | report request | none |
| `0x20` | answer over the program | 32 bytes |
| `0x21` | answer over the game data | 32 bytes |

### Baseline (3.1c)

3.1c checks nothing beyond the version bytes in its setup blocks.

### Network games

Every machine challenges, answers and reports. All machines must use the
same `revision`, which is part of the profile hash.

### Interactions

- Needs [network.chat-extension-channel](network.chat-extension-channel.md)
  with any framing other than `none`. Without the channel, the engine
  turns the check off.
- The program line in report answers comes from
  [recorder.ta-demo-recorder](recorder.ta-demo-recorder.md).

### Implementation notes

- The profile's `network.vercheck.revision` becomes
  `WireRules::integrity_check` in `src/app/netgame/wire_rules_binding.cpp`,
  which also turns it off without the channel.
- `fill_integrity_identity` in `src/app/netgame/runtime_net.cpp` builds
  the machine's digests.
- `src/netgame/match/src/integrity_check.cpp` (declared in
  `src/netgame/match/include/oa/netgame/match/integrity_check.hpp`)
  holds the timing, the keyed answer and the count.
- `integrity_challenge`, `handle_integrity` and `integrity_report` in
  `src/netgame/match/src/net_match.cpp` send, answer and report.
- Ops and sizes are in `src/netgame/include/oa/netgame/private_channel.hpp`.
- Tests:
  - `integrity_check_answers_and_reports` and
    `integrity_report_request_is_answered` in `net-match`
    (`src/netgame/match/tests/net_match_test.cpp`).
  - `every_rule_maps` and `rules_need_their_channel` in
    `netgame-wire-rules` (`src/app/netgame/tests/wire_rules_test.cpp`).

**Known limit:** the answers cover the engine's own identity: its program
line, the hash the match plays (the profile's sim hash, or the plain
baseline without a profile) and the map name. While the match holds a
mutator digest, that digest is folded into the hash the match plays as
well; without one, the game-data bytes stay that hash and the map (and
the bundles a managed copy already appends). They are not computed over
the game's files, modules and unit tables. Two machines running this
engine with the same rules, the same mutators and the same map agree.
A machine running another program, one that hashes those files, reports
a mismatch with this engine, and this engine reports one with it.

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Hack id `network.vercheck`, written under the profile's `hacks` block.

| Write | Means |
| --- | --- |
| `network.vercheck: true` | On, every parameter at its default. |
| `network.vercheck: {revision: single-challenge}` | On, the parameters named set and the rest at their defaults. |
| `network.vercheck: {preset: baseline}` | On, starting from a preset; parameters named beside `preset` replace its values. |
| `network.vercheck: single-challenge` | On, the shorthand: a bare value sets `revision`. |
| `network.vercheck: false` | Off, the same as leaving it out: 3.1c behaviour. |

### Parameters

| Parameter | Type | Unit | Allowed | Length | Baseline (3.1c) | Default | Adjustable | Scope | Overridden by |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `revision` | `enum` | - | `off`, `single-challenge`, `periodic-challenge` | - | `off` | `single-challenge` | `fixed` | `sim` | - |

Adjustable: `fixed`: only the profile sets it.

### Presets

| Preset | Values |
| --- | --- |
| `baseline` | `{revision: off}` |

`baseline` sets every parameter to its 3.1c value: the hack is on and plays as 3.1c.

### Shorthand

The hack has one parameter, so a bare value sets `revision`: `network.vercheck: single-challenge` is `network.vercheck: {revision: single-challenge}`.

### Every parameter at its default

```yaml
hacks:
  network.vercheck:
    revision: single-challenge
```
<!-- END GENERATED: schema -->
