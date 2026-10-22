// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <string_view>

namespace oa::ui::frontend_state {
// The values are the game's state and signal bytes. States are named by what
// they show, play or do; a signal is named by what the states that take it do
// with it, and the same value may do different things in different states.
namespace state_id {
inline constexpr uint8_t intro_gate = 0;
inline constexpr uint8_t intro_followup = 1;
inline constexpr uint8_t main_menu = 2;
inline constexpr uint8_t movie_5 = 3;
inline constexpr uint8_t movies_3_then_5 = 4;
inline constexpr uint8_t movies_4_then_5 = 5;
inline constexpr uint8_t inert = 6; // no state leads to it, and the dispatcher does nothing in it
inline constexpr uint8_t pump_only = 7; // pops input and presents frames, nothing more
inline constexpr uint8_t single_player_menu = 8;
inline constexpr uint8_t new_game_menu = 9;
inline constexpr uint8_t skirmish_menu = 10; // SKIRMISH.GUI
inline constexpr uint8_t options = 11;
// Mission briefing (MSNBRIEF.GUI) states; they differ in where BACK returns.
inline constexpr uint8_t briefing_from_campaign = 12;
inline constexpr uint8_t briefing_from_any_mission = 13;
inline constexpr uint8_t briefing_to_end_mission = 14;
inline constexpr uint8_t briefing_to_single_player = 15;
// States from 16 up are not the engine's: without an extension that runs them
// (StateHandler) they do nothing.
} // namespace state_id

namespace signal_id {
inline constexpr uint8_t initialize = 0;
inline constexpr uint8_t update = 1;
// The screen's go-ahead: Start in the skirmish and briefing menus, which then
// set flags::loading.
inline constexpr uint8_t proceed = 2;
inline constexpr uint8_t back = 3;
inline constexpr uint8_t single_player = 5;
inline constexpr uint8_t multiplayer = 6;
inline constexpr uint8_t restart_intro = 7;
inline constexpr uint8_t exit_application = 8;
inline constexpr uint8_t movie_5 = 9;
inline constexpr uint8_t new_campaign = 10;  // SINGLE.GUI NewCamp
inline constexpr uint8_t skirmish_menu = 11; // SINGLE.GUI Skirmish
inline constexpr uint8_t options = 13;
inline constexpr uint8_t any_mission = 14;            // SINGLE.GUI AnyMsn
inline constexpr uint8_t start_campaign_mission = 15; // NEWGAME.GUI, campaign mode
inline constexpr uint8_t start_any_mission = 16;      // NEWGAME.GUI, any-mission mode
} // namespace signal_id

namespace mode_id {
inline constexpr int32_t loading_return = 1; // handler resets to the frontend
inline constexpr int32_t frontend = 2;       // handler ticks this dispatcher
inline constexpr int32_t in_match = 6;       // handler runs the match
inline constexpr int32_t end_game = 7;       // handler ticks the end-game screen
} // namespace mode_id

namespace limits {
inline constexpr std::size_t player_count = 10;
}

namespace flags {
// Meanings below are limited to the tests and updates this dispatcher makes.
inline constexpr uint16_t live_game = 0x0001;     // Game.session_flags
inline constexpr uint16_t loading = 0x0004;       // Game.session_flags
inline constexpr uint16_t single_player = 0x0008; // Game.session_flags
// Bit of the display's mode flags: the display is full-screen. The intro
// plays, and the main menu's INTRO and Credits start their movies, only then.
inline constexpr uint8_t fullscreen_mode = 0x02;
inline constexpr uint8_t intro_enabled = fullscreen_mode; // dispatcher movie gate
} // namespace flags

struct Descriptor {    // PlayerSetupInfo, Player.info
    uint16_t role{};   // PlayerSetupInfo.role and the byte after it
    uint8_t options{}; // low byte of PlayerSetupInfo.options
};

struct PlayerSlot {          // Game.players: ten records of 0x14b bytes
    bool present{};          // Player.in_use
    uint32_t player_id{};    // Player.player_id
    uint8_t machine_flags{}; // ? Player.machine_flags; bit 1 set from the setup options
    uint8_t status{};        // Player.status, OA_PLAYER_STATUS_*
    std::optional<Descriptor> descriptor; // Player.info; required on dereferenced paths
};

inline constexpr std::size_t start_pattern_bytes = 25;
inline constexpr uint8_t start_pattern_value = 'U'; // unplayed; ENDMSN marks W, L or U per mission

struct State {
    uint8_t state{};                    // Game.frontend_state
    uint8_t signal{}, pending_signal{}; // Game.frontend_signal, Game.frontend_pending_signal
    uint16_t session_flags{};
    uint8_t local_player_index{};
    uint16_t player_count{};
    std::array<uint8_t, start_pattern_bytes + 1> mission_results{};
    int32_t play_intro_movie{}; // [PlayMovie] preference
    int32_t skip_intro{};       // command-line switch; suppresses the intro
    uint32_t movie_skip{}; // Game.movie_skip; INTRO sets it to 1 when Shift is held, 0 otherwise
    // Set by the main menu's INTRO: the logo movie plays before the intro,
    // as the game no longer plays either when it starts.
    bool logo_before_intro{};
    std::array<PlayerSlot, limits::player_count> players{};
    // The display's mode flags (flags::fullscreen_mode); the intro reads them
    // after Step::get_video_context and the cursor call.
    uint8_t video_context_flags{};
    uint16_t outcome_flags{}; // bits 2 and 4 cleared on entering the frontend
};
// Steps that take no scalar arguments; the host keeps the object each one
// acts on. The numeric values only identify the steps; each enumerator's
// value is distinct across Step and Query. Steps are labelled from 0x100 and
// queries from 0x200: an extension that runs states of its own labels its
// further steps and queries with the values left unnamed here.
enum class Step : uint32_t {
    check_state_checksum = 0x100,
    present_frame = 0x101,
    draw_current_frame = 0x102,
    setup_main_menu = 0x104,
    save_preferences = 0x105,
    get_video_context = 0x106,
    pop_input_event = 0x107,
    shut_down_resource = 0x108,
    return_to_main_menu = 0x10a,
    setup_single_player = 0x10c,
    reset_player_slots = 0x10d,
    load_preferences = 0x10e,
    open_options = 0x10f,
    setup_skirmish = 0x110,
    setup_mission_briefing = 0x111,
    get_cursor_context = 0x112,
    enter_end_mission = 0x113,
    draw_cursor = 0x114,
    reload_unit_overrides = 0x128,
    draw_panel_gadgets = 0x129,
    load_default_palette = 0x12a, // null name: the default palette
    reset_key_queue = 0x12b,      // empties the key queue pop_input_event reads
    enable_panel_keyboard = 0x12c // lets the frontend panel's gadgets take keyboard input
};
enum class Query : uint32_t {
    map_list_object_state = 0x206 // first word of the object Game.game_options names
};

// The routines the dispatcher calls. Every callback may change the state.
class Host {
  public:

    virtual ~Host() = default;

    /// Runs a routine that takes no scalar arguments.
    ///
    /// @param step Routine to run.
    /// @param[in,out] state Dispatcher state.
    virtual void step(Step step, State& state) = 0;

    /// Runs a routine and returns its result.
    ///
    /// @param query Routine to run.
    /// @param[in,out] state Dispatcher state.
    /// @return The routine's result.
    virtual uint32_t query(Query query, State& state) = 0;

    /// Plays a movie to its end or until skipped.
    ///
    /// @param[in,out] state Dispatcher state.
    /// @param filename Movie resource (1.zrb .. 5.zrb).
    virtual void play_movie(State& state, std::string_view filename) = 0;

    /// Shows or hides the pointer cursor.
    ///
    /// @param[in,out] state Dispatcher state.
    /// @param value 1 shows, 0 hides.
    virtual void set_cursor_visible(State& state, int32_t value) = 0;

    /// Selects the map list a menu offers.
    ///
    /// @param[in,out] state Dispatcher state.
    /// @param value 0 main menu, 1 campaign, 2 skirmish, 3 multiplayer.
    virtual void select_map_list(State& state, int32_t value) = 0;

    /// Opens the NEWGAME.GUI panel.
    ///
    /// @param[in,out] state Dispatcher state.
    /// @param value 0 for a new campaign, 1 for any mission.
    virtual void open_new_game_panel(State& state, int32_t value) = 0;

    /// Sets the application mode.
    ///
    /// @param[in,out] state Dispatcher state.
    /// @param mode A mode_id value.
    virtual void set_app_mode(State& state, int32_t mode) = 0;

    /// Starts a cursor animation.
    ///
    /// @param[in,out] state Dispatcher state.
    /// @param index Cursor index.
    virtual void set_cursor(State& state, int32_t index) = 0;

    /// Sets Game.endgame_state.
    ///
    /// @param[in,out] state Dispatcher state.
    /// @param step End-game step to enter.
    virtual void set_endgame_state(State& state, int32_t step) = 0;

    /// Shuts the application down.
    ///
    /// @param[in,out] state Dispatcher state.
    virtual void shut_down(State& state) = 0;
};

// States an extension runs in place of the engine's handlers. Every entry may
// be null, which leaves each state to the engine.
struct StateHandler {
    void* context{};
    // Runs the current state for the current signal after the pending signal
    // is applied; false leaves them to the engine.
    bool (*run)(void* context, State& state, Host& host){};
    // The application mode tick_frontend_mode hands over to from the current
    // state when the engine's states name none; 0 for none.
    int32_t (*handover_mode)(void* context, const State& state){};
};

/// Steps the frontend state machine once (intro and menus).
///
/// Applies a pending signal first, then offers the current state and signal
/// to the extension's handler and, when it does not take them, runs the
/// engine's handler for them. Host callbacks may change the state; each value
/// is read after the callbacks that come before it.
///
/// @param[in,out] state Dispatcher state.
/// @param[in,out] host Routines the dispatcher calls.
/// @param extension States an extension runs in place of the engine's; the
///        default leaves every state to the engine.
void dispatch(State& state, Host& host, const StateHandler& extension = {});
} // namespace oa::ui::frontend_state
