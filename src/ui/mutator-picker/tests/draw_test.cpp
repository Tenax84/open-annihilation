// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutator picker drawn in the installed game's fonts, over a made-up
// set: Compact in the 640x480 window at 1x, Regular in 1920x1080 and Large
// in 2560x1440 at 2x, Compact's details page, a drag at Regular, read-only
// at Regular and Compact in French. Each is laid out and drawn twice into a
// picture with a margin round the window: the two drawings are byte for byte
// the same, the margin keeps its colour, and every pixel of the window is
// drawn. In the game's fonts every text fits and every part lies in the
// window. The characters the game's fonts lack are drawn in the modern fonts
// beside the test, as the game draws them. Given --frames DIR, it writes
// mutator-picker-compact.png, mutator-picker-regular.png,
// mutator-picker-large.png, mutator-picker-details-compact.png,
// mutator-picker-drag-regular.png, mutator-picker-view-regular.png and
// mutator-picker-fr-compact.png.
//
//   oa-ui-mutator-picker-draw-test --data [--frames DIR]

#include "oa/formats/png.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/present/game_text.hpp"
#include "oa/test/check.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/test/game_data.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/mutator_picker/model.hpp"
#include "oa/ui/mutator_picker/screen.hpp"
#include "oa/ui/mutator_picker/text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace kit = oa::ui::kit;
namespace picker = oa::ui::mutator_picker;
namespace lib = oa::ui::library;
namespace renderer = oa::ui::frontend_renderer;
namespace png = oa::formats::png;
namespace text_font = oa::platform::text_font;

/// The points of margin drawn round the window, which nothing may touch.
constexpr int32_t margin_points = 4;
/// The colour the picture starts as. Only this test names it; the screen uses kit tokens.
constexpr kit::Colour untouched{0x12, 0x34, 0x56};

/// Draws a line in the modern fonts, as the game does for the characters its
/// fonts lack.
///
/// @param context the font stack
/// @param text the line, in UTF-8
/// @param face the game font it stands in for
/// @param scale screen pixels to a game pixel
/// @param size the text size, in percent
/// @return the line's mask; null when the fonts cannot draw it
std::shared_ptr<const oa::present::TextMask> draw_modern(
    void* context, std::string_view text, oa::present::TextFace face, int32_t scale, int32_t size
) {
    auto& stack = *static_cast<text_font::FontStack*>(context);
    if (text.empty() || text.size() > text_font::max_text_bytes)
        return nullptr;
    const bool label = face == oa::present::TextFace::label;
    const int32_t face_pixels =
        face == oa::present::TextFace::message  ? text_font::message_log_pixel_size
        : face == oa::present::TextFace::status ? text_font::status_readout_pixel_size
                                                : text_font::label_pixel_size;
    text_font::Style style;
    style.pixel_size = std::clamp(
        oa::present::face_pixel_size(face_pixels, scale, size),
        int32_t{1},
        text_font::max_pixel_size
    );
    style.weight = label ? text_font::Weight::regular : text_font::Weight::bold;
    style.rendering = text_font::Rendering::mono;
    const auto drawn = stack.draw(text, style);
    const auto placements = stack.layout(text, style);
    const auto characters = text_font::decode_utf8(text);
    if (!drawn || !placements || !characters)
        return nullptr;
    auto mask = std::make_shared<oa::present::TextMask>();
    mask->width = drawn->width;
    mask->height = drawn->height;
    mask->baseline = drawn->baseline;
    mask->origin = drawn->origin;
    mask->advance = drawn->advance;
    mask->alpha = drawn->alpha;
    std::size_t placed = 0;
    int32_t pen = 0;
    for (const char32_t character : *characters) {
        if (!text_font::is_invisible(character) && placed < placements->size()) {
            const auto& placement = (*placements)[placed++];
            pen = placement.pen + placement.advance;
        }
        mask->character_ends.push_back(pen);
    }
    return mask;
}

/// The French of the mockup, for the French picture only.
///
/// @param context unused
/// @param english the English word
/// @return the French, or the English when this picture leaves it
std::string_view french_shown(void* /*context*/, std::string_view english) {
    using picker::words::cancel;
    using picker::words::details_button;
    using picker::words::filter_all;
    using picker::words::heading_off;
    using picker::words::heading_on;
    using picker::words::no_mod;
    using picker::words::search_mutators;
    using picker::words::setup_skirmish;
    using picker::words::switch_on;
    using picker::words::title_mutators;
    using picker::words::use_these;
    if (english == title_mutators)
        return "Mutateurs";
    if (english == setup_skirmish)
        return "Escarmouche";
    if (english == no_mod)
        return "Sans mod";
    if (english == filter_all)
        return "Tous";
    if (english == search_mutators)
        return "Rechercher des mutateurs";
    if (english == heading_on)
        return "Actifs · appliqués de haut en bas";
    if (english == heading_off)
        return "Non";
    if (english == switch_on)
        return "Oui";
    if (english == details_button)
        return "Détails";
    if (english == cancel)
        return "Annuler";
    if (english == use_these)
        return "Valider";
    return english;
}

/// A made-up mutator.
///
/// @param id the id
/// @param name the name
/// @param installed true when it is installed
/// @return the mutator
picker::Mutator made(std::string id, std::string name, bool installed) {
    picker::Mutator mutator;
    mutator.id = std::move(id);
    mutator.name = std::move(name);
    mutator.installed = installed;
    mutator.version = "2.0";
    mutator.author = "Ridge";
    return mutator;
}

/// The picture's picker. Scout Post is selected.
///
/// @param text how texts are looked up
/// @param read_only true when a guest is looking
/// @return the opened picker
picker::Picker opened(lib::TextHooks text, bool read_only) {
    picker::Inputs inputs;
    inputs.setup = picker::Setup::skirmish;
    inputs.mod_name = "Ridge";
    inputs.remembered = true;
    inputs.read_only = read_only;
    inputs.text = text;

    picker::Mutator builders = made("fast-builders", "Fast Builders", true);
    for (int time = 0; time < 40; ++time)
        builders.summary += "ridge ";
    builders.tags = {"economy", "ridge-watch-outpost"};
    builders.verdict.state = lib::Verdict::State::compatible;
    builders.verdict.summary = "Changes the ridge.";

    picker::Mutator scout = made("scout-post", "Scout Post", true);
    scout.summary = "A post on the ridge.";
    for (int time = 0; time < 12; ++time)
        scout.description += "The scout watches the ridge. ";
    scout.tags = {"gameplay", "statistics"};
    scout.layers = {"units", "weapons"};
    picker::OptionSpec speed;
    speed.id = "speed";
    speed.type = picker::OptionSpec::Type::choice;
    speed.label = "Speed";
    speed.values = {"1x", "2x", "4x"};
    speed.captions = speed.values;
    speed.fallback = "2x";
    picker::OptionSpec signal;
    signal.id = "signal";
    signal.type = picker::OptionSpec::Type::choice;
    signal.label = "Signal";
    signal.values = {"North", "South", "East", "West", "Near", "Far"};
    signal.captions = signal.values;
    signal.fallback = "North";
    picker::OptionSpec alarm;
    alarm.id = "alarm";
    alarm.type = picker::OptionSpec::Type::toggle;
    alarm.label = "Alarm";
    alarm.values = {"No", "Yes"};
    alarm.fallback = "No";
    picker::OptionSpec range;
    range.id = "range";
    range.type = picker::OptionSpec::Type::number;
    range.label = "Range";
    range.least = "0";
    range.most = "10";
    range.step = "2";
    range.fallback = "4";
    scout.options.push_back(std::move(speed));
    scout.options.push_back(std::move(signal));
    scout.options.push_back(std::move(alarm));
    scout.options.push_back(std::move(range));
    scout.verdict.state = lib::Verdict::State::compatible;
    scout.verdict.summary = "Changes the ridge.";

    picker::Mutator gatehouse = made("gatehouse", "Gatehouse", true);
    gatehouse.summary = "The gate on the ridge.";
    gatehouse.tags = {"ai"};
    gatehouse.verdict.state = lib::Verdict::State::compatible;

    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep", false);
    keep.summary = "The keep above the gate.";
    lib::Prerequisite need;
    need.id = "gatehouse";
    keep.needs.push_back(std::move(need));
    keep.verdict.state = lib::Verdict::State::compatible;

    picker::Mutator walls = made("ridge-walls", "Ridge Walls", true);
    walls.summary = "Walls along the ridge.";
    walls.unavailable = "The ridge has no gate";

    picker::Mutator grounded = made("grounded", "Grounded", false);
    grounded.summary = "Nothing leaves the ground.";

    const auto choose = [](std::string id) {
        picker::Chosen chosen;
        chosen.id = std::move(id);
        return chosen;
    };
    inputs.last_set.push_back(choose("fast-builders"));
    inputs.last_set.push_back(choose("scout-post"));
    inputs.last_set.push_back(choose("gatehouse"));
    inputs.last_set.push_back(choose("gatehouse-keep"));
    inputs.mutators.push_back(std::move(builders));
    inputs.mutators.push_back(std::move(scout));
    inputs.mutators.push_back(std::move(gatehouse));
    inputs.mutators.push_back(std::move(keep));
    inputs.mutators.push_back(std::move(walls));
    inputs.mutators.push_back(std::move(grounded));

    picker::Picker model;
    picker::open(model, std::move(inputs));
    model.selected = "scout-post";
    return model;
}

/// Returns the part of an item that is drawn.
///
/// @param item the item
/// @return the rectangle
kit::Rect drawn_area(const kit::Item& item) {
    kit::Rect area = item.rect;
    if (item.role == kit::Role::focus_ring) {
        const auto* look = std::get_if<kit::FocusRingLook>(&item.look);
        if (look != nullptr && look->around)
            area = kit::grown(area, kit::compact_metrics.focus_inset);
    }
    if (item.clip.width > 0 && item.clip.height > 0)
        area = kit::intersect(area, item.clip);
    return area;
}

/// Returns the part of a control a press reaches.
///
/// @param control the control
/// @return its rectangle, within its clip when it has one
kit::Rect press_area(const kit::Control& control) {
    if (control.clip.width > 0 && control.clip.height > 0)
        return kit::intersect(control.rect, control.clip);
    return control.rect;
}

/// Tells whether a rectangle holds any point.
///
/// @param rect the rectangle
/// @return true when it is wide and high
bool has_room(const kit::Rect& rect) {
    return rect.width > 0 && rect.height > 0;
}

/// Lists the parts of a layout that lie outside its window.
///
/// @param list the display list
/// @param window the window's size
/// @return a line for each part outside
std::vector<std::string> outside_window(const kit::DisplayList& list, kit::Point window) {
    const kit::Rect whole{0, 0, window.x, window.y};
    std::vector<std::string> problems;
    for (const kit::Item& item : list.items) {
        const kit::Rect area = drawn_area(item);
        if (has_room(area) && !kit::wholly_in(area, whole))
            problems.push_back("item \"" + item.text + "\"");
    }
    for (const kit::Control& control : list.controls) {
        const kit::Rect area = press_area(control);
        if (has_room(area) && !kit::wholly_in(area, whole))
            problems.push_back("control " + control.name);
    }
    return problems;
}

/// Lists the texts that do not fit, by the kit's measure, in the game's fonts.
///
/// @param list the display list
/// @param measured the fonts
/// @return a line for each text that does not fit
std::vector<std::string>
texts_not_fitting(const kit::DisplayList& list, const kit::Fonts& measured) {
    const kit::Metrics& metrics = kit::compact_metrics;
    std::vector<std::string> problems;
    const auto check = [&](bool fits, const std::string& what) {
        if (!fits)
            problems.push_back(what);
    };
    const auto width = [&measured](kit::FontRole role, std::string_view text) {
        return kit::text_width(measured, role, text);
    };
    for (const kit::Item& item : list.items) {
        switch (item.role) {
        case kit::Role::text:
            check(width(item.font, item.text) <= item.rect.width, "text \"" + item.text + "\"");
            break;
        case kit::Role::button:
            if (const auto* look = std::get_if<kit::ButtonLook>(&item.look))
                check(
                    width(kit::FontRole::small, look->caption) <= item.rect.width,
                    "button \"" + look->caption + "\""
                );
            break;
        case kit::Role::chip:
            if (const auto* look = std::get_if<kit::ChipLook>(&item.look))
                check(
                    kit::chip_width(measured, *look) <= item.rect.width,
                    "chip \"" + look->text + "\""
                );
            break;
        case kit::Role::pill:
            if (const auto* look = std::get_if<kit::PillLook>(&item.look))
                check(
                    kit::pill_width(measured, *look) <= item.rect.width,
                    "pill \"" + look->text + "\""
                );
            break;
        case kit::Role::choice:
            if (const auto* look = std::get_if<kit::ChoiceLook>(&item.look))
                check(
                    width(kit::FontRole::regular, look->text) <=
                        item.rect.width - metrics.choice_text_inset - metrics.choice_arrow_room,
                    "drop-down \"" + look->text + "\""
                );
            break;
        case kit::Role::choice_menu:
            if (const auto* look = std::get_if<kit::ChoiceMenuLook>(&item.look))
                for (std::size_t place = 0; place < look->shown.size(); ++place) {
                    const kit::Rect box = kit::choice_item(item.rect, static_cast<int32_t>(place));
                    check(
                        width(kit::FontRole::regular, look->shown[place]) <=
                            box.width - metrics.choice_item_text_inset,
                        "menu item \"" + look->shown[place] + "\""
                    );
                }
            break;
        case kit::Role::header:
            if (const auto* look = std::get_if<kit::HeaderLook>(&item.look)) {
                check(
                    kit::tracked_width(
                        measured, kit::FontRole::regular, look->title, look->tracking
                    ) <= look->title_width,
                    "header title \"" + look->title + "\""
                );
                check(
                    width(kit::FontRole::small, look->version) <= look->version_width,
                    "header version \"" + look->version + "\""
                );
                const int32_t title_left = metrics.padding + metrics.mark_side + metrics.header_gap;
                const int32_t words_right = title_left + look->title_width;
                const int32_t version_left =
                    look->width - metrics.edge - metrics.padding - look->version_width;
                check(words_right <= version_left, "header words reach the version");
            }
            break;
        case kit::Role::heading:
            if (const auto* look = std::get_if<kit::HeadingLook>(&item.look))
                check(
                    kit::tracked_width(
                        measured, kit::FontRole::small, look->text, metrics.heading_tracking
                    ) <= item.rect.width,
                    "heading \"" + look->text + "\""
                );
            break;
        case kit::Role::field:
            if (const auto* look = std::get_if<kit::SearchLook>(&item.look);
                look != nullptr && look->field.text.empty())
                check(
                    width(kit::FontRole::regular, look->placeholder) <=
                        kit::field_text_rect(item.rect, *look).width,
                    "placeholder \"" + look->placeholder + "\""
                );
            break;
        default:
            break;
        }
    }
    return problems;
}

/// Which picture is drawn.
enum class SceneKind : uint8_t {
    list,    ///< the list and, at Regular and Large, the details
    details, ///< Compact's details page
    drag,    ///< a row being dragged
    viewing, ///< read-only
    french,  ///< Compact in French
};

/// One picture.
struct Scene {
    const char* name;                ///< the file's name after "mutator-picker-"
    kit::Viewport window{};          ///< the window it is laid out for
    SceneKind kind{SceneKind::list}; ///< which picture
};

/// A drawing of a scene.
struct Drawing {
    renderer::Surface surface{}; ///< the picture, the margin round the window
    int32_t scale{1};            ///< pixels a point
    kit::Point window{};         ///< the window's size, in points
    kit::DisplayList list{};     ///< what was drawn
};

/// Returns a window's viewport at a scale.
///
/// @param width the canvas's width, in pixels
/// @param height the canvas's height, in pixels
/// @param percent the scale, as a percentage
/// @return the viewport
kit::Viewport window_of(int32_t width, int32_t height, int32_t percent) {
    kit::Viewport viewport;
    viewport.width = width;
    viewport.height = height;
    viewport.scale_percent = percent;
    return viewport;
}

/// The bottom of the lowest row.
///
/// @param list the display list
/// @return the row's bottom, in window points
int32_t lowest_row_bottom(const kit::DisplayList& list) {
    int32_t bottom = 0;
    for (const kit::Control& control : list.controls) {
        if (control.kind != kit::ControlKind::list_item)
            continue;
        if (control.name.rfind("mutators.row.", 0) != 0)
            continue;
        if (control.name.ends_with(".grip") || control.name.ends_with(".switch"))
            continue;
        bottom = std::max(bottom, control.rect.y + control.rect.height);
    }
    return bottom;
}

/// Lays a scene out.
///
/// @param scene the scene
/// @param measured the fonts
/// @return the model, the state and the display list
kit::DisplayList layout_scene(const Scene& scene, const kit::Fonts& measured) {
    lib::TextHooks text;
    if (scene.kind == SceneKind::french)
        text.shown = french_shown;
    const bool read_only = scene.kind == SceneKind::viewing;
    const picker::Picker model = opened(text, read_only);
    const kit::Frame frame = kit::frame_of(scene.window);
    picker::ScreenState state;
    if (scene.kind == SceneKind::details)
        state.details_page = true;
    if (scene.kind == SceneKind::drag) {
        const kit::DisplayList resting = picker::picker_layout(model, state, frame, measured);
        picker::DragShown drag;
        drag.id = "scout-post";
        drag.y = lowest_row_bottom(resting);
        drag.target = picker::Drop{true, 1};
        state.drag = std::move(drag);
    }
    return picker::picker_layout(model, state, frame, measured);
}

/// Draws a scene once.
///
/// @param scene the scene
/// @param measured the fonts
/// @return the drawing
Drawing draw_scene(const Scene& scene, const kit::Fonts& measured) {
    const kit::Frame frame = kit::frame_of(scene.window);
    Drawing drawing;
    drawing.scale = frame.scale_percent / 100;
    drawing.window = picker::window_size(frame);
    drawing.list = layout_scene(scene, measured);
    const int32_t width = (drawing.window.x + 2 * margin_points) * drawing.scale;
    const int32_t height = (drawing.window.y + 2 * margin_points) * drawing.scale;
    drawing.surface.width = static_cast<uint32_t>(width);
    drawing.surface.height = static_cast<uint32_t>(height);
    drawing.surface.rgb.resize(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3
    );
    for (std::size_t at = 0; at < drawing.surface.rgb.size(); at += 3) {
        drawing.surface.rgb[at] = untouched.r;
        drawing.surface.rgb[at + 1] = untouched.g;
        drawing.surface.rgb[at + 2] = untouched.b;
    }
    kit::Canvas canvas;
    canvas.surface = &drawing.surface;
    canvas.placement = {
        margin_points * drawing.scale, margin_points * drawing.scale, drawing.scale, {}
    };
    canvas.fonts = &measured;
    picker::draw_picker(canvas, drawing.list, drawing.window);
    return drawing;
}

/// Tells whether a pixel keeps the colour it started as.
///
/// @param drawing the drawing
/// @param x the column, in pixels
/// @param y the row, in pixels
/// @return true when nothing drew there
bool kept(const Drawing& drawing, int32_t x, int32_t y) {
    const std::size_t at =
        (static_cast<std::size_t>(y) * drawing.surface.width + static_cast<std::size_t>(x)) * 3;
    return drawing.surface.rgb[at] == untouched.r && drawing.surface.rgb[at + 1] == untouched.g &&
           drawing.surface.rgb[at + 2] == untouched.b;
}

/// Checks that the margin keeps its colour and that every pixel of the window was drawn.
///
/// @param scene the scene
/// @param drawing its drawing
void check_edges(const Scene& scene, const Drawing& drawing) {
    const int32_t left = margin_points * drawing.scale;
    const int32_t right = left + drawing.window.x * drawing.scale;
    const int32_t top = margin_points * drawing.scale;
    const int32_t bottom = top + drawing.window.y * drawing.scale;
    std::size_t outside = 0;
    std::size_t blank = 0;
    for (int32_t y = 0; y < static_cast<int32_t>(drawing.surface.height); ++y) {
        for (int32_t x = 0; x < static_cast<int32_t>(drawing.surface.width); ++x) {
            const bool in_window = x >= left && x < right && y >= top && y < bottom;
            const bool untouched_here = kept(drawing, x, y);
            if (!in_window && !untouched_here)
                ++outside;
            if (in_window && untouched_here)
                ++blank;
        }
    }
    if (outside != 0)
        std::fprintf(stderr, "%s: %zu pixels drawn outside the window\n", scene.name, outside);
    if (blank != 0)
        std::fprintf(stderr, "%s: %zu pixels of the window not drawn\n", scene.name, blank);
    OA_CHECK(outside == 0);
    OA_CHECK(blank == 0);
}

/// Writes a drawing's window as a PNG file.
///
/// @param drawing the drawing
/// @param path the file
/// @return true when it was written
bool write_window(const Drawing& drawing, const std::filesystem::path& path) {
    const auto width = static_cast<std::size_t>(drawing.window.x * drawing.scale);
    const auto height = static_cast<std::size_t>(drawing.window.y * drawing.scale);
    const auto margin = static_cast<std::size_t>(margin_points * drawing.scale);
    std::vector<uint8_t> rows;
    rows.reserve(width * height * 3);
    for (std::size_t y = 0; y < height; ++y) {
        const std::size_t start = ((y + margin) * drawing.surface.width + margin) * 3;
        rows.insert(
            rows.end(),
            drawing.surface.rgb.begin() + static_cast<std::ptrdiff_t>(start),
            drawing.surface.rgb.begin() + static_cast<std::ptrdiff_t>(start + width * 3)
        );
    }
    png::Header header;
    header.width = static_cast<uint32_t>(width);
    header.height = static_cast<uint32_t>(height);
    header.bit_depth = 8;
    header.color_type = png::ColorType::rgb;
    std::vector<uint8_t> file;
    if (!png::write(png::Image{header, {}, rows}, &file))
        return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size())
    );
    return static_cast<bool>(output);
}

} // namespace

int main(int argc, char** argv) {
    std::optional<std::filesystem::path> frames;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--frames" && index + 1 < argc)
            frames = std::filesystem::path(argv[++index]);
        else if (argument != oa::test::kGameDataSwitch) {
            std::fprintf(stderr, "draw: unexpected argument %s\n", argv[index]);
            return 2;
        }
    }
    oa::AssetStore assets = oa::test::require_game_assets("the mutator picker's pictures");
    const kit::Fonts measured = kit::load_game_fonts(assets);
    const std::unique_ptr<text_font::FontStack> modern =
        text_font::FontStack::open(text_font::bundled_font_directory());
    if (modern) {
        oa::present::GameTextHooks hooks;
        hooks.context = modern.get();
        hooks.draw = draw_modern;
        oa::present::set_game_text_hooks(hooks);
    } else {
        std::printf(
            "draw: no modern fonts beside the test; the characters the game's fonts lack are not "
            "drawn\n"
        );
    }
    if (frames) {
        std::error_code error;
        std::filesystem::create_directories(*frames, error);
        if (error) {
            std::fprintf(stderr, "draw: cannot make %s\n", frames->string().c_str());
            return 1;
        }
    }

    const Scene scenes[] = {
        {"compact", window_of(640, 480, 100), SceneKind::list},
        {"regular", window_of(1920, 1080, 200), SceneKind::list},
        {"large", window_of(2560, 1440, 200), SceneKind::list},
        {"details-compact", window_of(640, 480, 100), SceneKind::details},
        {"drag-regular", window_of(1920, 1080, 200), SceneKind::drag},
        {"view-regular", window_of(1920, 1080, 200), SceneKind::viewing},
        {"fr-compact", window_of(640, 480, 100), SceneKind::french},
    };
    for (const Scene& scene : scenes) {
        const Drawing first = draw_scene(scene, measured);
        const Drawing second = draw_scene(scene, measured);
        if (first.surface.rgb != second.surface.rgb)
            std::fprintf(stderr, "%s: two drawings differ\n", scene.name);
        OA_CHECK(first.surface.rgb == second.surface.rgb);
        check_edges(scene, first);
        for (const std::string& problem : outside_window(first.list, first.window)) {
            std::fprintf(stderr, "%s: %s lies outside the window\n", scene.name, problem.c_str());
            ++oa::test::failed_checks();
        }
        for (const std::string& problem : texts_not_fitting(first.list, measured)) {
            std::fprintf(stderr, "%s: %s does not fit\n", scene.name, problem.c_str());
            ++oa::test::failed_checks();
        }
        if (frames) {
            const std::filesystem::path path =
                *frames / (std::string("mutator-picker-") + scene.name + ".png");
            const bool written = write_window(first, path);
            if (!written)
                std::fprintf(stderr, "draw: cannot write %s\n", path.string().c_str());
            OA_CHECK(written);
        }
    }
    oa::present::set_game_text_hooks({});
    return oa::test::check_exit_status();
}
