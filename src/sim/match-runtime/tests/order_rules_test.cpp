// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The rules a mod profile sets for unit orders and aircraft missions
// (orders.* and air.* of the match rules), each against 3.1c's behaviour:
// which weapon slots attack and busy orders occupy, the repair pad's
// activity test, blocked build sites and the units the builder sends off
// them, builder patrol and guard choices, and the aircraft that never break
// off to a repair pad or never leave Hold Position to defend what they guard.
#include "match_tick_access.hpp"
#include "oa/base/text.hpp"
#include "oa/sim/match_runtime/rule_state.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include "oa/test/match_services.hpp"

using namespace oa;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " #x);   \
    } while (false)

namespace {
namespace match_rules = oa::data::match_rules;
namespace runtime = oa::sim::match_runtime;

constexpr int32_t map_cells = 32;
constexpr uint16_t builder_type = 1;  // mobile builder with two guns and a D-gun
constexpr uint16_t building_type = 2; // 4x4 structure
constexpr uint16_t tank_type = 3;     // mobile gun
constexpr uint8_t move_ground_kind = 26;
constexpr uint8_t reclaim_kind = 32;

using Services = oa::test::QuietServices;
using Scenario = oa::test::EmptyScenario;

/// Dispatches an order once and applies the sweep's phase rule for results 0 and 1.
///
/// @param match the match
/// @param unit the order's unit
/// @param order the order
/// @param events events that woke it
/// @return the handler's result
uint32_t step(
    runtime::Match& match,
    sim::unit_spawn::Slot& unit,
    sim::simulation_state::Order& order,
    uint32_t events = 0
) {
    runtime::MatchTickAccess host(match);
    order.wait_events = 0;
    const auto result = host.dispatch_mission(match.state(), unit.record, order, events);
    if (result == 0)
        order.phase = 0;
    else if (result == 1)
        ++order.phase;
    return result;
}

std::array<uint32_t, 3> at(int32_t x, int32_t z) {
    return {static_cast<uint32_t>(x) << 16, 0, static_cast<uint32_t>(z) << 16};
}

sim::ground_orders::Point point(int32_t x, int32_t z) {
    return {x << 16, 0, z << 16};
}

/// Returns the whole world units of a plot's centre.
///
/// @param cell plot column or row
/// @return the centre, world units
int32_t centre(int32_t cell) {
    return cell * 16 + 8;
}

/// Tells whether a weapon slot picks its own targets (it is free) rather
/// than firing only at what an order gives it (it is occupied).
///
/// @param unit the unit
/// @param slot weapon slot 0..2
/// @return true when free
bool free_slot(const sim::unit_spawn::Slot& unit, uint32_t slot) {
    return (unit.record.weapons[slot].flags & OA_UNIT_WEAPON_RETALIATE) != 0;
}

/// Frees every enabled weapon slot of a unit.
///
/// @param unit the unit
void free_all(sim::unit_spawn::Slot& unit) {
    for (auto& weapon : unit.record.weapons)
        if (weapon.flags & OA_UNIT_WEAPON_ENABLED)
            weapon.flags |= OA_UNIT_WEAPON_RETALIATE;
}

/// Sets a unit's standing move order: 0 hold position, 1 manoeuvre, 2 roam.
///
/// @param unit the unit
/// @param stance the standing move order
void stand(sim::unit_spawn::Slot& unit, uint32_t stance) {
    unit.record.flags = (unit.record.flags & ~OA_UNIT_FLAG_MOVE_ORDER_MASK) |
                        (stance << OA_UNIT_FLAG_MOVE_ORDER_SHIFT);
}

// A 32x32-cell map with a builder, a 4x4 structure it builds and a tank, and
// one auto-reclaimable rock in the feature table.
struct Ground {
    formats::tnt::Map map;
    std::vector<sim::visibility_state::TerrainCell> terrain =
        std::vector<sim::visibility_state::TerrainCell>(map_cells * map_cells);
    std::array<sim::visibility_state::SightMask, 1> masks{};
    std::shared_ptr<formats::objects3d::Model> model =
        std::make_shared<formats::objects3d::Model>();
    std::shared_ptr<formats::cob::CobProgram> script = std::make_shared<formats::cob::CobProgram>();
    static constexpr size_t type_count = 4;
    std::array<sim::unit_spawn::LoadedType, type_count> loaded;
    std::array<sim::unit_spawn::Type, type_count> types;
    std::array<data::unit_definitions::UnitDefinition, type_count> defs;
    data::unit_definitions::UnitTargetCategoryMasks target_masks;
    data::unit_definitions::RuntimeDefinitionMetadata metadata;
    std::array<sim::match_runtime::RuntimeTypeFields, type_count> fields{};
    std::array<uint8_t, 16> yard{4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};
    std::vector<sim::spatial_state::Plot> plots =
        std::vector<sim::spatial_state::Plot>(map_cells * map_cells);
    std::vector<FeatureDef> features = std::vector<FeatureDef>(1);
    sim::combat_state::WeaponRegistry weapons;
    Services services;
    Scenario scenario;
    std::unique_ptr<runtime::Match> match;

    explicit Ground(const match_rules::MatchRules& rules = {}) {
        map.attribute_width = map.attribute_height = map_cells;
        map.attributes.resize(map_cells * map_cells);
        masks[0].width = masks[0].height = 1;
        masks[0].pixels = {1};
        model->objects.resize(1);
        model->objects[0].name = "root";
        model->objects[0].vertices = {{0, 0, 0}, {0, 20 << 16, 0}};
        // Every script entry returns at once; the D-gun aims at once.
        script->code = {sim::script_vm::opcode::push_constant, 1, sim::script_vm::opcode::return_};
        script->scripts = {{"Create", 2}, {"AimTertiary", 0}};
        script->entry_points = {2, 0};
        script->piece_names = {"root"};
        for (size_t i = 1; i < type_count; ++i) {
            loaded[i].model = model;
            loaded[i].script = script;
            types[i].simulation.flags = OA_UNIT_DEF_FLAG_AVAILABLE;
            types[i].simulation.maximum_health = 100;
            types[i].footprint_x = types[i].footprint_z = 1;
            types[i].bm_code = 1;
            types[i].model = reinterpret_cast<uintptr_t>(model.get());
            types[i].cob = reinterpret_cast<uintptr_t>(script.get());
            defs[i].sight_distance = 200;
            defs[i].acceleration_fixed = 65536;
            defs[i].brake_rate_fixed = 65536;
            defs[i].max_velocity_fixed = 2 * 65536;
            defs[i].turn_rate = 1024;
            defs[i].worker_time = 60;
            defs[i].build_distance = 64;
            defs[i].build_time = 100;
            defs[i].build_cost_energy = 200;
            defs[i].build_cost_metal = 50;
            defs[i].energy_storage = 1000.0F;
            defs[i].metal_storage = 1000.0F;
            fields[i].definition = &defs[i];
            fields[i].yard_mask = yard;
            fields[i].runtime_metadata = &metadata;
            fields[i].target_masks = &target_masks;
        }
        metadata.max_slope = 10;
        types[builder_type].simulation.flags |=
            OA_UNIT_DEF_FLAG_BUILDER | OA_UNIT_DEF_FLAG_HAS_WEAPONS;
        types[builder_type].simulation.abilities =
            OA_UNIT_DEF_ABILITY_CAN_MOVE | OA_UNIT_DEF_ABILITY_CAN_REPAIR |
            OA_UNIT_DEF_ABILITY_CAN_RECLAMATE | OA_UNIT_DEF_ABILITY_CAN_CAPTURE |
            OA_UNIT_DEF_ABILITY_CAN_PATROL | OA_UNIT_DEF_ABILITY_CAN_ATTACK |
            OA_UNIT_DEF_ABILITY_CAN_GUARD;
        types[building_type].simulation.flags |= OA_UNIT_DEF_FLAG_BUILDER;
        types[building_type].bm_code = 0;
        types[building_type].footprint_x = types[building_type].footprint_z = 4;
        types[tank_type].simulation.flags |= OA_UNIT_DEF_FLAG_HAS_WEAPONS;
        types[tank_type].simulation.abilities = OA_UNIT_DEF_ABILITY_CAN_MOVE |
                                                OA_UNIT_DEF_ABILITY_CAN_ATTACK |
                                                OA_UNIT_DEF_ABILITY_CAN_GUARD;
        defs[builder_type].weapon1 = "TESTGUN";
        defs[builder_type].weapon2 = "TESTGUN";
        defs[builder_type].weapon3 = "TESTDGUN";
        defs[builder_type].can_dgun = true;
        defs[tank_type].weapon1 = "TESTGUN";
        for (const auto mobile : {builder_type, tank_type})
            defs[mobile].can_move = defs[mobile].can_attack = defs[mobile].can_guard = true;
        for (size_t i = 1; i < type_count; ++i) {
            loaded[i].type = types[i];
            if (i != building_type)
                fields[i].movement_class = 0;
        }
        loaded[tank_type].unit_name = "TANK";
        (void)sim::combat_state::install_weapon_text(
            weapons,
            "[TESTGUN]{id=1; reloadtime=0.1; range=400; lineofsight=1; weaponvelocity=100; "
            "turret=1; [DAMAGE]{default=10;}}"
        );
        (void)sim::combat_state::install_weapon_text(
            weapons,
            "[TESTDGUN]{id=2; reloadtime=1.2; range=120; lineofsight=1; weaponvelocity=200; "
            "turret=1; commandfire=1; [DAMAGE]{default=10;}}"
        );
        oa::base::text::copy_terminated(features[0].name, "ROCK01");
        features[0].footprint_x = features[0].footprint_z = 1;
        features[0].metal = 50.0F;
        features[0].flags = OA_FEATURE_FLAG_RECLAIMABLE | OA_FEATURE_FLAG_AUTO_RECLAIMABLE;
        features[0].dead_feature = sim::feature_runtime::no_feature;
        features[0].burnt_feature = sim::feature_runtime::no_feature;
        features[0].reclamate_feature = sim::feature_runtime::no_feature;
        sim::match_runtime::OfflineInputs input{
            map,
            loaded,
            types,
            fields,
            weapons,
            terrain,
            masks,
            16,
            16,
            12,
            2,
            0,
            30,
            1,
            &scenario,
            {},
            plots,
            {}
        };
        input.feature_defs = features;
        input.rules = rules;
        match = std::make_unique<runtime::Match>(input, services);
        match->configure_strategic_environment({0, 0.5F, 0});
        for (uint8_t p = 0; p < 2; ++p) {
            match->simulation().players[p].present = true;
            match->simulation().players[p].status = p == 0 ? 1 : 2;
            std::array<uint8_t, 10> allies{};
            allies[p] = 1;
            match->configure_player_alliances(p, allies);
            auto& player = match->state().game.players[p];
            player.energy = player.energy_storage = 1000.0F;
            player.metal = player.metal_storage = 1000.0F;
        }
        match->state().game.tick = 100;
        match->state().game.local_player_index = 0;
        for (const auto mobile : {builder_type, tank_type})
            match->state().unit_defs[mobile].max_slope = 10;
    }

    sim::unit_spawn::Slot&
    spawn(uint8_t player, uint16_t type, int32_t x, int32_t z, bool finished = true) {
        auto* slot = match->create({player, type, at(x, z), finished, 1, 0});
        CHECK(slot && slot->unit);
        return *slot;
    }

    sim::simulation_state::Order* head(uint16_t unit) { return match->orders(unit).primary; }

    // The unit's economy block pays for any request.
    static void fund(sim::unit_spawn::Slot& unit) {
        auto& economy = unit.record.economy;
        economy.energy.requested = economy.energy.accepted = economy.energy.gate = 0.0F;
        economy.metal.requested = economy.metal.accepted = economy.metal.gate = 0.0F;
    }

    // The point of the unit's first order.
    sim::ground_orders::Point head_point(uint16_t unit) {
        std::array<runtime::Match::OrderRecordView, 1> view{};
        CHECK(match->queue_records(unit, false, view.data(), view.size()) == 1);
        return view[0].point;
    }

    // The kinds of the unit's primary queue, head first.
    std::vector<uint8_t> queue(uint16_t unit) {
        std::vector<uint8_t> kinds;
        for (auto* order = head(unit); order; order = order->next)
            kinds.push_back(order->kind);
        return kinds;
    }
};

match_rules::MatchRules occupying(match_rules::OrdersSelectiveWeaponOccupyAttack attack) {
    match_rules::MatchRules rules;
    rules.orders.selective_weapon_occupy.enabled = true;
    rules.orders.selective_weapon_occupy.attack = attack;
    return rules;
}

// orders.selective-weapon-occupy attack: before aiming, 3.1c occupies slots 0
// and 2 at both attack stages; the hack occupies only the ordered slot (2 for
// a slot past 1, else 0) at the stages it names.
void attack_occupies_the_ordered_slot() {
    using Attack = match_rules::OrdersSelectiveWeaponOccupyAttack;
    for (const auto attack :
         {Attack::slots_0_and_2,
          Attack::ordered_slot_first_stage,
          Attack::ordered_slot_both_stages}) {
        Ground f(occupying(attack));
        auto& unit = f.spawn(0, builder_type, 100, 100);
        auto& enemy = f.spawn(1, tank_type, 140, 100);
        const auto id = unit.unit_index;
        CHECK(free_slot(unit, 0) && free_slot(unit, 1) && free_slot(unit, 2));
        auto& chase = f.match->insert_ground_order(
            id, runtime::attack_chase_kind, std::nullopt, 0, enemy.unit_index
        );
        CHECK(step(*f.match, unit, chase) == 1);
        // The first stage: phase 1, the target in reach.
        CHECK(step(*f.match, unit, chase) == 2 && chase.phase == 1);
        CHECK(!free_slot(unit, 0) && free_slot(unit, 1));
        CHECK(free_slot(unit, 2) == (attack != Attack::slots_0_and_2));
        // The second stage: phase 3, the target in reach.
        free_all(unit);
        chase.phase = 3;
        CHECK(step(*f.match, unit, chase) == 2);
        CHECK(!free_slot(unit, 0) && free_slot(unit, 1));
        CHECK(free_slot(unit, 2) == (attack == Attack::ordered_slot_both_stages));
        f.match->stop_orders(id);
        // An order for the third slot occupies it alone under the hack; one
        // for the second slot occupies the first.
        for (const int32_t ordered : {1, 2}) {
            free_all(unit);
            auto& other = f.match->issue_order(
                id, runtime::attack_chase_kind, false, enemy.unit_index, nullptr, ordered, 0
            );
            CHECK(step(*f.match, unit, other) == 1);
            CHECK(step(*f.match, unit, other) == 2);
            if (attack == Attack::slots_0_and_2)
                CHECK(!free_slot(unit, 0) && free_slot(unit, 1) && !free_slot(unit, 2));
            else if (ordered == 2)
                CHECK(free_slot(unit, 0) && free_slot(unit, 1) && !free_slot(unit, 2));
            else
                CHECK(!free_slot(unit, 0) && free_slot(unit, 1) && free_slot(unit, 2));
            f.match->stop_orders(id);
        }
    }
}

// orders.selective-weapon-occupy suppress: a D-gun aimed at the ground
// occupies every slot in 3.1c, the third alone with slot-2.
void dgun_ground_occupies_the_third_slot() {
    using Suppress = match_rules::OrdersSelectiveWeaponOccupySuppress;
    for (const auto suppress : {Suppress::all, Suppress::slot_2}) {
        match_rules::MatchRules rules;
        rules.orders.selective_weapon_occupy.enabled = suppress != Suppress::all;
        rules.orders.selective_weapon_occupy.suppress = suppress;
        Ground f(rules);
        auto& unit = f.spawn(0, builder_type, 100, 100);
        const auto ground = point(140, 100);
        auto& order =
            f.match->issue_order(unit.unit_index, runtime::suppress_kind, false, 0, &ground, 2, 0);
        CHECK(step(*f.match, unit, order) == 1);
        CHECK(step(*f.match, unit, order) == 1);
        CHECK(!free_slot(unit, 2));
        CHECK(free_slot(unit, 0) == (suppress == Suppress::slot_2));
        CHECK(free_slot(unit, 1) == (suppress == Suppress::slot_2));
        // A ground attack with the first slot occupies the first two in both.
        f.match->stop_orders(unit.unit_index);
        free_all(unit);
        auto& plain =
            f.match->issue_order(unit.unit_index, runtime::suppress_kind, false, 0, &ground, 0, 0);
        CHECK(step(*f.match, unit, plain) == 1);
        CHECK(step(*f.match, unit, plain) == 1);
        CHECK(!free_slot(unit, 0) && !free_slot(unit, 1) && free_slot(unit, 2));
    }
}

// orders.weapons-free-while-busy: each busy state the set lists frees the
// builder's weapons where 3.1c occupies them; the others still occupy.
void busy_states_free_the_weapons() {
    using State = match_rules::OrdersWeaponsFreeWhileBusyStates;
    const std::array states{
        std::optional<State>{},
        std::optional{State::nanolathe},
        std::optional{State::help_build},
        std::optional{State::capture},
        std::optional{State::reclaim_unit},
        std::optional{State::repair}
    };
    for (const auto listed : states) {
        match_rules::MatchRules rules;
        if (listed) {
            rules.orders.weapons_free_while_busy.enabled = true;
            rules.orders.weapons_free_while_busy.states.insert(*listed);
        }
        const auto expect_free = [&](State state) { return listed && *listed == state; };
        Ground f(rules);
        auto& builder = f.spawn(0, builder_type, 100, 100);
        const auto id = builder.unit_index;

        // Nanolathing: the frame is placed in phase 1.
        auto& build = f.match->issue_mobile_build(id, building_type, point(200, 200), false);
        CHECK(step(*f.match, builder, build) == 1);
        CHECK(step(*f.match, builder, build) == 1);
        CHECK(free_slot(builder, 0) == expect_free(State::nanolathe));
        f.match->stop_orders(id);

        // Helping to build: phase 1 joins the frame.
        free_all(builder);
        auto& frame = f.spawn(0, building_type, 104, 140, false);
        auto& help = f.match->issue_help_build(id, frame.unit_index, false);
        CHECK(step(*f.match, builder, help) == 1);
        CHECK(step(*f.match, builder, help) == 1);
        CHECK(free_slot(builder, 0) == expect_free(State::help_build));
        f.match->stop_orders(id);

        // Capturing: phase 0.
        free_all(builder);
        auto& prize = f.spawn(1, tank_type, 104, 104);
        auto& capture = f.match->issue_capture(id, prize.unit_index, false);
        CHECK(step(*f.match, builder, capture) == 1);
        CHECK(free_slot(builder, 0) == expect_free(State::capture));
        f.match->stop_orders(id);

        // Reclaiming a unit: phase 0.
        free_all(builder);
        auto& reclaim = f.match->issue_reclaim(id, prize.unit_index, false);
        CHECK(step(*f.match, builder, reclaim) == 1);
        CHECK(free_slot(builder, 0) == expect_free(State::reclaim_unit));
        f.match->stop_orders(id);

        // Repairing: phase 1 within build reach.
        free_all(builder);
        auto& patient = f.spawn(0, tank_type, 96, 104);
        patient.record.health = 40;
        auto& repair = f.match->issue_repair(id, patient.unit_index, false);
        CHECK(step(*f.match, builder, repair) == 1);
        CHECK(step(*f.match, builder, repair) == 1);
        CHECK(free_slot(builder, 0) == expect_free(State::repair));
        f.match->stop_orders(id);
    }
}

// orders.repairing-state-target-activity: a unit landing for repairs needs
// its own active bit in 3.1c, the pad's under the hack.
void repair_pad_tests_the_pad() {
    for (const bool hack : {false, true}) {
        match_rules::MatchRules rules;
        rules.orders.repairing_state_target_activity.enabled = hack;
        Ground f(rules);
        auto& pad = f.spawn(0, builder_type, 100, 100);
        auto& tank = f.spawn(0, tank_type, 100, 100);
        tank.record.health = 40;
        for (const bool tank_active : {false, true}) {
            if (tank_active) {
                tank.record.state_flags |= OA_UNIT_STATE_ACTIVE;
                pad.record.state_flags &= ~OA_UNIT_STATE_ACTIVE;
            } else {
                tank.record.state_flags &= ~OA_UNIT_STATE_ACTIVE;
                pad.record.state_flags |= OA_UNIT_STATE_ACTIVE;
            }
            auto& mend = f.match->insert_ground_order(
                tank.unit_index, runtime::self_repair_kind, std::nullopt, 0, pad.unit_index
            );
            const bool proceeds = hack ? !tank_active : tank_active;
            CHECK(step(*f.match, tank, mend) == (proceeds ? 1u : 8u));
            f.match->stop_orders(tank.unit_index);
        }
    }
}

// orders.build-site-kickout retry-limit: a builder waiting for its site gives
// up once it has waited more times than the limit, 10 in 3.1c.
void blocked_site_retries() {
    for (const int32_t limit : {10, 20, 1}) {
        match_rules::MatchRules rules;
        rules.orders.build_site_kickout.enabled = limit != 10;
        rules.orders.build_site_kickout.retry_limit = limit;
        Ground f(rules);
        auto& builder = f.spawn(0, builder_type, 100, 100);
        (void)f.spawn(1, tank_type, centre(8), centre(5));
        auto& build =
            f.match->issue_mobile_build(builder.unit_index, building_type, point(150, 101), false);
        CHECK(step(*f.match, builder, build) == 1);
        uint32_t waits = 0;
        uint32_t result = 2;
        while (result == 2 && waits < 100) {
            result = step(*f.match, builder, build);
            ++waits;
        }
        CHECK(result == 8 && waits == static_cast<uint32_t>(limit) + 2);
        CHECK(f.match->rule_state().count == 0);
    }
}

match_rules::MatchRules kicking() {
    match_rules::MatchRules rules;
    rules.orders.build_site_kickout.enabled = true;
    rules.orders.build_site_kickout.place_over_own_units = true;
    rules.orders.build_site_kickout.kickout = true;
    rules.orders.build_site_kickout.retry_limit = 20;
    return rules;
}

/// Tells whether a whole point lies on the footprint of the 4x4 site at
/// cells 7..10 by 4..7.
///
/// @param where signed 16.16 point
/// @return true on the footprint
bool on_site(const sim::ground_orders::Point& where) {
    const auto x = (where[0] >> 16) / 16;
    const auto z = (where[2] >> 16) / 16;
    return x >= 7 && x <= 10 && z >= 4 && z <= 7;
}

// orders.build-site-kickout kickout: the blocked builder sends the local
// player's own mobile units off the footprint; other players' units, its
// own units walking elsewhere and units working on a cheap frame alone stay.
void kickout_clears_own_units() {
    {
        // Without the rule nothing moves and no state is kept.
        Ground f;
        auto& builder = f.spawn(0, builder_type, 100, 100);
        auto& parked = f.spawn(0, tank_type, centre(8), centre(5));
        auto& build =
            f.match->issue_mobile_build(builder.unit_index, building_type, point(150, 101), false);
        CHECK(step(*f.match, builder, build) == 1);
        CHECK(step(*f.match, builder, build) == 2);
        CHECK(f.head(parked.unit_index) == nullptr);
        CHECK(f.match->build_site_kickout_spots().empty());
    }
    Ground f(kicking());
    CHECK(f.match->rule_state().count == 1);
    CHECK(runtime::find_rule_state(f.match->rule_state(), "build-site-kickout"));
    auto& builder = f.spawn(0, builder_type, 100, 100);
    // Cells 7..10 by 4..7 hold the site.
    auto& idle = f.spawn(0, tank_type, centre(8), centre(5));
    auto& enemy = f.spawn(1, tank_type, centre(9), centre(5));
    auto& walking_in = f.spawn(0, tank_type, centre(10), centre(5));
    (void)f.match->issue_ground_move(walking_in.unit_index, point(centre(9), centre(6)), false);
    auto& walking_out = f.spawn(0, tank_type, centre(7), centre(6));
    (void)f.match->issue_ground_move(walking_out.unit_index, point(centre(20), centre(20)), false);
    auto& guard = f.spawn(0, tank_type, centre(8), centre(7));
    auto& guarded = f.spawn(0, tank_type, centre(20), centre(10));
    (void)f.match->issue_guard(guard.unit_index, guarded.unit_index, false);
    // A builder on a cheap frame works alone; one beside it moves when a
    // second builder works the same frame from phase 2 on.
    auto& cheap = f.spawn(0, building_type, centre(20), centre(20), false);
    auto& helper = f.spawn(0, builder_type, centre(10), centre(7));
    auto& help = f.match->issue_help_build(helper.unit_index, cheap.unit_index, false);
    CHECK(help.kind == runtime::help_build_kind);

    auto& build =
        f.match->issue_mobile_build(builder.unit_index, building_type, point(150, 101), false);
    CHECK(step(*f.match, builder, build) == 1);
    CHECK(step(*f.match, builder, build) == 2);
    // The idle tank, the one walking into the footprint and the guard move
    // off it to free plots; the enemy and the tank walking away stay.
    const auto moved_off = [&](const sim::unit_spawn::Slot& unit) {
        auto* head = f.head(unit.unit_index);
        if (!head || head->kind != move_ground_kind)
            return false;
        const auto to = f.head_point(unit.unit_index);
        const auto x = (to[0] >> 16) / 16;
        const auto z = (to[2] >> 16) / 16;
        return !on_site(to) &&
               f.match->spatial().plots[static_cast<size_t>(z) * map_cells + x].ground == 0;
    };
    CHECK(moved_off(idle) && moved_off(walking_in) && moved_off(guard));
    CHECK(f.head(enemy.unit_index) == nullptr);
    CHECK(f.queue(walking_out.unit_index) == std::vector<uint8_t>{move_ground_kind});
    CHECK(f.head(helper.unit_index) == &help);
    // The guard's order is given again behind the move; the tank walking
    // in walks to its old point again after it.
    CHECK(
        (f.queue(guard.unit_index) ==
         std::vector<uint8_t>{move_ground_kind, runtime::follow_ground_kind})
    );
    CHECK(
        (f.queue(walking_in.unit_index) == std::vector<uint8_t>{move_ground_kind, move_ground_kind})
    );
    // The kickout keeps each spot it sent a unit to.
    const auto& spots = f.match->build_site_kickout_spots();
    const auto guard_to = f.head_point(guard.unit_index);
    CHECK(spots[guard.unit_index][0] == 1);
    CHECK(spots[guard.unit_index][1] == (guard_to[0] >> 16));
    CHECK(spots[guard.unit_index][2] == (guard_to[2] >> 16));
    CHECK(spots[enemy.unit_index][0] == 0 && spots[helper.unit_index][0] == 0);
    // The kept spots are the rule's state: digested and saved as they are.
    const auto* table = runtime::find_rule_state(f.match->rule_state(), "build-site-kickout");
    CHECK(table && table->bytes(table->context).size() == spots.size() * 8);
    std::vector<uint8_t> saved(
        table->bytes(table->context).begin(), table->bytes(table->context).end()
    );
    spots[guard.unit_index][1] ^= 1;
    CHECK(table->restore(table->context, saved));
    CHECK(spots[guard.unit_index][1] == (guard_to[0] >> 16));
    CHECK(!table->restore(table->context, std::span<const uint8_t>(saved).first(8)));

    // A second blocked step sends a unit already on its way on, its later
    // orders kept.
    CHECK(step(*f.match, builder, build) == 2);
    CHECK(
        (f.queue(guard.unit_index) ==
         std::vector<uint8_t>{move_ground_kind, runtime::follow_ground_kind})
    );
    CHECK(spots[guard.unit_index][1] == (f.head_point(guard.unit_index)[0] >> 16));

    // The helper moves once another builder works the cheap frame.
    auto& second = f.spawn(0, builder_type, centre(22), centre(22));
    auto& joined = f.match->issue_help_build(second.unit_index, cheap.unit_index, false);
    joined.phase = 2;
    CHECK(step(*f.match, builder, build) == 2);
    CHECK(f.head(helper.unit_index) && f.head(helper.unit_index)->kind == move_ground_kind);
    // It helps the frame again from the start behind the move.
    CHECK(
        (f.queue(helper.unit_index) ==
         std::vector<uint8_t>{move_ground_kind, runtime::help_build_kind})
    );
    CHECK(f.head(helper.unit_index)->next != &help);
}

// ui.build-tools' drag with the snap override key: one of the local
// player's own mobile units goes to the point ahead of its orders, as the
// kickout sends a unit; another player's unit is not sent.
void drag_sends_units_ahead() {
    for (const bool rule : {false, true}) {
        Ground f(rule ? kicking() : match_rules::MatchRules{});
        auto& idle = f.spawn(0, tank_type, centre(4), centre(4));
        auto& walking = f.spawn(0, tank_type, centre(6), centre(4));
        (void)f.match->issue_ground_move(walking.unit_index, point(centre(20), centre(20)), false);
        auto& enemy = f.spawn(1, tank_type, centre(8), centre(4));
        // The point's fractions are dropped; its whole y is the move's.
        const sim::ground_orders::Point to{
            (centre(12) << 16) | 0x8000, (7 << 16) | 0x4000, (centre(14) << 16) | 0x1234
        };
        CHECK(!f.match->send_ahead_of_orders(enemy.unit_index, to));
        CHECK(f.head(enemy.unit_index) == nullptr);
        CHECK(!f.match->send_ahead_of_orders(0, to));
        // An idle unit is sent as a move order.
        CHECK(f.match->send_ahead_of_orders(idle.unit_index, to));
        CHECK(f.queue(idle.unit_index) == std::vector<uint8_t>{move_ground_kind});
        const auto idle_to = f.head_point(idle.unit_index);
        CHECK(idle_to[0] == centre(12) << 16 && idle_to[2] == centre(14) << 16);
        CHECK(idle_to[1] == 7 << 16);
        // A walking unit walks to its old point again after the move.
        CHECK(f.match->send_ahead_of_orders(walking.unit_index, to));
        CHECK((
            f.queue(walking.unit_index) == std::vector<uint8_t>{move_ground_kind, move_ground_kind}
        ));
        CHECK(f.head_point(walking.unit_index)[0] == centre(12) << 16);
        std::array<runtime::Match::OrderRecordView, 2> walks{};
        CHECK(f.match->queue_records(walking.unit_index, false, walks.data(), walks.size()) == 2);
        CHECK((walks[1].point[0] >> 16) == centre(20) && (walks[1].point[2] >> 16) == centre(20));
        // The kickout's table, kept under its rule, holds the point.
        const auto spots = f.match->build_site_kickout_spots();
        CHECK(spots.empty() == !rule);
        if (rule) {
            CHECK(spots[walking.unit_index][0] == 1);
            CHECK(spots[walking.unit_index][1] == centre(12));
            CHECK(spots[walking.unit_index][2] == centre(14));
            CHECK(spots[walking.unit_index][3] == 7);
            CHECK(spots[enemy.unit_index][0] == 0);
        }
    }
}

// orders.build-site-kickout kickout, the frame's energy: a unit on a frame
// that has taken 600 energy or more moves even alone.
void kickout_moves_off_costly_frames() {
    Ground f(kicking());
    f.match->state().unit_defs[building_type].build_cost_energy = 2000.0F;
    auto& builder = f.spawn(0, builder_type, 100, 100);
    // 2000 energy with 70% left: 600 taken, which moves; 71% left: 580
    // taken, which stays.
    auto& costly = f.spawn(0, building_type, centre(20), centre(20), false);
    costly.record.build_remaining = 0.7F;
    auto& cheap = f.spawn(0, building_type, centre(26), centre(20), false);
    cheap.record.build_remaining = 0.71F;
    auto& on_costly = f.spawn(0, builder_type, centre(8), centre(5));
    (void)f.match->issue_help_build(on_costly.unit_index, costly.unit_index, false);
    auto& on_cheap = f.spawn(0, builder_type, centre(9), centre(5));
    (void)f.match->issue_help_build(on_cheap.unit_index, cheap.unit_index, false);
    auto& build =
        f.match->issue_mobile_build(builder.unit_index, building_type, point(150, 101), false);
    CHECK(step(*f.match, builder, build) == 1);
    CHECK(step(*f.match, builder, build) == 2);
    CHECK(f.head(on_costly.unit_index)->kind == move_ground_kind);
    CHECK(f.head(on_cheap.unit_index)->kind == runtime::help_build_kind);
}

match_rules::MatchRules patrolling(
    match_rules::OrdersConPatrolGuardOptionsPatrolHoldPosition hold,
    match_rules::OrdersConPatrolGuardOptionsPatrolManeuver maneuver,
    match_rules::OrdersConPatrolGuardOptionsPatrolRoam roam
) {
    match_rules::MatchRules rules;
    auto& options = rules.orders.con_patrol_guard_options;
    options.enabled = true;
    options.patrol_hold_position = hold;
    options.patrol_maneuver = maneuver;
    options.patrol_roam = roam;
    options.guard_hook = true;
    return rules;
}

// orders.con-patrol-guard-options patrol-*: per standing move order, a
// patrolling builder skips the repair search (reclaim only) or the reclaim
// search (assist only); both is 3.1c's.
void patrol_choices() {
    using Hold = match_rules::OrdersConPatrolGuardOptionsPatrolHoldPosition;
    using Maneuver = match_rules::OrdersConPatrolGuardOptionsPatrolManeuver;
    using Roam = match_rules::OrdersConPatrolGuardOptionsPatrolRoam;
    // For each standing move order, a profile setting that order alone to
    // reclaim only, then one setting it alone to assist only.
    for (uint32_t stance = 0; stance < 3; ++stance) {
        for (const bool reclaim_only : {true, false}) {
            const uint8_t choice = reclaim_only ? 0 : 2;
            const auto rules = patrolling(
                stance == 0 ? static_cast<Hold>(choice) : Hold::both,
                stance == 1 ? static_cast<Maneuver>(choice) : Maneuver::both,
                stance == 2 ? static_cast<Roam>(choice) : Roam::both
            );
            for (const bool hack : {false, true}) {
                Ground f(hack ? rules : match_rules::MatchRules{});
                auto& builder = f.spawn(0, builder_type, 100, 100);
                stand(builder, stance);
                auto& player = f.match->state().game.players[0];
                if (reclaim_only) {
                    // Energy in store and a damaged ally in sight: 3.1c repairs it.
                    auto& damaged = f.spawn(0, tank_type, 140, 100);
                    damaged.record.health = 40;
                    auto& patrol =
                        f.match->issue_repair_patrol(builder.unit_index, point(40, 40), false);
                    CHECK(step(*f.match, builder, patrol) == 1);
                    const auto result = step(*f.match, builder, patrol);
                    if (hack)
                        CHECK(result == 2 && f.head(builder.unit_index) == &patrol);
                    else
                        CHECK(
                            (result == 6 || result == 3) &&
                            f.head(builder.unit_index)->kind != runtime::repair_patrol_kind
                        );
                } else {
                    // Metal low and a rock in sight: 3.1c reclaims it.
                    (void)sim::feature_runtime::place_feature(
                        f.match->state(),
                        f.match->feature_host(),
                        static_cast<size_t>(6) * map_cells + 9,
                        0,
                        nullptr,
                        nullptr,
                        sim::feature_runtime::no_player
                    );
                    player.metal = 10.0F;
                    auto& patrol =
                        f.match->issue_repair_patrol(builder.unit_index, point(40, 40), false);
                    CHECK(step(*f.match, builder, patrol) == 1);
                    const auto result = step(*f.match, builder, patrol);
                    if (hack)
                        CHECK(result == 2 && f.head(builder.unit_index) == &patrol);
                    else
                        CHECK(result == 3 && f.head(builder.unit_index)->kind == reclaim_kind);
                }
            }
        }
    }
}

match_rules::MatchRules guarding(
    match_rules::OrdersConPatrolGuardOptionsGuardHoldPosition hold,
    match_rules::OrdersConPatrolGuardOptionsGuardManeuver maneuver,
    match_rules::OrdersConPatrolGuardOptionsGuardRoam roam,
    bool hook
) {
    match_rules::MatchRules rules;
    auto& options = rules.orders.con_patrol_guard_options;
    options.enabled = true;
    options.guard_hold_position = hold;
    options.guard_maneuver = maneuver;
    options.guard_roam = roam;
    options.guard_hook = hook;
    return rules;
}

// orders.con-patrol-guard-options guard-* and guard-hook: per standing move
// order, a guard stands 7/20 of its spacing out (stay) or the whole spacing
// (scatter) on each axis, on its own side of the guarded unit; base, or no
// guard hook, keeps 3.1c's random offset.
void guard_choices() {
    using Hold = match_rules::OrdersConPatrolGuardOptionsGuardHoldPosition;
    using Maneuver = match_rules::OrdersConPatrolGuardOptionsGuardManeuver;
    using Roam = match_rules::OrdersConPatrolGuardOptionsGuardRoam;
    for (uint32_t stance = 0; stance < 3; ++stance) {
        for (const uint8_t choice : {uint8_t{0}, uint8_t{1}, uint8_t{2}}) {
            for (const bool hook : {true, false}) {
                const auto rules = guarding(
                    stance == 0 ? static_cast<Hold>(choice) : Hold::base,
                    stance == 1 ? static_cast<Maneuver>(choice) : Maneuver::base,
                    stance == 2 ? static_cast<Roam>(choice) : Roam::base,
                    hook
                );
                Ground f(rules);
                auto& unit = f.spawn(0, tank_type, 100, 120);
                auto& guarded = f.spawn(0, tank_type, 140, 100);
                stand(unit, stance);
                auto& guard = f.match->issue_guard(unit.unit_index, guarded.unit_index, false);
                CHECK(step(*f.match, unit, guard) == 1);
                const auto random_offset = f.head_point(unit.unit_index);
                CHECK(step(*f.match, unit, guard) == 2);
                const auto offset = f.head_point(unit.unit_index);
                // The spacing is (1 + 2 + 1) footprints of 16: 64.
                if (!hook || choice == 1) {
                    CHECK(offset == random_offset);
                    continue;
                }
                const int32_t distance = choice == 0 ? 64 * 7 / 20 : 64;
                // The guard stands west of the guarded unit and south of it.
                CHECK((offset[0] & 0xffff) == (random_offset[0] & 0xffff));
                CHECK((offset[2] & 0xffff) == (random_offset[2] & 0xffff));
                CHECK((offset[0] >> 16) == -distance && (offset[2] >> 16) == distance);
                CHECK(offset[1] == random_offset[1]);
            }
        }
    }
}

// An airfield world: fighters, a tank and an active repair pad.
struct Air {
    static constexpr uint16_t fighter = 1, tank = 2, pad = 3, builder = 4;
    static constexpr size_t type_count = 5;
    formats::tnt::Map map;
    std::vector<sim::visibility_state::TerrainCell> terrain =
        std::vector<sim::visibility_state::TerrainCell>(64 * 64);
    std::array<sim::visibility_state::SightMask, 1> masks{};
    std::shared_ptr<formats::objects3d::Model> model =
        std::make_shared<formats::objects3d::Model>();
    std::shared_ptr<formats::cob::CobProgram> script = std::make_shared<formats::cob::CobProgram>();
    std::array<sim::unit_spawn::LoadedType, type_count> loaded{};
    std::array<sim::unit_spawn::Type, type_count> types{};
    std::array<data::unit_definitions::UnitDefinition, type_count> defs{};
    data::unit_definitions::UnitTargetCategoryMasks target_masks;
    data::unit_definitions::RuntimeDefinitionMetadata metadata;
    std::vector<sim::spatial_state::Plot> plots = std::vector<sim::spatial_state::Plot>(64 * 64);
    std::array<sim::match_runtime::RuntimeTypeFields, type_count> fields{};
    std::array<uint8_t, 1> yard{4};
    sim::combat_state::WeaponRegistry weapons;
    Services services;
    Scenario scenario;
    std::unique_ptr<runtime::Match> match;

    explicit Air(const match_rules::MatchRules& rules, bool flagged) {
        map.attribute_width = map.attribute_height = 64;
        map.attributes.resize(64 * 64);
        masks[0].width = masks[0].height = 1;
        masks[0].pixels = {1};
        model->objects.resize(1);
        model->objects[0].name = "root";
        script->code = {sim::script_vm::opcode::return_};
        script->scripts = {{"Create", 0}};
        script->entry_points = {0};
        script->piece_names = {"root"};
        (void)sim::combat_state::install_weapon_text(
            weapons,
            "[AIRGUN]{id=1; reloadtime=0.1; range=300; lineofsight=1; weaponvelocity=100; "
            "turret=1; [DAMAGE]{default=10;}}"
        );
        for (size_t i = 1; i < type_count; ++i) {
            types[i].simulation.flags = OA_UNIT_DEF_FLAG_AVAILABLE;
            types[i].simulation.maximum_health = 100;
            types[i].footprint_x = types[i].footprint_z = 1;
            types[i].bm_code = 1;
            types[i].model = reinterpret_cast<uintptr_t>(model.get());
            types[i].cob = reinterpret_cast<uintptr_t>(script.get());
            loaded[i].model = model;
            loaded[i].script = script;
            defs[i].sight_distance = 400;
            defs[i].acceleration_fixed = 0x10000;
            defs[i].brake_rate_fixed = 0x10000;
            defs[i].max_velocity_fixed = 4 * 0x10000;
            defs[i].turn_rate = 1024;
            defs[i].weapon1 = "AIRGUN";
            fields[i].definition = &defs[i];
            fields[i].yard_mask = yard;
            fields[i].runtime_metadata = &metadata;
            fields[i].target_masks = &target_masks;
            fields[i].movement_class = 0;
        }
        types[fighter].simulation.flags |= OA_UNIT_DEF_FLAG_CAN_FLY | OA_UNIT_DEF_FLAG_HAS_WEAPONS;
        defs[fighter].can_fly = true;
        defs[fighter].can_attack = true;
        defs[fighter].cruise_altitude = 100;
        defs[fighter].attack_run_length = 50;
        types[pad].simulation.flags |= OA_UNIT_DEF_FLAG_BUILDER | OA_UNIT_DEF_FLAG_IS_AIRBASE;
        types[builder].simulation.flags |= OA_UNIT_DEF_FLAG_CAN_FLY | OA_UNIT_DEF_FLAG_BUILDER;
        types[builder].simulation.abilities =
            OA_UNIT_DEF_ABILITY_CAN_MOVE | OA_UNIT_DEF_ABILITY_CAN_PATROL |
            OA_UNIT_DEF_ABILITY_CAN_REPAIR | OA_UNIT_DEF_ABILITY_CAN_RECLAMATE;
        defs[builder].builder = defs[builder].can_fly = true;
        defs[builder].worker_time = 300;
        defs[builder].build_distance = 64;
        defs[builder].cruise_altitude = 50;
        defs[builder].weapon1.clear();
        for (size_t i = 1; i < type_count; ++i)
            loaded[i].type = types[i];
        sim::match_runtime::OfflineInputs input{map,   loaded, types,     fields, weapons, terrain,
                                                masks, 8,      8,         4,      2,       0,
                                                30,    1,      &scenario, {},     plots,   {},
                                                0,     0,      0.0F,      {}};
        input.rules = rules;
        match = std::make_unique<runtime::Match>(input, services);
        match->state().unit_defs[fighter].abilities =
            OA_UNIT_DEF_ABILITY_CAN_GUARD | OA_UNIT_DEF_ABILITY_CAN_ATTACK;
        if (flagged) {
            match->state().unit_defs[fighter].abilities |= OA_UNIT_DEF_ABILITY_CANT_BE_TRANSPORTED;
            match->state().unit_defs[builder].abilities |= OA_UNIT_DEF_ABILITY_CANT_BE_TRANSPORTED;
        }
        match->state().unit_defs[tank].max_slope = 10;
        match->state().unit_defs[tank].abilities = OA_UNIT_DEF_ABILITY_CAN_MOVE;
        match->state().game.local_player_index = 0;
        match->configure_strategic_environment({0, 0.5f, 0});
        for (uint8_t player = 0; player < 2; ++player) {
            match->simulation().players[player].present = true;
            match->simulation().players[player].status = player == 0 ? 1 : 2;
            std::array<uint8_t, 10> allies{};
            allies[player] = 1;
            match->configure_player_alliances(player, allies);
            auto& record = match->state().game.players[player];
            record.alliance[player] = 1;
            record.energy = record.energy_storage = 1000.0F;
            record.metal = record.metal_storage = 1000.0F;
        }
    }

    sim::unit_spawn::Slot& spawn(uint8_t player, uint16_t type, int32_t x, int32_t z) {
        auto* slot = match->create({player, type, at(x, z), true, 1, 0});
        CHECK(slot && slot->unit);
        slot->unit->flags |= OA_UNIT_FLAG_LIVE;
        return *slot;
    }

    uint32_t
    dispatch(sim::unit_spawn::Slot& s, sim::simulation_state::Order& order, uint32_t events) {
        runtime::MatchTickAccess host(*match);
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
};

// air.no-repair-retreat-flag: below three quarters of its health an aircraft
// breaks off for a repair pad; with flag cantbetransported a type that sets
// it keeps on, and one that does not still breaks off.
void flagged_aircraft_never_retreat() {
    for (const bool hack : {false, true}) {
        for (const bool flagged : {false, true}) {
            match_rules::MatchRules rules;
            if (hack) {
                rules.air.no_repair_retreat_flag.enabled = true;
                rules.air.no_repair_retreat_flag.flag =
                    match_rules::AirNoRepairRetreatFlagFlag::cantbetransported;
            }
            const bool retreats = !(hack && flagged);
            Air f(rules, flagged);
            auto& pad = f.spawn(0, Air::pad, 150, 150);
            pad.record.state_flags |= 1;
            f.file_pads(0);

            // A bombing run, past its pass.
            auto& bomber = f.spawn(0, Air::fighter, 100, 100);
            auto& target = f.spawn(1, Air::tank, 300, 100);
            const std::array<uint32_t, 3> there = target.unit->position;
            const sim::ground_orders::Point aim{
                std::bit_cast<int32_t>(there[0]),
                std::bit_cast<int32_t>(there[1]),
                std::bit_cast<int32_t>(there[2])
            };
            (void)f.match->issue_order(
                bomber.unit_index, runtime::attack_chase_kind, false, target.unit_index, &aim, 0, 0
            );
            auto& strike = *bomber.unit->primary;
            strike.kind = runtime::air_strike_kind;
            bomber.record.health = 70;
            strike.phase = 6;
            if (retreats)
                CHECK(f.dispatch(bomber, strike, 0) == 0);
            else
                CHECK(f.dispatch(bomber, strike, 0) == 2 && strike.phase == 3);

            // A patrol leg.
            auto& patroller = f.spawn(0, Air::fighter, 120, 120);
            patroller.record.health = 70;
            auto& patrol = f.match->issue_patrol(patroller.unit_index, point(400, 400), false);
            patrol.phase = 2;
            const auto result = f.dispatch(patroller, patrol, 0);
            CHECK((result == 0) == retreats);
            CHECK(
                (patroller.unit->primary->kind == sim::ground_orders::vtol_landing_kind) == retreats
            );

            // A repair patrol leg.
            auto& mender = f.spawn(0, Air::builder, 130, 130);
            mender.record.health = 70;
            auto& rounds = f.match->issue_patrol(mender.unit_index, point(400, 400), false);
            CHECK(rounds.kind == runtime::vtol_repair_patrol_kind);
            CHECK(f.dispatch(mender, rounds, 0) == 1);
            rounds.phase = 1;
            CHECK((f.dispatch(mender, rounds, 0) == 0) == retreats);
            CHECK(
                (mender.unit->primary->kind == sim::ground_orders::vtol_landing_kind) == retreats
            );
        }
    }
}

// air.guard-respects-hold-position: a guarding aircraft on Hold Position lets
// the guarded unit's attacker be; on any other standing move order, and in
// 3.1c, it attacks.
void air_guard_holds_position() {
    for (const bool hack : {false, true}) {
        for (const uint32_t stance : {0u, 1u}) {
            match_rules::MatchRules rules;
            rules.air.guard_respects_hold_position.enabled = hack;
            Air f(rules, false);
            auto& guard = f.spawn(0, Air::fighter, 100, 100);
            auto& guarded = f.spawn(0, Air::tank, 140, 100);
            auto& enemy = f.spawn(1, Air::tank, 200, 100);
            guarded.record.last_attacker_id = enemy.unit_index;
            stand(guard, stance);
            guard.record.flags |= 2u << OA_UNIT_FLAG_FIRE_ORDER_SHIFT;
            auto& follow = f.match->issue_guard(guard.unit_index, guarded.unit_index, false);
            follow.phase = 2;
            const auto result = f.dispatch(guard, follow, 0x10);
            const bool attacks = !(hack && stance == 0);
            CHECK((result == 3) == attacks);
            CHECK((guard.unit->primary != &follow) == attacks);
        }
    }
}

// orders.build-site-kickout and orders.con-patrol-guard-options for
// aircraft: a builder aircraft's blocked site waits up to the retry limit
// and its kickout moves the local player's units; a repair patrol with
// reclaim only skips the assist search.
void aircraft_builders() {
    for (const int32_t limit : {10, 20}) {
        match_rules::MatchRules rules;
        rules.orders.build_site_kickout.enabled = limit != 10;
        rules.orders.build_site_kickout.retry_limit = limit;
        Air f(rules, false);
        auto& builder = f.spawn(0, Air::builder, 100, 100);
        (void)f.spawn(1, Air::tank, 200, 200);
        auto& build =
            f.match->issue_mobile_build(builder.unit_index, Air::tank, point(200, 200), false);
        CHECK(build.kind == runtime::vtol_mobile_build_kind);
        build.phase = 2;
        uint32_t waits = 0;
        uint32_t result = 2;
        while (result == 2 && waits < 100) {
            build.wait_events = 0;
            result = f.dispatch(builder, build, 0);
            ++waits;
        }
        CHECK(result == 8 && waits == static_cast<uint32_t>(limit) + 2);
    }
    {
        auto rules = kicking();
        Air f(rules, false);
        auto& builder = f.spawn(0, Air::builder, 100, 100);
        auto& parked = f.spawn(0, Air::tank, 200, 200);
        auto& build =
            f.match->issue_mobile_build(builder.unit_index, Air::tank, point(200, 200), false);
        build.phase = 2;
        CHECK(f.dispatch(builder, build, 0) == 2);
        auto* head = parked.unit->primary;
        CHECK(head && head->kind == move_ground_kind);
    }
    for (const bool hack : {false, true}) {
        using Hold = match_rules::OrdersConPatrolGuardOptionsPatrolHoldPosition;
        using Maneuver = match_rules::OrdersConPatrolGuardOptionsPatrolManeuver;
        using Roam = match_rules::OrdersConPatrolGuardOptionsPatrolRoam;
        Air f(
            hack ? patrolling(Hold::reclaim_only, Maneuver::both, Roam::both)
                 : match_rules::MatchRules{},
            false
        );
        auto& mender = f.spawn(0, Air::builder, 100, 100);
        stand(mender, 0);
        auto& damaged = f.spawn(0, Air::tank, 140, 100);
        damaged.record.health = 40;
        auto& rounds = f.match->issue_patrol(mender.unit_index, point(400, 400), false);
        CHECK(f.dispatch(mender, rounds, 0) == 1);
        rounds.phase = 1;
        const auto result = f.dispatch(mender, rounds, 0);
        CHECK((result == 2) == hack);
        CHECK((mender.unit->primary == &rounds) == hack);
    }
}
} // namespace

int main() {
    try {
        attack_occupies_the_ordered_slot();
        dgun_ground_occupies_the_third_slot();
        busy_states_free_the_weapons();
        repair_pad_tests_the_pad();
        blocked_site_retries();
        kickout_clears_own_units();
        kickout_moves_off_costly_frames();
        drag_sends_units_ahead();
        patrol_choices();
        guard_choices();
        flagged_aircraft_never_retreat();
        air_guard_holds_position();
        aircraft_builders();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "order rules ok\n";
    return 0;
}
