// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's mission briefing screen on the OA layer
// (briefing_menu.hpp), briefing-menu, kept at the bottom of the layer while
// the mission briefing, MSNBRIEF.GUI, shows over New Campaign, Any Mission
// or the end of a mission (Runtime::keep_briefing_menu_screen, once a frame
// from OaLayer::tick). It shows the briefing's text whole, the panorama as
// it scrolls and the planet as it turns, the wind as it wanders and the
// gravity as the briefing tells them, the planet, the side and the
// difficulty. START, PREVIOUS MENU and the NARRATION switch press the
// briefing's own Start, PrevMenu and SHUTUP (Runtime::click_briefing_gadget),
// so that its checks, its sounds, its narration and the screens it leads to
// stay the game's. While a message box of the game's shows, the screen steps
// aside. The briefing opened from the pause menu of a match stays the
// game's.

#include "briefing_menu.hpp"
#include "column_menu.hpp"
#include "menu_paint.hpp"
#include "oa_layer.hpp"

#include "oa/app/runtime.hpp"
#include "oa/data/match_rules/difficulty_names.hpp"
#include "oa/present/game_text.hpp"
#include "oa/ui/gui_layout.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {

namespace {

/// The difficulties' words, by the name each carries.
constexpr std::array<std::string_view, 3> difficulty_words{"Easy", "Medium", "Hard"};
/// The brightest a pixel of the planet's black ground is.
constexpr uint8_t planet_black = 8;

/// Returns a button's quick key on the GUI layout shown.
///
/// @param gadgets the layout's records
/// @param name the button's name
/// @return the key; 0 for none
char quick_key_of(std::span<const oa::ui::gui_layout::Gadget> gadgets, std::string_view name) {
    const auto found = gui_record_named(gadgets, name);
    if (!found)
        return 0;
    if (const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadgets[*found].fields))
        return static_cast<char>(button->quick_key);
    return 0;
}

/// Returns frames of the briefing's art as one picture, side by side, left
/// to right, through a palette; a pixel a frame leaves uncovered is clear.
///
/// @param frames the frames
/// @param palette 4 bytes per colour
/// @return the picture; empty for no frames
WindowIcon picture_of_strip(
    std::span<const oa::formats::gaf::RenderedFrame> frames, const oa::PaletteBytes& palette
) {
    WindowIcon picture;
    for (const auto& frame : frames) {
        picture.width += frame.width;
        picture.height = std::max<uint32_t>(picture.height, frame.height);
    }
    if (picture.width == 0 || picture.height == 0)
        return {};
    picture.pixels.assign(
        static_cast<std::size_t>(picture.width) * picture.height * window_icon_pixel_bytes, 0
    );
    uint32_t left = 0;
    for (const auto& frame : frames) {
        for (uint32_t row = 0; row < frame.height; ++row)
            for (uint32_t column = 0; column < frame.width; ++column) {
                const std::size_t from = static_cast<std::size_t>(row) * frame.width + column;
                if (from >= frame.coverage.size() || frame.coverage[from] == 0 ||
                    from >= frame.pixels.size())
                    continue;
                const std::size_t entry = static_cast<std::size_t>(frame.pixels[from]) * 4U;
                if (entry + 2 >= palette.size())
                    continue;
                uint8_t* to = picture.pixels.data() +
                              (static_cast<std::size_t>(row) * picture.width + left + column) *
                                  window_icon_pixel_bytes;
                to[0] = palette[entry];
                to[1] = palette[entry + 1];
                to[2] = palette[entry + 2];
                to[3] = 255;
            }
        left += frame.width;
    }
    return picture;
}

} // namespace

bool Runtime::briefing_menu_covers() {
    return screen_ == Screen::briefing && !briefing_from_pause_ && oa_menu_may_show();
}

bool Runtime::oa_briefing_menu_shown() {
    return oa_layer().find(briefing_menu_screen_name) != nullptr;
}

void Runtime::keep_briefing_menu_screen() {
    const auto shows = [this] { return briefing_menu_covers(); };
    if (!shows())
        return;
    OaLayer& layer = oa_layer();
    if (layer.find(briefing_menu_screen_name) != nullptr)
        return;
    const auto facts = [this] {
        BriefingMenuFacts now;
        const MissionBriefingShown shown = mission_briefing_shown();
        std::string text = oa::present::decode_game_text(briefing_text_, game_text_utf8());
        std::erase(text, '\r');
        now.text = std::move(text);
        now.planet = shown.planet;
        now.side = menu_paint::worded(campaign_side_name(preferences_.side));
        now.wind = std::to_string(shown.wind);
        std::array<char, 32> gravity{};
        std::snprintf(gravity.data(), gravity.size(), "%.1f", shown.gravity);
        now.gravity = gravity.data();
        now.difficulty = menu_paint::worded(difficulty_words[oa::data::match_rules::difficulty_name_index(
            difficulty_names(), static_cast<int32_t>(preferences_.difficulty)
        )]);
        now.narration = shown.narration;
        now.panorama_scroll = shown.panorama_scroll;
        now.planet_frame = static_cast<std::size_t>(std::max(shown.rotation_frame, int32_t{0}));
        const auto& gadgets = resources_.layout.gadgets;
        now.start_key = quick_key_of(gadgets, "Start");
        now.back_key = quick_key_of(gadgets, "PrevMenu");
        const auto start = gui_record_named(gadgets, "Start");
        now.can_start = start && gadgets[*start].common.active != 0;
        now.version = menu_version_;
        return now;
    };
    BriefingMenuActions actions;
    actions.start = [this] { click_briefing_gadget("Start"); };
    actions.back = [this] { click_briefing_gadget("PrevMenu"); };
    actions.narration = [this] { click_briefing_gadget("SHUTUP"); };
    actions.panorama = [this] {
        const auto& palette =
            resources_.background.palette ? *resources_.background.palette : resources_.gui_palette;
        return picture_of_strip(briefing_art_.panorama, palette);
    };
    actions.planet = [this] {
        const auto& palette =
            resources_.background.palette ? *resources_.background.palette : resources_.gui_palette;
        std::vector<WindowIcon> frames;
        frames.reserve(briefing_art_.planet.size());
        // The planet turns on black, which the screen's sky shows through;
        // every frame is cut to the box that holds the planet in all of
        // them, so that it fills its ring and stays put as it turns.
        uint32_t left = UINT32_MAX, top = UINT32_MAX, right = 0, bottom = 0;
        for (const auto& frame : briefing_art_.planet) {
            WindowIcon picture = picture_of_strip({&frame, 1}, palette);
            for (uint32_t y = 0; y < picture.height; ++y)
                for (uint32_t x = 0; x < picture.width; ++x) {
                    uint8_t* pixel = picture.pixels.data() +
                                     (static_cast<std::size_t>(y) * picture.width + x) *
                                         window_icon_pixel_bytes;
                    if (std::max({pixel[0], pixel[1], pixel[2]}) <= planet_black) {
                        pixel[3] = 0;
                        continue;
                    }
                    left = std::min(left, x);
                    top = std::min(top, y);
                    right = std::max(right, x + 1);
                    bottom = std::max(bottom, y + 1);
                }
            frames.push_back(std::move(picture));
        }
        if (right <= left || bottom <= top)
            return frames;
        for (WindowIcon& picture : frames) {
            if (picture.width < right || picture.height < bottom)
                continue;
            WindowIcon cut;
            cut.width = right - left;
            cut.height = bottom - top;
            cut.pixels.resize(static_cast<std::size_t>(cut.width) * cut.height * window_icon_pixel_bytes);
            for (uint32_t y = 0; y < cut.height; ++y)
                std::copy_n(
                    picture.pixels.data() + (static_cast<std::size_t>(top + y) * picture.width + left) *
                                                window_icon_pixel_bytes,
                    static_cast<std::size_t>(cut.width) * window_icon_pixel_bytes,
                    cut.pixels.data() + static_cast<std::size_t>(y) * cut.width * window_icon_pixel_bytes
                );
            picture = std::move(cut);
        }
        return frames;
    };
    layer.push_under(make_briefing_menu(shows, facts, std::move(actions)));
}

} // namespace oa::app
