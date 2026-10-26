// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Captures: MakePoster's battlefield drawn view by view at 1:1 into 8-bit
// strips and streamed into a bottom-up BMP, and the numbered PCX screenshots
// and film frames of the frame on screen.
#include "oa/app/runtime.hpp"
#include "oa/core/map_plot.h"

#include "oa/present/pcx.hpp"
#include "oa/present/surface.hpp"
#include "oa/sim/messages.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/console/hotkeys.hpp"
#include "oa/ui/frontend/savegame_dialogs.hpp"
#include "oa/ui/hud/game_fields.hpp"
#include "oa/platform/files.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace oa::app {

namespace {

namespace console = oa::ui::console;
namespace present = oa::present;

constexpr const char* kPosterExtension = "bmp";
constexpr uint8_t kSightFlags =
    console::visibility_flag::mapping | console::visibility_flag::line_of_sight;

// The C stdio stream the strip file opens, as the writer's byte stream.
present::ByteStream file_stream(std::FILE* file) {
    present::ByteStream stream{};
    stream.user = file;
    stream.write = [](void* user, const void* data, int32_t size) {
        return static_cast<int32_t>(
            std::fwrite(data, 1, static_cast<std::size_t>(size), static_cast<std::FILE*>(user))
        );
    };
    stream.seek = [](void* user, int32_t position) {
        return static_cast<int32_t>(std::fseek(static_cast<std::FILE*>(user), position, SEEK_SET));
    };
    stream.tell = [](void* user) {
        return static_cast<int32_t>(std::ftell(static_cast<std::FILE*>(user)));
    };
    stream.close = [](void* user) { std::fclose(static_cast<std::FILE*>(user)); };
    return stream;
}

constexpr const char* kScreenshotPrefix = "SHOT";
constexpr const char* kFilmFramePrefix = "FRAM";
constexpr const char* kScreenshotFolder = "\\screenshots"; // after the output directory
constexpr int32_t kFilmTicksPerSecond = 30;
// The poster check's long output folder: folders of this many characters,
// until it is this much longer than Game.output_directory holds.
constexpr std::size_t kLongFolderNameLength = 60;
constexpr std::size_t kLongFolderExtra = 40;
// The tiers' poster check: its folder under the save root, the zoom the
// view stands at as each poster is made, and the fewest colours the
// standard tier's poster of the map holds.
constexpr const char* kTierPosterFolder = "poster-tiers";
constexpr float kTierPosterZoom = 0.5F;
constexpr std::size_t kLeastPosterColours = 16;

// Nearest palette entry by summed channel difference, the first on ties.
uint8_t nearest_index(const oa::PaletteBytes& palette, uint8_t r, uint8_t g, uint8_t b) {
    int best = 0;
    int best_distance = 0x7fffffff;
    for (int index = 0; index < static_cast<int>(oa::palette_color_count); ++index) {
        const auto* entry =
            palette.data() + static_cast<std::size_t>(index) * oa::palette_entry_bytes;
        const int distance =
            std::abs(entry[0] - r) + std::abs(entry[1] - g) + std::abs(entry[2] - b);
        if (distance < best_distance) {
            best = index;
            best_distance = distance;
            if (distance == 0)
                break;
        }
    }
    return static_cast<uint8_t>(best);
}

} // namespace

bool Runtime::indexed_frame(present::DisplayContext& capture, present::SurfaceBuffer& frame) {
    if (screen_ == Screen::match && match_use_layers_) {
        // A world the accelerated presentation scaled is drawn again as the
        // standard tier draws it, so the picture saved is the game's own.
        ensure_screen_world();
        compose_match_frame(surface_);
    }
    if (surface_.width == 0 || surface_.height == 0 ||
        surface_.rgb.size() != static_cast<std::size_t>(surface_.width) * surface_.height * 3U)
        return false;
    std::unordered_map<uint32_t, uint8_t> indices;
    const bool match = screen_ == Screen::match && match_;
    const oa::PaletteBytes& palette =
        resources_.background.palette ? *resources_.background.palette : resources_.gui_palette;
    if (match)
        capture.palette = match_display_context().palette;
    else
        capture.palette = present::palette_from_bytes(palette);
    frame = present::create_surface(
        static_cast<int32_t>(surface_.width), static_cast<int32_t>(surface_.height)
    );
    for (std::size_t pixel = 0; pixel < frame.pixels.size(); ++pixel) {
        const auto* rgb = surface_.rgb.data() + pixel * 3U;
        const uint32_t key =
            (static_cast<uint32_t>(rgb[0]) << 16) | (static_cast<uint32_t>(rgb[1]) << 8) | rgb[2];
        const auto found = indices.find(key);
        if (found != indices.end()) {
            frame.pixels[pixel] = found->second;
            continue;
        }
        const uint8_t index = match ? match_palette_index(rgb[0], rgb[1], rgb[2])
                                    : nearest_index(palette, rgb[0], rgb[1], rgb[2]);
        indices.emplace(key, index);
        frame.pixels[pixel] = index;
    }
    capture.active_surface = &frame.surface;
    capture.use_active_surface = 1;
    return true;
}

bool Runtime::save_numbered_frame(const char* directory, const char* prefix) {
    present::DisplayContext capture{};
    present::SurfaceBuffer frame{};
    if (!indexed_frame(capture, frame))
        return false;
    present::NumberedFileHost files{};
    files.context = this;
    files.list =
        [](void* context, const char* pattern, void (*visit)(void*, const char*), void* user) {
            static_cast<Runtime*>(context)->list_save_files(pattern, visit, user);
        };
    files.open = [](void* context, const char* path, present::ByteStream* stream) {
        std::FILE* file = oa::platform::open_file(
            static_cast<Runtime*>(context)->game_file_path(path, ui::frontend::SavePathUse::write),
            "wb"
        );
        if (file == nullptr)
            return false;
        *stream = file_stream(file);
        return true;
    };
    return present::save_numbered_pcx(capture, directory, prefix, files);
}

void Runtime::capture_screenshot() {
    if (!match_)
        return;
    oa::Game& game = match_->state().game;
    // The folder is kept whole, however long it is.
    const console::Console* con = match_console();
    const std::string output =
        console::output_directory(game, con != nullptr ? con->host : nullptr);
    const std::string folder = output + kScreenshotFolder;
    std::error_code error;
    fs::create_directories(game_file_path(output, ui::frontend::SavePathUse::write), error);
    fs::create_directories(game_file_path(folder, ui::frontend::SavePathUse::write), error);
    status_ = save_numbered_frame(folder.c_str(), kScreenshotPrefix)
                  ? std::string("Saved screenshot")
                  : std::string("screenshot not saved");
    match_timing_.previous_clock =
        oa::base::game_loop::scaled_clock(clock_milliseconds(), match_clock_scale());
}

void Runtime::capture_film_frame() {
    if (screen_ != Screen::match || !match_)
        return;
    oa::Game& game = match_->state().game;
    if (game.capture_enabled <= 0 || game.next_capture_tick > game.tick || game.capture_rate <= 0)
        return;
    // Game.capture_path holds the folder when it fits; a longer one is the
    // folder the capture began in.
    const std::string field(
        game.capture_path, strnlen(game.capture_path, sizeof game.capture_path)
    );
    const std::string& path = field.empty() ? film_folder_ : field;
    if (!save_numbered_frame(path.c_str(), kFilmFramePrefix)) {
        stop_film_capture(path.c_str());
        return;
    }
    note_film_content(save_game_root() / save_relative_path(path));
    game.next_capture_tick += static_cast<uint32_t>(kFilmTicksPerSecond / game.capture_rate);
    match_timing_.previous_clock =
        oa::base::game_loop::scaled_clock(clock_milliseconds(), match_clock_scale());
}

void Runtime::begin_film_capture(const char* path) {
    film_folder_ = path;
    render();
    if (!save_numbered_frame(path, kFilmFramePrefix))
        stop_film_capture(path);
    else
        note_film_content(save_game_root() / save_relative_path(path));
}

void Runtime::stop_film_capture(const char* path) {
    if (match_)
        match_->state().game.capture_enabled = 0;
    status_ = std::string("Film capture stopped: a frame could not be saved in ") + path;
    std::cerr << "open-annihilation: " << status_ << '\n';
}

Runtime::PosterScene Runtime::enter_poster_scene() {
    oa::Game& game = match_->state().game;
    PosterScene scene{
        match_camera_x_,
        match_camera_z_,
        static_cast<uint8_t>(game.visibility_flags & kSightFlags),
        static_cast<uint16_t>(game.sim_run_flags & console::kSimRunPaused),
        match_paused_,
        static_cast<uint16_t>(console::console_flags(game) & console::console_flag::clock),
        oa::ui::hud::message_lines(game),
    };
    game.visibility_flags = static_cast<uint8_t>(game.visibility_flags & ~kSightFlags);
    reset_match_sight(true);
    game.sim_run_flags = static_cast<uint16_t>(game.sim_run_flags & ~console::kSimRunPaused);
    match_paused_ = false;
    console::set_console_flags(
        game, static_cast<uint16_t>(console::console_flags(game) & ~console::console_flag::clock)
    );
    oa::ui::hud::set_message_lines(game, 0);
    return scene;
}

void Runtime::leave_poster_scene(const PosterScene& scene) {
    oa::Game& game = match_->state().game;
    game.sim_run_flags =
        static_cast<uint16_t>((game.sim_run_flags & ~console::kSimRunPaused) | scene.paused_flag);
    match_paused_ = scene.paused;
    console::set_console_flags(
        game,
        static_cast<uint16_t>(
            (console::console_flags(game) & ~console::console_flag::clock) | scene.clock_flag
        )
    );
    oa::ui::hud::set_message_lines(game, scene.message_lines);
    match_camera_x_ = scene.camera_x;
    match_camera_z_ = scene.camera_z;
    game.visibility_flags =
        static_cast<uint8_t>((game.visibility_flags & ~kSightFlags) | scene.sight_flags);
    reset_match_sight(true);
}

void Runtime::render_poster(
    const char* directory, const char* prefix, int32_t x, int32_t y, int32_t width, int32_t height
) {
    if (!match_)
        return;
    console::HotkeyHost files{};
    files.context = this;
    files.list_files =
        [](void* context, const char* pattern, void (*visit)(void*, const char*), void* user) {
            static_cast<Runtime*>(context)->list_save_files(pattern, visit, user);
        };
    const std::string name =
        console::next_indexed_file_name(&files, directory, prefix, kPosterExtension);
    std::FILE* file =
        oa::platform::open_file(game_file_path(name, ui::frontend::SavePathUse::write), "wb");
    present::ByteStream stream = file_stream(file);
    present::BmpStripWriter writer;
    present::bmp_strip_writer_init(writer);
    if (!present::bmp_strip_writer_begin(
            writer, file != nullptr ? &stream : nullptr, match_display_context(), width, height
        )) {
        present::bmp_strip_writer_close(writer);
        return;
    }
    // The capture is 1:1 whatever the engine's zoom.
    const float zoom = match_zoom_;
    match_zoom_ = 1.0F;
    bind_match_view();
    oa::Game& game = match_->state().game;
    const int32_t tile_width = game.view_cells_width * OA_MAP_CELL_PIXELS;
    int32_t strip_rows = game.view_cells_height * OA_MAP_CELL_PIXELS - 1;
    std::vector<uint8_t> strip;
    try {
        strip.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(strip_rows));
    } catch (const std::bad_alloc&) {
        strip_rows /= 2;
        try {
            strip.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(strip_rows));
        } catch (const std::bad_alloc&) {
            strip.clear();
        }
    }
    if (!strip.empty()) {
        // Every view is drawn as the standard tier draws it, whatever the
        // tier: in the Full tier the graphics card draws the battlefield and
        // the world layer holds only the overlay canvas. The view's place on
        // screen is left alone, as for every reader that keeps a picture, and
        // the frame drawn after the poster is the tier's own again.
        struct StandardTierDraws {
            bool& suspended;
            bool kept{};

            ~StandardTierDraws() { suspended = kept; }
        } standard_tier_draws{accelerated_.suspended, accelerated_.suspended};

        accelerated_.suspended = true;
        const PosterScene scene = enter_poster_scene();
        oa::Surface surface{};
        present::init_surface(surface, width, strip_rows, width, strip.data());
        int32_t row = 0;
        if (height > 0) {
            int32_t written_mark = 0;
            int32_t strip_end = strip_rows;
            int32_t skip_mark = -1;
            do {
                std::fill(strip.begin(), strip.end(), uint8_t{0});
                for (int32_t column = 0; column < width; column += tile_width) {
                    match_camera_x_ = column + x;
                    match_camera_z_ = row + y;
                    render_match_surface();
                    const int32_t clip_right = std::min(column + tile_width - 1, width - 1);
                    const int32_t shift_x = static_cast<int32_t>(game.camera_x) - x;
                    const int32_t shift_y = static_cast<int32_t>(game.camera_y) - row - y;
                    const auto& view = match_world_cpu_;
                    for (int32_t j = 0; j < static_cast<int32_t>(view.height); ++j) {
                        const int32_t target_row = j + shift_y;
                        if (target_row < 0 || target_row >= strip_rows)
                            continue;
                        for (int32_t i = 0; i < static_cast<int32_t>(view.width); ++i) {
                            const int32_t target_column = i + shift_x;
                            if (target_column < column || target_column > clip_right)
                                continue;
                            const auto* rgb =
                                view.rgb.data() + (static_cast<std::size_t>(j) * view.width +
                                                   static_cast<std::size_t>(i)) *
                                                      3U;
                            strip
                                [static_cast<std::size_t>(target_row) *
                                     static_cast<std::size_t>(width) +
                                 static_cast<std::size_t>(target_column)] =
                                    match_palette_index(rgb[0], rgb[1], rgb[2]);
                        }
                    }
                }
                int32_t source_row = 0;
                int32_t rows = strip_rows;
                int32_t remaining_mark = written_mark;
                int32_t first = row;
                if (skip_mark < -1) {
                    first = row + 1;
                    remaining_mark = skip_mark;
                    rows = strip_rows - 1;
                    source_row = 1;
                }
                if (height < first + rows)
                    rows = height + remaining_mark;
                if (!present::bmp_strip_writer_write_rows(writer, surface, rows, row, source_row))
                    break;
                if (strip_end < height) {
                    --row;
                    ++written_mark;
                    ++skip_mark;
                    --strip_end;
                }
                written_mark -= strip_rows;
                skip_mark -= strip_rows;
                strip_end += strip_rows;
                row += strip_rows;
            } while (row < height);
        }
        leave_poster_scene(scene);
    }
    match_zoom_ = zoom;
    render_match_surface();
    present::bmp_strip_writer_close(writer);
}

void Runtime::check_console_poster_command(const std::function<void(const char*)>& enter_line) {
    const auto require = [](bool ok, const std::string& what) {
        if (!ok)
            throw std::runtime_error("console poster check: " + what);
    };
    oa::Game& game = match_->state().game;
    const std::string kept_output(
        game.output_directory, strnlen(game.output_directory, sizeof game.output_directory)
    );
    const auto folder = save_game_root() / "posters" / "screenshots";
    std::error_code error;
    fs::remove_all(save_game_root() / "posters", error);
    enter_line("+Film posters");
    const auto camera_x = match_camera_x_;
    const auto camera_z = match_camera_z_;
    const auto sight = game.visibility_flags;
    const auto flags = console::console_flags(game);
    const auto lines = oa::ui::hud::message_lines(game);
    enter_line("+MakePoster all");
    require(
        match_camera_x_ == camera_x && match_camera_z_ == camera_z &&
            game.visibility_flags == sight && console::console_flags(game) == flags &&
            oa::ui::hud::message_lines(game) == lines,
        "the camera, sight rules, clock or message log were not put back"
    );
    const auto read = [&](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
    };
    const auto le32 = [](const std::vector<uint8_t>& contents, std::size_t at) {
        return static_cast<int32_t>(
            contents[at] | contents[at + 1] << 8 | contents[at + 2] << 16 |
            static_cast<uint32_t>(contents[at + 3]) << 24
        );
    };
    const auto bytes = read(folder / "BIGSHOT0001.bmp");
    const int32_t width = game.map_pixel_width;
    const int32_t height = game.map_pixel_height;
    const int32_t stride = (width + 3) & ~3;
    require(
        bytes.size() == static_cast<std::size_t>(present::bmp_pixel_offset) +
                            static_cast<std::size_t>(stride) * static_cast<std::size_t>(height),
        "BIGSHOT0001.bmp is missing or not one byte per map pixel"
    );
    require(
        bytes[0] == 'B' && bytes[1] == 'M' && le32(bytes, 10) == present::bmp_pixel_offset &&
            le32(bytes, 18) == width && le32(bytes, 22) == height && bytes[28] == 8,
        "BIGSHOT0001.bmp does not have an 8-bit header of the map's size"
    );
    const auto& palette = match_display_context().palette;
    for (int32_t index = 0; index < OA_PALETTE_COLORS; ++index) {
        const auto* quad = bytes.data() + present::bmp_file_header_size +
                           present::bmp_info_header_size + index * 4;
        const auto& entry = palette.entries[index];
        require(
            quad[0] == entry.b && quad[1] == entry.g && quad[2] == entry.r,
            "the colour table is not the match palette"
        );
    }
    const auto pixel = [&](int32_t column, int32_t image_row) {
        return bytes
            [static_cast<std::size_t>(present::bmp_pixel_offset) +
             static_cast<std::size_t>(height - 1 - image_row) * static_cast<std::size_t>(stride) +
             static_cast<std::size_t>(column)];
    };
    // The view the capture drew with the camera at (x, y), as palette indices.
    const float zoom = match_zoom_;
    match_zoom_ = 1.0F;
    const PosterScene scene = enter_poster_scene();
    const int32_t tile_width = game.view_cells_width * OA_MAP_CELL_PIXELS;
    const int32_t strip_rows = game.view_cells_height * OA_MAP_CELL_PIXELS - 1;
    const auto view_at = [&](int32_t x, int32_t y) {
        match_camera_x_ = x;
        match_camera_z_ = y;
        render_match_surface();
        const auto& view = match_world_cpu_;
        std::vector<uint8_t> indices(static_cast<std::size_t>(view.width) * view.height);
        for (std::size_t i = 0; i < indices.size(); ++i)
            indices[i] =
                match_palette_index(view.rgb[i * 3], view.rgb[i * 3 + 1], view.rgb[i * 3 + 2]);
        return std::pair{indices, static_cast<int32_t>(view.width)};
    };
    const auto [corner, corner_width] = view_at(0, 0);
    const auto [second, second_width] = view_at(0, strip_rows - 1);
    leave_poster_scene(scene);
    match_zoom_ = zoom;
    render_match_surface();
    require(height > strip_rows * 2, "the map is not three strips high");
    for (int32_t column = 0; column < tile_width; ++column) {
        for (int32_t row = 0; row < strip_rows - 1; ++row)
            require(
                pixel(column, row) == corner[static_cast<std::size_t>(row * corner_width + column)],
                "the first strip is not the view at the map's corner"
            );
        // The second strip's rows 1.. land from image row strip_rows - 1.
        for (int32_t row = 1; row < strip_rows; ++row)
            require(
                pixel(column, strip_rows - 2 + row) ==
                    second[static_cast<std::size_t>(row * second_width + column)],
                "the second strip is not one row higher than its map rows"
            );
    }
    for (int32_t column = 0; column < width; ++column)
        require(pixel(column, height - 1) == 0, "the image's bottom row was written");

    enter_line("+MakePoster 10 10");
    const auto small = read(folder / "BIGSHOT0002.bmp");
    require(
        small.size() > present::bmp_pixel_offset && le32(small, 18) == tile_width &&
            le32(small, 22) == game.view_cells_height * OA_MAP_CELL_PIXELS,
        "MakePoster 10 10 did not write BIGSHOT0002.bmp one view in size"
    );
    // Ctrl+F9 numbers the frame after the highest SHOTnnnn.pcx
    // there, and a running film writes FRAMnnnn.pcx each FilmSpeed tick.
    capture_screenshot();
    capture_screenshot();
    const auto shot = read(folder / "SHOT0002.pcx");
    const auto le16 = [](const std::vector<uint8_t>& contents, std::size_t at) {
        return static_cast<int32_t>(contents[at] | (contents[at + 1] << 8));
    };
    require(
        fs::exists(folder / "SHOT0001.pcx") && shot.size() > 128 && shot[0] == 0x0a &&
            shot[1] == 5 && le16(shot, 8) + 1 == static_cast<int32_t>(surface_.width) &&
            le16(shot, 10) + 1 == static_cast<int32_t>(surface_.height),
        "Ctrl+F9 did not write SHOT0001.pcx and SHOT0002.pcx of the frame"
    );
    const auto film = save_game_root() / "posters" / "MOVIE001";
    fs::create_directories(film, error);
    std::snprintf(game.capture_path, sizeof game.capture_path, "%s", "posters\\MOVIE001");
    game.capture_enabled = 1;
    game.capture_rate = 10;
    game.next_capture_tick = game.tick;
    capture_film_frame();
    capture_film_frame();
    require(
        fs::exists(film / "FRAM0001.pcx") && !fs::exists(film / "FRAM0002.pcx") &&
            game.next_capture_tick == game.tick + 3,
        "the film did not write FRAM0001.pcx once and wait three ticks"
    );
    game.capture_enabled = 0;
    game.capture_path[0] = '\0';
    const std::string deep_written = check_long_output_directory(enter_line);
    // Film's change is undone before any save takes it.
    std::snprintf(game.output_directory, sizeof game.output_directory, "%s", kept_output.c_str());
    game.output_directory_changed = 0U;
    // The log's ring fills with the check's echoes; the checks after it start
    // from an empty log.
    oa::sim::messages::clear_messages(game);
    fs::remove_all(save_game_root() / "posters", error);
    std::cout << "console poster check: MakePoster writes the map as BIGSHOTnnnn.bmp strip by "
                 "strip, each later strip a row higher and the bottom row black, and puts the "
                 "view back; Ctrl+F9 writes SHOT0001/0002.pcx and the film FRAM0001.pcx"
              << deep_written << '\n';
}

std::string
Runtime::check_long_output_directory(const std::function<void(const char*)>& enter_line) {
    const auto require = [](bool ok, const std::string& what) {
        if (!ok)
            throw std::runtime_error("console poster check: " + what);
    };
    oa::Game& game = match_->state().game;
    // A folder under the save root, longer than Game.output_directory holds.
    std::string relative = "posters";
    while (relative.size() < sizeof game.output_directory + kLongFolderExtra)
        relative += "\\" + std::string(kLongFolderNameLength, 'p');
    const auto folder = save_game_root() / save_relative_path(relative);
    // Only where the system opens the deepest picture's path.
    if ((folder / "screenshots" / "BIGSHOT0001.bmp").native().size() > oa::platform::longest_path())
        return "; the system opens no path long enough for the deep folder";
    const auto kept_preferences = preferences_.image_output_directory;
    preferences_.image_output_directory = relative;
    // Seeding a match leaves Game.output_directory empty for a folder this long.
    const auto seeded = std::make_unique<oa::Game>(game);
    seed_match_options(*seeded);
    require(seeded->output_directory[0] == '\0', "a long folder was put in Game.output_directory");
    game.output_directory[0] = '\0';
    capture_screenshot();
    enter_line("+MakePoster 10 10");
    const auto film = relative + "\\MOVIE001";
    std::error_code error;
    fs::create_directories(save_game_root() / save_relative_path(film), error);
    // A film whose folder Game.capture_path cannot hold runs in the folder
    // it began in, as Ctrl+F10 starts it (begin_film_capture).
    game.capture_path[0] = '\0';
    film_folder_ = film;
    game.capture_enabled = 1;
    game.capture_rate = 10;
    game.next_capture_tick = game.tick;
    capture_film_frame();
    const auto shots = folder / "screenshots";
    const auto frames = save_game_root() / save_relative_path(film);
    const bool written = fs::is_regular_file(shots / "SHOT0001.pcx", error) &&
                         fs::is_regular_file(shots / "BIGSHOT0001.bmp", error) &&
                         fs::is_regular_file(frames / "FRAM0001.pcx", error);
    game.capture_enabled = 0;
    film_folder_.clear();
    preferences_.image_output_directory = kept_preferences;
    require(
        written,
        "Ctrl+F9, MakePoster and the film did not write SHOT0001.pcx, BIGSHOT0001.bmp and "
        "FRAM0001.pcx into a folder " +
            std::to_string(relative.size()) + " characters long"
    );
    return "; and into a folder " + std::to_string(relative.size()) +
           " characters long, too long for Game.output_directory, SHOT0001.pcx, "
           "BIGSHOT0001.bmp and FRAM0001.pcx";
}

void Runtime::check_poster_tiers(
    const std::function<void(oa::ui::engine_settings::HardwareAcceleration)>& set_level,
    std::span<const oa::ui::engine_settings::HardwareAcceleration> levels,
    const std::function<void(float)>& at_zoom,
    const fs::path& report_directory
) {
    using oa::ui::engine_settings::HardwareAcceleration;
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("render tiers check: poster: " + what);
    };
    if (!match_ || levels.empty() || levels.front() != HardwareAcceleration::off)
        fail("needs a match and the standard tier first");
    const auto tier_name = [](HardwareAcceleration level) -> std::string {
        switch (level) {
        case HardwareAcceleration::off:
            return "standard";
        case HardwareAcceleration::basic:
            return "basic";
        case HardwareAcceleration::full:
            return "full";
        }
        return "standard";
    };
    // Whole ticks, the interface's clock held and the cursor in the blank
    // corner right of the bottom bar, so that every tier's poster and frame
    // are of one moment. The clock runs again when the check ends, however
    // it ends.
    set_presentation_alpha(1.0F);

    struct HeldClock {
        Runtime& runtime;
        std::optional<uint32_t> kept{};

        ~HeldClock() { runtime.fake_frontend_tick_ = kept; }
    } held_clock{*this, fake_frontend_tick_};

    fake_frontend_tick_ = frontend_tick();
    update_pointer(
        static_cast<float>(match_layout_.width - 1), static_cast<float>(match_layout_.height - 1)
    );
    const auto presented = [&]() {
        renderer::Surface frame;
        capture_frame_ = &frame;
        render();
        capture_frame_ = nullptr;
        return frame;
    };
    const auto read = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
    };
    const auto le32 = [](const std::vector<uint8_t>& contents, std::size_t at) {
        return static_cast<int32_t>(
            contents[at] | contents[at + 1] << 8 | contents[at + 2] << 16 |
            static_cast<uint32_t>(contents[at + 3]) << 24
        );
    };
    const oa::Game& game = match_->state().game;
    const auto folder = save_game_root() / kTierPosterFolder;
    const auto poster = folder / "BIGSHOT0001.bmp";
    std::error_code error;
    fs::remove_all(folder, error);
    fs::create_directories(folder, error);
    // The rectangle, two views at 1:1 wide and high within the map, is
    // taken about the view of the first tier and kept for the others.
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    std::vector<uint8_t> standard;
    std::size_t colours = 0;
    for (const auto level : levels) {
        const std::string tier = tier_name(level);
        set_level(level);
        if ((level == HardwareAcceleration::full) != full_presentation() ||
            (level != HardwareAcceleration::off) != accelerated_presentation())
            fail("the " + tier + " tier did not switch on");
        at_zoom(kTierPosterZoom);
        const auto before = presented();
        if (width == 0) {
            width = std::min(game.map_pixel_width, 2 * match_layout_.battlefield_width());
            height = std::min(game.map_pixel_height, 2 * match_layout_.battlefield_height());
            x = std::clamp(
                static_cast<int32_t>(game.camera_x) + visible_map_width() / 2 - width / 2,
                0,
                game.map_pixel_width - width
            );
            y = std::clamp(
                static_cast<int32_t>(game.camera_y) + visible_map_height() / 2 - height / 2,
                0,
                game.map_pixel_height - height
            );
        }
        const int32_t camera_x = match_camera_x_;
        const int32_t camera_z = match_camera_z_;
        fs::remove(poster, error);
        render_poster(kTierPosterFolder, "BIGSHOT", x, y, width, height);
        if (match_zoom_ != kTierPosterZoom || match_camera_x_ != camera_x ||
            match_camera_z_ != camera_z)
            fail("the " + tier + " tier's poster did not put the zoom and the camera back");
        if (level == HardwareAcceleration::full && !full_frame_drawn())
            fail("the full tier's poster left the frame drawn by the processor");
        const auto after = presented();
        // The first poster rebuilt the sight grids from what the units see
        // now (enter_poster_scene); from the second on, the frame is as it was.
        if (!standard.empty() && (after.width != before.width || after.height != before.height ||
                                  after.rgb != before.rgb))
            fail("the frame presented after the " + tier + " tier's poster changed");
        const auto bytes = read(poster);
        const int32_t stride = (width + 3) & ~3;
        if (bytes.size() !=
                static_cast<std::size_t>(present::bmp_pixel_offset) +
                    static_cast<std::size_t>(stride) * static_cast<std::size_t>(height) ||
            le32(bytes, 18) != width || le32(bytes, 22) != height || bytes[28] != 8)
            fail("the " + tier + " tier's poster is not an 8-bit picture of the rectangle");
        if (level == HardwareAcceleration::off) {
            // The map's terrain, in many colours: not a blank picture.
            std::array<bool, OA_PALETTE_COLORS> used{};
            for (int32_t row = 0; row < height; ++row)
                for (int32_t column = 0; column < width; ++column)
                    used[bytes
                             [static_cast<std::size_t>(present::bmp_pixel_offset) +
                              static_cast<std::size_t>(row) * static_cast<std::size_t>(stride) +
                              static_cast<std::size_t>(column)]] = true;
            colours = static_cast<std::size_t>(std::count(used.begin(), used.end(), true));
            if (colours < kLeastPosterColours)
                fail(
                    "the standard tier's poster holds " + std::to_string(colours) +
                    " colours, not the map's terrain"
                );
            standard = bytes;
            continue;
        }
        if (bytes != standard) {
            std::size_t differing = 0;
            for (std::size_t at = static_cast<std::size_t>(present::bmp_pixel_offset);
                 at < bytes.size();
                 ++at)
                differing += bytes[at] != standard[at] ? 1U : 0U;
            fs::create_directories(report_directory, error);
            fs::copy_file(
                poster,
                report_directory / ("native-render-tiers-poster-" + tier + ".bmp"),
                fs::copy_options::overwrite_existing,
                error
            );
            fail(
                "the " + tier + " tier's poster differs from the standard tier's in " +
                std::to_string(differing) + " bytes"
            );
        }
    }
    fs::remove_all(folder, error);
    std::cout << "render tiers check: poster: MakePoster of " << width << 'x' << height
              << " map pixels at zoom " << kTierPosterZoom
              << " writes the standard tier's battlefield in " << colours
              << " colours in every tier, and leaves the frame on screen as it was\n";
}

} // namespace oa::app
