// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "match_tick_access.hpp"
#include "oa/base/game_math.hpp"
#include <array>
#include <bit>
#include <iostream>
#include <stdexcept>
#include <string>
#include "oa/test/match_services.hpp"

using namespace oa;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " #x);   \
    } while (false)

namespace {
struct Services : oa::test::QuietServices {
    std::vector<uint32_t> sounds;

    void command_sound(sim::unit_spawn::Slot&, uint32_t category) override {
        sounds.push_back(category);
    }
};

struct Scenario : sim::scenario::DefinitionHost {
    int32_t gravity{112};

    int32_t integer(std::string_view key, int32_t fallback) override {
        return key == "gravity" ? gravity : fallback;
    }

    std::optional<std::string> text(std::string_view) override { return std::nullopt; }
};

constexpr uint8_t fighter_type = 1;
constexpr uint8_t tank_type = 2;
constexpr uint8_t pad_type = 3;
constexpr size_t type_count = 4;

constexpr uint32_t wait_attack = 0x100e8;
constexpr uint32_t wait_attack_or_cancel = 0x100ea;
// The standing fire order a unit fires at will on.
constexpr uint32_t fire_at_will = 2;
constexpr uint32_t wait_goal_or_cancel = 0xe2;

struct Fixture {
    formats::tnt::Map map;
    std::vector<sim::visibility_state::TerrainCell> terrain_values;
    std::array<sim::visibility_state::SightMask, 1> masks{};
    std::shared_ptr<formats::objects3d::Model> model;
    std::shared_ptr<formats::cob::CobProgram> script;
    std::array<sim::unit_spawn::LoadedType, type_count> loaded{};
    std::array<sim::unit_spawn::Type, type_count> types{};
    std::array<data::unit_definitions::UnitDefinition, type_count> defs{};
    data::unit_definitions::UnitTargetCategoryMasks target_masks;
    data::unit_definitions::RuntimeDefinitionMetadata metadata;
    std::vector<sim::spatial_state::Plot> collision_plots;
    std::array<sim::match_runtime::RuntimeTypeFields, type_count> fields{};
    std::array<uint8_t, 1> yard{4};
    sim::combat_state::WeaponRegistry weapons;
    Services services;
    Scenario scenario;
    std::unique_ptr<sim::match_runtime::Match> match;

    explicit Fixture(int32_t gravity = 112) {
        scenario.gravity = gravity;
        map.attribute_width = map.attribute_height = 64;
        map.attributes.resize(64 * 64);
        terrain_values.resize(64 * 64);
        collision_plots.resize(64 * 64);
        masks[0].width = masks[0].height = 1;
        masks[0].pixels = {1};
        model = std::make_shared<formats::objects3d::Model>();
        model->objects.resize(1);
        model->objects[0].name = "root";
        script = std::make_shared<formats::cob::CobProgram>();
        script->code = {sim::script_vm::opcode::return_};
        script->scripts = {{"Create", 0}};
        script->entry_points = {0};
        script->piece_names = {"root"};
        (void)sim::combat_state::install_weapon_text(
            weapons,
            "[AIRGUN]{id=1; reloadtime=0.1; range=300; lineofsight=1; weaponvelocity=100; "
            "turret=1; unitsonly=1; groundbounce=1; interceptor=1; [DAMAGE]{default=10;}}"
        );
        for (size_t i = 1; i < type_count; ++i) {
            auto& type = types[i];
            type.simulation.flags = OA_UNIT_DEF_FLAG_AVAILABLE;
            type.simulation.maximum_health = 100;
            type.footprint_x = type.footprint_z = 1;
            type.bm_code = 1;
            type.model = reinterpret_cast<uintptr_t>(model.get());
            type.cob = reinterpret_cast<uintptr_t>(script.get());
            loaded[i].model = model;
            loaded[i].script = script;
            auto& def = defs[i];
            def.sight_distance = 400;
            def.acceleration_fixed = 0x10000;
            def.brake_rate_fixed = 0x10000;
            def.max_velocity_fixed = 4 * 0x10000;
            def.turn_rate = 1024;
            def.weapon1 = "AIRGUN";
            fields[i].definition = &def;
            fields[i].yard_mask = yard;
            fields[i].runtime_metadata = &metadata;
            fields[i].target_masks = &target_masks;
            fields[i].movement_class = 0;
        }
        types[fighter_type].simulation.flags |= OA_UNIT_DEF_FLAG_CAN_FLY;
        defs[fighter_type].cruise_altitude = 100;
        defs[fighter_type].attack_run_length = 50;
        types[pad_type].simulation.flags |= OA_UNIT_DEF_FLAG_BUILDER | OA_UNIT_DEF_FLAG_IS_AIRBASE;
        for (size_t i = 1; i < type_count; ++i)
            loaded[i].type = types[i];
        sim::match_runtime::OfflineInputs input{
            map, loaded, types, fields,    weapons, terrain_values,  masks, 8, 8, 4,    2,
            0,   30,     1,     &scenario, {},      collision_plots, {},    0, 0, 0.0F, {}
        };
        match = std::make_unique<sim::match_runtime::Match>(input, services);
        match->state().unit_defs[fighter_type].abilities = OA_UNIT_DEF_ABILITY_CAN_GUARD;
        match->configure_strategic_environment({0, 0.5f, 0});
        for (uint8_t player = 0; player < 2; ++player) {
            match->simulation().players[player].present = true;
            match->simulation().players[player].status = player == 0 ? 1 : 2;
            std::array<uint8_t, 10> allies{};
            allies[player] = 1;
            match->configure_player_alliances(player, allies);
        }
    }

    sim::unit_spawn::Slot& spawn(uint8_t player, uint8_t type, int32_t x, int32_t z) {
        auto* slot = match->create(
            {player,
             type,
             {static_cast<uint32_t>(x) << 16, 0, static_cast<uint32_t>(z) << 16},
             true,
             1,
             0}
        );
        CHECK(slot && slot->unit->object_present);
        slot->unit->flags |= OA_UNIT_FLAG_LIVE;
        return *slot;
    }

    uint32_t
    dispatch(sim::unit_spawn::Slot& s, sim::simulation_state::Order& order, uint32_t events) {
        sim::match_runtime::MatchTickAccess host(*match);
        return host.dispatch_mission(match->state(), s.record, order, events);
    }

    // Rebuilds a player's repair pad list as its knowledge refresh does
    // every 30 ticks; the game tick is put back afterwards.
    void file_pads(uint8_t player) {
        auto& game = match->state().game;
        const auto tick = game.tick;
        game.tick = match->sightings(player).refreshed_tick + 30;
        match->refresh_player_knowledge(player);
        game.tick = tick;
    }

    // An attack order from `from` at `to` rewritten to `kind`.
    sim::simulation_state::Order&
    attack(sim::unit_spawn::Slot& from, sim::unit_spawn::Slot& to, uint8_t kind) {
        const std::array<uint32_t, 3> at = to.unit->position;
        const sim::ground_orders::Point point{
            std::bit_cast<int32_t>(at[0]),
            std::bit_cast<int32_t>(at[1]),
            std::bit_cast<int32_t>(at[2])
        };
        (void)match->issue_order(
            from.unit_index,
            sim::match_runtime::attack_chase_kind,
            true,
            to.unit_index,
            &point,
            0,
            0
        );
        auto* order = from.unit->primary;
        while (order && order->next)
            order = order->next;
        CHECK(order);
        order->kind = kind;
        return *order;
    }
};

sim::simulation_state::Order& step(sim::simulation_state::Order& order, uint32_t result) {
    if (result == 1)
        ++order.phase;
    return order;
}

void air_strike() {
    Fixture f;
    auto& bomber = f.spawn(0, fighter_type, 100, 100);
    auto& tank = f.spawn(1, tank_type, 300, 100);
    auto& order = f.attack(bomber, tank, sim::match_runtime::air_strike_kind);
    const uint32_t tick = f.match->simulation().tick;

    CHECK(f.dispatch(bomber, order, 0) == 1);
    step(order, 1);
    // 200 units out: inside the run-in distance, so a pass goal is set.
    order.wait_events = 0;
    CHECK(f.dispatch(bomber, order, 0) == 1);
    CHECK(order.wait_events == wait_goal_or_cancel);
    step(order, 1);
    order.wait_events = 0;
    CHECK(f.dispatch(bomber, order, 0x20) == 1);
    CHECK(order.wait_events == wait_attack);
    step(order, 1);
    order.wait_events = 0;
    CHECK(f.dispatch(bomber, order, 0) == 1);
    step(order, 1);
    CHECK(f.dispatch(bomber, order, 0x20) == 1);
    order.wait_events = 0;
    CHECK(f.dispatch(bomber, order, 0) == 2);
    CHECK(order.wait_events == (wait_attack | 1) && order.wake_tick == tick + 1);
    step(order, 1);
    order.phase = 5;
    order.wait_events = 0;
    CHECK(f.dispatch(bomber, order, 0) == 1);
    CHECK(order.wait_events == wait_goal_or_cancel);
    const auto& aim = bomber.record.weapons[0];
    CHECK(aim.target_a == 300 && aim.target_b == 100);
    order.phase = 6;
    CHECK(f.dispatch(bomber, order, 0) == 2);
    CHECK(order.phase == 3 && order.wait_events == wait_goal_or_cancel);

    // Damaged below three quarters with an active pad in reach: land there,
    // once the owner's knowledge refresh has filed the pad on its pad list.
    auto& pad = f.spawn(0, pad_type, 150, 150);
    pad.record.state_flags |= 1;
    bomber.record.health = 70;
    order.phase = 6;
    CHECK(f.dispatch(bomber, order, 0) == 0);
    CHECK(
        bomber.unit->primary && bomber.unit->primary->kind != sim::ground_orders::vtol_landing_kind
    );
    f.file_pads(0);
    CHECK(f.match->sightings(0).pad_count == 1);
    order.phase = 6;
    CHECK(f.dispatch(bomber, order, 0) == 0);
    CHECK(
        bomber.unit->primary && bomber.unit->primary->kind == sim::ground_orders::vtol_landing_kind
    );
    CHECK(bomber.unit->primary->next == &order && order.wait_events == 0);

    // Target lost while allowed to fire: hand over to VTOL_SeekAttack.
    bomber.unit->flags |= 1u << OA_UNIT_FLAG_FIRE_ORDER_SHIFT;
    CHECK(!order.next);
    CHECK(f.dispatch(bomber, order, 0x8) == 5);
    CHECK(order.next && order.next->kind == sim::match_runtime::vtol_seek_attack_kind);
}

void air_strike_without_gravity() {
    Fixture f(0);
    auto& bomber = f.spawn(0, fighter_type, 100, 100);
    auto& tank = f.spawn(1, tank_type, 300, 100);
    auto& order = f.attack(bomber, tank, sim::match_runtime::air_strike_kind);
    order.phase = 4;
    CHECK(f.dispatch(bomber, order, 0) == 7);
}

void air_to_air() {
    Fixture f;
    auto& fighter = f.spawn(0, fighter_type, 100, 100);
    auto& enemy = f.spawn(1, fighter_type, 400, 400);
    auto& order = f.attack(fighter, enemy, sim::match_runtime::air_to_air_kind);
    const uint32_t tick = f.match->simulation().tick;
    CHECK(f.dispatch(fighter, order, 0) == 1);
    // The fighter starts landed, so the take-off climb's goal events (0xe0)
    // join the one-tick timer.
    CHECK(order.wait_events == 0xe1 && order.wake_tick == tick + 1);
    step(order, 1);
    order.wait_events = 0;
    const bool aligned = sim::unit_movement::facing_toward(
        enemy.record.position.x,
        enemy.record.position.z,
        fighter.record.position.x,
        fighter.record.position.z,
        fighter.record.heading
    );
    CHECK(f.dispatch(fighter, order, 0) == 2);
    CHECK(order.wait_events == (wait_attack | 1) && order.wake_tick == tick + 0x2d);
    order.wait_events = 0;
    const auto result = f.dispatch(fighter, order, 0x20);
    if (aligned) {
        CHECK(result == 2 && order.wait_events == 1);
    } else {
        CHECK(result == 0 && order.wait_events == 0);
        CHECK(fighter.unit->primary && fighter.unit->primary->kind == 48);
        CHECK(fighter.unit->primary->next == &order);
    }
    CHECK(f.dispatch(fighter, order, 0x10000) == 5);
}

void air_to_ground_hover() {
    Fixture f;
    auto& gunship = f.spawn(0, fighter_type, 100, 100);
    auto& tank = f.spawn(1, tank_type, 200, 100);
    auto& order = f.attack(gunship, tank, sim::match_runtime::air_to_ground_hover_kind);
    CHECK(f.dispatch(gunship, order, 0) == 1);
    step(order, 1);
    CHECK(f.dispatch(gunship, order, 0) == 1);
    CHECK(order.wait_events == wait_attack);
    step(order, 1);
    order.wait_events = 0;
    CHECK(f.dispatch(gunship, order, 0) == 1);
    CHECK(order.wait_events == wait_attack);
    const auto& aim = gunship.record.weapons[0];
    CHECK(aim.target_a == tank.unit_index && aim.target_b == OA_UNIT_TARGET_IS_UNIT);
    step(order, 1);
    order.wait_events = 0;
    const auto result = f.dispatch(gunship, order, 0);
    CHECK(result == 2);
    CHECK(order.wait_events == wait_attack || order.wait_events == (wait_attack | 0x1000));
    CHECK(order.phase == 3);
    order.phase = 4;
    CHECK(f.dispatch(gunship, order, 0) == 7);
}

void seek_attack() {
    Fixture f;
    auto& fighter = f.spawn(0, fighter_type, 100, 100);
    auto& order = f.match->insert_ground_order(fighter.unit_index, 62);
    CHECK(f.dispatch(fighter, order, 0) == 1);
    step(order, 1);
    order.wait_events = 0;
    const uint32_t tick = f.match->simulation().tick;
    CHECK(f.dispatch(fighter, order, 0) == 2);
    CHECK((order.wait_events & 0xe1) == 0xe1);
    CHECK(order.wake_tick >= tick + 0x1e && order.wake_tick < tick + 0x3c);
    CHECK(f.dispatch(fighter, order, 0x40) == 5);
}

void seek_guard() {
    Fixture f;
    auto& fighter = f.spawn(0, fighter_type, 100, 100);
    auto& order = f.match->insert_ground_order(fighter.unit_index, 63);
    CHECK(f.dispatch(fighter, order, 0) == 1);
    step(order, 1);
    order.wait_events = 0;
    CHECK(f.dispatch(fighter, order, 0) == 2);
    CHECK((order.wait_events & 0xf9) == 0xf9);
    auto& ally = f.spawn(0, tank_type, 150, 120);
    order.wait_events = 0;
    CHECK(f.dispatch(fighter, order, 0) == 3);
    auto* guard = fighter.unit->primary;
    CHECK(guard && guard->kind == 49 && guard->next == &order);
    (void)ally;
}

// The goal an aircraft attack mission last handed its unit's air driver. No
// such mission steers the unit through the ground navigator, which would hold
// it to ground paths and bring it down to the ground at each point it reaches.
const sim::air::AirGoal& air_goal(Fixture& f, const sim::unit_spawn::Slot& slot) {
    const auto* driver = f.match->air_driver(slot.unit_index);
    CHECK(driver && driver->goal);
    const auto* ground = f.match->ground_runtime(slot.unit_index);
    CHECK(ground && ground->navigation.goal == nullptr);
    return *driver->goal;
}

// A target goal on a fixed point that arrives within a radius.
bool point_goal(const sim::air::AirGoal& goal, int16_t arrival) {
    return goal.kind == sim::air::AirGoalKind::target && goal.target == nullptr &&
           (goal.flags & sim::air::goal_arrival_radius) != 0 && goal.arrival_radius == arrival;
}

// A gunship's attack flies air goals: a point halfway to its target, the
// target's position within weapon range, then strafe points beside it that
// keep it facing the target at its cruise altitude over the ground there.
void hover_flies_air_goals() {
    Fixture f;
    auto& gunship = f.spawn(0, fighter_type, 100, 100);
    auto& tank = f.spawn(1, tank_type, 200, 100);
    auto& order = f.attack(gunship, tank, sim::match_runtime::air_to_ground_hover_kind);
    step(order, f.dispatch(gunship, order, 0));
    step(order, f.dispatch(gunship, order, 0));
    CHECK(point_goal(air_goal(f, gunship), 0x80));
    step(order, f.dispatch(gunship, order, 0));
    const auto& closing = air_goal(f, gunship);
    CHECK(point_goal(closing, 300));
    CHECK(closing.point.x == tank.record.position.x && closing.point.z == tank.record.position.z);
    CHECK(f.dispatch(gunship, order, 0) == 2);
    const auto& strafe = air_goal(f, gunship);
    CHECK(strafe.kind == sim::air::AirGoalKind::target && strafe.target == &tank.record);
    CHECK((strafe.flags & sim::air::goal_face_target) != 0 && strafe.arrival_radius == 0x10);
    CHECK((strafe.flags & sim::air::goal_fixed_altitude) != 0 && strafe.altitude == 100);
}

// Bombers follow their target into the run with an air goal on it, and the
// passes, breakaways and VTOL_SeekAttack's circling fly air point goals.
void strike_and_seek_fly_air_goals() {
    Fixture f;
    auto& bomber = f.spawn(0, fighter_type, 100, 100);
    auto& tank = f.spawn(1, tank_type, 300, 100);
    auto& order = f.attack(bomber, tank, sim::match_runtime::air_strike_kind);
    order.phase = 4;
    CHECK(f.dispatch(bomber, order, 0) == 2);
    const auto& follow = air_goal(f, bomber);
    CHECK(follow.kind == sim::air::AirGoalKind::target && follow.target == &tank.record);
    CHECK((follow.flags & sim::air::goal_track_unit) != 0);
    CHECK((follow.flags & sim::air::goal_arrival_radius) != 0);
    order.phase = 6;
    CHECK(f.dispatch(bomber, order, 0) == 2);
    CHECK(point_goal(air_goal(f, bomber), 0x80));

    auto& seeker = f.spawn(0, fighter_type, 500, 500);
    auto& seek = f.match->insert_ground_order(seeker.unit_index, 62);
    step(seek, f.dispatch(seeker, seek, 0));
    seek.wait_events = 0;
    CHECK(f.dispatch(seeker, seek, 0) == 2);
    CHECK(point_goal(air_goal(f, seeker), 0x80));
}

// Fighters chase a target with a seek goal that leads it.
void dogfight_flies_seek_goals() {
    Fixture f;
    auto& fighter = f.spawn(0, fighter_type, 100, 100);
    auto& enemy = f.spawn(1, fighter_type, 400, 400);
    auto& order = f.attack(fighter, enemy, sim::match_runtime::air_to_air_kind);
    step(order, f.dispatch(fighter, order, 0));
    order.wait_events = 0;
    CHECK(f.dispatch(fighter, order, 0) == 2);
    const auto& lead = air_goal(f, fighter);
    CHECK(lead.kind == sim::air::AirGoalKind::seek);
    CHECK(lead.point.x == enemy.record.position.x && lead.point.z == enemy.record.position.z);
}

// The attack command on a ground point gives an armed aircraft AirToGround
// at that point, with no target unit. It takes off, closes on a point
// halfway there, then flies at the point with its first weapon aimed at it,
// pulls out three weapon ranges past it, swings a weapon range to one side
// and runs at the point again until the order is given up.
void air_to_ground_at_a_point() {
    Fixture f;
    f.defs[fighter_type].can_attack = true;
    auto& gunship = f.spawn(0, fighter_type, 100, 100);
    gunship.unit->flags |= OA_UNIT_FLAG_HAS_WEAPONS;
    const sim::ground_orders::Point at{300 << 16, 0, 140 << 16};
    auto* ordered = f.match->issue_attack_ground(gunship.unit_index, at, false);
    CHECK(ordered && ordered->kind == sim::match_runtime::air_to_ground_kind);
    CHECK(gunship.unit->primary == ordered && !ordered->next);
    auto& order = *ordered;
    step(order, f.dispatch(gunship, order, 0));
    order.wait_events = 0;
    CHECK(f.dispatch(gunship, order, 0) == 1 && order.wait_events == wait_attack);
    CHECK(point_goal(air_goal(f, gunship), 0x80));
    step(order, 1);
    CHECK(f.dispatch(gunship, order, 0x20) == 1 && order.wait_events == wait_attack);
    const auto& aim = gunship.record.weapons[0];
    CHECK(aim.target_a == 300 && aim.target_b == 140);
    const auto& run = air_goal(f, gunship);
    CHECK(point_goal(run, 300) && run.point.x == at[0] && run.point.z == at[2]);
    step(order, 1);
    CHECK(f.dispatch(gunship, order, 0x20) == 1 && order.wait_events == wait_attack_or_cancel);
    const auto& pull_out = air_goal(f, gunship);
    CHECK(pull_out.arrival_radius >= 0x80 && pull_out.arrival_radius < 0x100);
    // Three ranges past the point, on the far side from the aircraft.
    CHECK(pull_out.point.x > at[0] + (800 << 16));
    step(order, 1);
    CHECK(f.dispatch(gunship, order, 0x20) == 1 && order.wait_events == wait_attack_or_cancel);
    CHECK(point_goal(air_goal(f, gunship), 0x80));
    step(order, 1);
    CHECK(f.dispatch(gunship, order, 0x20) == 2 && order.phase == 2);
    CHECK(f.dispatch(gunship, order, 0x20) == 1);
    // Given up while it may fire, it looks for targets around the point.
    gunship.unit->flags |= fire_at_will << OA_UNIT_FLAG_FIRE_ORDER_SHIFT;
    CHECK(f.dispatch(gunship, order, 0x2) == 5);
    CHECK(order.next && order.next->kind == sim::match_runtime::vtol_seek_attack_kind);
}

// The attack command on a ground point resolves as the attack command on a
// unit does: an armed unit that does not fly takes Suppress, and a unit that
// cannot attack takes nothing.
void attack_ground_resolves_by_type() {
    Fixture f;
    f.defs[fighter_type].can_attack = true;
    f.defs[tank_type].can_attack = true;
    auto& tank = f.spawn(0, tank_type, 100, 300);
    tank.unit->flags |= OA_UNIT_FLAG_HAS_WEAPONS;
    const sim::ground_orders::Point at{300 << 16, 0, 140 << 16};
    auto* suppress = f.match->issue_attack_ground(tank.unit_index, at, false);
    CHECK(suppress && suppress->kind == sim::match_runtime::suppress_kind);
    auto& pad = f.spawn(0, pad_type, 400, 400);
    CHECK(!f.match->issue_attack_ground(pad.unit_index, at, false) && !pad.unit->primary);
}

// VTOL_Evade breaks a quarter turn to one side of the heading it starts on:
// a first leg one weapon range out and a second leg two ranges out, each
// arriving within 128 world units, on the same side; then it finishes.
void evade_flies_two_legs() {
    Fixture f;
    auto& fighter = f.spawn(0, fighter_type, 500, 500);
    auto& enemy = f.spawn(1, fighter_type, 800, 800);
    auto& order = f.attack(fighter, enemy, 48);
    fighter.record.heading = 0;
    const auto x = fighter.record.position.x;
    const auto z = fighter.record.position.z;
    constexpr int32_t range = 300 << 16;
    // The point a leg reaches: `reach` along heading `turn`.
    const auto leg = [&](uint16_t turn, int32_t reach) {
        return std::array<int32_t, 2>{
            x - sim::unit_movement::sine_scaled(turn, reach),
            z - sim::unit_movement::cosine_scaled(turn, reach)
        };
    };
    CHECK(f.dispatch(fighter, order, 0) == 1 && order.wait_events == wait_attack);
    const auto& first = air_goal(f, fighter);
    CHECK(point_goal(first, 0x80));
    const std::array<int32_t, 2> reached{first.point.x, first.point.z};
    const uint16_t turn = reached == leg(0x4000, range) ? 0x4000 : 0xc000;
    CHECK(reached == leg(turn, range));
    step(order, 1);
    order.wait_events = 0;
    CHECK(f.dispatch(fighter, order, 0) == 1 && order.wait_events == wait_attack);
    const auto& second = air_goal(f, fighter);
    CHECK(point_goal(second, 0x80));
    CHECK((std::array<int32_t, 2>{second.point.x, second.point.z} == leg(turn, 2 * range)));
    step(order, 1);
    CHECK(f.dispatch(fighter, order, 0) == 5);
}

// An aircraft attack taken up on manoeuvre orders carries the type's
// manoeuvre leash from where the unit stood; the attack ends once the
// aircraft has strayed that far from there.
void air_attack_ends_at_its_leash() {
    Fixture f;
    f.defs[fighter_type].can_attack = true;
    f.defs[fighter_type].can_move = true;
    f.defs[fighter_type].can_fly = true;
    f.defs[fighter_type].maneuver_leash_length = 200;
    auto& fighter = f.spawn(0, fighter_type, 100, 100);
    auto& enemy = f.spawn(1, fighter_type, 400, 400);
    constexpr uint32_t manoeuvre = 1;
    fighter.unit->flags =
        (fighter.unit->flags & ~(OA_UNIT_FLAG_MOVE_ORDER_MASK | OA_UNIT_FLAG_FIRE_ORDER_MASK)) |
        OA_UNIT_FLAG_HAS_WEAPONS | (manoeuvre << OA_UNIT_FLAG_MOVE_ORDER_SHIFT) |
        (fire_at_will << OA_UNIT_FLAG_FIRE_ORDER_SHIFT);
    CHECK(f.match->issue_attack(fighter.unit_index, enemy.unit_index, false));
    auto* order = fighter.unit->primary;
    CHECK(order && order->kind == sim::match_runtime::air_to_air_kind);
    step(*order, f.dispatch(fighter, *order, 0));
    order->wait_events = 0;
    fighter.record.position.x += 199 << 16;
    CHECK(f.dispatch(fighter, *order, 0) == 2);
    fighter.record.position.x += 1 << 16;
    CHECK(f.dispatch(fighter, *order, 0) == 5);
}

// A fighter that has strayed off the map during its attack turns back: it
// flies 0x320 world units along the bearing to the map's centre, arriving
// within 128, and keeps its attack.
void off_map_attack_turns_back() {
    Fixture f;
    auto& fighter = f.spawn(0, fighter_type, -40, 512);
    auto& enemy = f.spawn(1, fighter_type, 400, 400);
    auto& order = f.attack(fighter, enemy, sim::match_runtime::air_to_air_kind);
    order.phase = 1;
    order.wait_events = 0;
    CHECK(f.dispatch(fighter, order, 0) == 2);
    CHECK((order.wait_events & 0xe0) == 0xe0);
    const auto& back = air_goal(f, fighter);
    CHECK(point_goal(back, 0x80));
    const auto x = fighter.record.position.x;
    const auto z = fighter.record.position.z;
    const auto& game = f.match->state().game;
    const auto toward = base::game_math::direction(
        x - static_cast<int32_t>(game.map_pixel_width / 2 << 16),
        z - static_cast<int32_t>(game.map_pixel_height / 2 << 16)
    );
    CHECK(back.point.x == x - sim::unit_movement::sine_scaled(toward, 0x320 << 16));
    CHECK(back.point.z == z - sim::unit_movement::cosine_scaled(toward, 0x320 << 16));
}

void evade() {
    Fixture f;
    auto& fighter = f.spawn(0, fighter_type, 100, 100);
    auto& enemy = f.spawn(1, fighter_type, 400, 400);
    auto& lone = f.match->insert_ground_order(fighter.unit_index, 48);
    CHECK(f.dispatch(fighter, lone, 0) == 5);
    auto& order = f.attack(fighter, enemy, 48);
    CHECK(f.dispatch(fighter, order, 0) == 1 && order.wait_events == wait_attack);
    step(order, 1);
    order.wait_events = 0;
    CHECK(f.dispatch(fighter, order, 0) == 1 && order.wait_events == wait_attack);
    step(order, 1);
    CHECK(f.dispatch(fighter, order, 0) == 5);
}
} // namespace

int main() {
    air_strike();
    air_strike_without_gravity();
    air_to_air();
    air_to_ground_hover();
    seek_attack();
    seek_guard();
    evade();
    evade_flies_two_legs();
    air_attack_ends_at_its_leash();
    off_map_attack_turns_back();
    hover_flies_air_goals();
    strike_and_seek_fly_air_goals();
    dogfight_flies_seek_goals();
    air_to_ground_at_a_point();
    attack_ground_resolves_by_type();
    std::cout << "air attack missions passed\n";
}
