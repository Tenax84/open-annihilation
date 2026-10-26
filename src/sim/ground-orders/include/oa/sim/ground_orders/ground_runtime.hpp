// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/unit_movement/terrain.hpp"
#include "oa/sim/unit_spawn/spawn.hpp"
#include "oa/data/unit_definitions.hpp"

namespace oa::sim::ground_orders {
// Values a new movement object starts from: construction keeps the flag bits
// it does not set, and the last-motion tick. Zero for a new object, or the
// values a caller carries over.
struct ConstructorStorage {
    uint8_t movement_flags{}, navigation_flags{};
    uint32_t last_motion_tick{};
};

// Player.status of a player another machine simulates.
inline constexpr uint8_t mirrored_player_status = 3;

// Size of the saved movement block ("u%04xmob").
inline constexpr std::size_t mobility_record_size = 0x23;

// The movement object behind Unit.movement for a ground unit: movement
// state, and a local navigator for units simulated here or a mirrored
// navigator for units another player's machine simulates.
class GroundRuntime {
  public:

    /// Constructs the movement object of a ground unit.
    ///
    /// Zeroes velocity, previous vector, speed and turn, copies the movement-class
    /// handle, sets occupancy flag 1 and the navigator's changed flag, and chooses
    /// the mirrored navigator when the owner has mirrored_player_status (another
    /// player's machine simulates it). Speed, turn rate, acceleration and brake come
    /// from the definition. A slot with no bound unit, or one without a type,
    /// gives a runtime left at its defaults, which projects and writes nothing.
    ///
    /// @param slot spawn slot holding the bound unit and its record; must outlive the runtime
    /// @param definition the unit's type definition
    /// @param movement_class movement-class handle (UnitDef.move_class)
    /// @param storage flag bits and last motion tick that construction keeps
    GroundRuntime(
        sim::unit_spawn::Slot& slot,
        const data::unit_definitions::UnitDefinition& definition,
        sim::unit_spawn::AssetHandle movement_class = 0,
        ConstructorStorage storage = {}
    );

    /// Constructs a movement object bound to no slot, every field at its default.
    ///
    /// It projects, writes and fits nothing; a match holds one to stand in
    /// for a unit's missing movement object.
    GroundRuntime() noexcept : movement_class(0), acceleration(0), deceleration(0) {}

    GroundRuntime(const GroundRuntime&) = delete;
    GroundRuntime& operator=(const GroundRuntime&) = delete;
    /// Copies the slot's unit position, cell, footprint, angles, flags and type flags into `geometry`.
    ///
    /// A slot with no bound unit, or one without a type, copies nothing.
    void project_slot();
    /// Writes `geometry`'s position, flags, angles and cell back to the slot.
    ///
    /// A slot with no bound unit takes nothing.
    void write_slot();
    /// Fits the unit to the ground under its model's selection quad and writes the pose back.
    ///
    /// @param terrain terrain view
    /// @param model the unit's model
    /// @param clock simulation tick and bob readings
    /// @return false, leaving the slot unchanged, when the pose cannot be fitted
    ///         or the runtime is bound to no typed unit
    bool fit_height(
        const sim::unit_movement::Terrain& terrain,
        const formats::objects3d::Model& model,
        const sim::unit_movement::GroundClock& clock
    );
    /// Runs the navigator tick then the ground driver for this movement object.
    ///
    /// A mirrored driver only steers along its shared route head.
    ///
    /// @param unit the unit's simulation state
    /// @param tick current game tick
    /// @param sea_level map sea level in whole world units
    /// @param host order host for the navigator tick
    /// @param fast_units how a unit faster than any stock unit is steered
    void drive(
        sim::simulation_state::Unit& unit,
        uint32_t tick,
        uint8_t sea_level,
        Host& host,
        FastUnitSteering fast_units
    );
    /// Saves the movement block ("u%04xmob").
    ///
    /// @return velocity, filtered vector, speed, turn, occupancy tick and the layer and blocked flag bits, little-endian
    [[nodiscard]] std::array<uint8_t, mobility_record_size> save_mobility() const noexcept;
    /// Restores a saved movement block.
    ///
    /// Only the layer and blocked bits of the flags are restored.
    ///
    /// @param record block save_mobility wrote
    void load_mobility(const std::array<uint8_t, mobility_record_size>& record) noexcept;

    sim::unit_movement::Unit geometry;
    sim::unit_movement::Movement movement;
    Navigation navigation;                  // local navigator (unused when mirrored_driver)
    MirroredNavigation mirrored_navigation; // used when mirrored_driver
    bool mirrored_driver{};
    const sim::unit_spawn::AssetHandle movement_class; // copied from UnitDef.move_class
    std::array<Fixed, 3> previous_vector{};            // filtered attitude vector
    // Tick of the last occupancy change; compared with the movement map's
    // projection tick.
    uint32_t occupancy_changed_tick{};
    const Fixed acceleration, deceleration;

  private:

    sim::unit_spawn::Slot* slot_{}; // null for a runtime bound to no slot
};
} // namespace oa::sim::ground_orders
