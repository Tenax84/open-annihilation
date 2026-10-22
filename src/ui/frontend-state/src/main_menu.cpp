// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/frontend_state/main_menu.hpp"

namespace oa::ui::frontend_state::main_menu {
namespace {
class DocumentScope {
  public:

    explicit DocumentScope(Host& host) : host_(host), handle_(host.construct_document()) {}

    ~DocumentScope() { host_.destroy_document(handle_); }

    DocumentScope(const DocumentScope&) = delete;
    DocumentScope& operator=(const DocumentScope&) = delete;

    DocumentHandle handle() const noexcept { return handle_; }

  private:

    Host& host_;
    DocumentHandle handle_;
};

void notify_if_available(Host& host, const Environment& environment, Message message) {
    if (environment.messages_enabled != 0 && environment.message_target.value != 0)
        host.show_message(environment.message_target, message);
}

bool movie_available(Host& host, const Environment& captured_environment) {
    if ((host.application_flags() & flags::fullscreen_mode) == 0) {
        notify_if_available(host, captured_environment, Message::fullscreen_required);
        return false;
    }
    if (static_cast<uint8_t>(host.find_disc(Disc::campaign)) != 0 ||
        static_cast<uint8_t>(host.find_disc(Disc::multiplayer)) != 0)
        return true;
    notify_if_available(host, captured_environment, Message::game_disc_required);
    return false;
}
} // namespace

void handle_event(State& state, const Event& event, Host& host) {
    auto& captured_environment = host.environment();
    if (event.code == destroy_event) {
        host.release_sparks();
        return;
    }
    if (host.button_result(event.menu, Button::single_player) != 0) {
        host.play_sound(Sound::big_button, sound_argument);
        host.select_cursor_animation(menu_cursor_animation_index);
        state.pending_signal = signal_id::single_player;
        return;
    }
    if (host.button_result(event.menu, Button::multiplayer) != 0) {
        host.play_sound(Sound::big_button, sound_argument);
        host.select_cursor_animation(menu_cursor_animation_index);
        host.prepare_multiplayer();
        const auto path = host.resolve_resource(ResourceRequest{});
        const DocumentScope document(host);
        if (host.load_document(document.handle(), path) != 0) {
            state.pending_signal = signal_id::multiplayer;
            host.reset_after_multiplayer_selection();
        } else {
            notify_if_available(host, host.environment(), Message::multiplayer_disc_required);
        }
        return;
    }
    if (host.button_result(event.menu, Button::intro) != 0) {
        host.play_sound(Sound::small_button, sound_argument);
        if (!movie_available(host, captured_environment))
            return;
        host.select_cursor_animation(menu_cursor_animation_index);
        state.movie_skip = host.shift_key_state() < 0 ? 1U : 0U;
        state.logo_before_intro = true;
        host.drain_input();
        host.check_frontend_integrity();
        state.state = state_id::intro_followup;
        host.check_frontend_integrity();
        state.signal = signal_id::initialize;
        state.pending_signal = signal_id::initialize;
        return;
    }
    if (host.button_result(event.menu, Button::exit) != 0) {
        host.play_sound(Sound::exit, sound_argument);
        host.select_cursor_animation(menu_cursor_animation_index);
        state.pending_signal = signal_id::exit_application;
        return;
    }
    if (host.button_result(event.menu, Button::credits) != 0) {
        host.play_sound(Sound::small_button, sound_argument);
        if (!movie_available(host, captured_environment))
            return;
        host.select_cursor_animation(menu_cursor_animation_index);
        state.pending_signal = signal_id::movie_5;
        return;
    }
    host.default_event(event.menu);
}
} // namespace oa::ui::frontend_state::main_menu
