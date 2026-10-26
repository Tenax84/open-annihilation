// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/sim/feature_runtime.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

using namespace oa;
using namespace oa::sim::gameplay_input;

namespace {
int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++failures;
    }
}

struct Fixture {
    World* world = world_create();
    FeatureDef reclaimable{};
    bool mapped = true;
    bool feature_here = false;
    bool in_range = true;

    Fixture() {
        const WorldCapacity capacity{16, 8, 1};
        if (world == nullptr || world_alloc_tables(world, &capacity) == 0)
            std::abort();
        world->game.local_player_index = 0;
        for (uint8_t i = 0; i < 2; ++i) {
            Player& player = world->game.players[i];
            player.index = i;
            player.alliance[i] = 1;
            player.energy = 1000.0F;
            player.metal = 1000.0F;
        }
        world->game.players[0].first_unit = world_unit_ref(world, &world->units[1]);
        world->game.players[0].last_unit = world_unit_ref(world, &world->units[4]);
        world->game.players[1].first_unit = world_unit_ref(world, &world->units[5]);
        world->game.players[1].last_unit = world_unit_ref(world, &world->units[8]);
        reclaimable.flags = OA_FEATURE_FLAG_RECLAIMABLE;
    }

    ~Fixture() { world_destroy(world); }

    UnitDef& def(uint32_t index) { return world->unit_defs[index]; }

    Unit& unit(uint32_t slot, uint8_t owner, uint32_t def_index) {
        Unit& u = world->units[slot];
        u.id = static_cast<uint16_t>(slot);
        u.type_index = static_cast<uint16_t>(def_index);
        u.def = oa_ref_from_index(def_index);
        u.owner = world_player_ref_of(world, &world->game.players[owner]);
        u.owner_index = owner;
        u.economy.player = u.owner;
        u.flags = OA_UNIT_FLAG_SELECTABLE;
        u.movement = 1;
        u.health = 10;
        def(def_index).max_damage = 100;
        return u;
    }

    OrderCursorHooks hooks() {
        return {
            this,
            [](void* c, const World&, const FixedVec3&) {
                return static_cast<Fixture*>(c)->mapped;
            },
            [](void* c, const World&, const FixedVec3&) -> const FeatureDef* {
                auto* self = static_cast<Fixture*>(c);
                return self->feature_here ? &self->reclaimable : nullptr;
            },
            [](void* c, const World&, const Unit&, const Unit&) {
                return static_cast<Fixture*>(c)->in_range;
            },
            [](void* c, const World&, const Unit&, const FixedVec3&) {
                return static_cast<Fixture*>(c)->in_range;
            },
        };
    }
};

void test_accepts_order() {
    Fixture f;
    Unit& u = f.unit(1, 0, 1);
    check(unit_accepts_order(*f.world, u), "spawned finished unit accepts orders");
    u.build_remaining = std::numeric_limits<float>::quiet_NaN();
    check(unit_accepts_order(*f.world, u), "NaN build fraction counts as finished");
    u.build_remaining = 0.5F;
    check(!unit_accepts_order(*f.world, u), "unfinished unit refuses orders");
    u.build_remaining = 0.0F;
    u.capture_cooldown = 3;
    check(!unit_accepts_order(*f.world, u), "capture cooldown refuses orders");
    u.capture_cooldown = 0;
    Unit& carrier = f.unit(2, 0, 2);
    u.attach_parent = world_unit_ref(f.world, &carrier);
    check(!unit_accepts_order(*f.world, u), "carried unit refuses orders");
    carrier.flags |= OA_UNIT_FLAG_AIR_BASE;
    check(unit_accepts_order(*f.world, u), "unit on an air base accepts orders");
    u.flags &= ~OA_UNIT_FLAG_SELECTABLE;
    check(!unit_accepts_order(*f.world, u), "unit without spawn flag refuses orders");
}

void test_default_order() {
    Fixture f;
    const auto hooks = f.hooks();
    Unit& tank = f.unit(1, 0, 1);
    f.def(1).abilities = OA_UNIT_DEF_ABILITY_CAN_ATTACK | OA_UNIT_DEF_ABILITY_CAN_MOVE;
    Unit& enemy = f.unit(5, 1, 2);
    Unit& friendly = f.unit(2, 0, 2);
    const FixedVec3 at{};
    check(
        order_cursor(*f.world, OrderCommand::default_order, tank, &enemy, at, hooks) ==
            OrderCursor::attack,
        "default order over an enemy attacks"
    );
    check(
        order_cursor(*f.world, OrderCommand::default_order, tank, &friendly, at, hooks) ==
            OrderCursor::select,
        "default order over an own unit selects"
    );
    check(
        order_cursor(*f.world, OrderCommand::default_order, tank, nullptr, at, hooks) ==
            OrderCursor::move,
        "default order over ground moves"
    );
    f.def(1).abilities = 0;
    check(
        order_cursor(*f.world, OrderCommand::default_order, tank, nullptr, at, hooks) ==
            OrderCursor::normal,
        "immobile unit shows the normal cursor over ground"
    );

    // A constructor: reclaims enemies and features, repairs unfinished friends.
    f.def(1).abilities = OA_UNIT_DEF_ABILITY_CAN_RECLAMATE | OA_UNIT_DEF_ABILITY_CAN_REPAIR |
                         OA_UNIT_DEF_ABILITY_CAN_MOVE;
    check(
        order_cursor(*f.world, OrderCommand::default_order, tank, &enemy, at, hooks) ==
            OrderCursor::reclaim,
        "constructor over an enemy reclaims"
    );
    friendly.build_remaining = 0.5F;
    check(
        order_cursor(*f.world, OrderCommand::default_order, tank, &friendly, at, hooks) ==
            OrderCursor::repair,
        "constructor over an unfinished unit repairs"
    );
    f.feature_here = true;
    check(
        order_cursor(*f.world, OrderCommand::default_order, tank, nullptr, at, hooks) ==
            OrderCursor::reclaim,
        "constructor over a reclaimable feature on mapped ground reclaims"
    );
    f.mapped = false;
    check(
        order_cursor(*f.world, OrderCommand::default_order, tank, nullptr, at, hooks) ==
            OrderCursor::move,
        "a feature on ground never mapped is not offered"
    );
    f.mapped = true;

    // The other interface only highlights.
    int32_t other = interface_right_click;
    f.world->game.interface_type = other;
    check(
        order_cursor(*f.world, OrderCommand::default_order, tank, &enemy, at, hooks) ==
            OrderCursor::enemy,
        "highlight interface marks enemies"
    );
    friendly.build_remaining = 0.0F;
    check(
        order_cursor(*f.world, OrderCommand::default_order, tank, &friendly, at, hooks) ==
            OrderCursor::select,
        "highlight interface selects own units"
    );
}

void test_armed_orders() {
    Fixture f;
    const auto hooks = f.hooks();
    Unit& actor = f.unit(1, 0, 1);
    Unit& enemy = f.unit(5, 1, 2);
    const FixedVec3 at{};
    f.def(1).abilities = OA_UNIT_DEF_ABILITY_CAN_ATTACK;
    actor.movement = 0;
    f.in_range = false;
    check(
        order_cursor(*f.world, OrderCommand::attack, actor, &enemy, at, hooks) ==
            OrderCursor::attack_out_of_range,
        "static attacker out of range"
    );
    f.in_range = true;
    check(
        order_cursor(*f.world, OrderCommand::attack, actor, &enemy, at, hooks) ==
            OrderCursor::attack,
        "static attacker in range"
    );
    f.world->game.weapon_defs[3].flags = OA_WEAPON_FLAG_DROPPED;
    f.def(1).weapon1 = oa_ref_from_index(3);
    check(
        order_cursor(*f.world, OrderCommand::attack, actor, &enemy, at, hooks) ==
            OrderCursor::attack_dropped,
        "bomber shows the dropped-weapon cursor"
    );

    f.def(1).abilities = OA_UNIT_DEF_ABILITY_CAN_DGUN;
    f.world->game.weapon_defs[4].energy_per_shot = 500.0F;
    actor.weapons[2].def = oa_ref_from_index(4);
    check(
        order_cursor(*f.world, OrderCommand::blast, actor, &enemy, at, hooks) ==
            OrderCursor::attack,
        "affordable d-gun"
    );
    f.world->game.players[0].energy = 100.0F;
    check(
        order_cursor(*f.world, OrderCommand::blast, actor, &enemy, at, hooks) ==
            OrderCursor::attack_out_of_range,
        "unaffordable d-gun"
    );

    f.def(1).abilities = OA_UNIT_DEF_ABILITY_CAN_PATROL | OA_UNIT_DEF_ABILITY_CAN_LOAD |
                         OA_UNIT_DEF_ABILITY_CAN_CAPTURE;
    check(
        order_cursor(*f.world, OrderCommand::patrol, actor, nullptr, at, hooks) ==
            OrderCursor::patrol,
        "patrol"
    );
    check(
        order_cursor(*f.world, OrderCommand::unload, actor, nullptr, at, hooks) ==
            OrderCursor::unload,
        "unload"
    );
    check(
        order_cursor(*f.world, OrderCommand::capture, actor, &enemy, at, hooks) ==
            OrderCursor::capture,
        "capture enemy"
    );
    check(
        order_cursor(*f.world, OrderCommand::capture, actor, &actor, at, hooks) ==
            OrderCursor::normal,
        "cannot capture own unit"
    );
    check(
        order_cursor(*f.world, OrderCommand::teleport, actor, nullptr, at, hooks) ==
            OrderCursor::teleport,
        "an armed teleport command shows the teleport cursor"
    );
    check(
        order_cursor(*f.world, OrderCommand::stop, actor, nullptr, at, hooks) ==
            OrderCursor::normal,
        "stop has no cursor of its own"
    );
}

void test_transport() {
    Fixture f;
    Unit& transport = f.unit(1, 0, 1);
    Unit& cargo = f.unit(2, 0, 2);
    f.def(1).abilities = OA_UNIT_DEF_ABILITY_CAN_LOAD;
    f.def(1).transport_capacity = 1;
    f.def(1).transport_size = 3;
    f.def(1).flags = OA_UNIT_DEF_FLAG_CAN_FLY;
    f.def(2).footprint_x = 2;
    cargo.position.y = 1 << 16;
    check(can_load_unit(*f.world, transport, cargo), "air transport loads a small unit");
    const auto hooks = f.hooks();
    check(
        order_cursor(*f.world, OrderCommand::load, transport, &cargo, {}, hooks) ==
            OrderCursor::load_by_air,
        "air load cursor"
    );
    f.def(2).footprint_x = 4;
    check(!can_load_unit(*f.world, transport, cargo), "too large to load");
    f.def(2).footprint_x = 2;
    Unit& passenger = f.unit(3, 0, 2);
    transport.attach_first_child = world_unit_ref(f.world, &passenger);
    check(!can_load_unit(*f.world, transport, cargo), "transport full");
    transport.attach_first_child = 0;
    f.def(2).abilities = OA_UNIT_DEF_ABILITY_CANT_BE_TRANSPORTED;
    check(!can_load_unit(*f.world, transport, cargo), "cargo refuses transport");
}

// ui.interface-fixes pad-cursor: a flyer moving over its own air base shows
// the unload cursor, or the load cursor under the fix.
void test_pad_cursor() {
    Fixture f;
    Unit& flyer = f.unit(1, 0, 1);
    Unit& pad = f.unit(2, 0, 2);
    f.def(1).abilities = OA_UNIT_DEF_ABILITY_CAN_MOVE;
    f.def(1).flags = OA_UNIT_DEF_FLAG_CAN_FLY;
    f.def(2).flags = OA_UNIT_DEF_FLAG_IS_AIRBASE;
    auto hooks = f.hooks();
    check(
        order_cursor(*f.world, OrderCommand::move, flyer, &pad, {}, hooks) == OrderCursor::unload,
        "3.1c pad cursor unloads"
    );
    hooks.pad_load_cursor = true;
    check(
        order_cursor(*f.world, OrderCommand::move, flyer, &pad, {}, hooks) == OrderCursor::load,
        "fixed pad cursor loads"
    );
    f.def(2).flags = 0;
    check(
        order_cursor(*f.world, OrderCommand::move, flyer, &pad, {}, hooks) == OrderCursor::move,
        "no pad, no load cursor"
    );
}

// The air layer (occupancy 2) is a flying unit's; a carried unit is in
// layer 0. Water weapons decide targets under the sea.
void test_airborne_and_water_targets() {
    Fixture f;
    const auto hooks = f.hooks();
    Unit& gun = f.unit(1, 0, 1);
    Unit& target = f.unit(5, 1, 2);
    Unit& carrier = f.unit(6, 1, 3);
    for (Unit* unit : {&gun, &target, &carrier})
        unit->flags |= OA_UNIT_FLAG_LIVE;
    f.def(1).abilities = OA_UNIT_DEF_ABILITY_CAN_ATTACK | OA_UNIT_DEF_ABILITY_CAN_MOVE;
    gun.flags |= OA_UNIT_FLAG_HAS_WEAPONS;
    const FixedVec3 at{};
    const auto attack = [&] {
        return unit_order(*f.world, OrderCommand::attack, gun, &target, &at, hooks);
    };
    constexpr uint32_t airborne = 2;
    target.attach_parent = world_unit_ref(f.world, &carrier);
    check(attack() == UnitOrder::attack_chase, "a ground gun takes a carried unit");
    gun.weapons[0].def = oa_ref_from_index(1);
    f.world->game.weapon_defs[1].flags = OA_WEAPON_FLAG_TO_AIR_WEAPON;
    check(attack() == UnitOrder::none, "an anti-air gun does not take a carried unit");
    target.attach_parent = 0;
    target.flags |= airborne;
    check(attack() == UnitOrder::attack_chase, "an anti-air gun takes a flying unit");
    target.flags &= ~OA_UNIT_FLAG_OCCUPANCY_MASK;
    f.world->game.weapon_defs[1].flags = 0;

    // A submarine: its top (model height plus y) is under the sea.
    f.world->game.sea_level = 10;
    f.def(2).model_height = 4 << 16;
    target.position.y = 0;
    check(attack() == UnitOrder::none, "a gun ship does not take a submerged submarine");
    gun.weapons[1].def = oa_ref_from_index(2);
    f.world->game.weapon_defs[2].flags = OA_WEAPON_FLAG_WATER_WEAPON;
    check(attack() == UnitOrder::none, "a depth charge that is not enabled takes nothing");
    gun.weapons[1].flags = OA_UNIT_WEAPON_ENABLED;
    check(
        attack() == UnitOrder::attack_chase, "a destroyer's enabled depth charge takes a submarine"
    );

    // A hovercraft with a water primary takes nothing at or above the sea.
    gun.weapons[1] = {};
    gun.weapons[0].def = oa_ref_from_index(2);
    f.def(1).flags = OA_UNIT_DEF_FLAG_CAN_HOVER;
    target.position.y = 6 << 16;
    check(attack() == UnitOrder::none, "a hovercraft's water weapon misses a surface target");
    target.position.y = 0;
    check(
        attack() == UnitOrder::attack_chase, "a hovercraft's water weapon takes a submerged target"
    );
}

// The order table gives load and unload orders only where they apply.
void test_transport_orders() {
    Fixture f;
    const auto hooks = f.hooks();
    Unit& atlas = f.unit(1, 0, 1);
    Unit& tank = f.unit(2, 0, 2);
    Unit& pad = f.unit(3, 0, 3);
    Unit& cargo = f.unit(5, 1, 2);
    for (Unit* unit : {&atlas, &tank, &pad, &cargo})
        unit->flags |= OA_UNIT_FLAG_LIVE;
    f.def(1).abilities = OA_UNIT_DEF_ABILITY_CAN_LOAD | OA_UNIT_DEF_ABILITY_CAN_MOVE;
    f.def(1).flags = OA_UNIT_DEF_FLAG_CAN_FLY;
    f.def(1).transport_capacity = 1;
    f.def(1).transport_size = 3;
    f.def(2).abilities = OA_UNIT_DEF_ABILITY_CAN_MOVE;
    f.def(2).footprint_x = 2;
    f.def(2).model_height = 4 << 16;
    f.def(3).flags = OA_UNIT_DEF_FLAG_IS_AIRBASE;
    const FixedVec3 at{};
    const auto order = [&](OrderCommand command, const Unit& actor, const Unit* target) {
        return unit_order(*f.world, command, actor, target, &at, hooks);
    };
    check(order(OrderCommand::load, atlas, &cargo) == UnitOrder::vtol_pickup, "an Atlas loads");
    check(order(OrderCommand::load, tank, &cargo) == UnitOrder::none, "a tank loads nothing");
    f.def(2).abilities |= OA_UNIT_DEF_ABILITY_CANT_BE_TRANSPORTED;
    check(
        order(OrderCommand::load, atlas, &cargo) == UnitOrder::none,
        "cantbetransported refuses the load"
    );
    f.def(2).abilities &= ~OA_UNIT_DEF_ABILITY_CANT_BE_TRANSPORTED;
    cargo.flags |= 2U;
    check(order(OrderCommand::load, atlas, &cargo) == UnitOrder::none, "a flying unit is refused");
    cargo.flags &= ~OA_UNIT_FLAG_OCCUPANCY_MASK;
    cargo.build_remaining = 0.5F;
    check(order(OrderCommand::load, atlas, &cargo) == UnitOrder::none, "an unfinished unit");
    cargo.build_remaining = 0.0F;
    f.world->game.sea_level = 10;
    check(order(OrderCommand::load, atlas, &cargo) == UnitOrder::none, "a submerged unit");
    f.world->game.sea_level = 0;

    check(
        order(OrderCommand::unload, atlas, &pad) == UnitOrder::vtol_landing,
        "a canload flyer's unload over an air base lands"
    );
    check(
        order(OrderCommand::unload, atlas, nullptr) == UnitOrder::vtol_unload,
        "a canload flyer's unload elsewhere drops"
    );
    check(order(OrderCommand::unload, tank, nullptr) == UnitOrder::none, "a tank unloads nothing");
    check(
        order(OrderCommand::move, atlas, &pad) == UnitOrder::vtol_landing,
        "a flyer moved onto an allied pad lands"
    );

    // The group order binds the pointer unit for LOAD and MOVE but not for
    // UNLOAD, which drops at the ground under the pointer.
    atlas.flags |= OA_UNIT_FLAG_SELECTED;
    tank.flags |= OA_UNIT_FLAG_SELECTED;
    set_pointer_flags(f.world->game, pointer_over_view);
    SelectionOrder orders[4];
    f.world->game.cursor_unit_id = cargo.id;
    uint32_t count = selection_orders(*f.world, OrderCommand::load, hooks, orders, 4);
    check(
        count == 1 && orders[0].actor == &atlas && orders[0].order == UnitOrder::vtol_pickup,
        "an armed LOAD orders the Atlas alone"
    );
    f.world->game.cursor_unit_id = pad.id;
    count = selection_orders(*f.world, OrderCommand::unload, hooks, orders, 4);
    check(
        count == 1 && orders[0].actor == &atlas && orders[0].order == UnitOrder::vtol_unload,
        "an armed UNLOAD drops through the Atlas alone"
    );
    count = selection_orders(*f.world, OrderCommand::move, hooks, orders, 4);
    check(
        count == 2 && orders[0].order == UnitOrder::vtol_landing &&
            orders[1].order == UnitOrder::move_ground,
        "MOVE onto a pad lands the Atlas and moves the tank"
    );
}

void test_repair_and_reclaim() {
    Fixture f;
    Unit& builder = f.unit(1, 0, 1);
    Unit& damaged = f.unit(2, 0, 2);
    f.def(1).abilities = OA_UNIT_DEF_ABILITY_CAN_REPAIR | OA_UNIT_DEF_ABILITY_CAN_RECLAMATE;
    f.def(1).max_water_depth = 0;
    f.world->game.sea_level = 0;
    check(can_repair_unit(*f.world, builder, damaged), "damaged unit is repairable");
    damaged.health = 100;
    check(!can_repair_unit(*f.world, builder, damaged), "full health is not repairable");
    damaged.health = 10;
    f.world->game.sea_level = 20;
    check(
        !can_repair_unit(*f.world, builder, damaged),
        "submerged target out of a land builder's reach"
    );
    f.def(1).max_water_depth = 30;
    check(can_repair_unit(*f.world, builder, damaged), "builder reaches into its water depth");
    check(can_reclaim_unit(*f.world, builder, damaged), "reclaimable unit");
    f.def(2).abilities = OA_UNIT_DEF_ABILITY_CAN_CAPTURE;
    check(!can_reclaim_unit(*f.world, builder, damaged), "capturing units cannot be reclaimed");
}

void test_selection_cursor() {
    Fixture f;
    const auto hooks = f.hooks();
    Unit& own = f.unit(1, 0, 1);
    Unit& enemy = f.unit(5, 1, 2);
    f.def(1).abilities = OA_UNIT_DEF_ABILITY_CAN_ATTACK | OA_UNIT_DEF_ABILITY_CAN_MOVE;
    set_pointer_flags(f.world->game, pointer_over_view);
    set_pointer_command(f.world->game, OrderCommand::default_order);
    check(pointer_command(f.world->game) == OrderCommand::default_order, "pointer command field");
    f.world->game.cursor_unit_id = own.id;
    check(
        selection_order_cursor(*f.world, OrderCommand::default_order, hooks) == OrderCursor::select,
        "nothing selected: own unit under the pointer selects"
    );
    f.world->game.cursor_unit_id = enemy.id;
    check(
        selection_order_cursor(*f.world, OrderCommand::default_order, hooks) == OrderCursor::normal,
        "nothing selected: enemy under the pointer is normal"
    );
    own.flags |= OA_UNIT_FLAG_SELECTED;
    check(
        selection_order_cursor(*f.world, OrderCommand::default_order, hooks) == OrderCursor::attack,
        "selected attacker over an enemy"
    );
    const Unit* selected[4];
    check(
        collect_selected_units(*f.world, selected, 4) == 1 && selected[0] == &own, "selected list"
    );
    OrderCursor cursor{};
    check(
        pointer_cursor(*f.world, hooks, &cursor) && cursor == OrderCursor::attack,
        "pointer over the view resolves the selection cursor"
    );
    set_pointer_flags(f.world->game, 0);
    check(
        pointer_cursor(*f.world, hooks, &cursor) && cursor == OrderCursor::normal,
        "pointer outside the view shows the normal cursor"
    );
    set_pointer_flags(f.world->game, pointer_over_view);
    set_pointer_command(f.world->game, OrderCommand::build);
    check(!pointer_cursor(*f.world, hooks, &cursor), "build placement owns the pointer");
    const FixedVec3 at{1, 2, 3};
    set_pointer_position(f.world->game, at);
    const FixedVec3 back = pointer_position(f.world->game);
    check(back.x == 1 && back.y == 2 && back.z == 3, "pointer position field");
}

void test_unit_orders() {
    Fixture f;
    const auto hooks = f.hooks();
    Unit& builder = f.unit(1, 0, 1);
    Unit& unfinished = f.unit(2, 0, 2);
    Unit& damaged = f.unit(3, 0, 2);
    Unit& tank = f.unit(4, 0, 3);
    Unit& enemy = f.unit(5, 1, 2);
    for (Unit* unit : {&builder, &unfinished, &damaged, &tank, &enemy})
        unit->flags |= OA_UNIT_FLAG_LIVE;
    f.def(1).abilities = OA_UNIT_DEF_ABILITY_CAN_REPAIR | OA_UNIT_DEF_ABILITY_CAN_RECLAMATE |
                         OA_UNIT_DEF_ABILITY_CAN_MOVE | OA_UNIT_DEF_ABILITY_CAN_GUARD;
    f.def(3).abilities = OA_UNIT_DEF_ABILITY_CAN_ATTACK | OA_UNIT_DEF_ABILITY_CAN_MOVE |
                         OA_UNIT_DEF_ABILITY_CAN_GUARD;
    unfinished.build_remaining = 0.5F;
    const FixedVec3 at{};
    const auto order = [&](OrderCommand command, const Unit& actor, const Unit* target) {
        return unit_order(*f.world, command, actor, target, &at, hooks);
    };
    using enum OrderCommand;
    check(
        order(default_order, builder, &unfinished) == UnitOrder::help_build,
        "builder assists a unit under construction by default"
    );
    check(
        order(default_order, builder, &damaged) == UnitOrder::none,
        "default click on a finished own unit selects instead"
    );
    check(
        order(default_order, builder, &enemy) == UnitOrder::reclaim_unit,
        "constructor reclaims an enemy by default"
    );
    check(
        order(default_order, builder, nullptr) == UnitOrder::move_ground,
        "default over ground moves"
    );
    check(
        order(default_order, tank, &unfinished) == UnitOrder::move_ground,
        "a tank moves to an unfinished unit"
    );
    check(order(repair, builder, &damaged) == UnitOrder::repair_unit, "repair a damaged unit");
    check(
        order(repair, builder, &unfinished) == UnitOrder::help_build,
        "repair on an unfinished unit assists"
    );
    check(order(repair, tank, &damaged) == UnitOrder::none, "a tank cannot repair");
    damaged.health = 100;
    check(order(repair, builder, &damaged) == UnitOrder::none, "nothing to repair at full health");
    check(
        order(move, builder, &damaged) == UnitOrder::follow_ground,
        "move over a healthy ally guards"
    );
    damaged.health = 10;
    check(
        order(move, builder, &damaged) == UnitOrder::repair_unit, "move over a damaged ally repairs"
    );
    check(
        order(move, builder, &unfinished) == UnitOrder::help_build,
        "move over an unfinished ally assists"
    );
    check(
        order(move, tank, &enemy) == UnitOrder::move_ground, "move over an enemy moves a plain tank"
    );
    builder.movement = 0;
    check(order(move, builder, nullptr) == UnitOrder::qmove, "an immobile actor queues its move");
    builder.movement = 1;
    check(order(guard, tank, &damaged) == UnitOrder::follow_ground, "guard an ally");
    check(order(guard, tank, &enemy) == UnitOrder::none, "cannot guard an enemy");
    f.def(1).flags = OA_UNIT_DEF_FLAG_CAN_FLY;
    check(
        order(repair, builder, &unfinished) == UnitOrder::vtol_help_build,
        "an aircraft builder assists with the VTOL order"
    );
    check(
        order(repair, builder, &damaged) == UnitOrder::vtol_repair_unit,
        "an aircraft builder repairs with the VTOL order"
    );
    check(
        order(guard, builder, &damaged) == UnitOrder::vtol_follow,
        "an aircraft guards with the VTOL order"
    );
    f.def(1).flags = 0;
    enemy.flags &= ~OA_UNIT_FLAG_LIVE;
    check(
        order(guard, tank, &enemy) == UnitOrder::none &&
            order(default_order, builder, &enemy) == UnitOrder::none,
        "a target that is not live yields nothing"
    );
    enemy.flags |= OA_UNIT_FLAG_LIVE;

    tank.flags |= OA_UNIT_FLAG_HAS_WEAPONS;
    check(
        order(default_order, tank, &enemy) == UnitOrder::attack_chase,
        "an armed mobile unit chases an enemy by default"
    );
    tank.movement = 0;
    tank.flags |= OA_UNIT_FLAG_BUILDING;
    check(order(attack, tank, &enemy) == UnitOrder::attack_nomove, "a structure attacks in place");
    tank.movement = 1;
    tank.flags &= ~OA_UNIT_FLAG_BUILDING;
    check(order(attack, tank, &damaged) == UnitOrder::suppress, "attack on an ally suppresses");
    f.def(3).flags = OA_UNIT_DEF_FLAG_CAN_FLY;
    check(
        order(attack, tank, &enemy) == UnitOrder::air_to_ground, "aircraft attack ground targets"
    );
    f.world->game.weapon_defs[3].flags = OA_WEAPON_FLAG_DROPPED;
    f.def(3).weapon1 = oa_ref_from_index(3);
    check(order(attack, tank, &enemy) == UnitOrder::air_strike, "bombers strike");
    f.def(2).flags = OA_UNIT_DEF_FLAG_CAN_FLY;
    check(order(attack, tank, &enemy) == UnitOrder::none, "bombers cannot hit aircraft");
    f.world->game.weapon_defs[3].flags = 0;
    check(order(attack, tank, &enemy) == UnitOrder::air_to_air, "fighters engage aircraft");
    f.def(2).flags = 0;
    tank.flags &= ~OA_UNIT_FLAG_HAS_WEAPONS;
    f.def(3).flags = OA_UNIT_DEF_FLAG_KAMIKAZE;
    check(order(attack, tank, &enemy) == UnitOrder::attack_kamikaze, "an unarmed kamikaze rams");
    f.def(3).flags = 0;

    int32_t other = interface_right_click;
    f.world->game.interface_type = other;
    check(
        order(default_order, builder, &damaged) == UnitOrder::repair_unit,
        "right-click interface repairs a damaged ally by default"
    );
    check(
        order(default_order, builder, &unfinished) == UnitOrder::help_build,
        "right-click interface assists by default"
    );
    check(
        order(default_order, tank, &damaged) == UnitOrder::follow_ground,
        "right-click interface guards by default"
    );
    check(
        order(default_order, builder, &enemy) == UnitOrder::reclaim_unit,
        "right-click interface reclaims an enemy by default"
    );
    other = 0;
    f.world->game.interface_type = other;

    f.def(1).abilities |= OA_UNIT_DEF_ABILITY_CAN_PATROL;
    f.def(3).abilities |= OA_UNIT_DEF_ABILITY_CAN_PATROL;
    check(
        order(patrol, builder, nullptr) == UnitOrder::repair_patrol, "a repairer patrols repairing"
    );
    check(order(patrol, tank, nullptr) == UnitOrder::patrol, "patrol");
    check(order(stop, tank, nullptr) == UnitOrder::stop, "stop");
    check(order(capture, tank, &enemy) == UnitOrder::none, "capture needs the ability");
    f.def(3).abilities |= OA_UNIT_DEF_ABILITY_CAN_CAPTURE;
    check(order(capture, tank, &enemy) == UnitOrder::capture, "capture an enemy");
    check(order(move, tank, &enemy) == UnitOrder::capture, "move over an enemy captures when able");
    f.feature_here = true;
    check(
        order(reclaim, builder, nullptr) == UnitOrder::reclaim,
        "reclaim the feature under the pointer"
    );
    check(
        order(reclaim, builder, &damaged) == UnitOrder::reclaim,
        "a feature on mapped ground wins over the unit"
    );
    f.mapped = false;
    check(
        order(reclaim, builder, nullptr) == UnitOrder::none,
        "a feature on ground never mapped is not reclaimed"
    );
    f.mapped = true;
    f.feature_here = false;
    check(order(reclaim, builder, &damaged) == UnitOrder::reclaim_unit, "reclaim a unit");
    check(order(reclaim, builder, nullptr) == UnitOrder::none, "nothing to reclaim");

    // The movement object, not Unit.movement, marks a mobile actor.
    auto structures = hooks;
    structures.movement_object = [](void*, const World&, const Unit&) { return false; };
    const auto structure_order = [&](OrderCommand command, const Unit& actor) {
        return unit_order(*f.world, command, actor, nullptr, &at, structures);
    };
    check(tank.movement != 0, "the tank has a model");
    check(
        structure_order(patrol, tank) == UnitOrder::qpatrol,
        "no movement object patrols with QPatrol"
    );
    check(structure_order(move, tank) == UnitOrder::qmove, "no movement object moves with QMove");
    check(
        std::strcmp(unit_order_name(UnitOrder::qpatrol), "QPATROL") == 0 &&
            std::strcmp(unit_order_name(UnitOrder::vtol_unload), "VTOL_UNLOAD") == 0 &&
            unit_order_name(UnitOrder::none)[0] == '\0',
        "order-table names"
    );
}

void test_selection_orders() {
    Fixture f;
    const auto hooks = f.hooks();
    Unit& builder = f.unit(1, 0, 1);
    Unit& tank = f.unit(2, 0, 3);
    Unit& unfinished = f.unit(3, 0, 2);
    Unit& damaged = f.unit(4, 0, 2);
    for (Unit* unit : {&builder, &tank, &unfinished, &damaged})
        unit->flags |= OA_UNIT_FLAG_LIVE;
    f.def(1).abilities = OA_UNIT_DEF_ABILITY_CAN_REPAIR | OA_UNIT_DEF_ABILITY_CAN_MOVE;
    f.def(3).abilities = OA_UNIT_DEF_ABILITY_CAN_MOVE;
    unfinished.build_remaining = 0.5F;
    builder.flags |= OA_UNIT_FLAG_SELECTED;
    tank.flags |= OA_UNIT_FLAG_SELECTED;
    set_pointer_flags(f.world->game, pointer_over_view);
    set_pointer_command(f.world->game, OrderCommand::default_order);
    f.world->game.cursor_unit_id = unfinished.id;
    const auto cursor_for = [&](OrderCommand command) {
        return selection_order_cursor(*f.world, command, hooks);
    };
    check(
        cursor_for(OrderCommand::default_order) == OrderCursor::repair,
        "a selected builder shows the repair cursor over a unit under construction"
    );
    check(
        click_action(*f.world, OrderCommand::default_order, OrderCursor::repair) ==
            ClickAction::issue_command,
        "the repair cursor issues on click"
    );
    SelectionOrder orders[4];
    uint32_t count = selection_orders(*f.world, OrderCommand::default_order, hooks, orders, 4);
    check(
        count == 2 && orders[0].actor == &builder && orders[0].order == UnitOrder::help_build &&
            orders[1].actor == &tank && orders[1].order == UnitOrder::move_ground,
        "the builder assists and the tank moves"
    );
    unfinished.flags |= OA_UNIT_FLAG_SELECTED;
    count = selection_orders(*f.world, OrderCommand::default_order, hooks, orders, 4);
    check(count == 2, "the pointer unit never acts on itself");
    count = selection_orders(*f.world, OrderCommand::stop, hooks, orders, 4);
    check(count == 3, "stop does not bind the pointer unit");
    unfinished.flags &= ~OA_UNIT_FLAG_SELECTED;
    count = selection_orders(*f.world, OrderCommand::repair, hooks, orders, 4);
    check(
        count == 1 && orders[0].actor == &builder && orders[0].order == UnitOrder::help_build,
        "an armed REPAIR assists through the builder only"
    );

    f.world->game.cursor_unit_id = damaged.id;
    check(
        cursor_for(OrderCommand::default_order) == OrderCursor::select,
        "a damaged finished unit shows the select cursor by default"
    );
    check(
        click_action(*f.world, OrderCommand::default_order, OrderCursor::select) ==
            ClickAction::select_unit,
        "the select cursor selects"
    );
    check(
        cursor_for(OrderCommand::repair) == OrderCursor::repair,
        "an armed REPAIR shows the repair cursor over it"
    );
    count = selection_orders(*f.world, OrderCommand::repair, hooks, orders, 4);
    check(count == 1 && orders[0].order == UnitOrder::repair_unit, "an armed REPAIR repairs it");
    const auto action_for = [&](OrderCommand command, OrderCursor cursor) {
        return click_action(*f.world, command, cursor);
    };
    check(
        action_for(OrderCommand::default_order, OrderCursor::normal) == ClickAction::none &&
            action_for(OrderCommand::repair, OrderCursor::normal) == ClickAction::none,
        "a highlight cursor does nothing in the left-click interface"
    );
    int32_t other = interface_right_click;
    f.world->game.interface_type = other;
    check(
        action_for(OrderCommand::default_order, OrderCursor::enemy) ==
                ClickAction::clear_selection &&
            action_for(OrderCommand::repair, OrderCursor::normal) == ClickAction::none,
        "the right-click interface clears the selection on a default highlight click"
    );
}

// A 16.16 value of whole map pixels and a fraction of 1/65536 pixels.
constexpr int32_t pixels(int32_t whole, int32_t fraction = 0) {
    return whole * 0x10000 + fraction;
}

// A unit at a point, for the group order's arithmetic alone.
Unit unit_at(int32_t x, int32_t z) {
    Unit unit{};
    unit.position = {x, 0, z};
    return unit;
}

GroupCentre centre_of(std::initializer_list<const Unit*> units) {
    GroupCentre centre{};
    for (const Unit* unit : units)
        add_to_group_centre(centre, *unit);
    return centre;
}

bool is_at(const FixedVec3& point, int32_t x, int32_t y, int32_t z) {
    return point.x == x && point.y == y && point.z == z;
}

void test_group_order_point() {
    const FixedVec3 point{pixels(500), pixels(40), pixels(700)};
    // Three units whose whole pixels average 110.67 across and down: the
    // centre is 110, 110, and each keeps its displacement from it, the
    // fraction of a pixel included, at the ordered point's height.
    const Unit a = unit_at(pixels(100, 0x8000), pixels(100));
    const Unit b = unit_at(pixels(132), pixels(100));
    const Unit c = unit_at(pixels(100), pixels(132, 0x4000));
    GroupCentre centre = centre_of({&a, &b, &c});
    check(centre.units == 3 && centre.sum_x == 332 && centre.sum_z == 332, "the centre's sums");
    check(
        is_at(group_order_point(centre, a, point), pixels(490, 0x8000), pixels(40), pixels(690)) &&
            is_at(group_order_point(centre, b, point), pixels(522), pixels(40), pixels(690)) &&
            is_at(
                group_order_point(centre, c, point), pixels(490), pixels(40), pixels(722, 0x4000)
            ),
        "a compact group keeps its shape around the point"
    );

    // Three of a group make a reach of 9000 square pixels: 90 and 30 pixels
    // from the centre (8100 + 900) is inside, 90 and 31 (8100 + 961) is not.
    const Unit edge = unit_at(pixels(1090), pixels(1030));
    const Unit inner = unit_at(pixels(955), pixels(985));
    centre = centre_of({&edge, &inner, &inner});
    check(
        is_at(group_order_point(centre, edge, point), pixels(590), pixels(40), pixels(730)),
        "a unit exactly at the reach keeps its place"
    );
    const Unit beyond = unit_at(pixels(1090), pixels(1031));
    centre = centre_of({&beyond, &inner, &inner});
    check(
        is_at(group_order_point(centre, beyond, point), point.x, point.y, point.z) &&
            is_at(group_order_point(centre, inner, point), pixels(455), pixels(40), pixels(685)),
        "a unit one pixel past the reach goes to the point; the others keep their places"
    );

    // Units far apart all go to the point: two 400 pixels apart are each 200
    // from their centre, past the 77 pixels two of them reach.
    const Unit west = unit_at(pixels(1000), pixels(1000));
    const Unit east = unit_at(pixels(1400), pixels(1000));
    centre = centre_of({&west, &east});
    check(
        is_at(group_order_point(centre, west, point), point.x, point.y, point.z) &&
            is_at(group_order_point(centre, east, point), point.x, point.y, point.z),
        "a spread-out group all goes to the point"
    );

    // The average whole pixel is truncated toward zero: -5 / 2 is -2, so the
    // unit at -3 lands one pixel left of the point and the one at -2 on it.
    const Unit left = unit_at(pixels(-3), pixels(0));
    const Unit right = unit_at(pixels(-2), pixels(0));
    centre = centre_of({&left, &right});
    check(
        centre.sum_x == -5 &&
            is_at(group_order_point(centre, left, point), pixels(499), pixels(40), pixels(700)) &&
            is_at(group_order_point(centre, right, point), pixels(500), pixels(40), pixels(700)),
        "the centre is truncated toward zero"
    );
    // A unit's whole pixel is the signed high half of its 16.16 value: -1.5
    // counts as -2, half a pixel left of the unit.
    const Unit half = unit_at(-0x18000, pixels(0));
    centre = centre_of({&half});
    check(
        centre.sum_x == -2 &&
            is_at(
                group_order_point(centre, half, point), pixels(500, 0x8000), pixels(40), pixels(700)
            ),
        "a negative position counts by its signed high half"
    );
    check(
        is_at(group_order_point(GroupCentre{}, half, point), point.x, point.y, point.z),
        "an empty group sends a unit to the point"
    );

    using enum UnitOrder;
    for (const UnitOrder order :
         {move_ground, vtol_move, patrol, vtol_patrol, repair_patrol, vtol_repair_patrol})
        check(order_keeps_group_shape(order), "moves and patrols keep the group's shape");
    for (const UnitOrder order :
         {none,
          qmove,
          qpatrol,
          attack_chase,
          suppress,
          air_to_ground,
          ground_unload,
          vtol_unload,
          vtol_landing,
          follow_ground,
          vtol_follow,
          reclaim,
          help_build,
          attack_special})
        check(!order_keeps_group_shape(order), "other orders go to the point itself");
}

void test_selection_group_orders() {
    Fixture f;
    const auto hooks = f.hooks();
    // Two tanks, a solar collector, an aircraft and a factory of the local
    // player, selected; a selected tank further off and an unselected one.
    f.world->game.players[0].last_unit = world_unit_ref(f.world, &f.world->units[10]);
    f.world->game.players[1].first_unit = world_unit_ref(f.world, &f.world->units[11]);
    f.world->game.players[1].last_unit = world_unit_ref(f.world, &f.world->units[14]);
    Unit& tank = f.unit(1, 0, 1);
    Unit& second = f.unit(2, 0, 1);
    Unit& solar = f.unit(3, 0, 2);
    Unit& flyer = f.unit(4, 0, 3);
    Unit& outlier = f.unit(5, 0, 1);
    Unit& idle = f.unit(6, 0, 1);
    Unit& factory = f.unit(7, 0, 4);
    f.def(1).abilities = OA_UNIT_DEF_ABILITY_CAN_MOVE | OA_UNIT_DEF_ABILITY_CAN_PATROL;
    f.def(3).abilities = OA_UNIT_DEF_ABILITY_CAN_MOVE | OA_UNIT_DEF_ABILITY_CAN_PATROL;
    f.def(3).flags = OA_UNIT_DEF_FLAG_CAN_FLY;
    f.def(4).abilities = OA_UNIT_DEF_ABILITY_CAN_MOVE | OA_UNIT_DEF_ABILITY_CAN_PATROL;
    solar.movement = 0;
    factory.movement = 0;
    const std::initializer_list<std::pair<Unit*, std::pair<int32_t, int32_t>>> places = {
        {&tank, {100, 100}},
        {&second, {132, 100}},
        {&solar, {100, 132}},
        {&flyer, {132, 132}},
        {&outlier, {500, 100}},
        {&idle, {2000, 2000}},
        {&factory, {116, 116}},
    };
    for (const auto& [unit, xz] : places) {
        unit->position = {pixels(xz.first), pixels(10), pixels(xz.second)};
        unit->flags |= OA_UNIT_FLAG_LIVE | (unit == &idle ? 0U : OA_UNIT_FLAG_SELECTED);
    }
    const FixedVec3 point{pixels(500), pixels(20), pixels(500)};
    set_pointer_flags(f.world->game, pointer_over_view);
    set_pointer_position(f.world->game, point);
    // The outlying tank is the unit under the pointer: neither counted nor
    // ordered. The centre of the other five, the solar collector among them,
    // is 116, 116.
    f.world->game.cursor_unit_id = outlier.id;
    check(selection_centre(*f.world, &outlier).units == 5, "the selection's centre counts five");
    SelectionOrder orders[8];
    uint32_t count = selection_orders(*f.world, OrderCommand::move, hooks, orders, 8);
    check(
        count == 4 && orders[0].actor == &tank && orders[0].order == UnitOrder::move_ground &&
            is_at(orders[0].position, pixels(484), pixels(20), pixels(484)) &&
            orders[1].actor == &second &&
            is_at(orders[1].position, pixels(516), pixels(20), pixels(484)) &&
            orders[2].actor == &flyer && orders[2].order == UnitOrder::vtol_move &&
            is_at(orders[2].position, pixels(516), pixels(20), pixels(516)) &&
            orders[3].actor == &factory && orders[3].order == UnitOrder::qmove &&
            is_at(orders[3].position, point.x, point.y, point.z),
        "a move keeps the group's shape, measured with the collector that takes no move; "
        "the factory's move goes to the point"
    );
    count = selection_orders(*f.world, OrderCommand::patrol, hooks, orders, 8);
    check(
        count == 4 && orders[0].order == UnitOrder::patrol &&
            is_at(orders[0].position, pixels(484), pixels(20), pixels(484)) &&
            orders[1].order == UnitOrder::patrol && orders[2].order == UnitOrder::vtol_patrol &&
            is_at(orders[2].position, pixels(516), pixels(20), pixels(516)) &&
            orders[3].order == UnitOrder::qpatrol &&
            is_at(orders[3].position, point.x, point.y, point.z),
        "a patrol keeps the shape; the factory's patrol goes to the point"
    );
    count = selection_orders(*f.world, OrderCommand::stop, hooks, orders, 8);
    check(
        count == 6 && orders[4].actor == &outlier &&
            is_at(orders[0].position, point.x, point.y, point.z),
        "a stop binds no unit and moves no point"
    );
    // With nothing under the pointer the outlying tank counts: the centre of six
    // is 180, 113, and the outlying tank, 320 pixels from it, goes to the point.
    f.world->game.cursor_unit_id = 0;
    count = selection_orders(*f.world, OrderCommand::move, hooks, orders, 8);
    check(
        count == 5 && is_at(orders[0].position, pixels(420), pixels(20), pixels(487)) &&
            is_at(orders[2].position, pixels(452), pixels(20), pixels(519)) &&
            orders[3].actor == &outlier && is_at(orders[3].position, point.x, point.y, point.z),
        "a unit far from the selection's centre goes to the point"
    );
}

// An 8x8-cell map carrying a 1x1 tree at cell (2, 3), a 2x2 rock with its
// origin at (4, 4), a scar at (1, 1) and a 3x2 wreck at (0, 6), with the
// continuation cells the feature placement writes (rows back in the low byte
// of MapPlot.feature_record, columns back in its high byte).
struct FeatureMap {
    static constexpr int32_t width = 8;
    static constexpr uint16_t tree = 0;
    static constexpr uint16_t rock = 1;
    static constexpr uint16_t scar = 2;
    static constexpr uint16_t wreck = 3;
    static constexpr uint16_t continuation = 0xfffe;
    static constexpr uint16_t none = 0xffff;
    World* world = world_create();
    MapPlot plots[width * width]{};

    FeatureMap() {
        const WorldCapacity capacity{16, 8, 4};
        if (world == nullptr || world_alloc_tables(world, &capacity) == 0)
            std::abort();
        world->game.local_player_index = 0;
        for (uint8_t i = 0; i < 2; ++i) {
            world->game.players[i].index = i;
            world->game.players[i].alliance[i] = 1;
        }
        world->game.players[0].first_unit = world_unit_ref(world, &world->units[1]);
        world->game.players[0].last_unit = world_unit_ref(world, &world->units[4]);
        world->game.map_width = width;
        world->game.map_height = width;
        world->game.feature_def_count = 4;
        world->plots = plots;
        for (MapPlot& plot : plots)
            plot.feature = none;
        FeatureDef* defs = world->feature_defs;
        defs[tree].flags =
            OA_FEATURE_FLAG_SPRITE | OA_FEATURE_FLAG_FLAMABLE | OA_FEATURE_FLAG_RECLAIMABLE;
        defs[tree].footprint_x = defs[tree].footprint_z = 1;
        defs[tree].height = 66;
        defs[rock].flags =
            OA_FEATURE_FLAG_SPRITE | OA_FEATURE_FLAG_BLOCKING | OA_FEATURE_FLAG_RECLAIMABLE;
        defs[rock].footprint_x = defs[rock].footprint_z = 2;
        defs[scar].flags = OA_FEATURE_FLAG_SPRITE;
        defs[wreck].flags = OA_FEATURE_FLAG_BLOCKING | OA_FEATURE_FLAG_RECLAIMABLE;
        defs[wreck].footprint_x = 3;
        defs[wreck].footprint_z = 2;
        place(tree, 2, 3, 1, 1);
        place(rock, 4, 4, 2, 2);
        place(scar, 1, 1, 1, 1);
        place(wreck, 0, 6, 3, 2);
    }

    ~FeatureMap() { world_destroy(world); }

    MapPlot& plot(int32_t x, int32_t z) { return plots[z * width + x]; }

    void place(uint16_t feature, int32_t x, int32_t z, int32_t size_x, int32_t size_z) {
        for (int32_t row = 0; row < size_z; ++row)
            for (int32_t column = 0; column < size_x; ++column) {
                MapPlot& cell = plot(x + column, z + row);
                cell.feature = row == 0 && column == 0 ? feature : continuation;
                cell.feature_record =
                    static_cast<uint16_t>(row == 0 && column == 0 ? 0 : (column << 8) | row);
            }
    }

    // A 16.16 point `px`, `pz` map pixels into the map at height `py`.
    static FixedVec3 at(int32_t px, int32_t py, int32_t pz) {
        return {px * 0x10000, py * 0x10000, pz * 0x10000};
    }
};

void test_feature_at_position() {
    FeatureMap m;
    const FeatureDef* defs = m.world->feature_defs;
    const auto feature_at = [&](int32_t px, int32_t py, int32_t pz) {
        return feature_at_position(*m.world, FeatureMap::at(px, py, pz));
    };
    for (int32_t px = 32; px < 48; px += 5)
        for (int32_t pz = 48; pz < 64; pz += 5)
            check(
                feature_at(px, 0, pz) == &defs[FeatureMap::tree],
                "every point of the tree's cell names the tree"
            );
    check(
        feature_at_position(*m.world, {0x2FFFFF, 0, 0x30FFFF}) == &defs[FeatureMap::tree] &&
            feature_at_position(*m.world, {0x200000, 0, 0x300000}) == &defs[FeatureMap::tree],
        "the cell is the position shifted down 20 bits"
    );
    check(
        feature_at(40, 200, 56) == &defs[FeatureMap::tree], "the height plays no part in the cell"
    );
    check(
        feature_at(31, 0, 56) == nullptr && feature_at(48, 0, 56) == nullptr &&
            feature_at(40, 0, 47) == nullptr && feature_at(40, 0, 64) == nullptr,
        "the cells around the tree are empty"
    );
    for (int32_t cell_x = 4; cell_x < 6; ++cell_x)
        for (int32_t cell_z = 4; cell_z < 6; ++cell_z)
            check(
                feature_at(cell_x * 16 + 8, 30, cell_z * 16 + 8) == &defs[FeatureMap::rock],
                "each cell of the rock's footprint names the rock"
            );
    for (int32_t cell_x = 0; cell_x < 3; ++cell_x)
        for (int32_t cell_z = 6; cell_z < 8; ++cell_z)
            check(
                feature_at(cell_x * 16 + 1, 0, cell_z * 16 + 15) == &defs[FeatureMap::wreck],
                "each cell of the wreck's footprint names the wreck"
            );
    check(
        feature_at(24, 0, 24) == &defs[FeatureMap::scar],
        "a feature that is not reclaimable is still found"
    );
    check(
        feature_at(-1, 0, 56) == nullptr && feature_at(40, 0, -1) == nullptr &&
            feature_at(128, 0, 56) == nullptr && feature_at(40, 0, 128) == nullptr,
        "off the map there is no plot"
    );
    m.plot(7, 0).feature = 4;
    check(feature_at(120, 0, 8) == nullptr, "a word past the feature table is no feature");
}

void test_drawn_feature() {
    FeatureMap m;
    const FeatureDef* defs = m.world->feature_defs;
    constexpr int32_t map_pixels = FeatureMap::width * 16;
    const auto found = [&](const FixedVec3& ground) -> const FeatureDef* {
        const auto drawn = drawn_feature(*m.world, ground);
        return drawn ? drawn->def : nullptr;
    };
    for (const int32_t height : {0, 40}) {
        for (MapPlot& plot : m.plots)
            plot.height = static_cast<uint8_t>(height);
        bool same = true;
        for (int32_t px = 0; px < map_pixels; px += 3)
            for (int32_t pz = 0; pz < map_pixels; pz += 3) {
                const auto ground = FeatureMap::at(px, height, pz);
                same = same && found(ground) == feature_at_position(*m.world, ground);
            }
        check(
            same,
            "on level ground a feature is found on the cell the ground point falls in, as "
            "feature_at_position finds it"
        );
    }

    // A wreck sunk to a sea bed at height 0 under a sea at height 60: the
    // pointer meets the sea, 30 rows above the bed as drawn.
    std::vector<sim::feature_runtime::PlacedFeature> records(2);
    m.world->placed_features = reinterpret_cast<uint8_t*>(records.data());
    m.world->placed_feature_count = static_cast<uint32_t>(records.size());
    for (MapPlot& plot : m.plots)
        plot.height = 0;
    m.world->game.sea_level = 60;
    m.place(FeatureMap::wreck, 3, 1, 3, 2);
    MapPlot& origin = m.plot(3, 1);
    origin.flags = static_cast<uint8_t>(origin.flags | OA_PLOT_FLAG_ANIMATING_FEATURE);
    origin.feature_record = 1;
    records[1].model.position = FeatureMap::at(72, 0, 32);
    // The ground point over the water drawn on a screen row, as map Z.
    const auto over_water = [](int32_t px, int32_t row) {
        return FeatureMap::at(px, 60, row + 30);
    };
    bool on_bed = true;
    for (int32_t px = 48; px < 96; px += 3)
        for (int32_t row = 16; row < 48; row += 3) {
            const auto drawn = drawn_feature(*m.world, over_water(px, row));
            on_bed = on_bed && drawn && drawn->def == &defs[FeatureMap::wreck] &&
                     drawn->position.x == px * 0x10000 && drawn->position.y == 0 &&
                     drawn->position.z == row * 0x10000;
        }
    check(
        on_bed,
        "a sunken wreck is found over every row it is drawn on, at the point of its footprint "
        "on the sea bed"
    );
    check(
        found(over_water(56, 40)) == &defs[FeatureMap::wreck] &&
            feature_at_position(*m.world, over_water(56, 40)) == nullptr,
        "the cell under the sea's ground point misses the sunken wreck where it is drawn"
    );
    bool water_above = true;
    for (int32_t row = 0; row < 16; row += 3)
        water_above =
            water_above && found(over_water(72, row)) == nullptr &&
            feature_at_position(*m.world, over_water(72, row)) == &defs[FeatureMap::wreck];
    check(
        water_above,
        "the water above a sunken wreck, which the cell under the sea's ground point names, "
        "finds nothing"
    );
    records[1].model.position.y = 30 * 0x10000;
    check(
        found(over_water(72, 20)) == &defs[FeatureMap::wreck] &&
            found(over_water(72, 40)) == nullptr,
        "a sinking wreck is found where its record is drawn on the way down"
    );

    // A tree on a plot at height 64 one row south of the sunken wreck is
    // drawn over the wreck's upper rows, nearer the viewer.
    records[1].model.position.y = 0;
    m.place(FeatureMap::tree, 4, 3, 1, 1);
    m.plot(4, 3).height = 64;
    check(
        found(over_water(72, 20)) == &defs[FeatureMap::tree] &&
            found(over_water(72, 40)) == &defs[FeatureMap::wreck],
        "of two features drawn over one point, the one nearer the viewer is found"
    );
    m.world->placed_features = nullptr;
    m.world->placed_feature_count = 0;
}

void test_feature_cursor_across_footprint() {
    FeatureMap m;
    OrderCursorHooks hooks{
        nullptr,
        [](void*, const World&, const FixedVec3&) { return true; },
        [](void*, const World& world, const FixedVec3& position) {
            return feature_at_position(world, position);
        },
        nullptr,
        nullptr,
    };
    Unit& builder = m.world->units[1];
    builder.id = 1;
    builder.def = oa_ref_from_index(1);
    builder.owner = world_player_ref_of(m.world, &m.world->game.players[0]);
    builder.flags = OA_UNIT_FLAG_SELECTABLE | OA_UNIT_FLAG_SELECTED;
    builder.movement = 1;
    UnitDef& def = m.world->unit_defs[1];
    def.abilities = OA_UNIT_DEF_ABILITY_CAN_MOVE | OA_UNIT_DEF_ABILITY_CAN_RECLAMATE;
    const auto cursor_at = [&](int32_t px, int32_t py, int32_t pz, OrderCommand command) {
        return order_cursor(*m.world, command, builder, nullptr, FeatureMap::at(px, py, pz), hooks);
    };
    // The default order shows 0xb over a reclaimable feature's cell and
    // 0x13 less 5 (move) for a mover over anything else.
    for (int32_t px = 32; px < 48; px += 3)
        for (int32_t pz = 48; pz < 64; pz += 3)
            check(
                cursor_at(px, 90, pz, OrderCommand::default_order) == OrderCursor::reclaim,
                "a builder shows Reclaim anywhere over the tree's cell"
            );
    for (int32_t pz = 64; pz < 96; pz += 7)
        check(
            cursor_at(40, 90, pz, OrderCommand::default_order) == OrderCursor::move,
            "the cells in front of the tree show Move"
        );
    for (int32_t cell_x = 4; cell_x < 6; ++cell_x)
        for (int32_t cell_z = 4; cell_z < 6; ++cell_z)
            check(
                cursor_at(cell_x * 16 + 3, 0, cell_z * 16 + 12, OrderCommand::default_order) ==
                    OrderCursor::reclaim,
                "a builder shows Reclaim over every cell of the rock"
            );
    for (int32_t cell_x = 0; cell_x < 3; ++cell_x)
        check(
            cursor_at(cell_x * 16 + 8, 0, 7 * 16 + 8, OrderCommand::reclaim) ==
                OrderCursor::reclaim,
            "RECLAIM armed shows Reclaim over the wreck's far row"
        );
    check(
        cursor_at(24, 0, 24, OrderCommand::default_order) == OrderCursor::move,
        "a scar that is not reclaimable shows Move"
    );
    check(
        cursor_at(24, 0, 24, OrderCommand::reclaim) == OrderCursor::normal,
        "RECLAIM armed over a scar shows the normal cursor"
    );
    def.abilities |= OA_UNIT_DEF_ABILITY_CAN_RESURRECT;
    check(
        cursor_at(88, 0, 72, OrderCommand::default_order) == OrderCursor::resurrect,
        "a resurrecting builder shows Resurrect over the rock"
    );
    def.abilities = OA_UNIT_DEF_ABILITY_CAN_MOVE;
    check(
        cursor_at(40, 0, 56, OrderCommand::default_order) == OrderCursor::move,
        "a unit that cannot reclaim shows Move over the tree"
    );
    def.abilities = OA_UNIT_DEF_ABILITY_CAN_MOVE | OA_UNIT_DEF_ABILITY_CAN_RECLAMATE;
    set_pointer_flags(m.world->game, pointer_over_view);
    set_pointer_command(m.world->game, OrderCommand::default_order);
    set_pointer_position(m.world->game, FeatureMap::at(33, 70, 62));
    OrderCursor cursor{};
    check(
        pointer_cursor(*m.world, hooks, &cursor) && cursor == OrderCursor::reclaim,
        "the frame's cursor over the tree is Reclaim"
    );
    const FixedVec3 at_tree = FeatureMap::at(40, 0, 56);
    check(
        unit_order(*m.world, OrderCommand::default_order, builder, nullptr, &at_tree, hooks) ==
            UnitOrder::reclaim,
        "a click over the tree gives the builder Reclaim"
    );
}

void set_interface(Game& game, int32_t type) {
    game.interface_type = type;
}

// The radar wins over the view unless a drag box is open, bit 0x04
// follows either, and the other bits stay.
void test_pointer_area() {
    Fixture f;
    Game& game = f.world->game;
    set_pointer_flags(game, pointer_radar_scroll | pointer_over_view);
    set_pointer_area(game, true, false);
    check(
        pointer_flags(game) == (pointer_radar_scroll | pointer_over_radar | pointer_over_map),
        "over the radar the view bit drops and the scroll bit stays"
    );
    set_pointer_area(game, false, true);
    check(
        pointer_flags(game) == (pointer_radar_scroll | pointer_over_view | pointer_over_map),
        "over the view the radar bit drops"
    );
    set_pointer_area(game, false, false);
    check(pointer_flags(game) == pointer_radar_scroll, "over the panel neither area is set");
    set_pointer_flags(game, pointer_box_drag);
    set_pointer_area(game, true, false);
    check(pointer_flags(game) == pointer_box_drag, "a drag box keeps the radar out");
    set_pointer_area(game, true, true);
    check(
        pointer_flags(game) == (pointer_box_drag | pointer_over_view | pointer_over_map),
        "a drag box over both areas keeps the view"
    );
}

// A right press in both interface types.
void test_right_press() {
    Fixture f;
    Game& game = f.world->game;
    const auto press = [&](OrderCommand command, uint8_t flags, uint32_t keys) {
        set_pointer_command(game, command);
        set_pointer_flags(game, flags);
        game.pointer_state[2] = keys;
        return right_press(game);
    };
    const uint8_t view = pointer_over_view | pointer_over_map;
    const uint8_t radar = pointer_over_radar | pointer_over_map;
    for (const int32_t type : {interface_left_click, interface_right_click}) {
        set_interface(game, type);
        for (const auto command : {OrderCommand::move, OrderCommand::build, OrderCommand::none})
            check(
                press(command, view, pointer_key_right) == RightPress::cancel_command,
                "an armed command is cancelled in either interface"
            );
        check(
            press(OrderCommand::patrol, 0, pointer_key_right) == RightPress::cancel_command,
            "the cancel does not depend on the pointer area"
        );
        check(pointer_flags(game) == 0, "a cancel leaves the pointer flags");
    }
    set_interface(game, interface_left_click);
    check(
        press(OrderCommand::default_order, view, pointer_key_right) == RightPress::clear_selection,
        "left-click interface: a right press over the view deselects"
    );
    check(
        press(OrderCommand::default_order, view, pointer_key_right | pointer_key_shift) ==
            RightPress::clear_selection,
        "shift does not change the deselect"
    );
    check(
        press(OrderCommand::default_order, view, pointer_key_right | pointer_key_control) ==
            RightPress::mouse_look,
        "control over the view starts mouse look"
    );
    check(
        press(OrderCommand::default_order, radar, pointer_key_right) == RightPress::radar_scroll,
        "a right press over the radar scrolls with it"
    );
    check(pointer_flags(game) == (radar | pointer_radar_scroll), "the scroll flag is set");
    check(
        press(OrderCommand::default_order, radar, pointer_key_right | pointer_key_control) ==
            RightPress::radar_scroll,
        "control does not change the radar scroll"
    );
    check(
        press(OrderCommand::default_order, 0, pointer_key_right) == RightPress::none,
        "over the panel a right press does nothing"
    );
    check(
        press(OrderCommand::default_order, pointer_over_map, pointer_key_right) == RightPress::none,
        "the left-click interface does not read the map bit"
    );
    set_interface(game, interface_right_click);
    check(
        press(OrderCommand::default_order, view, pointer_key_right | pointer_key_control) ==
            RightPress::default_order,
        "right-click interface: a right press over the view gives the default order"
    );
    for (const uint32_t keys : {pointer_key_right, pointer_key_right | pointer_key_shift})
        check(
            press(OrderCommand::default_order, radar, keys) == RightPress::radar_jump,
            "right-click interface: a right press over the radar moves the view, not an order"
        );
    check(pointer_flags(game) == radar, "the jump sets no scroll flag");
    for (const uint8_t area : {pointer_over_view, pointer_over_radar})
        check(
            press(OrderCommand::default_order, area, pointer_key_right) == RightPress::none,
            "the right-click interface needs the map bit"
        );
    set_interface(game, 2);
    check(
        press(OrderCommand::default_order, view, pointer_key_right) == RightPress::default_order,
        "any interface value but 0 takes the right-click branch"
    );
}

// The left button scrolls with the radar in the right-click
// interface; each interface's scroll ends only on its own button's release.
void test_radar_scroll() {
    Fixture f;
    Game& game = f.world->game;
    const uint8_t radar = pointer_over_radar | pointer_over_map;
    set_pointer_command(game, OrderCommand::default_order);
    set_interface(game, interface_left_click);
    set_pointer_flags(game, radar);
    check(!start_left_radar_scroll(game), "no left scroll in the left-click interface");
    set_interface(game, interface_right_click);
    set_pointer_command(game, OrderCommand::attack);
    check(!start_left_radar_scroll(game), "no left scroll with a command armed");
    set_pointer_command(game, OrderCommand::default_order);
    game.mouse_look_active = 1;
    check(!start_left_radar_scroll(game), "no left scroll during mouse look");
    game.mouse_look_active = 0;
    set_pointer_flags(game, radar | pointer_box_drag);
    check(!start_left_radar_scroll(game), "no left scroll with a drag box open");
    set_pointer_flags(game, pointer_over_view | pointer_over_map);
    check(!start_left_radar_scroll(game), "no left scroll over the view");
    set_pointer_flags(game, radar);
    check(
        start_left_radar_scroll(game) && (pointer_flags(game) & pointer_radar_scroll) != 0,
        "a left press over the radar scrolls in the right-click interface"
    );
    check(!end_radar_scroll(game, true), "the right release does not end the left scroll");
    check(
        end_radar_scroll(game, false) && pointer_flags(game) == radar, "the left release ends it"
    );
    check(!end_radar_scroll(game, false), "nothing to end twice");
    set_interface(game, interface_left_click);
    check(right_press(game) == RightPress::radar_scroll, "right press starts the scroll");
    check(!end_radar_scroll(game, false), "the left release does not end the right scroll");
    check(
        end_radar_scroll(game, true) && pointer_flags(game) == radar, "the right release ends it"
    );
}

// orders.reclaim-command-any-unit: the Reclaim command's cursor over a unit is
// reclaim whatever the unit; the order it issues is the one 3.1c issues.
void test_reclaim_command_any_unit() {
    Fixture f;
    auto hooks = f.hooks();
    data::match_rules::MatchRules rules{};
    hooks.rules.match = &rules;
    Unit& builder = f.unit(1, 0, 1);
    Unit& tank = f.unit(2, 0, 2);
    Unit& commander = f.unit(5, 1, 3);
    Unit& fighter = f.unit(6, 1, 4);
    Unit& wreck_tank = f.unit(7, 1, 2);
    for (Unit* unit : {&builder, &tank, &commander, &fighter, &wreck_tank})
        unit->flags |= OA_UNIT_FLAG_LIVE;
    f.def(1).abilities = OA_UNIT_DEF_ABILITY_CAN_RECLAMATE | OA_UNIT_DEF_ABILITY_CAN_MOVE;
    f.def(2).abilities = OA_UNIT_DEF_ABILITY_CAN_MOVE;
    f.def(3).abilities = OA_UNIT_DEF_ABILITY_CAN_CAPTURE;
    fighter.flags |= 2U; // airborne
    const FixedVec3 at{};
    const auto cursor = [&](const Unit& actor, const Unit* target) {
        return order_cursor(*f.world, OrderCommand::reclaim, actor, target, at, hooks);
    };
    const auto order = [&](const Unit& actor, const Unit* target) {
        return unit_order(*f.world, OrderCommand::reclaim, actor, target, &at, hooks);
    };

    check(cursor(builder, &wreck_tank) == OrderCursor::reclaim, "3.1c: a plain unit reclaims");
    check(cursor(builder, &commander) == OrderCursor::normal, "3.1c: a commander refuses");
    check(cursor(builder, &fighter) == OrderCursor::normal, "3.1c: an airborne unit refuses");
    check(cursor(tank, &wreck_tank) == OrderCursor::normal, "3.1c: a non-reclaimer refuses");

    rules.orders.reclaim_command_any_unit.enabled = true;
    check(cursor(builder, &commander) == OrderCursor::reclaim, "any unit: over a commander");
    check(cursor(builder, &fighter) == OrderCursor::reclaim, "any unit: over an airborne unit");
    check(cursor(builder, &wreck_tank) == OrderCursor::reclaim, "any unit: over a plain unit");
    check(cursor(tank, &commander) == OrderCursor::reclaim, "any unit: even for a non-reclaimer");
    check(cursor(builder, nullptr) == OrderCursor::normal, "any unit: bare ground stays normal");
    check(
        click_action(*f.world, OrderCommand::reclaim, cursor(builder, &commander)) ==
            ClickAction::issue_command,
        "any unit: the click over a commander issues the command"
    );
    check(order(builder, &commander) == UnitOrder::reclaim_unit, "the reclaimer is ordered");
    check(order(tank, &commander) == UnitOrder::none, "a non-reclaimer gets no order");
    check(
        order_cursor(*f.world, OrderCommand::move, builder, &commander, at, hooks) ==
            OrderCursor::move,
        "any unit: the Move command keeps its own test"
    );
}

// orders.resurrector-reclaims-features: the Reclaim command makes a resurrector
// reclaim a feature; the default order still resurrects it.
void test_resurrector_reclaims_features() {
    Fixture f;
    auto hooks = f.hooks();
    data::match_rules::MatchRules rules{};
    hooks.rules.match = &rules;
    f.feature_here = true;
    Unit& necro = f.unit(1, 0, 1);
    Unit& plane = f.unit(2, 0, 2);
    f.def(1).abilities = OA_UNIT_DEF_ABILITY_CAN_RECLAMATE | OA_UNIT_DEF_ABILITY_CAN_RESURRECT |
                         OA_UNIT_DEF_ABILITY_CAN_MOVE;
    f.def(2).abilities = f.def(1).abilities;
    f.def(2).flags = OA_UNIT_DEF_FLAG_CAN_FLY;
    const FixedVec3 at{};
    const auto order = [&](OrderCommand command, const Unit& actor) {
        return unit_order(*f.world, command, actor, nullptr, &at, hooks);
    };
    check(order(OrderCommand::reclaim, necro) == UnitOrder::resurrect, "3.1c: Reclaim resurrects");
    rules.orders.resurrector_reclaims_features.enabled = true;
    check(order(OrderCommand::reclaim, necro) == UnitOrder::reclaim, "Reclaim now reclaims");
    check(
        order(OrderCommand::reclaim, plane) == UnitOrder::vtol_reclaim,
        "an aircraft reclaims through its VTOL order"
    );
    check(
        order(OrderCommand::default_order, necro) == UnitOrder::resurrect,
        "the default order still resurrects"
    );
    f.world->game.interface_type = interface_right_click;
    check(
        order(OrderCommand::default_order, necro) == UnitOrder::resurrect,
        "the right-click default order still resurrects"
    );
    f.world->game.interface_type = interface_left_click;
    check(
        order_cursor(*f.world, OrderCommand::reclaim, necro, nullptr, at, hooks) ==
            OrderCursor::reclaim,
        "the Reclaim cursor is unchanged"
    );
    f.feature_here = false;
    check(order(OrderCommand::reclaim, necro) == UnitOrder::none, "nothing to reclaim");
}

// air.no-repair-retreat-flag: the Move command and a right press in the
// right-click interface keep aircraft marked cantbetransported off repair
// pads, cursor and order alike.
void test_no_repair_retreat_flag() {
    Fixture f;
    auto hooks = f.hooks();
    data::match_rules::MatchRules rules{};
    hooks.rules.match = &rules;
    using Flag = data::match_rules::AirNoRepairRetreatFlagFlag;
    Unit& bomber = f.unit(1, 0, 1);
    Unit& lifter = f.unit(2, 0, 2);
    Unit& pad = f.unit(3, 0, 3);
    Unit& cargo = f.unit(4, 0, 4);
    for (Unit* unit : {&bomber, &lifter, &pad, &cargo})
        unit->flags |= OA_UNIT_FLAG_LIVE;
    f.def(1).abilities = OA_UNIT_DEF_ABILITY_CAN_MOVE | OA_UNIT_DEF_ABILITY_CANT_BE_TRANSPORTED;
    f.def(1).flags = OA_UNIT_DEF_FLAG_CAN_FLY;
    f.def(2).abilities = OA_UNIT_DEF_ABILITY_CAN_MOVE | OA_UNIT_DEF_ABILITY_CAN_LOAD |
                         OA_UNIT_DEF_ABILITY_CANT_BE_TRANSPORTED | OA_UNIT_DEF_ABILITY_CAN_GUARD;
    f.def(2).flags = OA_UNIT_DEF_FLAG_CAN_FLY;
    f.def(2).transport_capacity = 1;
    f.def(2).transport_size = 3;
    f.def(3).flags = OA_UNIT_DEF_FLAG_IS_AIRBASE;
    f.def(3).model_height = 4 << 16;
    f.def(3).footprint_x = 2;
    f.def(4).abilities = OA_UNIT_DEF_ABILITY_CAN_MOVE;
    const FixedVec3 at{};
    const auto cursor = [&](OrderCommand command, const Unit& actor) {
        return order_cursor(*f.world, command, actor, &pad, at, hooks);
    };
    const auto order = [&](OrderCommand command, const Unit& actor) {
        return unit_order(*f.world, command, actor, &pad, &at, hooks);
    };

    check(cursor(OrderCommand::move, bomber) == OrderCursor::unload, "3.1c: the pad cursor");
    check(order(OrderCommand::move, bomber) == UnitOrder::vtol_landing, "3.1c: Move lands");

    rules.air.no_repair_retreat_flag.enabled = true;
    rules.air.no_repair_retreat_flag.flag = Flag::none;
    check(cursor(OrderCommand::move, bomber) == OrderCursor::unload, "flag none: the pad cursor");
    check(order(OrderCommand::move, bomber) == UnitOrder::vtol_landing, "flag none: Move lands");

    rules.air.no_repair_retreat_flag.flag = Flag::cantbetransported;
    check(cursor(OrderCommand::move, bomber) == OrderCursor::move, "marked: the move cursor");
    check(order(OrderCommand::move, bomber) == UnitOrder::vtol_move, "marked: Move just moves");
    // A marked transport falls through to the load test, then to guard.
    pad.movement = 1;
    check(
        cursor(OrderCommand::move, lifter) == OrderCursor::load_by_air,
        "marked transport: the load cursor"
    );
    check(
        order(OrderCommand::move, lifter) == UnitOrder::vtol_pickup,
        "marked transport: Move picks the pad up"
    );
    pad.movement = 0;
    check(
        cursor(OrderCommand::move, lifter) == OrderCursor::guard, "marked: then the guard cursor"
    );
    check(order(OrderCommand::move, lifter) == UnitOrder::vtol_follow, "marked: then Move guards");
    check(
        order(OrderCommand::unload, lifter) == UnitOrder::vtol_landing,
        "marked: Unload over a pad still lands"
    );
    // A right press on a pad in the right-click interface does not land a
    // marked aircraft either: it falls through to the load and guard tests,
    // then moves.
    f.world->game.interface_type = interface_right_click;
    check(
        order(OrderCommand::default_order, bomber) == UnitOrder::vtol_move,
        "marked: the right-click default order moves"
    );
    check(
        order(OrderCommand::default_order, lifter) == UnitOrder::vtol_follow,
        "marked: the right-click default order guards"
    );
    f.world->game.interface_type = interface_left_click;
    f.def(1).abilities &= ~OA_UNIT_DEF_ABILITY_CANT_BE_TRANSPORTED;
    check(cursor(OrderCommand::move, bomber) == OrderCursor::unload, "unmarked: the pad cursor");
    check(order(OrderCommand::move, bomber) == UnitOrder::vtol_landing, "unmarked: Move lands");
    f.world->game.interface_type = interface_right_click;
    check(
        order(OrderCommand::default_order, bomber) == UnitOrder::vtol_landing,
        "unmarked: the right-click default order lands"
    );
    f.world->game.interface_type = interface_left_click;
}
} // namespace

int main() {
    test_accepts_order();
    test_default_order();
    test_armed_orders();
    test_transport();
    test_pad_cursor();
    test_airborne_and_water_targets();
    test_transport_orders();
    test_repair_and_reclaim();
    test_selection_cursor();
    test_unit_orders();
    test_selection_orders();
    test_group_order_point();
    test_selection_group_orders();
    test_feature_at_position();
    test_drawn_feature();
    test_feature_cursor_across_footprint();
    test_pointer_area();
    test_right_press();
    test_radar_scroll();
    test_reclaim_command_any_unit();
    test_resurrector_reclaims_features();
    test_no_repair_retreat_flag();
    if (failures != 0)
        return 1;
    std::puts("order cursor tests passed");
    return 0;
}
