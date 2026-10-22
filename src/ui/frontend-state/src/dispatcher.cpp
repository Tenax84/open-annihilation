// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/frontend_state/dispatcher.hpp"
#include "oa/ui/frontend_state/app_modes.hpp"

namespace oa::ui::frontend_state {
namespace {
namespace map_list {
constexpr int32_t main_menu = 0, campaign = 1, skirmish = 2;
}

namespace new_game_panel {
constexpr int32_t new_campaign = 0, any_mission = 1;
}

namespace argument {
constexpr int32_t disabled = 0, enabled = 1;
} // namespace argument

namespace movie {
constexpr std::string_view intro = "1.zrb", intro_followup = "2.zrb";
constexpr std::string_view ending_3 = "3.zrb", ending_4 = "4.zrb", ending_5 = "5.zrb";
} // namespace movie

/// Makes a pending signal current.
///
/// The pending byte is read before the checksum step, then written to both
/// signal bytes. Nothing happens when it equals the current signal.
///
/// @param[in,out] s Dispatcher state.
/// @param[in,out] h Host that runs the checksum step.
void apply_pending_signal(State& s, Host& h) {
    const auto pending = s.pending_signal;
    if (pending == s.signal)
        return;
    h.step(Step::check_state_checksum, s);
    s.signal = pending;
    s.pending_signal = pending;
}
} // namespace

void dispatch(State& s, Host& h, const StateHandler& extension) {
    const auto call = [&](Step f) { h.step(f, s); };
    const auto check = [&] { call(Step::check_state_checksum); };
    const auto signal = [&](uint8_t value) { set_frontend_signal(s, h, value); };
    const auto state = [&](uint8_t value) {
        check();
        s.state = value;
    };
    const auto transition = [&](uint8_t value) {
        state(value);
        signal(signal_id::initialize);
    };
    const auto pump = [&] { call(Step::present_frame); };
    apply_pending_signal(s, h);
    if (extension.run != nullptr && extension.run(extension.context, s, h))
        return;
    switch (s.state) {
    case state_id::intro_gate:
        call(Step::get_video_context);
        h.set_cursor_visible(s, argument::disabled);
        if ((s.video_context_flags & flags::intro_enabled) != 0) {
            if (s.play_intro_movie != 0) {
                h.play_movie(s, movie::intro);
                transition(state_id::intro_followup);
                s.play_intro_movie = 0;
                call(Step::save_preferences);
                return;
            }
            if (s.skip_intro == 0)
                h.play_movie(s, movie::intro);
        }
        transition(state_id::main_menu);
        return;
    case state_id::intro_followup:
    case state_id::movie_5: {
        const auto resource =
            s.state == state_id::intro_followup ? movie::intro_followup : movie::ending_5;
        call(Step::shut_down_resource);
        if (s.state == state_id::intro_followup && s.logo_before_intro) {
            s.logo_before_intro = false;
            h.play_movie(s, movie::intro);
        }
        h.play_movie(s, resource);
        transition(state_id::main_menu);
        return;
    }
    case state_id::main_menu:
        call(Step::pop_input_event);
        switch (s.signal) {
        case signal_id::initialize:
            h.select_map_list(s, map_list::main_menu);
            call(Step::setup_main_menu);
            call(Step::return_to_main_menu);
            h.set_cursor_visible(s, argument::enabled);
            return;
        case signal_id::update:
            pump();
            return;
        case signal_id::single_player:
            s.session_flags |= flags::single_player;
            transition(state_id::single_player_menu);
            call(Step::shut_down_resource);
            return;
        case signal_id::restart_intro:
            transition(state_id::intro_gate);
            return;
        case signal_id::exit_application:
            call(Step::draw_current_frame);
            call(Step::shut_down_resource);
            h.shut_down(s);
            return;
        case signal_id::movie_5:
            transition(state_id::movie_5);
            return;
        default:
            return;
        }
    case state_id::movies_3_then_5:
    case state_id::movies_4_then_5:
        if (s.signal == signal_id::initialize)
            signal(signal_id::update);
        else if (s.signal == signal_id::update) {
            h.play_movie(
                s, s.state == state_id::movies_3_then_5 ? movie::ending_3 : movie::ending_4
            );
            h.play_movie(s, movie::ending_5);
            s.session_flags &= static_cast<uint16_t>(~flags::loading);
            transition(state_id::main_menu);
            h.set_app_mode(s, mode_id::frontend);
        }
        return;
    case state_id::pump_only:
        call(Step::pop_input_event);
        if (s.signal == signal_id::update)
            pump();
        return;
    case state_id::single_player_menu:
        call(Step::pop_input_event);
        switch (s.signal) {
        case signal_id::initialize:
            call(Step::setup_single_player);
            call(Step::reset_player_slots);
            signal(signal_id::update);
            return;
        case signal_id::update:
            pump();
            return;
        case signal_id::back:
            transition(state_id::main_menu);
            return;
        case signal_id::new_campaign:
            h.open_new_game_panel(s, new_game_panel::any_mission);
            transition(state_id::new_game_menu);
            signal(signal_id::update);
            return;
        case signal_id::skirmish_menu:
            call(Step::load_preferences);
            h.select_map_list(s, map_list::skirmish);
            transition(state_id::skirmish_menu);
            return;
        case signal_id::options:
            transition(state_id::options);
            call(Step::open_options);
            signal(signal_id::update);
            return;
        case signal_id::any_mission:
            h.open_new_game_panel(s, new_game_panel::any_mission);
            transition(state_id::new_game_menu);
            signal(signal_id::update);
            return;
        default:
            return;
        }
    case state_id::new_game_menu:
        call(Step::pop_input_event);
        switch (s.signal) {
        case signal_id::update:
            pump();
            return;
        case signal_id::back:
            transition(state_id::single_player_menu);
            return;
        case signal_id::start_campaign_mission:
            h.select_map_list(s, map_list::campaign);
            transition(state_id::briefing_from_campaign);
            return;
        case signal_id::start_any_mission:
            h.select_map_list(s, map_list::campaign);
            transition(state_id::briefing_from_any_mission);
            return;
        default:
            return;
        }
    case state_id::skirmish_menu:
        call(Step::pop_input_event);
        switch (s.signal) {
        case signal_id::initialize:
            call(Step::setup_skirmish);
            signal(signal_id::update);
            return;
        case signal_id::update:
            pump();
            return;
        case signal_id::proceed:
            s.session_flags |= flags::loading;
            return;
        case signal_id::back:
            transition(state_id::single_player_menu);
            return;
        default:
            return;
        }
    case state_id::options:
        if (s.signal == signal_id::update)
            pump();
        else if (s.signal == signal_id::back)
            transition(state_id::single_player_menu);
        return;
    case state_id::briefing_from_campaign:
    case state_id::briefing_from_any_mission:
    case state_id::briefing_to_end_mission:
    case state_id::briefing_to_single_player:
        call(Step::pop_input_event);
        switch (s.signal) {
        case signal_id::initialize:
            call(Step::setup_mission_briefing);
            signal(signal_id::update);
            return;
        case signal_id::update:
            pump();
            return;
        case signal_id::proceed:
            s.session_flags |= flags::loading;
            return;
        case signal_id::back:
            switch (s.state) { // read after the callbacks, which may change it
            case state_id::briefing_from_campaign:
                h.open_new_game_panel(s, new_game_panel::new_campaign);
                transition(state_id::new_game_menu);
                signal(signal_id::update);
                return;
            case state_id::briefing_from_any_mission:
                h.open_new_game_panel(s, new_game_panel::any_mission);
                transition(state_id::new_game_menu);
                signal(signal_id::update);
                return;
            case state_id::briefing_to_end_mission:
                call(Step::get_cursor_context);
                call(Step::enter_end_mission);
                h.set_app_mode(s, mode_id::end_game);
                h.set_endgame_state(s, mode_id::end_game);
                call(Step::draw_cursor);
                return;
            case state_id::briefing_to_single_player:
                h.set_app_mode(s, mode_id::frontend);
                transition(state_id::single_player_menu);
                return;
            default:
                return;
            }
        default:
            return;
        }
    default:
        return; // state 6 and the states the engine does not own
    }
}
} // namespace oa::ui::frontend_state
