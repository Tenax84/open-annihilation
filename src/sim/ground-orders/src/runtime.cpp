// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/ground_orders/ground_runtime.hpp"
#include "oa/base/bytes.hpp"
#include <bit>

namespace oa::sim::ground_orders {
namespace {
using base::bytes::load_le32;

int16_t signed_half(uint32_t value) {
    return std::bit_cast<int16_t>(static_cast<uint16_t>(value));
}
} // namespace

GroundRuntime::GroundRuntime(
    sim::unit_spawn::Slot& slot,
    const data::unit_definitions::UnitDefinition& definition,
    sim::unit_spawn::AssetHandle class_handle,
    ConstructorStorage storage
)
    : movement_class(class_handle), acceleration(definition.acceleration_fixed),
      deceleration(definition.brake_rate_fixed), slot_(&slot) {
    if (!slot.unit || !slot.unit->type)
        return;
    // Aircraft use this ground object until their own driver is wired in;
    // they still spawn and accept Move_Ground.
    // On the ground (occupancy 1) and not blocked.
    movement.flags = uint8_t(
        (storage.movement_flags &
         ~(sim::unit_movement::occupancy_mask | sim::unit_movement::collision_blocked)) |
        sim::unit_spawn::ground_occupancy_state
    );
    movement.last_motion_tick = storage.last_motion_tick;
    mirrored_driver = slot.unit->owner && slot.unit->owner->present &&
                      slot.unit->owner->status == mirrored_player_status;
    if (!mirrored_driver)
        navigation.flags = uint8_t(
            (storage.navigation_flags & ~uint32_t{route_present_flag | search_pending_flag}) |
            route_changed_flag
        );
    geometry.type.maximum_speed = definition.max_velocity_fixed;
    geometry.type.maximum_turn = static_cast<uint16_t>(definition.turn_rate);
    project_slot();
}

void GroundRuntime::project_slot() {
    if (!slot_ || !slot_->unit || !slot_->unit->type)
        return;
    const auto& u = *slot_->unit;
    const std::array<uint32_t, 3> position = u.position;
    for (std::size_t i = 0; i < 3; ++i)
        geometry.position[i] = std::bit_cast<Fixed>(position[i]);
    geometry.cell = {slot_->record.cell_x, slot_->record.cell_z};
    geometry.footprint = {slot_->record.footprint_x, slot_->record.footprint_z};
    geometry.heading = slot_->record.heading;
    geometry.pitch = slot_->record.pitch;
    geometry.flags = u.flags;
    geometry.id = signed_half(slot_->unit_index);
    geometry.type.flags = u.type->flags;
}

void GroundRuntime::write_slot() {
    if (!slot_ || !slot_->unit)
        return;
    auto& u = *slot_->unit;
    for (std::size_t i = 0; i < 3; ++i)
        u.position[i] = static_cast<uint32_t>(geometry.position[i]);
    u.flags = geometry.flags;
    slot_->record.heading = geometry.heading;
    slot_->record.pitch = geometry.pitch;
    slot_->record.cell_x = geometry.cell[0];
    slot_->record.cell_z = geometry.cell[1];
}

bool GroundRuntime::fit_height(
    const sim::unit_movement::Terrain& terrain,
    const formats::objects3d::Model& model,
    const sim::unit_movement::GroundClock& clock
) {
    if (!slot_ || !slot_->unit || !slot_->unit->type)
        return false;
    project_slot();
    sim::unit_movement::GroundPose pose;
    pose.position = geometry.position;
    pose.heading = geometry.heading;
    pose.pitch = geometry.pitch;
    pose.roll = slot_->record.bank;
    pose.bob_phase = slot_->record.bob_phase;
    pose.flags = geometry.flags;
    pose.type_flags = geometry.type.flags;
    pose.maximum_speed = geometry.type.maximum_speed;
    const bool fitted = sim::unit_movement::fit_ground(
        terrain, sim::unit_movement::ground_quad(model), pose, movement, clock
    );
    if (fitted) {
        geometry.position = pose.position;
        geometry.pitch = pose.pitch;
        slot_->record.bank = pose.roll;
        write_slot();
    }
    return fitted;
}

void GroundRuntime::drive(
    sim::simulation_state::Unit& unit,
    uint32_t tick,
    uint8_t sea_level,
    Host& host,
    FastUnitSteering fast_units
) {
    if (mirrored_driver) {
        steer_ground(
            geometry,
            movement,
            mirrored_navigation,
            acceleration,
            deceleration,
            sea_level,
            fast_units
        );
        return;
    }
    std::array<uint8_t, 3> unused_weapon_flags{};
    tick_navigation({unit, geometry, movement, navigation, false, unused_weapon_flags}, tick, host);
    steer_ground(geometry, movement, navigation, acceleration, deceleration, sea_level, fast_units);
}

namespace {
void put32(uint8_t* out, uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i)
        out[i] = static_cast<uint8_t>(v >> (8 * i));
}

constexpr uint8_t saved_flag_mask =
    sim::unit_movement::occupancy_mask | sim::unit_movement::collision_blocked;
} // namespace

std::array<uint8_t, mobility_record_size> GroundRuntime::save_mobility() const noexcept {
    std::array<uint8_t, mobility_record_size> out{};
    for (std::size_t i = 0; i < 3; ++i) {
        put32(&out[i * 4], std::bit_cast<uint32_t>(movement.velocity[i]));
        put32(&out[12 + i * 4], std::bit_cast<uint32_t>(previous_vector[i]));
    }
    put32(&out[24], std::bit_cast<uint32_t>(movement.speed));
    const auto turn = std::bit_cast<uint16_t>(movement.turn);
    out[28] = static_cast<uint8_t>(turn);
    out[29] = static_cast<uint8_t>(turn >> 8);
    put32(&out[30], occupancy_changed_tick);
    out[34] = static_cast<uint8_t>(movement.flags & saved_flag_mask);
    return out;
}

void GroundRuntime::load_mobility(const std::array<uint8_t, mobility_record_size>& in) noexcept {
    for (std::size_t i = 0; i < 3; ++i) {
        movement.velocity[i] = std::bit_cast<Fixed>(load_le32(&in[i * 4]));
        previous_vector[i] = std::bit_cast<Fixed>(load_le32(&in[12 + i * 4]));
    }
    movement.speed = std::bit_cast<Fixed>(load_le32(&in[24]));
    movement.turn =
        std::bit_cast<int16_t>(static_cast<uint16_t>(in[28] | static_cast<uint16_t>(in[29]) << 8));
    occupancy_changed_tick = load_le32(&in[30]);
    movement.flags =
        static_cast<uint8_t>((movement.flags & ~saved_flag_mask) | (in[34] & saved_flag_mask));
}
} // namespace oa::sim::ground_orders
