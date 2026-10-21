// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "tick_internal.hpp"

#include "oa/sim/ai.hpp"
#include "oa/sim/detection.hpp"

namespace oa::sim::match_runtime {
namespace {

int32_t sub_fixed(int32_t a, int32_t b) {
    return std::bit_cast<int32_t>(std::bit_cast<uint32_t>(a) - std::bit_cast<uint32_t>(b));
}

int32_t add_fixed(int32_t a, int32_t b) {
    return std::bit_cast<int32_t>(std::bit_cast<uint32_t>(a) + std::bit_cast<uint32_t>(b));
}

int32_t squared_high(int32_t delta) {
    return static_cast<int32_t>((static_cast<int64_t>(delta) * delta) >> 32);
}

int32_t whole_to_fixed(int32_t whole) {
    return static_cast<int32_t>(static_cast<uint32_t>(whole) << 16);
}

} // namespace

uint16_t Match::participant_count() const noexcept {
    const auto recorded = simulation_.record.game.player_count;
    if (recorded != 0)
        return recorded;
    uint16_t live = 0;
    for (uint8_t player = 0; player < simulation_.players.size(); ++player)
        if (sim::simulation_state::player_slot_active(player, simulation_.players[player]))
            ++live;
    return live;
}

void Match::for_each_unit_in_radius(
    const sim::ground_orders::Point& centre,
    int32_t radius,
    void (*visit)(void* context, sim::unit_spawn::Slot& slot),
    void* context
) {
    prepare_spatial_state();
    walk_units_in_radius(centre, radius, visit, context);
}

void Match::walk_units_in_radius(
    const sim::ground_orders::Point& centre,
    int32_t radius,
    void (*visit)(void* context, sim::unit_spawn::Slot& slot),
    void* context
) {
    const auto clamp = [](int32_t value, uint32_t count) -> uint32_t {
        if (static_cast<uint32_t>(value) < count)
            return static_cast<uint32_t>(value);
        return value < 0 ? 0 : count - 1;
    };
    constexpr int bucket_shift = 23;
    const auto width = spatial_.bucket_width;
    const auto height = spatial_.bucket_height;
    if (width == 0 || height == 0)
        return;
    const auto left = clamp(sub_fixed(centre[0], radius) >> bucket_shift, width);
    const auto top = clamp(sub_fixed(centre[2], radius) >> bucket_shift, height);
    const auto right = clamp(add_fixed(centre[0], radius) >> bucket_shift, width);
    const auto bottom = clamp(add_fixed(centre[2], radius) >> bucket_shift, height);
    const auto reach = squared_high(radius);
    for (auto z = top; z <= bottom; ++z)
        for (auto x = left; x <= right; ++x) {
            const auto bucket = static_cast<std::size_t>(z) * width + x;
            if (bucket >= spatial_.buckets.size())
                continue;
            auto steps = slots_.size();
            for (auto id = spatial_.buckets[bucket].head; id != 0 && steps-- > 0;
                 id = spatial_units_[id].next_in_bucket) {
                if (id >= slots_.size())
                    break;
                auto& slot = slots_[id];
                const auto dx = sub_fixed(slot.record.position.x, centre[0]);
                const auto dz = sub_fixed(slot.record.position.z, centre[2]);
                if (add_fixed(squared_high(dz), squared_high(dx)) <= reach)
                    visit(context, slot);
            }
        }
}

void Match::scan_contacts() {
    namespace detection = sim::detection;
    auto& world = state();
    const auto viewpoint = world.game.viewpoint_player;
    if (participant_count() <= 1 || viewpoint >= OA_PLAYER_COUNT)
        return;
    auto& viewer = world.game.players[viewpoint];
    const auto* viewer_info = oa::world_player_info(&world, &viewer);
    // A slotless replay viewer looks through a recorded player's slot and
    // sees every unit as a watcher does while its full radar holds.
    const bool watching = (viewer.in_use != 0 && viewer_info != nullptr &&
                           (viewer_info->options & OA_SETUP_OPTION_WATCHER) != 0) ||
                          slotless_full_radar();
    const uint32_t count = world.unit_slot_count;
    const bool allied_vision = rules().intel.allied_los_sharing.enabled;

    for (uint32_t slot = 1; slot < count; ++slot) {
        auto& unit = world.units[slot];
        if ((unit.flags & OA_UNIT_FLAG_LIVE) == 0)
            continue;
        unit.flags &= ~OA_UNIT_FLAG_CLOAK_LOCKED;
        if (allied_vision) {
            // Every unit whose owner allies the viewer, the viewer's own
            // included, is a contact; no shared-radar role is needed.
            if (!owner_allies(unit, viewpoint) && !watching) {
                unit.flags &= ~detection::contact_bits;
                continue;
            }
        } else if (unit.owner_index != viewpoint) {
            const auto* owner = oa::world_unit_owner(&world, &unit);
            if (!(owner != nullptr && detection::shares_radar(world, viewer, *owner)) &&
                !watching) {
                unit.flags &= ~detection::contact_bits;
                continue;
            }
        }
        unit.flags |= detection::radar_contact | detection::sonar_contact;
    }

    uint32_t owned = 0;
    (void)oa::world_player_units(&world, &viewer, &owned);
    if (!allied_vision || owned == 0) {
        scan_contacts_for(viewer, owned != 0);
        return;
    }
    // Allied vision runs the passes once for each player whose row allies
    // the viewer, in player order, the viewer among them, each as the
    // viewing player: their scanners, jamming as they see it and their line
    // of sight all add to the one radar picture. Jamming in a later pass
    // clears contacts an earlier one made.
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        const auto& ally = world.game.players[player];
        if (ally.alliance[viewpoint] == 0)
            continue;
        uint32_t ally_owned = 0;
        (void)oa::world_player_units(&world, &ally, &ally_owned);
        scan_contacts_for(ally, ally_owned != 0);
    }
}

void Match::scan_contacts_for(const oa::Player& viewer, bool scan_own_units) {
    namespace detection = sim::detection;
    auto& world = state();
    const uint32_t count = world.unit_slot_count;

    // Radar reaches twice the scanner's integer altitude further; the walk
    // covers the larger of the plain radar and sonar ranges.
    struct Stamp {
        detection::ScanRecord scan;
        oa::World* world;
    };

    // The scan's walks follow only the bucket links of the projections, and
    // its visitors change only the flags of the units they visit, which no
    // bucket link comes from. So the first walk refreshes every projection
    // and each later one only those of the units the walk before it visited:
    // every walk finds the units it would after refreshing every projection,
    // and the projections end as that leaves them. After a walk that visits
    // more units than the list holds, the next walk refreshes every
    // projection.
    constexpr std::size_t listed_visit_capacity = 1024;

    struct Walk {
        Match* match{};
        void (*visit)(void* context, sim::unit_spawn::Slot& slot){};
        void* context{};
        std::array<uint32_t, listed_visit_capacity> visited{};
        std::size_t visited_count{};
        bool projected{};
        bool visits_fit{true};
    } walk;

    walk.match = this;
    const auto walk_radius = [&](const sim::ground_orders::Point& centre,
                                 int32_t radius,
                                 void (*visit)(void* context, sim::unit_spawn::Slot& slot),
                                 void* context) {
        if (!walk.projected || !walk.visits_fit)
            prepare_spatial_state();
        else
            for (std::size_t i = 0; i < walk.visited_count; ++i)
                fill_spatial_unit(this, spatial_units_[walk.visited[i]]);
        walk.projected = true;
        walk.visits_fit = true;
        walk.visited_count = 0;
        walk.visit = visit;
        walk.context = context;
        walk_units_in_radius(
            centre,
            radius,
            [](void* forward, sim::unit_spawn::Slot& slot) {
                auto& w = *static_cast<Walk*>(forward);
                if (w.visited_count < w.visited.size())
                    w.visited[w.visited_count++] =
                        static_cast<uint32_t>(&slot - w.match->slots_.data());
                else
                    w.visits_fit = false;
                w.visit(w.context, slot);
            },
            &walk
        );
    };

    uint32_t owned = 0;
    auto* units = scan_own_units ? oa::world_player_units(&world, &viewer, &owned) : nullptr;
    if (units != nullptr)
        for (uint32_t i = 0; i < owned; ++i) {
            const auto& scanner = units[i];
            if (!unit_is_live_target(scanner.flags) ||
                (scanner.state_flags & OA_UNIT_STATE_ACTIVE) == 0)
                continue;
            const auto* def = oa::world_unit_def_of(&world, &scanner);
            if (def == nullptr || (def->radar_distance == 0 && def->sonar_distance == 0))
                continue;
            const int16_t radar = def->radar_distance;
            const int16_t sonar = def->sonar_distance;
            const auto altitude = static_cast<int32_t>(
                static_cast<int16_t>(static_cast<uint32_t>(scanner.position.y) >> 16)
            );
            const auto lifted = static_cast<uint32_t>(radar + altitude * 2);
            Stamp stamp{{}, &world};
            stamp.scan.radar_range_squared = static_cast<int32_t>(lifted * lifted);
            stamp.scan.sonar_range_squared = static_cast<int32_t>(sonar) * sonar;
            stamp.scan.position = scanner.position;
            const int16_t reach = radar <= sonar ? sonar : radar;
            walk_radius(
                {scanner.position.x, scanner.position.y, scanner.position.z},
                whole_to_fixed(reach),
                [](void* context, sim::unit_spawn::Slot& slot) {
                    const auto& s = *static_cast<const Stamp*>(context);
                    detection::stamp_contact(s.scan, slot.record, *s.world);
                },
                &stamp
            );
        }

    for (uint32_t slot = 1; slot < count; ++slot) {
        const auto& jammer = world.units[slot];
        if ((jammer.flags & OA_UNIT_FLAG_LIVE) == 0 || jammer.owner_index == viewer.index ||
            (jammer.state_flags & OA_UNIT_STATE_ACTIVE) == 0 || !jammer_jams(viewer, jammer))
            continue;
        const auto* def = oa::world_unit_def_of(&world, &jammer);
        if (def == nullptr)
            continue;
        const sim::ground_orders::Point centre{
            jammer.position.x, jammer.position.y, jammer.position.z
        };
        if (def->radar_distance_jam != 0)
            walk_radius(
                centre,
                whole_to_fixed(def->radar_distance_jam),
                [](void*, sim::unit_spawn::Slot& slot) { detection::jam_radar(slot.record); },
                nullptr
            );
        if (def->sonar_distance_jam != 0)
            walk_radius(
                centre,
                whole_to_fixed(def->sonar_distance_jam),
                [](void*, sim::unit_spawn::Slot& slot) { detection::jam_sonar(slot.record); },
                nullptr
            );
    }

    // Every locally simulated cloaker, cloaked or not, whose owner sees an
    // enemy inside its mincloakdistance.
    for (uint32_t slot = 1; slot < count; ++slot) {
        auto& unit = world.units[slot];
        if ((unit.flags & OA_UNIT_FLAG_LIVE) == 0 ||
            !sim::simulation_state::locally_simulated(world, unit))
            continue;
        const auto* def = oa::world_unit_def_of(&world, &unit);
        if (def == nullptr || (def->abilities & OA_UNIT_DEF_ABILITY_CAN_CLOAK) == 0 ||
            unit.owner_index >= sightings_.size())
            continue;
        if (detection::sighted_within(
                sightings_[unit.owner_index], world, unit.position, def->min_cloak_distance
            ))
            detection::expose_cloaker(unit, world.game.tick);
    }

    for (uint32_t slot = 1; slot < count; ++slot) {
        auto& unit = world.units[slot];
        if ((unit.flags & OA_UNIT_FLAG_LIVE) != 0 && (unit.flags & detection::radar_contact) == 0 &&
            (unit.state_flags & OA_UNIT_STATE_CLOAKED) == 0 &&
            point_seen_by(viewer.index, fixed_words(unit.position)))
            unit.flags |= detection::radar_contact;
    }
}

void Match::refresh_player_knowledge(uint8_t player) {
    namespace detection = sim::detection;
    auto& world = state();
    const auto& viewer = world.game.players[player];
    // The knowledge record is built together with the controller.
    if (!sim::ai::player_has_controller(viewer))
        return;
    auto& sightings = sightings_.at(player);
    const auto tick = world.game.tick;
    if (!detection::sightings_due(sightings, tick))
        return;
    auto* computers = sim::ai::match_computer_players(*this);
    auto* knowledge = sim::ai::computer_player_knowledge(computers, player);
    sim::ai::KnowledgeTally tally;
    detection::clear_sightings(sightings);
    if (knowledge != nullptr)
        sim::ai::computer_knowledge_clear(computers, *knowledge);
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
        const auto& unit = world.units[slot];
        const bool seen = detection::sighting_candidate(world, viewer, unit) &&
                          unit_visible(player, static_cast<uint16_t>(slot));
        if (detection::file_sighting(sightings, world, viewer, unit, seen) && knowledge != nullptr)
            sim::ai::computer_knowledge_count(computers, *knowledge, unit, tally);
    }
    if (knowledge != nullptr)
        sim::ai::computer_knowledge_settle(*knowledge, tally);
    sightings.refreshed_tick = tick;
    if (random_.bounded(detection::sighting_period) == 0)
        refresh_strategic_state(player);
}

} // namespace oa::sim::match_runtime
