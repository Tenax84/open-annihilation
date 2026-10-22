// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's main menu on the OA layer: one screen, main-menu, kept
// at the bottom of the layer while the main menu shows
// (Runtime::keep_main_menu_screen, once a frame from OaLayer::tick). It
// covers the whole window with its own art (branding/main-menu): the
// battlefield behind, the OPEN ANNIHILATION title and one column of seven
// buttons, Single Player, Multiplayer, Library, Settings, Intro, Credits and
// Exit, and under them a plate with the main menu's version label. The
// game's buttons are pressed as a click on MAINMENU.GUI's own presses them
// (Runtime::press_main_menu_gadget), so their sounds, their checks and the
// screens they lead to are the game's; Library and Settings open as the
// game's OA buttons open them. A button MAINMENU.GUI grays out or hides, as
// Intro and Credits are without movies, shows faded and takes no press.
//
// It is not modal: the screens over it, Settings, the Library, a notice,
// take what they show over it, and nothing under it sees the pointer. The
// arrows, Tab, Return and Space move between and press its buttons; every
// other key goes on to MAINMENU.GUI, whose quick keys and Escape stay the
// game's.

#include "oa_layer.hpp"

#include "oa/app/main_menu_art.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/view_rules.hpp"
#include "oa/app/window_icon.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/frontend/main_menu.hpp"
#include "oa/ui/frontend_state/main_menu.hpp"
#include "oa/ui/gui_layout.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/paint/painter.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <iterator>
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

namespace kit = oa::ui::kit;
namespace artless = oa::ui::frontend_renderer;
namespace menu = oa::ui::frontend_state::main_menu;

/// The menu's screen's name on the OA layer.
constexpr std::string_view main_menu_screen_name = "main-menu";

/// The menu's buttons, top to bottom.
enum class MenuEntry : uint8_t {
    single_player,
    multiplayer,
    library,
    settings,
    intro,
    credits,
    exit,
};

/// How many buttons the menu has.
constexpr std::size_t entry_count = 7;

/// A button's automation name and its caption in the interface catalogue.
struct EntryWords {
    std::string_view name{};    ///< its automation name, after main-menu.
    std::string_view english{}; ///< its caption, looked up in the catalogue
};

/// Each button's words, in MenuEntry's order.
constexpr std::array<EntryWords, entry_count> entry_words{{
    {"single-player", "SINGLE PLAYER"},
    {"multiplayer", "MULTIPLAYER"},
    {"library", "LIBRARY"},
    {"settings", "SETTINGS"},
    {"intro", "INTRO"},
    {"credits", "CREDITS"},
    {"exit", "EXIT"},
}};

// The menu's measures, in rows of a window 1080 rows high: the art is drawn
// for that window, and every other window takes them at its own height.

/// The window height the measures are given for.
constexpr double design_rows = 1080.0;
/// The narrowest width, in the same rows, the measures keep their size in;
/// a narrower window shrinks them to fit its width.
constexpr double design_least_columns = 1000.0;
constexpr double title_top = 70.0;            ///< the title's top
constexpr double title_columns = 960.0;       ///< the title's width
constexpr double first_button_top = 400.0;    ///< the first button's top
constexpr double button_columns = 422.0;      ///< a button's width
constexpr double button_rows = 64.0;          ///< a button's height
constexpr double button_pitch = 76.0;         ///< from one button's top to the next's
constexpr double plate_top = 968.0;           ///< the version plate's top
constexpr double plate_rows = 40.0;           ///< the version plate's height
constexpr double plate_margin_columns = 30.0; ///< the plate's room either side of its text
/// How far the background's left edge moves left of the window's when the
/// window is narrower than the picture, as a share of what is cut: the
/// commander on its left stays in view.
constexpr double background_cut_left_share = 0.1;
/// How many bands the shade over the top and the bottom of the picture is
/// drawn in.
constexpr int32_t shade_bands = 24;
/// How far down the shade over the top reaches, as a share of the height.
constexpr double top_shade_share = 0.24;
/// How far up the shade over the bottom reaches, as a share of the height.
constexpr double bottom_shade_share = 0.22;
/// The top shade's opacity at the top edge, in 256ths.
constexpr uint32_t top_shade_opacity = 200;
/// The bottom shade's opacity at the bottom edge, in 256ths.
constexpr uint32_t bottom_shade_opacity = 150;

constexpr kit::Colour ground_colour{8, 10, 12};            ///< under the picture
constexpr kit::Colour shade_colour{3, 7, 10};              ///< the shade's
constexpr kit::Colour caption_colour{236, 230, 198};       ///< a caption, cream
constexpr kit::Colour caption_lit_colour{255, 250, 222};   ///< a highlighted caption
constexpr kit::Colour caption_faded_colour{150, 152, 144}; ///< a disabled caption
constexpr kit::Colour outline_colour{18, 20, 16};          ///< round every caption
constexpr kit::Colour key_colour{70, 190, 70};             ///< the quick key's underline
constexpr kit::Colour plate_colour{14, 20, 26};            ///< the version plate
constexpr kit::Colour plate_edge_colour{92, 104, 112};     ///< the version plate's edge
constexpr kit::Colour plate_text_colour{196, 202, 204};    ///< the version label
/// The version plate's opacity, in 256ths.
constexpr uint32_t plate_opacity = 210;

/// A button's look, the face it is drawn with.
enum class Face : uint8_t { normal, hover, pressed, disabled };

/// The menu's art, decoded.
struct MenuArt {
    WindowIcon background{};           ///< behind everything
    WindowIcon title{};                ///< OPEN ANNIHILATION
    std::array<WindowIcon, 4> faces{}; ///< a button's face in each Face
};

/// Decodes one picture of the art, saying so when it cannot be read.
///
/// @param png the embedded file
/// @param what what it is, for the message
/// @return the picture; empty when it cannot be decoded
WindowIcon decoded_art(std::span<const uint8_t> png, std::string_view what) {
    WindowIcon picture;
    std::string error;
    if (!decode_rgba_icon(png, picture, error)) {
        std::cerr << "open-annihilation: the main menu's " << what << " is missing: " << error
                  << '\n';
        picture = {};
    }
    return picture;
}

/// Returns the menu's art, decoded the first time it is asked for and kept
/// for the rest of the run.
///
/// @return the art; a picture that cannot be decoded is empty and draws nothing
const MenuArt& menu_art() {
    static const MenuArt art = [] {
        MenuArt decoded;
        decoded.background = decoded_art(main_menu_background_png(), "background");
        decoded.title = decoded_art(main_menu_title_png(), "title");
        decoded.faces[static_cast<std::size_t>(Face::normal)] =
            decoded_art(main_menu_button_normal_png(), "button");
        decoded.faces[static_cast<std::size_t>(Face::hover)] =
            decoded_art(main_menu_button_hover_png(), "highlighted button");
        decoded.faces[static_cast<std::size_t>(Face::pressed)] =
            decoded_art(main_menu_button_pressed_png(), "pressed button");
        decoded.faces[static_cast<std::size_t>(Face::disabled)] =
            decoded_art(main_menu_button_disabled_png(), "disabled button");
        return decoded;
    }();
    return art;
}

/// Returns a decoded picture as the canvas draws it.
///
/// @param icon the picture
/// @return the picture's view of its pixels
artless::RgbaPicture picture_of(const WindowIcon& icon) noexcept {
    return {icon.width, icon.height, icon.pixels};
}

/// A picture of the art filtered to the pixels it is drawn on.
struct FittedArt {
    const WindowIcon* picture{};   ///< the picture it is made from
    int32_t columns{};             ///< its width, in canvas pixels
    int32_t rows{};                ///< its height, in canvas pixels
    std::vector<uint8_t> pixels{}; ///< four bytes a pixel; empty when it cannot be made
};

/// The most filtered pictures the menu keeps: the background, the title,
/// and the four faces at each of the buttons' heights, which rounding may
/// make differ by a point.
constexpr std::size_t most_fitted_art = 16;

/// What the menu shows of the game's own main menu now.
struct MenuFacts {
    /// Which buttons take a press, in MenuEntry's order.
    std::array<bool, entry_count> enabled{};
    /// Each button's quick key, as MAINMENU.GUI gives it; 0 for none.
    std::array<char, entry_count> quick_keys{};
    /// The main menu's version label, as MAINMENU.GUI's shows it; empty
    /// while that label is hidden.
    std::string version{};

    friend bool operator==(const MenuFacts&, const MenuFacts&) = default;
};

/// Where the menu's parts lie, in the screen's points.
struct MenuLayout {
    kit::Rect background{};                       ///< the picture, cut by the screen's edges
    kit::Rect title{};                            ///< the title
    std::array<kit::Rect, entry_count> buttons{}; ///< the buttons, in MenuEntry's order
    int32_t plate_top{};                          ///< the version plate's top
    int32_t plate_height{};                       ///< the version plate's height
    int32_t plate_margin{};                       ///< its room either side of the text
};

/// Rounds a measure to the nearest point.
///
/// @param value the measure, in points
/// @return the point
int32_t nearest(double value) noexcept {
    return static_cast<int32_t>(std::lround(value));
}

/// Lays the menu out on a screen of a size.
///
/// @param width the screen's width, in points
/// @param height the screen's height, in points
/// @param title_size the title picture's width and height, in pixels
/// @return where its parts lie
MenuLayout lay_out_menu(int32_t width, int32_t height, kit::Point title_size) {
    MenuLayout placed;
    const double columns = std::max(width, 1);
    const double rows = std::max(height, 1);
    // A point is this many rows of the 1080-row window; a window narrower
    // than design_least_columns of them takes a smaller one, centred down.
    const double unit = std::min(rows / design_rows, columns / design_least_columns);
    const double top = (rows - design_rows * unit) / 2.0;
    const double middle = columns / 2.0;

    // The picture covers the screen, cut at the edges it overhangs.
    const double cover = std::max(
        columns / static_cast<double>(main_menu_background_width),
        rows / static_cast<double>(main_menu_background_height)
    );
    const double picture_width = static_cast<double>(main_menu_background_width) * cover;
    const double picture_height = static_cast<double>(main_menu_background_height) * cover;
    placed.background = {
        nearest(-(picture_width - columns) * background_cut_left_share),
        nearest(-(picture_height - rows) / 2.0),
        nearest(picture_width),
        nearest(picture_height)
    };

    const double title_width = title_columns * unit;
    const double title_height = title_size.x > 0 ? title_width * static_cast<double>(title_size.y) /
                                                       static_cast<double>(title_size.x)
                                                 : 0.0;
    placed.title = {
        nearest(middle - title_width / 2.0),
        nearest(top + title_top * unit),
        nearest(title_width),
        nearest(title_height)
    };

    const int32_t button_width = nearest(button_columns * unit);
    const int32_t button_height = nearest(button_rows * unit);
    for (std::size_t index = 0; index < entry_count; ++index)
        placed.buttons[index] = {
            nearest(middle) - button_width / 2,
            nearest(top + (first_button_top + button_pitch * static_cast<double>(index)) * unit),
            button_width,
            button_height
        };
    placed.plate_top = nearest(top + plate_top * unit);
    placed.plate_height = std::max(nearest(plate_rows * unit), 1);
    placed.plate_margin = nearest(plate_margin_columns * unit);
    return placed;
}

/// Returns where a UTF-8 caption holds its quick key: the first letter that
/// is the key, in either case.
///
/// @param caption the caption
/// @param key the key; 0 for none
/// @return the byte it starts at; none when the caption does not hold it
std::optional<std::size_t> quick_key_at(std::string_view caption, char key) {
    if (key == 0)
        return std::nullopt;
    const auto lower = [](char letter) {
        return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
    };
    for (std::size_t at = 0; at < caption.size(); ++at)
        if (lower(caption[at]) == lower(key))
            return at;
    return std::nullopt;
}

/// Open Annihilation's main menu as a screen of the OA layer.
///
/// Its points are the layer's: one point is the view's scale in window
/// pixels, from the room's top left corner, and the screen covers the room.
class MainMenuScreen final : public LayerScreen {
  public:

    /// Makes the screen.
    ///
    /// @param shows tells whether the menu shows: the main menu is up and
    ///     the run takes Open Annihilation's
    /// @param facts reads what the game's main menu shows now
    /// @param press presses a button
    MainMenuScreen(
        std::function<bool()> shows,
        std::function<MenuFacts()> facts,
        std::function<void(MenuEntry)> press
    )
        : shows_(std::move(shows)), facts_(std::move(facts)), press_(std::move(press)) {
        facts_now_ = facts_();
        if (!facts_now_.enabled[static_cast<std::size_t>(focus_)])
            focus_ = next_enabled(focus_, 1);
    }

    /// Returns "main-menu".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return main_menu_screen_name; }

    /// Returns where the screen shows: the whole room, at the view's scale;
    /// nowhere in a match.
    ///
    /// @param placed_on what the screen is placed on
    /// @return its place
    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        if (placed_on.match || placed_on.room.width <= 0 || placed_on.room.height <= 0)
            return {};
        const int32_t percent = std::max(placed_on.scale_percent, int32_t{100});
        const kit::Point size = points_of(placed_on);
        const kit::Rect at = kit::to_canvas({0, 0, size.x, size.y}, percent);
        return {{placed_on.room.x, placed_on.room.y, at.width, at.height}, size.x, size.y, percent};
    }

    /// Tells that the screen lets the screens over it take what they show.
    ///
    /// @return false
    [[nodiscard]] bool modal() const override { return false; }

    /// Tells that the screen darkens nothing.
    ///
    /// @return false
    [[nodiscard]] bool backdrop() const override { return false; }

    /// Draws the background, the title, the buttons and the version plate.
    ///
    /// @param canvas where it draws, at the layer's scale
    void draw(const kit::Canvas& canvas) const override {
        const MenuArt& art = menu_art();
        draw_backdrop(canvas, art);
        draw_art(canvas, layout_.title, art.title);
        for (std::size_t index = 0; index < entry_count; ++index)
            draw_button(canvas, index, art);
        draw_plate(canvas);
    }

    /// Follows the view: the room's size in the layer's points.
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override {
        if (placed_on.match)
            return;
        const kit::Point size = points_of(placed_on);
        if (size.x == size_.x && size.y == size_.y && laid_out_)
            return;
        size_ = size;
        const MenuArt& art = menu_art();
        layout_ = lay_out_menu(
            size_.x,
            size_.y,
            {static_cast<int32_t>(art.title.width), static_cast<int32_t>(art.title.height)}
        );
        laid_out_ = true;
        fitted_.clear();
        backdrop_.clear();
        ++revision_;
    }

    /// Follows the pointer over the buttons and presses the one a press is
    /// released on. Takes every pointer, so that MAINMENU.GUI under it sees
    /// none.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button, 1 for the left
    /// @param at the pointer, in the screen's points
    /// @return redraw when the look changed; none otherwise
    LayerAnswer pointer(
        ScreenInputKind kind, uint8_t button, kit::Point at, int32_t /*finger_reach*/
    ) override {
        const uint64_t before = revision_;
        const int32_t over = button_at(at);
        set_hovered(over);
        if (over >= 0 && over != focus_) {
            focus_ = over;
            ++revision_;
        }
        if (kind == ScreenInputKind::pointer_down && button == SDL_BUTTON_LEFT) {
            set_pressed(over);
        } else if (kind == ScreenInputKind::pointer_up && button == SDL_BUTTON_LEFT) {
            const int32_t held = pressed_;
            set_pressed(-1);
            if (held >= 0 && held == over) {
                press(held);
                return LayerAnswer::redraw;
            }
        }
        return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
    }

    /// Moves the focus with the arrows and Tab, and presses the focused
    /// button with Return and Space. Lets every other key go on.
    ///
    /// @param pressed the key
    /// @return redraw when the focus moved or a button was pressed; pass otherwise
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        switch (pressed) {
        case kit::Key::up:
        case kit::Key::back_tab:
            move_focus(-1);
            return LayerAnswer::redraw;
        case kit::Key::down:
        case kit::Key::tab:
            move_focus(1);
            return LayerAnswer::redraw;
        case kit::Key::enter:
        case kit::Key::space:
            focus_shown_ = true;
            press(focus_);
            ++revision_;
            return LayerAnswer::redraw;
        default:
            return LayerAnswer::pass;
        }
    }

    /// Takes the wheel, which turns nothing here.
    ///
    /// @return none
    LayerAnswer wheel(kit::Point /*at*/, float /*notches*/) override { return LayerAnswer::none; }

    /// Returns the buttons, one named control each, for automation.
    ///
    /// @return the list
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList list;
        for (std::size_t index = 0; index < entry_count; ++index) {
            kit::Control control;
            control.id = static_cast<kit::ControlId>(index);
            control.rect = layout_.buttons[index];
            control.enabled = facts_now_.enabled[index];
            control.name =
                std::string(main_menu_screen_name) + "." + std::string(entry_words[index].name);
            control.kind = kit::ControlKind::button;
            control.text = caption(index);
            list.controls.push_back(std::move(control));
            list.tab_order.push_back(static_cast<kit::ControlId>(index));
        }
        return list;
    }

    /// Returns the hovered, pressed and focused buttons.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override {
        kit::Interaction shown;
        shown.hovered = hovered_;
        shown.pressed = pressed_;
        shown.focused = focus_;
        shown.focus_shown = focus_shown_;
        return shown;
    }

    /// Closes the screen when the main menu goes, and follows what the
    /// game's main menu shows: which buttons take a press, and its label.
    ///
    /// @return close when the menu no longer shows; redraw when what it
    ///     shows changed; otherwise none
    LayerAnswer tick() override {
        if (!shows_())
            return LayerAnswer::close;
        const uint64_t before = revision_;
        MenuFacts now = facts_();
        if (!(now == facts_now_)) {
            facts_now_ = std::move(now);
            if (!facts_now_.enabled[static_cast<std::size_t>(focus_)])
                focus_ = next_enabled(focus_, 1);
            ++revision_;
        }
        return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
    }

    /// Returns a number that changes with what the screen draws.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Tells that the screen draws nothing that moves on its own.
    ///
    /// @return true
    [[nodiscard]] bool revision_covers_drawing() const override { return true; }

    /// Has nothing of its own to close.
    void close(bool /*by_key*/) override {}

  private:

    /// Returns the room's size in the view's points, rounded up, so that the
    /// screen covers every pixel of the room.
    ///
    /// @param placed_on what the screen is placed on
    /// @return the width and height, in points
    [[nodiscard]] static kit::Point points_of(const LayerView& placed_on) noexcept {
        const int64_t percent = std::max(placed_on.scale_percent, int32_t{100});
        const auto up = [percent](int32_t pixels) {
            return static_cast<int32_t>(
                (static_cast<int64_t>(pixels) * 100 + percent - 1) / percent
            );
        };
        return {up(placed_on.room.width), up(placed_on.room.height)};
    }

    /// Returns a button's caption, in the language shown.
    ///
    /// @param index the button
    /// @return the caption
    [[nodiscard]] static std::string caption(std::size_t index) {
        return std::string(oa::data::languages::interface_text(entry_words[index].english));
    }

    /// Returns the enabled button under a point.
    ///
    /// @param at the point, in the screen's points
    /// @return the button; -1 for none
    [[nodiscard]] int32_t button_at(kit::Point at) const noexcept {
        for (std::size_t index = 0; index < entry_count; ++index)
            if (facts_now_.enabled[index] && kit::contains(layout_.buttons[index], at))
                return static_cast<int32_t>(index);
        return -1;
    }

    /// Returns the next enabled button from one, a step at a time, round
    /// the column.
    ///
    /// @param from the button to step from
    /// @param step 1 down, -1 up
    /// @return the button; from itself when no other is enabled
    [[nodiscard]] int32_t next_enabled(int32_t from, int32_t step) const noexcept {
        const auto count = static_cast<int32_t>(entry_count);
        for (int32_t moved = 1; moved <= count; ++moved) {
            const int32_t index = ((from + step * moved) % count + count) % count;
            if (facts_now_.enabled[static_cast<std::size_t>(index)])
                return index;
        }
        return from;
    }

    /// Moves the focus a step, and shows it.
    ///
    /// @param step 1 down, -1 up
    void move_focus(int32_t step) {
        // The first key shows the focus where it is; the next ones move it.
        if (focus_shown_)
            focus_ = next_enabled(focus_, step);
        focus_shown_ = true;
        ++revision_;
    }

    /// Sets the hovered button, counting a change.
    ///
    /// @param index the button; -1 for none
    void set_hovered(int32_t index) {
        if (index == hovered_)
            return;
        hovered_ = index;
        ++revision_;
    }

    /// Sets the held button, counting a change.
    ///
    /// @param index the button; -1 for none
    void set_pressed(int32_t index) {
        if (index == pressed_)
            return;
        pressed_ = index;
        ++revision_;
    }

    /// Presses an enabled button.
    ///
    /// @param index the button
    void press(int32_t index) {
        if (index < 0 || static_cast<std::size_t>(index) >= entry_count ||
            !facts_now_.enabled[static_cast<std::size_t>(index)])
            return;
        press_(static_cast<MenuEntry>(index));
    }

    /// Draws a picture of the art scaled to fill a rectangle, from a copy of
    /// it filtered to the pixels the rectangle covers, made the first time it
    /// is drawn there and kept until the screen's size changes: a redraw,
    /// such as the one a button the pointer comes over asks for, copies the
    /// pixels and does not filter the large pictures again.
    ///
    /// @param canvas where it draws
    /// @param rect the rectangle, in points
    /// @param picture the picture
    void draw_art(const kit::Canvas& canvas, const kit::Rect& rect, const WindowIcon& picture)
        const {
        const kit::Point covered = kit::covered_pixels(canvas, rect);
        if (covered.x <= 0 || covered.y <= 0 || picture.width == 0 || picture.height == 0)
            return;
        auto kept = std::find_if(fitted_.begin(), fitted_.end(), [&](const FittedArt& fitted) {
            return fitted.picture == &picture && fitted.columns == covered.x &&
                   fitted.rows == covered.y;
        });
        if (kept == fitted_.end()) {
            if (fitted_.size() >= most_fitted_art)
                fitted_.clear();
            FittedArt fitted;
            fitted.picture = &picture;
            fitted.columns = covered.x;
            fitted.rows = covered.y;
            fitted.pixels = oa::ui::paint::resized_rgba(
                picture.pixels,
                static_cast<int>(picture.width),
                static_cast<int>(picture.height),
                covered.x,
                covered.y
            );
            fitted_.push_back(std::move(fitted));
            kept = std::prev(fitted_.end());
        }
        if (kept->pixels.empty()) {
            kit::draw_picture(canvas, rect, picture_of(picture));
            return;
        }
        kit::draw_picture(
            canvas,
            rect,
            {static_cast<uint32_t>(kept->columns),
             static_cast<uint32_t>(kept->rows),
             kept->pixels}
        );
    }

    /// Draws what lies under the title and the buttons: the ground, the
    /// picture over it cut by the screen's edges, and the shade over its top
    /// and bottom, so that the title and the version plate stand out of it.
    /// They are put together once, at the pixels the screen covers, and kept
    /// until its size changes: a redraw, such as the one a button the
    /// pointer comes over asks for, copies them.
    ///
    /// @param canvas where it draws
    /// @param art the menu's art
    void draw_backdrop(const kit::Canvas& canvas, const MenuArt& art) const {
        const kit::Rect whole{0, 0, size_.x, size_.y};
        const kit::Point covered = kit::covered_pixels(canvas, whole);
        if (covered.x <= 0 || covered.y <= 0 || size_.x <= 0 || size_.y <= 0)
            return;
        if (backdrop_size_.x != covered.x || backdrop_size_.y != covered.y || backdrop_.empty())
            backdrop_ = backdrop_pixels(canvas, covered, art.background);
        backdrop_size_ = covered;
        kit::draw_picture(
            canvas,
            whole,
            {static_cast<uint32_t>(covered.x), static_cast<uint32_t>(covered.y), backdrop_}
        );
    }

    /// Puts the backdrop together at the pixels the screen covers
    /// (draw_backdrop): the ground, the picture over it, and the shade's
    /// bands, each band's opacity falling off as the square of its distance
    /// from the edge, from the edge's opacity to none.
    ///
    /// @param canvas where it is drawn
    /// @param covered the pixels the screen covers, across and down
    /// @param picture the background's picture
    /// @return four bytes a pixel, every one opaque
    [[nodiscard]] std::vector<uint8_t> backdrop_pixels(
        const kit::Canvas& canvas, kit::Point covered, const WindowIcon& picture
    ) const {
        const auto columns = static_cast<std::size_t>(covered.x);
        const auto rows = static_cast<std::size_t>(covered.y);
        std::vector<uint8_t> pixels(columns * rows * 4U);
        for (std::size_t at = 0; at < columns * rows; ++at) {
            pixels[at * 4U] = ground_colour.r;
            pixels[at * 4U + 1U] = ground_colour.g;
            pixels[at * 4U + 2U] = ground_colour.b;
            pixels[at * 4U + 3U] = 255;
        }
        // A point is this many of the screen's pixels across and down.
        const double across = static_cast<double>(covered.x) / size_.x;
        const double down = static_cast<double>(covered.y) / size_.y;
        const kit::Point fitted = kit::covered_pixels(canvas, layout_.background);
        if (fitted.x > 0 && fitted.y > 0 && picture.width > 0 && picture.height > 0) {
            const std::vector<uint8_t> scaled = oa::ui::paint::resized_rgba(
                picture.pixels,
                static_cast<int>(picture.width),
                static_cast<int>(picture.height),
                fitted.x,
                fitted.y
            );
            const auto left = static_cast<int64_t>(std::lround(layout_.background.x * across));
            const auto top = static_cast<int64_t>(std::lround(layout_.background.y * down));
            const int64_t first = std::max<int64_t>(left, 0);
            const int64_t end = std::min<int64_t>(left + fitted.x, covered.x);
            if (scaled.size() == static_cast<std::size_t>(fitted.x) * fitted.y * 4U && end > first)
                for (int64_t row = std::max<int64_t>(top, 0);
                     row < std::min<int64_t>(top + fitted.y, covered.y);
                     ++row) {
                    const uint8_t* from =
                        scaled.data() +
                        static_cast<std::size_t>((row - top) * fitted.x + (first - left)) * 4U;
                    uint8_t* to =
                        pixels.data() + static_cast<std::size_t>(row * covered.x + first) * 4U;
                    for (int64_t column = first; column < end; ++column, from += 4, to += 4) {
                        const uint32_t alpha = from[3];
                        for (std::size_t channel = 0; channel < 3U; ++channel)
                            to[channel] = static_cast<uint8_t>(
                                (from[channel] * alpha + to[channel] * (255U - alpha) + 127U) /
                                255U
                            );
                    }
                }
        }
        const auto band = [&](double from, double to, uint32_t opacity) {
            if (opacity == 0)
                return;
            const auto first = static_cast<int64_t>(std::lround(nearest(from) * down));
            const auto end = static_cast<int64_t>(std::lround(nearest(to) * down));
            for (int64_t row = std::max<int64_t>(first, 0); row < std::min<int64_t>(end, covered.y);
                 ++row) {
                uint8_t* shaded = pixels.data() + static_cast<std::size_t>(row) * columns * 4U;
                for (std::size_t column = 0; column < columns; ++column, shaded += 4) {
                    shaded[0] = static_cast<uint8_t>(
                        (shade_colour.r * opacity + shaded[0] * (256U - opacity)) >> 8U
                    );
                    shaded[1] = static_cast<uint8_t>(
                        (shade_colour.g * opacity + shaded[1] * (256U - opacity)) >> 8U
                    );
                    shaded[2] = static_cast<uint8_t>(
                        (shade_colour.b * opacity + shaded[2] * (256U - opacity)) >> 8U
                    );
                }
            }
        };
        const auto screen_rows = static_cast<double>(size_.y);
        const double top_rows = screen_rows * top_shade_share;
        const double bottom_rows = screen_rows * bottom_shade_share;
        for (int32_t index = 0; index < shade_bands; ++index) {
            const double near = 1.0 - (static_cast<double>(index) + 0.5) / shade_bands;
            const double share = near * near;
            band(
                top_rows * index / shade_bands,
                top_rows * (index + 1) / shade_bands,
                static_cast<uint32_t>(std::lround(top_shade_opacity * share))
            );
            band(
                screen_rows - bottom_rows * (index + 1) / shade_bands,
                screen_rows - bottom_rows * index / shade_bands,
                static_cast<uint32_t>(std::lround(bottom_shade_opacity * share))
            );
        }
        return pixels;
    }

    /// Draws one button: its face in its look, its caption outlined, and
    /// the quick key's underline.
    ///
    /// @param canvas where it draws
    /// @param index the button
    /// @param art the menu's art
    void draw_button(const kit::Canvas& canvas, std::size_t index, const MenuArt& art) const {
        const auto at = static_cast<int32_t>(index);
        const bool enabled = facts_now_.enabled[index];
        const bool held = pressed_ == at && hovered_ == at;
        const bool lit = hovered_ == at || (focus_ == at && (focus_shown_ || hovered_ < 0));
        Face face = Face::normal;
        if (!enabled)
            face = Face::disabled;
        else if (held)
            face = Face::pressed;
        else if (lit)
            face = Face::hover;
        const kit::Rect& rect = layout_.buttons[index];
        draw_art(canvas, rect, art.faces[static_cast<std::size_t>(face)]);

        const std::string text = caption(index);
        kit::Rect box = rect;
        // A held button's caption sinks a point right and down, as the
        // game's do.
        if (held) {
            box.x += 1;
            box.y += 1;
        }
        const kit::Colour ink = !enabled ? caption_faded_colour
                                : lit    ? caption_lit_colour
                                         : caption_colour;
        constexpr std::array<kit::Point, 8> around{
            {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}}
        };
        for (const kit::Point step : around)
            kit::box_text(
                canvas,
                kit::FontRole::regular,
                text,
                {box.x + step.x, box.y + step.y, box.width, box.height},
                kit::Align::centre,
                outline_colour
            );
        kit::box_text(canvas, kit::FontRole::regular, text, box, kit::Align::centre, ink);

        const auto key = quick_key_at(text, facts_now_.quick_keys[index]);
        if (!enabled || !key)
            return;
        const int32_t width = kit::measure_text(canvas, kit::FontRole::regular, text);
        const int32_t before =
            kit::measure_text(canvas, kit::FontRole::regular, text.substr(0, *key));
        const int32_t letter =
            kit::measure_text(canvas, kit::FontRole::regular, text.substr(*key, 1));
        const int32_t left = box.x + (box.width - width) / 2 + before;
        // Under the capitals, which box_text centres on the box's height.
        const int32_t row =
            box.y + box.height / 2 + kit::smooth_text_points(kit::FontRole::regular) / 2;
        kit::fill_rect(canvas, {left - 1, row, letter + 2, 3}, outline_colour);
        kit::fill_rect(canvas, {left, row + 1, letter, 1}, key_colour);
    }

    /// Draws the version plate under the buttons, as wide as its label.
    ///
    /// @param canvas where it draws
    void draw_plate(const kit::Canvas& canvas) const {
        if (facts_now_.version.empty())
            return;
        const int32_t text_width =
            kit::measure_text(canvas, kit::FontRole::small, facts_now_.version);
        const int32_t width = std::min(text_width + 2 * layout_.plate_margin, size_.x);
        const kit::Rect plate{
            (size_.x - width) / 2, layout_.plate_top, width, layout_.plate_height
        };
        kit::blend_rect(canvas, plate, plate_colour, plate_opacity);
        kit::outline_rect(canvas, plate, plate_edge_colour);
        kit::box_text(
            canvas,
            kit::FontRole::small,
            facts_now_.version,
            plate,
            kit::Align::centre,
            plate_text_colour
        );
    }

    std::function<bool()> shows_{};          ///< whether the menu shows
    std::function<MenuFacts()> facts_{};     ///< reads the game's main menu
    std::function<void(MenuEntry)> press_{}; ///< presses a button
    MenuFacts facts_now_{};                  ///< what the game's main menu shows, as last read
    kit::Point size_{};                      ///< the screen's size, in points
    MenuLayout layout_{};                    ///< where its parts lie
    bool laid_out_{};                        ///< layout_ is for size_
    int32_t hovered_{-1};                    ///< the button under the pointer; -1 for none
    int32_t pressed_{-1};                    ///< the button a press holds; -1 for none
    int32_t focus_{};                        ///< the button the keys act on
    bool focus_shown_{};                     ///< a key has shown the focus
    /// The pictures of the art filtered to the sizes they are drawn at.
    mutable std::vector<FittedArt> fitted_{};
    /// The backdrop at the pixels the screen covers (draw_backdrop), four
    /// bytes a pixel; empty until it is first drawn at the screen's size.
    mutable std::vector<uint8_t> backdrop_{};
    mutable kit::Point backdrop_size_{}; ///< the pixels backdrop_ covers, across and down
    uint64_t revision_{1};                   ///< counts the changes to what it draws
};

/// Tells whether two names of MAINMENU.GUI's records are the same, as the
/// game's own lookup compares them, in either case.
///
/// @param first a name
/// @param second another
/// @return true when they are
bool same_record_name(std::string_view first, std::string_view second) noexcept {
    if (first.size() != second.size())
        return false;
    const auto lower = [](char letter) {
        return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
    };
    for (std::size_t index = 0; index < first.size(); ++index)
        if (lower(first[index]) != lower(second[index]))
            return false;
    return true;
}

/// Returns a record of a layout by its name.
///
/// @param gadgets the layout's records
/// @param name the name
/// @return its place among them; none without such a record
std::optional<std::size_t>
record_named(std::span<const oa::ui::gui_layout::Gadget> gadgets, std::string_view name) {
    if (name.empty())
        return std::nullopt;
    for (std::size_t index = 0; index < gadgets.size(); ++index)
        if (same_record_name(gadgets[index].common.name, name))
            return index;
    return std::nullopt;
}

} // namespace

bool Runtime::oa_main_menu_shown() {
    return oa_layer().find(main_menu_screen_name) != nullptr;
}

bool Runtime::main_menu_covers_front_end() {
    if (screen_ != Screen::main_menu || sdl_.renderer == nullptr || options_.classic_main_menu ||
        main_menu_overlay_ || frame_owned_by_package() || frame_without_cursor_)
        return false;
    OaLayer& layer = oa_layer();
    const LayerScreen* menu = layer.find(main_menu_screen_name);
    if (menu == nullptr || !menu->opaque())
        return false;
    const LayerView seen = layer.view();
    const auto& shown = menu->placement(seen).shown;
    const auto& picture = seen.picture;
    return shown.width > 0 && shown.height > 0 && shown.x <= picture.x && shown.y <= picture.y &&
           shown.x + shown.width >= picture.x + picture.width &&
           shown.y + shown.height >= picture.y + picture.height;
}

bool Runtime::press_main_menu_gadget(std::string_view name) {
    if (screen_ != Screen::main_menu)
        return false;
    const auto found = record_named(resources_.layout.gadgets, name);
    if (!found || resources_.layout.gadgets[*found].common.active == 0 ||
        !frontend_gadget_pressable(*found))
        return false;
    // As a click released on the record: the record is the selection while
    // the main menu's handler reads it.
    selected_ = static_cast<int32_t>(*found);
    activate();
    selected_ = -1;
    return true;
}

void Runtime::keep_main_menu_screen() {
    const auto shows = [this] {
        return screen_ == Screen::main_menu && !options_.classic_main_menu && !main_menu_overlay_ &&
               !frame_owned_by_package() && oa_layer().screen_fonts() != nullptr;
    };
    if (!shows())
        return;
    OaLayer& layer = oa_layer();
    if (layer.find(main_menu_screen_name) != nullptr)
        return;
    // The record each button of the game's presses, in MenuEntry's order;
    // Library and Settings press none.
    const auto record_of = [this](MenuEntry entry) -> std::string_view {
        switch (entry) {
        case MenuEntry::single_player:
            return menu::resource_name(menu::Button::single_player);
        case MenuEntry::multiplayer:
            return menu::resource_name(menu::Button::multiplayer);
        case MenuEntry::intro:
            return menu::resource_name(menu::Button::intro);
        case MenuEntry::credits:
            return view_rules::credits_gadget(mod_profile());
        case MenuEntry::exit:
            return menu::resource_name(menu::Button::exit);
        case MenuEntry::library:
        case MenuEntry::settings:
            break;
        }
        return {};
    };
    const auto facts = [this, record_of] {
        MenuFacts now;
        const auto& gadgets = resources_.layout.gadgets;
        for (std::size_t index = 0; index < entry_count; ++index) {
            const auto entry = static_cast<MenuEntry>(index);
            if (entry == MenuEntry::library || entry == MenuEntry::settings) {
                now.enabled[index] = true;
                continue;
            }
            const auto found = record_named(gadgets, record_of(entry));
            if (!found || gadgets[*found].common.active == 0)
                continue;
            now.enabled[index] = frontend_gadget_pressable(*found);
            if (const auto* button =
                    std::get_if<oa::ui::gui_layout::ButtonFields>(&gadgets[*found].fields))
                now.quick_keys[index] = static_cast<char>(button->quick_key);
        }
        const auto label = record_named(gadgets, oa::ui::frontend::kMainMenuVersionControl);
        if (label && gadgets[*label].common.active != 0)
            if (const auto* text =
                    std::get_if<oa::ui::gui_layout::LabelFields>(&gadgets[*label].fields))
                now.version = text->text;
        return now;
    };
    const auto press = [this, record_of](MenuEntry entry) {
        if (entry == MenuEntry::library)
            open_library(std::nullopt);
        else if (entry == MenuEntry::settings)
            open_engine_settings_from_menu();
        else
            press_main_menu_gadget(record_of(entry));
    };
    layer.push_under(std::make_unique<MainMenuScreen>(shows, facts, press));
}

} // namespace oa::app
