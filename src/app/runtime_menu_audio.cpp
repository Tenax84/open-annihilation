// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The main menu's hum, the menu voice loop, and when it starts: with the main
// menu, once the movies the game starts with are over. A start that shows the
// setup keeps those movies for the setup's end, and the hum waits for them.
#include "oa/app/runtime.hpp"
#include "oa/ui/frontend/main_menu.hpp"
#include <iostream>
#include <string_view>
#include <tuple>

namespace oa::app {

void Runtime::start_menu_music() {
    music_main_menu();
    play_menu_voice(oa::ui::frontend::kMainMenuMusic);
}

bool Runtime::menu_hum_waits() const {
    // A --first-run walk shows again each time the menu settles: its hum
    // waits for the walk's first end alone.
    if (options_.first_run && setup_finished_)
        return false;
    return options_.hum_after_setup;
}

void Runtime::play_menu_voice(std::string_view sound) {
    // The setup is still to finish: the hum waits for it
    // (start_waiting_menu_hum).
    if (menu_hum_waits()) {
        if (!menu_hum_waiting_)
            std::cout << "menu hum: waits for the setup\n" << std::flush;
        menu_hum_waiting_ = true;
        return;
    }
    if (menu_music_playing_)
        return;
    // The line is written when the hum starts, so that the log shows where
    // it falls among the movies' lines, in a run that keeps it silent too.
    if (!menu_hum_on_) {
        menu_hum_on_ = true;
        const char* silent = options_.mute             ? " (muted)"
                             : options_.headless_check ? " (headless)"
                                                       : "";
        std::cout << "menu hum: starts" << silent << '\n' << std::flush;
    }
    // A voice that does not start is reported by play_alternate_sound.
    std::ignore = play_alternate_sound(sound);
}

void Runtime::start_waiting_menu_hum() {
    if (!menu_hum_waiting_ || menu_hum_waits())
        return;
    menu_hum_waiting_ = false;
    start_menu_music();
}

void Runtime::stop_menu_music() {
    audio_player_.stop_loop();
    menu_music_playing_ = false;
    menu_hum_on_ = false;
    menu_hum_waiting_ = false;
    music_begin_match();
}

} // namespace oa::app
