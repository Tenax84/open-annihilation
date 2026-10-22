// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The art of Open Annihilation's main menu (branding/main-menu), embedded as
// 8-bit RGBA PNG files: the battlefield behind the menu, the OPEN
// ANNIHILATION title and the face of its buttons in each of their four
// looks. The menu's screen (runtime_main_menu.cpp) decodes them the first
// time it shows.
#pragma once

#include <cstdint>
#include <span>

namespace oa::app {

/// Width of the main menu's background, in pixels.
inline constexpr uint32_t main_menu_background_width = 1920;

/// Height of the main menu's background, in pixels.
inline constexpr uint32_t main_menu_background_height = 1080;

/// Width of a main menu button's face, in pixels: twice the 422 columns the
/// face is drawn at in a window 1080 pixels high.
inline constexpr uint32_t main_menu_button_width = 844;

/// Height of a main menu button's face, in pixels: twice its 64 rows.
inline constexpr uint32_t main_menu_button_height = 128;

/// Returns the main menu's background: a battlefield at dusk, a commander on
/// the left and factories on the right, calm in the middle where the title
/// and the buttons stand.
///
/// @return a PNG file of main_menu_background_width by
///         main_menu_background_height pixels, which lasts as long as the
///         program
std::span<const uint8_t> main_menu_background_png();

/// Returns the main menu's title: OPEN spaced out over ANNIHILATION, whose
/// first and last letters stand taller, on a clear ground.
///
/// @return a PNG file of 1920 by 488 pixels, which lasts as long as the program
std::span<const uint8_t> main_menu_title_png();

/// Returns the face of a main menu button at rest: a worn metal frame with
/// a screw in each corner round a sunken, speckled plate.
///
/// @return a PNG file of main_menu_button_width by main_menu_button_height
///         pixels, which lasts as long as the program
std::span<const uint8_t> main_menu_button_normal_png();

/// Returns the face of a main menu button under the pointer or the focus:
/// its plate lighter and its edge lit warm.
///
/// @return a PNG file of the same size as main_menu_button_normal_png's
std::span<const uint8_t> main_menu_button_hover_png();

/// Returns the face of a main menu button held down: its plate darker and
/// sunk deeper.
///
/// @return a PNG file of the same size as main_menu_button_normal_png's
std::span<const uint8_t> main_menu_button_pressed_png();

/// Returns the face of a main menu button that does nothing now: grey and
/// faded.
///
/// @return a PNG file of the same size as main_menu_button_normal_png's
std::span<const uint8_t> main_menu_button_disabled_png();

} // namespace oa::app
