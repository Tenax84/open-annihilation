// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "ground_missions.hpp"
#include "tick_internal.hpp"

#include <vector>

namespace oa::sim::match_runtime {

namespace {
constexpr uint32_t cancel_event = 0x2;
constexpr uint32_t build_wake_events = 0x8; // with the timer: wakes a build or repair step
constexpr uint32_t target_lost_event = 0x8;
constexpr uint32_t reclaim_abort_events = 0x10008;
constexpr uint32_t patrol_abort_events = 0x48;
constexpr uint32_t facing_wait_events = 0xa; // with 4 while the builder is out of build stance
constexpr uint32_t build_stance_wait = 0x4;
constexpr uint32_t goal_events = sim::ground_orders::arrived_event |
                                 sim::ground_orders::path_failed_event |
                                 sim::ground_orders::goal_replaced_event;

constexpr uint32_t speech_acknowledge = 5;
constexpr uint32_t speech_failed = 7;
constexpr uint32_t speech_complete = 8;
constexpr uint32_t speech_started = 9;
constexpr uint32_t speech_repaired = 10;
constexpr uint32_t speech_reclaiming = 11;

constexpr uint32_t facing_period = 150;
constexpr uint16_t facing_offset = 0x2492; // hover position swings about 51 degrees off the bearing
constexpr uint32_t reclaim_retry_ticks = 0x1e;
constexpr int32_t reclaim_damage_threshold = 0xe;
constexpr uint8_t reclaim_damage_kind = 5;
constexpr uint32_t reclaim_duration_ticks = 15;
constexpr float reclaim_minimum_metal = 10.0F;
constexpr float reclaim_duration_scale = 300.0F;
constexpr int32_t feature_reclaim_particle_floor = 30;
constexpr int32_t feature_reclaim_sprays_per_step = 2;
constexpr uint32_t decloak_hold_ticks = 300;
constexpr uint32_t patrol_step_ticks = 0x2d;
constexpr oa_fixed feature_scan_radius = 0xf00000;
constexpr oa_fixed feature_scan_step = 0x300000;
constexpr double reserve_fraction = 0.2;
// Command flags bits.
constexpr uint8_t patrol_clone_flag = 0x80; // the patrol loop already has its return leg
constexpr uint8_t has_target_flag = 0x02;

constexpr uint8_t move_ground_kind = sim::ground_orders::move_ground_kind;
constexpr uint8_t vtol_move_kind = sim::ground_orders::vtol_move_kind;
constexpr uint8_t vtol_landing_kind = sim::ground_orders::vtol_landing_kind;
// Standing move orders as Unit.flags holds them.
constexpr uint32_t move_order_hold = 0u << OA_UNIT_FLAG_MOVE_ORDER_SHIFT;
constexpr uint32_t move_order_maneuver = 1u << OA_UNIT_FLAG_MOVE_ORDER_SHIFT;
constexpr uint32_t move_order_roam = 2u << OA_UNIT_FLAG_MOVE_ORDER_SHIFT;

sim::ground_orders::Point point_of(const FixedVec3& position) {
    return {position.x, position.y, position.z};
}

/// Returns the sum of the high words of the squared 16.16 differences in x
/// and z: a squared world distance, as 3.1c measures it here.
///
/// @param a One signed 16.16 point.
/// @param b The other.
/// @return The squared distance in whole world units.
int32_t squared_world_distance(const FixedVec3& a, const FixedVec3& b) {
    const auto dx = static_cast<int32_t>(static_cast<uint32_t>(a.x) - static_cast<uint32_t>(b.x));
    const auto dz = static_cast<int32_t>(static_cast<uint32_t>(a.z) - static_cast<uint32_t>(b.z));
    const auto high = [](int32_t v) {
        return static_cast<int32_t>((static_cast<int64_t>(v) * v) >> 32);
    };
    return high(dz) + high(dx);
}

int16_t world_high(oa_fixed value) {
    return static_cast<int16_t>(static_cast<uint32_t>(value) >> 16);
}

bool finished(const oa::Unit& unit) {
    return unit.build_remaining == 0.0F;
}

struct FeatureCandidate {
    FixedVec3 position{};
    float amount{};
};

struct FeatureChoice {
    bool found{};
    FixedVec3 position{};
    float amount{};
};

// Best of three random picks, by amount.
FeatureChoice pick_feature(const std::vector<FeatureCandidate>& list, TickHost& host) {
    FeatureChoice choice;
    if (list.empty())
        return choice;
    float best = 0.0F;
    size_t chosen = 0;
    for (int draw = 0; draw < 3; ++draw) {
        const auto i = host.random(static_cast<uint32_t>(list.size()));
        if (best < list[i].amount) {
            best = list[i].amount;
            chosen = i;
        }
    }
    choice.found = true;
    choice.position = list[chosen].position;
    choice.amount = list[chosen].amount;
    return choice;
}
} // namespace

// Aircraft construction, repair and reclaim missions. One instance serves a
// single dispatch of one order.
class TickHost::VtolBuildMissions {
    TickHost& host;
    Match& match;
    sim::unit_spawn::Slot& s;
    oa::Unit& unit;
    Match::RuntimeOrder& record;
    sim::simulation_state::Order& order;

    oa::World& world() { return match.state(); }

    uint32_t tick() { return world().game.tick; }

    const UnitDef* def_of(const oa::Unit& u) { return oa::world_unit_def_of(&world(), &u); }

    // A builder without a type is noted and reads the reserved type 0.
    const UnitDef& def() {
        const auto* d = def_of(unit);
        if (d)
            return *d;
        match.fault_.note("aircraft builder without a unit def");
        return world().unit_defs[0];
    }

    sim::simulation_state::Unit* target() {
        return record.construction.target ? record.construction.target : record.attack.target;
    }

    bool flight_ready() {
        const auto* d = def_of(unit);
        return unit.movement != 0 && d && (d->flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0;
    }

    uint16_t build_distance() { return static_cast<uint16_t>(def().build_distance); }

    /// Plays the order's acknowledgement once per issued command.
    ///
    /// @param caption The order's caption.
    void acknowledge(const char* caption) {
        if ((record.extra.command_flags & sim::ground_orders::order_announce_flag) == 0)
            return;
        record.extra.command_flags &=
            static_cast<uint8_t>(~sim::ground_orders::order_announce_flag);
        speak(speech_acknowledge, caption);
    }

    /// Plays a speech category for the unit.
    ///
    /// @param category Speech category (5 acknowledge, 7 failed, 8 complete,
    ///     9 started, 10 repaired, 11 reclaiming).
    void speak(uint32_t category) { host.play_sound(*s.unit, category); }

    /// Plays a speech category for the unit, captioned with the order's text.
    ///
    /// @param category Speech category, as speak(category) takes.
    /// @param caption The order's caption in place of the category's own.
    void speak(uint32_t category, const char* caption) {
        host.play_sound(*s.unit, category, caption);
    }

    /// Flags the build panel for a redraw when the viewpoint player has the
    /// unit selected.
    void refresh_selection() {
        if (unit.owner_index == world().game.viewpoint_player &&
            (unit.flags & OA_UNIT_FLAG_SELECTED) != 0)
            match.selection_.frame_flags |= OA_FRAME_FLAG_REFRESH_ORDER_PANEL;
    }

    // Waits `ticks` ticks for the timer event.
    void wait(uint32_t ticks) {
        order.wait_events |= 1;
        order.wake_tick = tick() + ticks;
    }

    // A goal of this order at a fixed point, for the air driver.
    sim::air::AirGoal goal_at(const sim::ground_orders::Point& point) {
        return sim::air::air_goal_at_point(
            &order.raised_events, &unit, {point[0], point[1], point[2]}
        );
    }

    void fly_to(const sim::air::AirGoal& goal) { host.install_air_goal(s, order, &goal); }

    // Arrive anywhere within build distance of the point.
    void fly_within_reach(const sim::ground_orders::Point& point) {
        auto goal = goal_at(point);
        sim::air::air_goal_set_arrival_radius(&goal, static_cast<int16_t>(build_distance()));
        fly_to(goal);
    }

    // Arrive over the point at cruise altitude above the surface there.
    void fly_at_cruise(const sim::ground_orders::Point& point) {
        auto goal = goal_at(point);
        sim::air::air_goal_set_altitude(&goal, host.air_host(), def().cruise_alt);
        fly_to(goal);
    }

    void drop_goal() { host.install_air_goal(s, order, nullptr); }

    std::optional<size_t> plot_index(int32_t cell_x, int32_t cell_z) {
        const auto& grid = match.spatial();
        if (cell_x < 0 || cell_z < 0 || static_cast<uint32_t>(cell_x) >= grid.terrain_width ||
            static_cast<uint32_t>(cell_z) >= grid.terrain_height)
            return std::nullopt;
        const auto index =
            static_cast<size_t>(cell_z) * grid.terrain_width + static_cast<size_t>(cell_x);
        if (index >= grid.plots.size())
            return std::nullopt;
        return index;
    }

    /// Returns a plot's feature word, following a footprint continuation
    /// once.
    ///
    /// @param index Plot index (row-major).
    /// @return The FeatureDef index, or sim::spatial_state::no_feature.
    uint16_t plot_feature(size_t index) {
        const auto& grid = match.spatial();
        const auto word = grid.plots[index].feature_word;
        if (word < sim::spatial_state::first_reserved_feature)
            return word;
        if (word != sim::spatial_state::feature_continuation)
            return sim::spatial_state::no_feature;
        const auto back =
            static_cast<size_t>(grid.plots[index].feature_back_z) * grid.terrain_width +
            grid.plots[index].feature_back_x;
        return back <= index ? grid.plots[index - back].feature_word
                             : sim::spatial_state::no_feature;
    }

    const FeatureDef* feature_def(uint16_t word) {
        if (word >= sim::spatial_state::first_reserved_feature)
            return nullptr;
        return oa::world_feature_def(&world(), oa::oa_ref_from_index(word));
    }

    /// Collects auto-reclaimable features on a 3-cell lattice around a point
    /// and picks one energy and one metal feature, each the richest of three
    /// random draws.
    ///
    /// The picked positions' y is zero.
    ///
    /// @param centre Signed 16.16 centre of the search.
    /// @param radius Side of the searched square, signed 16.16.
    /// @param[out] energy The energy pick; found is false when none.
    /// @param[out] metal The metal pick; found is false when none.
    /// @return True when either kind was found.
    bool select_reclaim_features(
        const FixedVec3& centre, oa_fixed radius, FeatureChoice& energy, FeatureChoice& metal
    ) {
        std::vector<FeatureCandidate> energy_list, metal_list;
        const auto half = radius / 2;
        for (auto z = centre.z - half; z <= centre.z + half; z += feature_scan_step) {
            for (auto x = centre.x - half; x <= centre.x + half; x += feature_scan_step) {
                const auto index = plot_index(x >> 20, z >> 20);
                if (!index)
                    continue;
                const auto* feature = feature_def(plot_feature(*index));
                if (!feature || (feature->flags & OA_FEATURE_FLAG_RECLAIMABLE) == 0 ||
                    (feature->flags & OA_FEATURE_FLAG_AUTO_RECLAIMABLE) == 0)
                    continue;
                if (feature->energy != 0.0F)
                    energy_list.push_back({{x, 0, z}, feature->energy});
                if (feature->metal != 0.0F)
                    metal_list.push_back({{x, 0, z}, feature->metal});
            }
        }
        energy = pick_feature(energy_list, host);
        metal = pick_feature(metal_list, host);
        return energy.found || metal.found;
    }

    /// Credits the unit with the feature under a point and starts its
    /// reclamate sequence; a sprite already playing a sequence is left
    /// alone.
    ///
    /// @param point Signed 16.16 map point.
    /// @return False when there is nothing to reclaim there.
    bool finish_feature_reclaim(const sim::ground_orders::Point& point) {
        return match.reclaim_feature(unit, {point[0], point[1], point[2]});
    }

    /// Tells whether this unit may repair or assist a unit
    /// (can_repair_target).
    ///
    /// @param other Unit to repair or assist.
    /// @return True when allowed; false for a unit without a type.
    bool can_assist(const oa::Unit& other) {
        const auto* other_def = def_of(other);
        if (!other_def)
            return false;
        const auto& d = def();
        const RepairEligibility eligibility{
            static_cast<uint8_t>(d.abilities >> 8),
            d.flags,
            d.max_water_depth,
            other.health,
            static_cast<int32_t>(other_def->max_damage),
            static_cast<uint8_t>(other.flags & OA_UNIT_FLAG_OCCUPANCY_MASK),
            world_high(other_def->model_height),
            world_high(other.position.y),
            world().game.sea_level
        };
        return can_repair_target(eligibility);
    }

    /// Tells whether this unit may reclaim a unit: it can reclaim, and the
    /// other is not airborne and cannot capture.
    ///
    /// @param other Unit to reclaim.
    /// @return True when allowed.
    bool can_reclaim(const oa::Unit& other) {
        const auto* other_def = def_of(other);
        return (def().abilities & OA_UNIT_DEF_ABILITY_CAN_RECLAMATE) != 0 &&
               (other.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) != 2 && other_def &&
               (other_def->abilities & OA_UNIT_DEF_ABILITY_CAN_CAPTURE) == 0;
    }

    /// Returns the damage one reclaim bite deals, from worker time,
    /// veterancy and the target's health and metal cost (at least 10).
    ///
    /// @param other Unit being reclaimed.
    /// @param ticks Bite duration in ticks.
    /// @return Damage in health points, at least 1.
    int32_t reclaim_bite(const oa::Unit& other, uint32_t ticks) {
        const auto* other_def = def_of(other);
        if (!other_def)
            return 1;
        const auto metal = other_def->build_cost_metal > reclaim_minimum_metal
                               ? other_def->build_cost_metal
                               : reclaim_minimum_metal;
        const auto work = static_cast<uint32_t>(
            static_cast<int32_t>((unit.veteran_level + 5u) / 5u) *
            static_cast<int32_t>(static_cast<uint16_t>(def().worker_time)) *
            static_cast<int32_t>(other_def->max_damage) * static_cast<int32_t>(ticks)
        );
        const auto bite = truncate_low32(
            static_cast<float>(
                static_cast<double>(work) / (static_cast<double>(metal) * reclaim_duration_scale)
            )
        );
        return bite < 2 ? 1 : bite;
    }

    /// Applies reclaim damage (kind 5) from this unit to a victim through
    /// the health path.
    ///
    /// @param victim Unit being reclaimed.
    /// @param amount Damage in health points.
    void apply_reclaim_damage(sim::simulation_state::Unit& victim, int32_t amount) {
        auto& victim_slot = host.slot(victim);
        const auto& fbi = *match.fields(victim_slot).definition;
        sim::unit_health::UnitType type{
            fbi.damage_modifier_fixed,
            static_cast<float>(fbi.build_cost_energy),
            fbi.build_time,
            victim.type->maximum_health,
            static_cast<float>(fbi.build_cost_metal)
        };
        sim::unit_health::Unit source{
            s.unit_index, unit.state_flags, unit.veteran_level, unit.health, nullptr
        };
        sim::unit_health::Unit projected{
            victim_slot.unit_index,
            victim_slot.record.state_flags,
            victim_slot.record.veteran_level,
            victim.health,
            &type
        };
        projected.type_index = victim_slot.record.type_index;
        HealthHost health(match, victim_slot.unit_index);
        sim::unit_health::submit_damage(&source, projected, amount, reclaim_damage_kind, health, 0);
        health.store();
    }

    // Construction step shared by the build missions: every 150 ticks the
    // aircraft is sent to hover beside the frame, then it adds worker time.
    void build_step(sim::simulation_state::Unit& frame) {
        const auto& frame_record = frame.record;
        if (tick() % facing_period == 0) {
            const auto bearing = static_cast<uint16_t>(
                base::game_math::direction(
                    static_cast<int32_t>(
                        static_cast<uint32_t>(unit.position.x) -
                        static_cast<uint32_t>(frame_record.position.x)
                    ),
                    static_cast<int32_t>(
                        static_cast<uint32_t>(unit.position.z) -
                        static_cast<uint32_t>(frame_record.position.z)
                    )
                ) -
                facing_offset
            );
            const auto reach = static_cast<sim::unit_movement::Fixed>(
                static_cast<uint32_t>(build_distance()) << 16
            );
            const sim::ground_orders::Point hover{
                frame_record.position.x + sim::unit_movement::sine_scaled(bearing, reach),
                frame_record.position.y,
                frame_record.position.z + sim::unit_movement::cosine_scaled(bearing, reach)
            };
            auto goal = goal_at(hover);
            sim::air::air_goal_set_bearing(&goal, bearing);
            fly_to(goal);
        }
        const auto rate = static_cast<float>(
            static_cast<uint32_t>(static_cast<uint16_t>(def().worker_time)) / 30u
        );
        ConstructionAdapter adapter(host, s, record);
        (void)adapter.construct(frame, rate);
    }

    // Heading from the frame to this unit.
    int16_t bearing_from(const oa::Unit& other) {
        return static_cast<int16_t>(base::game_math::direction(
            static_cast<int32_t>(
                static_cast<uint32_t>(unit.position.x) - static_cast<uint32_t>(other.position.x)
            ),
            static_cast<int32_t>(
                static_cast<uint32_t>(unit.position.z) - static_cast<uint32_t>(other.position.z)
            )
        ));
    }

    /// Queues a new order at the head of the unit's queue.
    ///
    /// @param kind Mission kind.
    /// @param order_target The new order's target unit; null clears the
    ///     order's has-target bit.
    /// @param point Signed 16.16 destination, or null for none.
    /// @return The new order.
    Match::RuntimeOrder&
    push_order(uint8_t kind, sim::simulation_state::Unit* order_target, const FixedVec3* point) {
        auto& pushed = host.owned(
            point ? match.insert_ground_order(s.unit_index, kind, point_of(*point))
                  : match.insert_ground_order(s.unit_index, kind)
        );
        if (order_target)
            host.retarget(pushed, order_target);
        else
            pushed.extra.command_flags &= static_cast<uint8_t>(~has_target_flag);
        return pushed;
    }

    /// Queues a copy of this order at the unit's position behind the rest of
    /// the queue, once per patrol loop, then marks this order
    /// (patrol_clone_flag).
    void clone_patrol_leg() {
        bool present = false;
        for_each_primary_uncycled(s.unit->primary, [&](sim::simulation_state::Order* queued) {
            if (host.owned(*queued).extra.command_flags & patrol_clone_flag)
                present = true;
        });
        if (!present) {
            auto& copy = push_order(order.kind, nullptr, &unit.position);
            auto& queue = s.unit->primary;
            queue = copy.order.next;
            auto** tail = &queue;
            size_t steps = overlay_order_budget;
            while (*tail && steps-- > 0)
                tail = &(*tail)->next;
            copy.order.next = nullptr;
            *tail = &copy.order;
        }
        record.extra.command_flags |= patrol_clone_flag;
    }

    /// Returns the order kind the command resolver picks for a plain move to
    /// the unit's own position.
    ///
    /// @return QMove without a movement object, VTOL_Move for an aircraft,
    ///     Move_Ground otherwise, or 0 when the type cannot move.
    uint8_t move_kind() {
        const auto& d = def();
        if ((d.abilities & OA_UNIT_DEF_ABILITY_CAN_MOVE) == 0)
            return 0;
        if (unit.movement == 0)
            return qmove_kind;
        return (d.flags & OA_UNIT_DEF_FLAG_CAN_FLY) ? vtol_move_kind : move_ground_kind;
    }

    /// Returns the order kind the command resolver picks for assisting a
    /// unit.
    ///
    /// @param other Unit to assist.
    /// @return RepairUnit for a finished unit, HelpBuild for an unfinished
    ///     one (their VTOL forms for an aircraft), or 0 when the unit is not
    ///     live or may not be assisted.
    uint8_t assist_kind(const oa::Unit& other) {
        if ((other.flags & OA_UNIT_FLAG_LIVE) == 0 || !can_assist(other))
            return 0;
        const bool aircraft = (def().flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0;
        if (finished(other))
            return aircraft ? vtol_repair_unit_kind : repair_unit_kind;
        return aircraft ? vtol_help_build_kind : help_build_kind;
    }

    /// Queues a repair or build assist of an ally; unless the unit roams, a
    /// move back to where it is now goes first and the assist is leashed by
    /// the sight distance (standing move order 0) or the manoeuvre leash.
    ///
    /// @param other Ally to assist.
    /// @return False when no assist applies or the standing move order is
    ///     the unused value 3.
    bool issue_assist(sim::simulation_state::Unit& other) {
        const auto kind = assist_kind(other.record);
        if (kind == 0)
            return false;
        const auto mode = unit.flags & OA_UNIT_FLAG_MOVE_ORDER_MASK;
        if (mode != move_order_hold && mode != move_order_maneuver && mode != move_order_roam)
            return false;
        if (mode == move_order_roam) {
            (void)push_order(kind, &other, nullptr);
            return true;
        }
        const auto home = unit.position;
        (void)push_order(move_kind(), nullptr, &home);
        auto& assist = push_order(kind, &other, nullptr);
        assist.attack.leash = mode == move_order_hold
                                  ? def().sight_distance
                                  : static_cast<uint16_t>(def().maneuver_leash_length);
        // VTOL_HelpBuild keeps its third parameter as its blocked-site count.
        assist.construction.blocked_retries = assist.attack.leash;
        assist.extra.anchor_x = world_high(home.x);
        assist.extra.anchor_z = world_high(home.z);
        return true;
    }

    /// Sends a damaged aircraft to a random pad of those its owner's pad
    /// list offers within 0xf00 world units (Match::repair_pads_in_reach).
    ///
    /// @return True when a pad was found and VTOL_Landing queued.
    bool land_at_repair_pad() {
        const auto pads = match.repair_pads_in_reach(unit);
        if (pads == 0)
            return false;
        drop_goal();
        const auto* pad = match.repair_pad_in_reach(unit, host.random(pads));
        (void)push_order(vtol_landing_kind, &match.units_[pad->id], nullptr);
        order.wait_events = 0;
        return true;
    }

    /// Collects allied ground units near the aircraft that need repair or
    /// are still being built, excluding ones its owner is reclaiming.
    ///
    /// @param radius Search radius, signed 16.16.
    /// @return The candidates in spatial bucket order.
    std::vector<sim::simulation_state::Unit*> repair_candidates(oa_fixed radius) {
        struct Search {
            VtolBuildMissions* self;
            const oa::Player* owner;
            std::vector<sim::simulation_state::Unit*> found;
        } search{this, oa::world_unit_owner(&world(), &unit), {}};

        if (!search.owner)
            return {};
        match.for_each_unit_in_radius(
            {unit.position.x, unit.position.y, unit.position.z},
            radius,
            [](void* context, sim::unit_spawn::Slot& slot) {
                auto& search = *static_cast<Search*>(context);
                auto& self = *search.self;
                auto& other = slot.record;
                const auto* other_def = self.def_of(other);
                const auto* other_owner = oa::world_unit_owner(&self.world(), &other);
                if (&other == &self.unit || !other_def || !other_owner ||
                    search.owner->alliance[other_owner->index] == 0 ||
                    (other.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) != 1)
                    return;
                const bool damaged = static_cast<uint32_t>(static_cast<int32_t>(other.health)) <
                                     other_def->max_damage;
                if (!damaged && finished(other))
                    return;
                if (other.last_attacker_owner == search.owner->index &&
                    other.damage_kind == reclaim_damage_kind)
                    return;
                search.found.push_back(slot.unit);
            },
            &search
        );
        return std::move(search.found);
    }

  public:

    VtolBuildMissions(TickHost& h, sim::unit_spawn::Slot& slot, sim::simulation_state::Order& o)
        : host(h), match(h.match), s(slot), unit(slot.record), record(h.owned(o)), order(o) {}

    /// Runs one step of VTOL_MobileBuild: the aircraft flies within build
    /// distance of the site, waits for it to clear, places the frame and
    /// builds it, hovering beside it.
    ///
    /// @param events Events raised on the order since its last step.
    /// @return 1 (next phase), 2 (keep waiting), 5 (done: cancelled or
    ///     complete), 8 (failed: target lost, no path, blocked after ten
    ///     retries or no free slot) or 7 (invalid: cannot fly, or the frame
    ///     is gone).
    uint32_t mobile_build(uint32_t events) {
        if (events & cancel_event) {
            refresh_selection();
            return 5;
        }
        if (events & target_lost_event) {
            speak(speech_failed, "Construction terminated");
            refresh_selection();
            return 8;
        }
        switch (order.phase) {
        case 0:
            if (!flight_ready())
                return 7;
            acknowledge("Building");
            host.take_off(s, order);
            return 1;
        case 1: {
            record.construction.blocked_retries = 0;
            ConstructionAdapter adapter(host, s, record);
            sim::ground_orders::snap_to_footprint_centre(
                record.extra.destination, adapter.footprint_x(), adapter.footprint_z()
            );
            fly_within_reach(record.extra.destination);
            order.wait_events = goal_events;
            return 1;
        }
        case 2: {
            if (events & sim::ground_orders::path_failed_event)
                return 8;
            ConstructionAdapter adapter(host, s, record);
            if (!adapter.site_clear()) {
                const auto& kickout = host.match.rules().orders.build_site_kickout;
                if (kickout.kickout)
                    GroundMissions::clear_build_site(
                        host, adapter.footprint_x(), adapter.footprint_z(), record.extra.destination
                    );
                if (record.construction.blocked_retries == 0)
                    speak(speech_failed, "Waiting for target area to clear");
                else if (record.construction.blocked_retries > kickout.retry_limit) {
                    speak(speech_failed, "Target area was blocked");
                    return 8;
                }
                ++record.construction.blocked_retries;
                wait(reclaim_retry_ticks);
                return 2;
            }
            adapter.snap_build_height();
            auto* frame = adapter.spawn_nanoframe();
            // The order watches its frame, so a frame destroyed before it is
            // finished ends the order as a lost target.
            host.retarget(record, frame);
            if (!frame) {
                speak(speech_failed, "Unable to create any more units");
                return 8;
            }
            speak(speech_started, "Starting construction");
            adapter.issue_get_built(*frame);
            adapter.start_building(
                static_cast<int16_t>(bearing_from(frame->record) - unit.heading)
            );
            refresh_selection();
            return 1;
        }
        case 3:
        case 4: {
            // The stance check only arms the wait mask; the build step runs
            // either way.
            if (order.phase == 3 && (unit.build_flags & 1) == 0)
                order.wait_events = facing_wait_events | build_stance_wait;
            auto* frame = target();
            if (!frame)
                return 7;
            build_step(*frame);
            if (finished(frame->record))
                return 1;
            wait(1);
            order.wait_events |= facing_wait_events;
            return 2;
        }
        case 5:
            speak(speech_complete, "Building complete");
            return 5;
        default:
            return 7;
        }
    }

    /// Runs one step of VTOL_HelpBuild: the aircraft helps finish another
    /// unit's construction.
    ///
    /// @param events Events raised on the order since its last step.
    /// @return 1 (next phase), 2 (keep waiting), 5 (done: cancelled or
    ///     finished), 8 (failed: no frame, target lost or no path) or
    ///     7 (invalid: cannot fly or builds nothing).
    /// @quirk StartBuilding gets the absolute bearing to the frame, not one
    ///     relative to the builder's heading as VTOL_MobileBuild passes.
    uint32_t help_build(uint32_t events) {
        auto* frame = target();
        if (!frame || (events & target_lost_event)) {
            speak(speech_failed, "Construction terminated by hostile action");
            refresh_selection();
            return 8;
        }
        if (events & cancel_event) {
            refresh_selection();
            return 5;
        }
        switch (order.phase) {
        case 0:
            if (!flight_ready() || def().build_ids == 0)
                return 7;
            acknowledge("Building");
            host.take_off(s, order);
            return 1;
        case 1:
            record.construction.blocked_retries = 0;
            fly_within_reach(point_of(frame->record.position));
            order.wait_events = goal_events;
            return 1;
        case 2:
            if (events & sim::ground_orders::path_failed_event)
                return 8;
            if (finished(frame->record))
                return 5;
            {
                // The absolute bearing is passed, not one relative to the
                // builder's heading as MobileBuild does.
                ConstructionAdapter adapter(host, s, record);
                adapter.start_building(bearing_from(frame->record));
            }
            refresh_selection();
            return 1;
        case 3:
            build_step(*frame);
            if (!finished(frame->record)) {
                wait(1);
                order.wait_events |= facing_wait_events;
                return 2;
            }
            return 5;
        default:
            return 7;
        }
    }

    /// Runs one step of VTOL_RepairUnit: the aircraft repairs a landed unit,
    /// within an optional leash of its anchor.
    ///
    /// @param events Events raised on the order since its last step.
    /// @return 1 (next phase), 2 (keep waiting), 0 (restart: the patient
    ///     moves), 5 (done: repaired, the patient left the ground or past
    ///     the leash), 8 (failed: no patient, one it may not repair, or no
    ///     path) or 7 (invalid).
    /// @quirk The nano spray is drawn whether or not the repair step was
    ///     afforded.
    uint32_t repair_unit(uint32_t events) {
        auto* patient = target();
        if (!patient) {
            speak(speech_failed, "Repairs unsuccessful.");
            return 8;
        }
        if (record.attack.leash != 0) {
            const auto dx =
                static_cast<int32_t>(world_high(unit.position.x)) - record.extra.anchor_x;
            const auto dz =
                static_cast<int32_t>(world_high(unit.position.z)) - record.extra.anchor_z;
            if (static_cast<int32_t>(base::game_math::distance(dx, dz)) >= record.attack.leash)
                return 5;
        }
        auto& patient_record = patient->record;
        if ((patient_record.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) != 1) {
            speak(speech_failed, "Repairs unsuccessful.");
            return 5;
        }
        record.extra.destination = point_of(patient_record.position);
        switch (order.phase) {
        case 0:
            if (!flight_ready())
                return 7;
            if (!can_assist(patient_record)) {
                speak(speech_failed, "Repair mission failed");
                return 8;
            }
            acknowledge("Repairing");
            host.take_off(s, order);
            return 1;
        case 1:
            fly_at_cruise(record.extra.destination);
            order.wait_events = goal_events | build_wake_events;
            return 1;
        case 2: {
            if (events & sim::ground_orders::path_failed_event)
                return 8;
            if (patient_record.flags & OA_UNIT_FLAG_MOVE_RATE_MASK) {
                wait(0xf);
                return 0;
            }
            const auto* patient_def = def_of(patient_record);
            if (!patient_def ||
                static_cast<uint32_t>(static_cast<int32_t>(patient_record.health)) >=
                    patient_def->max_damage)
                return 1;
            // The nano spray is drawn whether or not the repair was afforded.
            if (!host.nano_repair(s, *patient))
                match.spray_nano(s, *patient, Match::NanoSpray::repair);
            wait(1);
            order.wait_events |= build_wake_events;
            return 2;
        }
        case 3:
            speak(speech_repaired, "Unit repaired");
            return 5;
        default:
            return 7;
        }
    }

    /// Runs one step of VTOL_ReclaimUnit: the aircraft flies to a unit and
    /// sprays it, biting off health once more than 14 ticks of spraying
    /// have built up.
    ///
    /// It says "working" as it sets off; under display rules that hold the
    /// voice back, it says it once on arrival instead and reclaims in a
    /// stage 3 of their own.
    ///
    /// @param events Events raised on the order since its last step.
    /// @return 1 (next phase), 2 (keep waiting), 0 (restart: out of reach),
    ///     9 (retry: no path), 5 (done: target gone or an abort event),
    ///     8 (failed: a target it may not reclaim) or 7 (invalid).
    uint32_t reclaim_unit(uint32_t events) {
        auto* victim = target();
        if (!victim || (events & reclaim_abort_events))
            return 5;
        switch (order.phase) {
        case 0:
            if (!flight_ready())
                return 7;
            if ((def().abilities & OA_UNIT_DEF_ABILITY_CAN_RECLAMATE) == 0) {
                speak(speech_failed, "Reclamation failed");
                return 7;
            }
            if (!can_reclaim(victim->record)) {
                speak(speech_failed, "That unit cannot be reclaimed");
                return 8;
            }
            acknowledge("Reclaiming");
            host.take_off(s, order);
            return 1;
        case 1:
            record.construction.type_index = reclaim_bite(victim->record, reclaim_duration_ticks);
            record.construction.remaining = 0;
            sim::ground_orders::snap_to_footprint_centre(
                record.extra.destination, unit.footprint_x, unit.footprint_z
            );
            fly_to(goal_at(point_of(victim->record.position)));
            order.wait_events |= goal_events | reclaim_abort_events;
            // The display rules may hold the voice back until reclaiming
            // starts, in stage 2.
            if (!match.display_rules().vtol_reclaim_voice_at_start)
                speak(speech_reclaiming);
            return 1;
        case 2:
        case 3: {
            // Stage 3 is the display rules' own: reached after the voice
            // they hold back until the aircraft is there to reclaim.
            const bool voice_at_start = match.display_rules().vtol_reclaim_voice_at_start;
            if (order.phase == 3 && !voice_at_start)
                return 7;
            if (events & sim::ground_orders::path_failed_event)
                return 9;
            if (order.phase == 2 && voice_at_start) {
                order.phase = 3;
                speak(speech_reclaiming);
            }
            order.wait_events |= reclaim_abort_events;
            const auto reach = static_cast<int32_t>(build_distance());
            if (reach * reach < squared_world_distance(unit.position, victim->record.position) ||
                !can_reclaim(victim->record)) {
                wait(reclaim_retry_ticks);
                return 0;
            }
            if (record.construction.remaining > reclaim_damage_threshold) {
                apply_reclaim_damage(*victim, record.construction.type_index);
                record.construction.remaining = 0;
            }
            match.spray_nano(s, *victim, Match::NanoSpray::reclaim);
            wait(2);
            record.construction.remaining += 2;
            return 2;
        }
        default:
            return 7;
        }
    }

    /// Runs one step of VTOL_Reclaim: the aircraft reclaims the feature under
    /// its destination, spraying it for a time set by its metal and energy.
    ///
    /// @param events Events raised on the order since its last step.
    /// @return 1 (next phase), 2 (keep waiting), 5 (done), 8 (failed: no
    ///     feature, one that cannot be reclaimed, or no path) or
    ///     7 (invalid).
    uint32_t reclaim_feature(uint32_t events) {
        int16_t cell_x = 0, cell_z = 0;
        const auto word = match.feature_word_under(record.extra.destination, &cell_x, &cell_z);
        if (word == sim::spatial_state::no_feature) {
            speak(speech_failed, "Reclamation failed");
            return 8;
        }
        const auto* feature = feature_def(word);
        if (!feature || (feature->flags & OA_FEATURE_FLAG_RECLAIMABLE) == 0)
            return 8;
        switch (order.phase) {
        case 0:
            if (!flight_ready() || (def().abilities & OA_UNIT_DEF_ABILITY_CAN_RECLAMATE) == 0)
                return 7;
            acknowledge("Reclaiming");
            host.take_off(s, order);
            return 1;
        case 1:
            fly_to(goal_at(record.extra.destination));
            order.wait_events = goal_events;
            return 1;
        case 2:
            if (events & sim::ground_orders::path_failed_event)
                return 8;
            record.construction.type_index =
                truncate_low32((feature->metal + feature->energy) * 0.5F + 30.0F);
            speak(speech_reclaiming);
            return 1;
        case 3:
            wait(2);
            record.construction.type_index -= 2;
            if (record.construction.type_index > 0) {
                unit.decloak_until_tick = tick() + decloak_hold_ticks;
                if (record.construction.type_index > feature_reclaim_particle_floor)
                    for (int32_t spray = 0; spray < feature_reclaim_sprays_per_step; ++spray)
                        match.spray_nano_feature(s, cell_x, cell_z, *feature, true);
                return 2;
            }
            return 1;
        case 4:
            (void)finish_feature_reclaim(record.extra.destination);
            return 5;
        default:
            return 7;
        }
    }

    /// Runs one step of VTOL_RepairPatrol: an aircraft patrol that repairs,
    /// assists and reclaims on the way, and lands on a repair pad below
    /// three quarters health.
    ///
    /// @param events Events raised on the order since its last step.
    /// @return 1 (next phase), 2 (keep waiting), 0 (restart: an abort event,
    ///     or a landing queued), 6 (rotate: arrived, or an assist queued),
    ///     3 (retry later: an assist not queued, or a help-build or reclaim
    ///     queued) or 7 (invalid).
    uint32_t repair_patrol(uint32_t events) {
        if (events & patrol_abort_events) {
            wait(reclaim_retry_ticks);
            return 0;
        }
        if (order.phase == 0) {
            if (!flight_ready() || (def().abilities & OA_UNIT_DEF_ABILITY_CAN_REPAIR) == 0)
                return 7;
            if (auto* followed = target())
                record.extra.destination = point_of(followed->record.position);
            clone_patrol_leg();
            acknowledge("Patrolling");
            host.take_off(s, order);
            return 1;
        }
        if (order.phase != 1)
            return 7;
        if (events & goal_events)
            return 6;
        fly_at_cruise(record.extra.destination);
        wait(patrol_step_ticks);
        order.wait_events |= goal_events;
        const auto& d = def();
        if (static_cast<uint32_t>(static_cast<int32_t>(unit.health)) < (d.max_damage >> 2) * 3 &&
            !never_retreats_to_repair(host.match.rules(), d.abilities) && land_at_repair_pad())
            return 0;
        auto* owner = oa::world_unit_owner(&world(), &unit);
        if (!owner)
            return 2;
        // orders.con-patrol-guard-options: reclaim only skips the repair and
        // assist search, assist only stops before the reclaim search.
        const auto choice = ground::patrol_choice(host.match.rules(), unit);
        if (choice != ground::patrol_reclaim_only &&
            static_cast<double>(owner->energy_storage) * reserve_fraction <=
                static_cast<double>(owner->energy)) {
            const auto candidates = repair_candidates(
                static_cast<oa_fixed>(static_cast<int32_t>(d.sight_distance) << 16)
            );
            if (!candidates.empty()) {
                auto* pick = candidates[host.random(static_cast<uint32_t>(candidates.size()))];
                if (can_assist(pick->record) && finished(pick->record))
                    return issue_assist(*pick) ? 6 : 3;
                if (can_assist(pick->record) && !finished(pick->record)) {
                    drop_goal();
                    (void)push_order(vtol_help_build_kind, pick, nullptr);
                    order.wait_events = 0;
                    return 3;
                }
            }
        }
        if (choice == ground::patrol_assist_only)
            return 2;
        FeatureChoice energy, metal;
        if (!select_reclaim_features(unit.position, feature_scan_radius, energy, metal))
            return 2;
        const auto low = [](float stored, float capacity) {
            return static_cast<double>(capacity) * reserve_fraction > static_cast<double>(stored);
        };
        const auto fits = [](float amount, float stored, float capacity) {
            return !(
                static_cast<double>(amount) + static_cast<double>(stored) >
                static_cast<double>(capacity)
            );
        };
        const FeatureChoice* chosen = nullptr;
        if (low(owner->metal, owner->metal_storage) && metal.found)
            chosen = &metal;
        else if (low(owner->energy, owner->energy_storage) && energy.found)
            chosen = &energy;
        else if (metal.found && fits(metal.amount, owner->metal, owner->metal_storage))
            chosen = &metal;
        else if (energy.found && fits(energy.amount, owner->energy, owner->energy_storage))
            chosen = &energy;
        if (!chosen)
            return 2;
        drop_goal();
        (void)push_order(vtol_reclaim_kind, nullptr, &chosen->position);
        order.wait_events = 0;
        return 3;
    }
};

bool TickHost::dispatch_vtol_build_mission(
    sim::unit_spawn::Slot& s, sim::simulation_state::Order& order, uint32_t events, uint32_t& result
) {
    switch (order.kind) {
    case vtol_mobile_build_kind:
        result = VtolBuildMissions(*this, s, order).mobile_build(events);
        return true;
    case vtol_help_build_kind:
        result = VtolBuildMissions(*this, s, order).help_build(events);
        return true;
    case vtol_repair_unit_kind:
        result = VtolBuildMissions(*this, s, order).repair_unit(events);
        return true;
    case vtol_reclaim_unit_kind:
        result = VtolBuildMissions(*this, s, order).reclaim_unit(events);
        return true;
    case vtol_reclaim_kind:
        result = VtolBuildMissions(*this, s, order).reclaim_feature(events);
        return true;
    case vtol_repair_patrol_kind:
        result = VtolBuildMissions(*this, s, order).repair_patrol(events);
        return true;
    default:
        return false;
    }
}

} // namespace oa::sim::match_runtime
