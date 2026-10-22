// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's New Campaign screen on the OA layer (campaign_menu.hpp),
// new-campaign-menu, kept at the bottom of the layer while NEWGAME.GUI shows
// for a new campaign (Runtime::keep_new_campaign_menu_screen, once a frame
// from OaLayer::tick). Its strip has a tile for every side the side data
// names: Arm's and Core's show their logos, the first frames of NEWGAME's
// Side0 and Side1 art, and a mod's side its commander's picture from
// unitpics. Its list holds
// the chosen side's campaigns, as NEWGAME.GUI's Campaign list does, and the
// panel beside it the chosen campaign's mission count and first mission,
// with the difficulty under them. Choosing a side, a campaign or a
// difficulty sets the setup as NEWGAME.GUI's own controls do
// (Runtime::choose_campaign_side, choose_campaign_row and
// choose_campaign_difficulty); Arm's and Core's tiles press Side0 and Side1,
// and Start and Previous Menu press NEWGAME.GUI's Start and PrevMenu
// (Runtime::press_menu_gadget), so their checks and the screens they lead
// to stay the game's. Any Mission keeps NEWGAME.GUI as the game shows it.
// While a message box of the game's shows, the screen steps aside, and
// NEWGAME.GUI shows under the box.

#include "campaign_menu.hpp"
#include "column_menu.hpp"
#include "menu_paint.hpp"
#include "oa_layer.hpp"

#include "oa/app/runtime.hpp"
#include "oa/app/window_icon.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/match_rules/difficulty_names.hpp"
#include "oa/formats/gaf.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/ui/gui_layout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {

namespace {

/// How many sides the game itself has buttons for on NEWGAME.GUI, Arm and
/// Core; a side past them is a mod's.
constexpr std::size_t game_sides = 2;

/// NEWGAME.GUI's side buttons, Arm's and Core's.
constexpr std::array<std::string_view, game_sides> side_records{"Side0", "Side1"};

/// The difficulties' captions, by the name each carries.
constexpr std::array<std::string_view, 3> difficulty_captions{"EASY", "MEDIUM", "HARD"};

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

} // namespace

bool Runtime::oa_new_campaign_menu_shown() {
    return oa_layer().find(new_campaign_menu_screen_name) != nullptr;
}

void Runtime::keep_new_campaign_menu_screen() {
    const auto shows = [this] { return screen_ == Screen::new_campaign && oa_menu_may_show(); };
    if (!shows())
        return;
    OaLayer& layer = oa_layer();
    if (layer.find(new_campaign_menu_screen_name) != nullptr)
        return;
    // Which sides have a campaign is read once, as NEWGAME.GUI reads it when
    // it opens: it searches the game data's campaign files.
    const uint32_t side_count = campaign_side_count();
    auto sides = std::make_shared<std::vector<CampaignMenuSide>>();
    for (uint32_t side = 0; side < side_count; ++side)
        sides->push_back({campaign_side_name(side), side_has_campaign(side), side >= game_sides});
    const auto facts = [this, sides] {
        CampaignMenuFacts now;
        now.sides = *sides;
        now.side = preferences_.side;
        now.campaigns = campaign_shown_labels_.size() == campaign_labels_.size()
                            ? campaign_shown_labels_
                            : campaign_labels_;
        now.campaign = selected_campaign_index_;
        now.missions = campaign_mission_labels_.size();
        if (!campaign_mission_labels_.empty())
            now.first_mission = campaign_mission_labels_.front();
        const auto names = difficulty_names();
        for (std::size_t level = 0; level < now.difficulties.size(); ++level)
            now.difficulties[level] = std::string(oa::data::languages::interface_text(
                difficulty_captions[oa::data::match_rules::difficulty_name_index(
                    names, static_cast<int32_t>(level)
                )]
            ));
        now.difficulty = preferences_.difficulty;
        const auto& gadgets = resources_.layout.gadgets;
        now.start_key = quick_key_of(gadgets, "Start");
        now.back_key = quick_key_of(gadgets, "PrevMenu");
        const auto start = gui_record_named(gadgets, "Start");
        now.can_start = !now.campaigns.empty() && start && gadgets[*start].common.active != 0 &&
                        frontend_gadget_pressable(*start);
        now.version = menu_version_;
        return now;
    };
    CampaignMenuActions actions;
    actions.choose_side = [this](std::size_t side) {
        // Arm and Core are chosen as their buttons choose them.
        if (side < game_sides && press_menu_gadget(Screen::new_campaign, side_records[side]))
            return;
        choose_campaign_side(static_cast<uint32_t>(side));
    };
    actions.choose_campaign = [this](std::size_t row) { choose_campaign_row(row); };
    actions.choose_difficulty = [this](uint32_t level) { choose_campaign_difficulty(level); };
    actions.start = [this] { press_menu_gadget(Screen::new_campaign, "Start"); };
    actions.back = [this] { press_menu_gadget(Screen::new_campaign, "PrevMenu"); };
    actions.emblem = [this](std::size_t side) { return side_emblem(side, resources_.sprites); };
    layer.push_under(make_campaign_menu(shows, facts, std::move(actions)));
}

WindowIcon Runtime::side_emblem(std::size_t side, const oa::formats::gaf::Archive& logos) {
    WindowIcon picture;
    if (side < game_sides) {
        // Arm's and Core's logos, as NEWGAME.GUI's side buttons show them
        // unpressed.
        const auto* logo = gaf_sequence(logos, side_records[side]);
        if (logo != nullptr && !logo->frames.empty()) {
            picture = menu_paint::picture_of_frame(logo->frames.front(), screen_palette());
            if (picture.width > 0)
                return picture;
        }
    }
    if (side >= side_table_.count)
        return picture;
    const auto& commander = side_table_.sides[side].commander;
    const std::string name(commander, ::strnlen(commander, sizeof commander));
    if (name.empty())
        return picture;
    const auto bytes = read("unitpics/" + name + ".pcx");
    if (!bytes)
        return picture;
    auto decoded = oa::decode_pcx(*bytes);
    if (!decoded.ok())
        return picture;
    const oa::Image& image = *decoded.value;
    const std::size_t pixels = static_cast<std::size_t>(image.width) * image.height;
    if (image.rgb.size() < pixels * 3U)
        return picture;
    picture.width = image.width;
    picture.height = image.height;
    picture.pixels.resize(pixels * 4U);
    for (std::size_t at = 0; at < pixels; ++at) {
        picture.pixels[at * 4U] = image.rgb[at * 3U];
        picture.pixels[at * 4U + 1U] = image.rgb[at * 3U + 1U];
        picture.pixels[at * 4U + 2U] = image.rgb[at * 3U + 2U];
        picture.pixels[at * 4U + 3U] = 255;
    }
    return picture;
}

} // namespace oa::app
