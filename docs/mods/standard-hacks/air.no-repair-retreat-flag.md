# No Repair Retreat

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Hack id | `air.no-repair-retreat-flag` |
| Area | Aircraft (`air`) |
| Scope | sim: part of the profile hash; every machine must agree |
| Runs on | The machine that owns the unit or player concerned. |
| Parameters | 1 |
| Status | implemented |
<!-- END GENERATED: facts -->

## Description

Aircraft whose unit definition sets a chosen tag never break off to a repair pad when damaged, and neither the Move command nor a right press in the right-click interface lands them on a pad. In 3.1c every aircraft below three quarters of its health leaves its order to find a pad.

## Configuration example

```yaml
hacks:
  air.no-repair-retreat-flag: true    # aircraft with CantBeTransported=1 never retreat
```

```yaml
hacks:
  air.no-repair-retreat-flag:
    flag: none                       # on, but no unit is marked: 3.1c behaviour
```

## Details

### Behaviour

The `flag` parameter names the unit definition tag that marks an aircraft. The only marker is `cantbetransported`: a type whose definition has `CantBeTransported=1` is marked. The tag keeps its usual meaning as well; aircraft are not transported in any case.

A marked aircraft below three quarters of its maximum health (`MaxDamage`) does not break off to find a repair pad. It keeps its current order in each place where 3.1c checks for a pad:

- seeking a target to attack, and both forms of engaging it;
- air strikes (bombing runs);
- seeking a unit to guard;
- patrols;
- the repair patrol of builder aircraft.

The Move command also changes for a marked aircraft. Over a repair pad, it neither shows the pad cursor nor orders a landing; the pointer falls through to the load, guard and move tests, as it does over any other unit. A right press on a repair pad in the right-click interface does not land a marked aircraft either: the default order falls through to the same tests, so the aircraft guards the pad or moves to it. The Unload command still lands a marked transport on an allied pad.

Unmarked aircraft behave as in 3.1c.

### 3.1c baseline

Every aircraft below three quarters of its health looks for a nearby repair pad and lands on it, ending its order. The Move command over a repair pad shows the pad cursor and lands the aircraft there.

### Network games

The machine that owns the aircraft applies the rule. The hack is part of the profile hash, so every machine must use the same `flag`, and every machine must have the same unit definitions.

### Interactions

- [`air.guard-respects-hold-position`](air.guard-respects-hold-position.md) also changes the aircraft guard order, independently.

### Implementation notes

- The test for a marked type is `never_retreats_to_repair` in `src/sim/match-runtime/include/oa/sim/match_runtime/attack_orders.hpp`. It is used in `src/sim/match-runtime/src/tick_missions_air.cpp` (attack and strike orders), `src/sim/match-runtime/src/tick_missions_vtol.cpp` (guard and patrol) and `src/sim/match-runtime/src/tick_missions_vtol_build.cpp` (repair patrol). It reads `MatchRules::air.no_repair_retreat_flag.flag` and the type's `OA_UNIT_DEF_ABILITY_CANT_BE_TRANSPORTED` ability bit.
- The Move command's cursor and order, and the right-click default order, test `kept_off_pads` in `src/sim/gameplay-input/src/order_cursor.cpp` (module `src/sim/gameplay-input`).
- Tested by `flagged_aircraft_never_retreat` in `src/sim/match-runtime/tests/order_rules_test.cpp` (ctest `match-order-rules`), for marked and unmarked aircraft with the hack on and off, and by `test_no_repair_retreat_flag` in `src/sim/gameplay-input/tests/order_cursor_test.cpp` (ctest `gameplay-input-order-cursor`) for the Move command and the right-click default order.

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Hack id `air.no-repair-retreat-flag`, written under the profile's `hacks` block.

| Write | Means |
| --- | --- |
| `air.no-repair-retreat-flag: true` | On, every parameter at its default. |
| `air.no-repair-retreat-flag: {flag: cantbetransported}` | On, the parameters named set and the rest at their defaults. |
| `air.no-repair-retreat-flag: {preset: baseline}` | On, starting from a preset; parameters named beside `preset` replace its values. |
| `air.no-repair-retreat-flag: cantbetransported` | On, the shorthand: a bare value sets `flag`. |
| `air.no-repair-retreat-flag: false` | Off, the same as leaving it out: 3.1c behaviour. |

### Parameters

| Parameter | Type | Unit | Allowed | Length | Baseline (3.1c) | Default | Adjustable | Scope | Overridden by |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `flag` | `enum` | - | `none`, `cantbetransported` | - | `none` | `cantbetransported` | `fixed` | `sim` | - |

Adjustable: `fixed`: only the profile sets it.

### Presets

| Preset | Values |
| --- | --- |
| `baseline` | `{flag: none}` |

`baseline` sets every parameter to its 3.1c value: the hack is on and plays as 3.1c.

### Shorthand

The hack has one parameter, so a bare value sets `flag`: `air.no-repair-retreat-flag: cantbetransported` is `air.no-repair-retreat-flag: {flag: cantbetransported}`.

### Every parameter at its default

```yaml
hacks:
  air.no-repair-retreat-flag:
    flag: cantbetransported
```
<!-- END GENERATED: schema -->
