// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's Skirmish screen on the OA layer (skirmish_menu.hpp),
// skirmish-menu, kept at the bottom of the layer while SKIRMISH.GUI shows
// (Runtime::keep_skirmish_menu_screen, once a frame from OaLayer::tick). Its
// rows show the skirmish setup's slots as SKIRMISH.GUI's slot buttons do,
// with each side's emblem (Runtime::side_emblem, from NEWGAME's art) and
// each colour's logo, the first frames of the colour logos. Its MAP panel
// shows the selected map's minimap, the part of it the map covers, with the
// start positions of the map's schema for the players in the setup, and the
// map's player counts and size as its OTA file gives them. Every press
// presses the SKIRMISH.GUI button it stands for (Runtime::press_menu_gadget),
// with the mouse button it was made with, so that the game's handler, its
// sounds, its checks and the screens it leads to stay the game's. While a
// message box of the game's shows, the screen steps aside, and SKIRMISH.GUI
// shows under the box.

#include "column_menu.hpp"
#include "menu_paint.hpp"
#include "oa_layer.hpp"
#include "skirmish_menu.hpp"

#include "oa/app/runtime.hpp"
#include "oa/formats/gaf.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/ota.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/gui_layout.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {

namespace {

/// A rule of SKIRMISH.GUI's: its button, the label over it in English and
/// how many values its press steps through, as the skirmish handler steps
/// them.
struct SkirmishRule {
    std::string_view record{}; ///< its button
    std::string_view label{};  ///< what it sets
    uint32_t stages{};         ///< how many values it steps through
};

/// The rules, in the order the GAME OPTIONS panel lists them: Line of Sight
/// steps through no line of sight, true and circular, Difficulty through
/// easy, medium and hard, the others on and off.
constexpr std::array<SkirmishRule, skirmish_menu_options> rules{{
    {"CommanderDeath", "Commander", 2},
    {"StartLocation", "Location", 2},
    {"Mapping", "Mapping", 2},
    {"LineOfSight", "Line of Sight", 3},
    {"Difficulty", "Difficulty", 3},
}};

/// The slot fields whose hints the screen takes from SKIRMISH.GUI, by the
/// names their buttons start with.
constexpr std::array<std::string_view, 6> hinted_fields{
    "Player", "Side", "Color", "Allies", "Metal", "Energy"
};

/// The alliance group of a slot in none.
constexpr int32_t no_team = oa::ui::frontend_state::game_entry::unassigned_alliance;

/// The columns of the right edge, and the rows of the bottom edge, of a
/// map's terrain that its minimap shows but the map does not cover.
constexpr int32_t unshown_columns = 32;
constexpr int32_t unshown_rows = 128;

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

bool Runtime::oa_skirmish_menu_shown() {
    return oa_layer().find(skirmish_menu_screen_name) != nullptr;
}

void Runtime::keep_skirmish_menu_screen() {
    const auto shows = [this] { return screen_ == Screen::skirmish && oa_menu_may_show(); };
    if (!shows())
        return;
    OaLayer& layer = oa_layer();
    if (layer.find(skirmish_menu_screen_name) != nullptr)
        return;
    // The caption a record of SKIRMISH.GUI shows now: a button's at the
    // stage it shows, a label's text.
    const auto caption = [this](std::string_view name) -> std::string {
        const auto& gadgets = resources_.layout.gadgets;
        const auto found = gui_record_named(gadgets, name);
        if (!found)
            return {};
        const auto& gadget = gadgets[*found];
        if (const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields)) {
            const auto stage = widget_text_stages_.find(gadget.common.name);
            const std::size_t shown = stage != widget_text_stages_.end() ? stage->second : 0;
            if (auto translated = renderer::translated_stage_caption(*button, shown))
                return *translated;
            return std::string(renderer::staged_caption(button->text, shown));
        }
        if (const auto* label = std::get_if<oa::ui::gui_layout::LabelFields>(&gadget.fields))
            return label->text;
        return {};
    };
    // Whether a record of SKIRMISH.GUI takes a press now.
    const auto pressable = [this](std::string_view name) {
        const auto& gadgets = resources_.layout.gadgets;
        const auto found = gui_record_named(gadgets, name);
        return found && gadgets[*found].common.active != 0 && frontend_gadget_pressable(*found);
    };
    // The help a record of SKIRMISH.GUI gives now.
    const auto help = [this](std::string_view name) -> std::string {
        const auto& gadgets = resources_.layout.gadgets;
        const auto found = gui_record_named(gadgets, name);
        return found ? gadgets[*found].common.runtime_help : std::string{};
    };
    // Whether the terrain and the OTA file held are the setup's map's, as
    // they are but while the map selection shows another.
    const auto map_held = [this] {
        return selected_tnt_ && selected_map_metadata_ &&
               selected_map_name_runtime_ == skirmish_settings_.map_name;
    };
    const auto facts = [this, caption, pressable, help, map_held] {
        SkirmishMenuFacts now;
        namespace entry = oa::ui::frontend_state::game_entry;
        const auto count = static_cast<std::size_t>(std::clamp<int32_t>(
            skirmish_settings_.slot_count, 0, static_cast<int32_t>(skirmish_settings_.slots.size())
        ));
        int32_t playing = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const auto& slot = skirmish_settings_.slots[index];
            SkirmishMenuSlot shown;
            const std::string digit = std::to_string(index);
            shown.player = caption("Player" + digit);
            shown.open = slot.controller == entry::controller::disabled;
            shown.local = slot.controller == entry::controller::human;
            shown.side = static_cast<std::size_t>(std::max(slot.side, 0));
            shown.side_name = campaign_side_name(static_cast<uint32_t>(shown.side));
            shown.colour = slot.color;
            shown.team = slot.alliance >= 0 && slot.alliance < no_team ? slot.alliance + 1 : 0;
            shown.metal = std::to_string(slot.metal);
            shown.energy = std::to_string(slot.energy);
            if (!shown.open)
                ++playing;
            now.slots.push_back(std::move(shown));
        }
        for (std::size_t option = 0; option < rules.size(); ++option) {
            const SkirmishRule& rule = rules[option];
            SkirmishMenuOption& shown = now.options[option];
            shown.record = std::string(rule.record);
            shown.label = translate_ui(rule.label);
            shown.value = caption(rule.record);
            shown.stages = pressable(rule.record) ? rule.stages : 0;
            shown.help = help(rule.record);
        }
        for (std::size_t field = 0; field < hinted_fields.size(); ++field)
            now.help[field] = help(std::string(hinted_fields[field]) + "0");
        now.map = caption("MapName");
        now.map_record = skirmish_settings_.map_name;
        if (map_held()) {
            now.map_record += "\n" + selected_map_name_runtime_;
            now.players = selected_map_metadata_->permitted_player_counts;
            now.size = selected_map_metadata_->map_size;
            const int32_t wide =
                static_cast<int32_t>(selected_tnt_->attribute_width) * 16 - unshown_columns;
            const int32_t high =
                static_cast<int32_t>(selected_tnt_->attribute_height) * 16 - unshown_rows;
            const auto* schema =
                oa::formats::ota::select_multiplayer_schema(*selected_map_metadata_, playing);
            // The first start positions, one for each player in the setup,
            // as fixed locations seat them.
            if (schema != nullptr && wide > 0 && high > 0)
                for (const auto& position : schema->start_positions)
                    if (position.index < playing)
                        now.starts.push_back(
                            {position.index + 1, static_cast<double>(position.x) / wide,
                             static_cast<double>(position.z) / high}
                        );
        }
        const auto& gadgets = resources_.layout.gadgets;
        now.start_key = quick_key_of(gadgets, "Start");
        now.back_key = quick_key_of(gadgets, "PrevMenu");
        now.map_key = quick_key_of(gadgets, "SelectMap");
        now.can_start = pressable("Start");
        now.can_select_map = pressable("SelectMap");
        now.version = menu_version_;
        return now;
    };
    // NEWGAME's art holds Arm's and Core's logos; the textures' palette
    // colours the colour logos. Each is read once, when first asked for.
    auto logos = std::make_shared<std::optional<oa::formats::gaf::Archive>>();
    auto palette = std::make_shared<std::optional<oa::PaletteBytes>>();
    const auto game_palette = [this, palette]() -> const oa::PaletteBytes& {
        if (!*palette) {
            oa::PaletteBytes bytes = screen_palette();
            if (const auto read_bytes = read("palettes/palette.pal");
                read_bytes && read_bytes->size() == bytes.size())
                std::copy(read_bytes->begin(), read_bytes->end(), bytes.begin());
            *palette = bytes;
        }
        return **palette;
    };
    SkirmishMenuActions actions;
    actions.press = [this](std::string_view name, int32_t mouse_button) {
        press_menu_gadget(Screen::skirmish, name, mouse_button);
    };
    actions.emblem = [this, logos](std::size_t side) {
        if (!*logos) {
            *logos = oa::formats::gaf::Archive{};
            if (const auto bytes = read("anims/newgame.gaf")) {
                auto parsed = oa::formats::gaf::parse(*bytes);
                if (parsed.ok())
                    *logos = std::move(*parsed.archive);
            }
        }
        return side_emblem(side, **logos);
    };
    actions.swatch = [this, game_palette](int32_t colour) {
        if (logo_sequence_ == nullptr || colour < 0 ||
            static_cast<std::size_t>(colour) >= logo_sequence_->frames.size())
            return WindowIcon{};
        return menu_paint::picture_of_frame(
            logo_sequence_->frames[static_cast<std::size_t>(colour)], game_palette()
        );
    };
    actions.map_picture = [this, map_held, game_palette] {
        WindowIcon picture;
        if (!map_held() || !selected_tnt_->minimap)
            return picture;
        const auto& minimap = *selected_tnt_->minimap;
        const int64_t wide =
            static_cast<int64_t>(selected_tnt_->attribute_width) * 16 - unshown_columns;
        const int64_t high =
            static_cast<int64_t>(selected_tnt_->attribute_height) * 16 - unshown_rows;
        if (wide <= 0 || high <= 0 || minimap.width == 0 || minimap.height == 0 ||
            minimap.palette_indices.size() < static_cast<std::size_t>(minimap.width) * minimap.height)
            return picture;
        // The map's longer side fills the minimap; the rest of the other
        // side is not the map's.
        uint32_t columns = minimap.width;
        uint32_t rows = minimap.height;
        if (wide < high)
            columns = static_cast<uint32_t>(minimap.width * wide / high);
        else
            rows = static_cast<uint32_t>(minimap.height * high / wide);
        columns = std::clamp<uint32_t>(columns, 1, minimap.width);
        rows = std::clamp<uint32_t>(rows, 1, minimap.height);
        const oa::PaletteBytes& colours = game_palette();
        picture.width = columns;
        picture.height = rows;
        picture.pixels.resize(static_cast<std::size_t>(columns) * rows * 4U);
        for (uint32_t y = 0; y < rows; ++y)
            for (uint32_t x = 0; x < columns; ++x) {
                const std::size_t colour =
                    static_cast<std::size_t>(
                        minimap.palette_indices[static_cast<std::size_t>(y) * minimap.width + x]
                    ) *
                    4U;
                uint8_t* pixel =
                    picture.pixels.data() + (static_cast<std::size_t>(y) * columns + x) * 4U;
                pixel[0] = colours[colour];
                pixel[1] = colours[colour + 1U];
                pixel[2] = colours[colour + 2U];
                pixel[3] = 255;
            }
        return picture;
    };
    layer.push_under(make_skirmish_menu(shows, facts, std::move(actions)));
}

} // namespace oa::app
