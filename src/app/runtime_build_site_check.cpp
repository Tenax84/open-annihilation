// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Headless check of the build ghost and the placing click over the sites the
// game's building-site test refuses, and over bare ground and a metal deposit,
// which it accepts alike; of the cursor build mode shows; of the site under
// the pointer zoomed out on the game's screen and a larger window; and of the
// ghost's outline over the map's edges and at several zooms.
#include "oa/app/runtime.hpp"
#include "oa/core/map_plot.h"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

// The one rule of the site test a site breaks for a yard of level, unit, claim
// and feature cells ('o'), judged from the plots under it; `several` when it
// breaks more than one or has a blocking feature.
enum class SiteFault : uint8_t { none, edge, water, slope, occupied, several };

const char* fault_name(SiteFault fault) {
    switch (fault) {
    case SiteFault::edge:
        return "map edge";
    case SiteFault::water:
        return "water";
    case SiteFault::slope:
        return "slope";
    case SiteFault::occupied:
        return "occupied";
    case SiteFault::several:
        return "several faults";
    case SiteFault::none:
        break;
    }
    return "clear";
}

SiteFault site_fault(
    const oa::sim::spatial_state::World& spatial,
    const oa::data::unit_definitions::RuntimeDefinitionMetadata& limits,
    int32_t x,
    int32_t z,
    int32_t footprint_x,
    int32_t footprint_z
) {
    const auto width = static_cast<int32_t>(spatial.terrain_width);
    const auto height = static_cast<int32_t>(spatial.terrain_height);
    if (x < 0 || z < 0 || x + footprint_x > width || z + footprint_z > height)
        return SiteFault::several;
    int32_t low = 255, high = 0;
    bool occupied = false;
    for (int32_t row = 0; row < footprint_z; ++row)
        for (int32_t column = 0; column < footprint_x; ++column) {
            const auto& plot = spatial.plots
                                   [static_cast<std::size_t>(z + row) * spatial.terrain_width +
                                    static_cast<std::size_t>(x + column)];
            if (plot.blocking_feature)
                return SiteFault::several;
            low = std::min<int32_t>(low, plot.low_height);
            high = std::max<int32_t>(high, plot.high_height);
            occupied = occupied || plot.ground != oa::sim::spatial_state::no_unit ||
                       (plot.flags & oa::sim::spatial_state::plot_claimed) != 0;
        }
    const auto sea = static_cast<int32_t>(spatial.sea_level);
    const std::array<std::pair<SiteFault, bool>, 4> faults{{
        {SiteFault::edge, x < 1 || z < 1 || width <= x + footprint_x || height <= z + footprint_z},
        {SiteFault::water,
         low < sea - limits.max_water_depth || sea - limits.min_water_depth < high},
        {SiteFault::slope, static_cast<int32_t>(limits.max_slope) < high - low},
        {SiteFault::occupied, occupied},
    }};
    auto found = SiteFault::none;
    for (const auto& [fault, broken] : faults) {
        if (!broken)
            continue;
        if (found != SiteFault::none)
            return SiteFault::several;
        found = fault;
    }
    return found;
}

// The metal on the plots under a footprint.
int32_t metal_under(
    const oa::sim::spatial_state::World& spatial,
    int32_t x,
    int32_t z,
    int32_t footprint_x,
    int32_t footprint_z
) {
    int32_t metal = 0;
    for (int32_t row = 0; row < footprint_z; ++row)
        for (int32_t column = 0; column < footprint_x; ++column)
            metal += spatial
                         .plots
                             [static_cast<std::size_t>(z + row) * spatial.terrain_width +
                              static_cast<std::size_t>(x + column)]
                         .metal;
    return metal;
}

} // namespace

void Runtime::check_build_site_pointer(uint16_t builder, uint16_t type) {
    const auto& fields = offline_type_fields_.at(type);
    if (fields.runtime_metadata == nullptr)
        throw std::runtime_error("build site check: the extractor has no placement limits");
    const auto& limits = *fields.runtime_metadata;
    const auto fx = static_cast<int32_t>(spawn_types_[type].footprint_x);
    const auto fz = static_cast<int32_t>(spawn_types_[type].footprint_z);
    const auto& spatial = match_->spatial();
    const auto width = static_cast<int32_t>(spatial.terrain_width);
    const auto height = static_cast<int32_t>(spatial.terrain_height);
    const auto primary_count = [&] {
        std::size_t count = 0;
        for (const auto* order = match_->orders(builder).primary; order != nullptr;
             order = order->next)
            ++count;
        return count;
    };
    // Centres the view on the site, puts the pointer on its footprint centre
    // at the terrain height there, draws the frame (written to `snapshot`
    // when one is named) and clicks.
    const auto try_site = [&](int32_t x,
                              int32_t z,
                              bool expect_legal,
                              const char* what,
                              const char* snapshot = nullptr) {
        match_command_ = MatchCommand::build;
        pending_build_type_ = type;
        const auto centre_x = (x * 2 + fx) * 8;
        const auto centre_z = (z * 2 + fz) * 8;
        match_camera_x_ = centre_x - visible_map_width() / 2;
        match_camera_z_ = centre_z - visible_map_height() / 2;
        render_match_surface();
        const oa::sim::unit_movement::Terrain terrain(*selected_tnt_);
        const auto ground =
            std::max<int32_t>(terrain.sea_level(), terrain.height(centre_x << 16, centre_z << 16));
        const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
        const auto pointer = project_match_point(
            viewport,
            {static_cast<uint32_t>(centre_x) << 16,
             static_cast<uint32_t>(ground) << 16,
             static_cast<uint32_t>(centre_z) << 16}
        );
        const auto px = static_cast<float>(pointer.x);
        const auto py = static_cast<float>(pointer.y);
        update_pointer(px, py);
        const auto site = build_site_under(px, py);
        if (!site || site->cell_x != x || site->cell_z != z)
            throw std::runtime_error(
                std::string("build site check: the pointer missed the ") + what + " site " +
                std::to_string(x) + ',' + std::to_string(z) + " for " +
                (site ? std::to_string(site->cell_x) + ',' + std::to_string(site->cell_z)
                      : "none") +
                " at " + std::to_string(pointer.x) + ',' + std::to_string(pointer.y) + " height " +
                std::to_string(ground) + " camera " + std::to_string(match_camera_x_) + ',' +
                std::to_string(match_camera_z_)
            );
        if (site->legal != expect_legal)
            throw std::runtime_error(
                std::string("build site check: the ghost ") +
                (site->legal ? "accepted" : "refused") + " the " + what + " site " +
                std::to_string(x) + ',' + std::to_string(z)
            );
        render_match_surface();
        renderer::Surface frame;
        compose_match_frame(frame);
        if (snapshot != nullptr) {
            const fs::path report_directory = "local/reports";
            fs::create_directories(report_directory);
            write_ppm(report_directory / snapshot, frame);
        }
        const auto top_left = project_match_point(
            viewport,
            {static_cast<uint32_t>(x * 16) << 16,
             static_cast<uint32_t>(site->world[1]),
             static_cast<uint32_t>(z * 16) << 16}
        );
        const auto top_right = project_match_point(
            viewport,
            {static_cast<uint32_t>((x + fx) * 16) << 16,
             static_cast<uint32_t>(site->world[1]),
             static_cast<uint32_t>(z * 16) << 16}
        );
        const auto edge_x = (top_left.x + top_right.x) / 2;
        const auto edge_y = top_left.y;
        if (edge_x < 0 || edge_y < 0 || edge_x >= static_cast<int>(frame.width) ||
            edge_y >= static_cast<int>(frame.height))
            throw std::runtime_error(
                std::string("build site check: the ") + what + " outline is off the frame"
            );
        const auto at =
            (static_cast<std::size_t>(edge_y) * frame.width + static_cast<std::size_t>(edge_x)) *
            3U;
        const std::array<uint8_t, 3> drawn{frame.rgb[at], frame.rgb[at + 1], frame.rgb[at + 2]};
        if (drawn != ui_color_rgb(expect_legal ? kBuildSiteClearColor : kBuildSiteRefusedColor))
            throw std::runtime_error(
                std::string("build site check: the ") + what + " site's outline is not the " +
                (expect_legal ? "clear" : "refused") + " colour"
            );
        const auto orders_before = primary_count();
        handle_match_left_click(px, py, 1);
        if (!expect_legal) {
            if (primary_count() != orders_before || match_command_ != MatchCommand::build ||
                pending_build_type_ != type)
                throw std::runtime_error(
                    std::string("build site check: a click on the ") + what +
                    " site placed the building"
                );
            return;
        }
        std::optional<oa::sim::match_runtime::Match::QueuedCommandView> first;
        match_->visit_primary_queue(builder, [&](const auto& queued) {
            if (!first)
                first = queued;
        });
        if (!first || first->kind != oa::sim::match_runtime::mobile_build_kind ||
            first->build_type != type || first->destination[0] != site->world[0] ||
            first->destination[2] != site->world[2])
            throw std::runtime_error(
                "build site check: a click on the clear site did not order MobileBuild there"
            );
        match_->stop_orders(builder);
    };

    std::string report;
    const auto note = [&](SiteFault fault, int32_t x, int32_t z) {
        report += std::string(report.empty() ? "" : ", ") + fault_name(fault) + ' ' +
                  std::to_string(x) + ',' + std::to_string(z);
    };
    // The builder's own plots, in its own sight, refuse the ghost: the ghost's
    // site test passes no unit to skip.
    const auto& builder_slot = match_->world().slots[builder];
    const auto builder_x = static_cast<int32_t>(builder_slot.unit->position[0] >> 20);
    const auto builder_z = static_cast<int32_t>(builder_slot.unit->position[2] >> 20);
    std::optional<std::pair<int32_t, int32_t>> occupied;
    for (int32_t dz = 1 - fz; dz <= 0 && !occupied; ++dz)
        for (int32_t dx = 1 - fx; dx <= 0 && !occupied; ++dx)
            if (site_fault(spatial, limits, builder_x + dx, builder_z + dz, fx, fz) ==
                SiteFault::occupied)
                occupied = std::pair{builder_x + dx, builder_z + dz};
    if (!occupied)
        throw std::runtime_error("build site check: the builder stands on no otherwise clear site");
    try_site(occupied->first, occupied->second, false, "occupied");
    note(SiteFault::occupied, occupied->first, occupied->second);

    // The rest of the map is mapped by the NowISee cheat, which also turns
    // line of sight off, so only the site's own rule can refuse it.
    chat_buffer_ = "+NowISee";
    submit_chat_line();
    // The first site, searching from the middle rows out so the view can
    // centre on it, that breaks `wanted` and whose metal `metal_fits`.
    const auto find_where = [&](
                                SiteFault wanted, const auto& metal_fits
                            ) -> std::optional<std::pair<int32_t, int32_t>> {
        for (int32_t step = 0; step < height; ++step) {
            const auto z = height / 2 + ((step & 1) != 0 ? -(step + 1) / 2 : step / 2);
            for (int32_t x = 0; x + fx <= width; ++x)
                if (site_fault(spatial, limits, x, z, fx, fz) == wanted &&
                    metal_fits(metal_under(spatial, x, z, fx, fz)))
                    return std::pair{x, z};
        }
        return std::nullopt;
    };
    const auto find = [&](SiteFault wanted) {
        return find_where(wanted, [](int32_t) { return true; });
    };
    for (const auto fault : {SiteFault::edge, SiteFault::water, SiteFault::slope}) {
        const auto site = find(fault);
        if (!site)
            throw std::runtime_error(
                std::string("build site check: the map has no ") + fault_name(fault) + " site"
            );
        try_site(site->first, site->second, false, fault_name(fault));
        note(fault, site->first, site->second);
    }
    const auto clear = find(SiteFault::none);
    if (!clear)
        throw std::runtime_error("build site check: the map has no clear site");
    try_site(clear->first, clear->second, true, "clear");
    // The site test has no metal rule: an extractor's ghost is green, and the
    // click orders it, on the clear site with the least metal under it (none,
    // off the deposits of a map without surface metal) as on the richest.
    auto poorest = std::numeric_limits<int32_t>::max();
    int32_t richest = 0;
    for (int32_t z = 0; z + fz <= height; ++z)
        for (int32_t x = 0; x + fx <= width; ++x)
            if (site_fault(spatial, limits, x, z, fx, fz) == SiteFault::none) {
                const auto metal = metal_under(spatial, x, z, fx, fz);
                poorest = std::min(poorest, metal);
                richest = std::max(richest, metal);
            }
    if (richest <= poorest)
        throw std::runtime_error("build site check: the map has no clear site on a deposit");
    const auto bare = find_where(SiteFault::none, [&](int32_t metal) { return metal == poorest; });
    const auto deposit =
        find_where(SiteFault::none, [&](int32_t metal) { return metal == richest; });
    try_site(bare->first, bare->second, true, "bare", "native-build-site-bare.ppm");
    try_site(deposit->first, deposit->second, true, "deposit", "native-build-site-deposit.ppm");
    std::cout << "build site check: ghost red and click refused over " << report
              << "; green and MobileBuild at " << clear->first << ',' << clear->second
              << ", on metal " << poorest << " at " << bare->first << ',' << bare->second
              << " and on metal " << richest << " at " << deposit->first << ',' << deposit->second
              << '\n';
    check_build_cursor(type);
    check_build_site_zooms(builder, type);
    // A solar collector's wide footprint hangs well over the edges.
    const auto solar = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMSOLAR");
    check_build_site_edges(solar != 0 && solar < spawn_types_.size() ? solar : type);
}

void Runtime::check_build_cursor(uint16_t type) {
    namespace input = oa::sim::gameplay_input;
    if (!cursors_loaded_)
        throw std::runtime_error("build cursor check: the game's cursors are not loaded");
    const auto normal = static_cast<uint8_t>(input::OrderCursor::normal);
    const auto build = static_cast<uint8_t>(input::OrderCursor::build);
    match_command_ = MatchCommand::build;
    pending_build_type_ = type;
    // The middle of the side column, of the battlefield and of the radar.
    const auto column_x = static_cast<float>(match_layout_.left / 2);
    const auto column_y =
        static_cast<float>(match_layout_.top + match_layout_.battlefield_height() / 2);
    const auto field_x =
        static_cast<float>(match_layout_.left + match_layout_.battlefield_width() / 2);
    const auto field_y = column_y;
    const auto radar_x = static_cast<float>(radar_picture_.x + radar_picture_.width / 2);
    const auto radar_y = static_cast<float>(radar_picture_.y + radar_picture_.height / 2);
    if (battlefield_contains(column_x, column_y) || radar_contains(column_x, column_y) ||
        !battlefield_contains(field_x, field_y) || radar_contains(field_x, field_y) ||
        !radar_contains(radar_x, radar_y))
        throw std::runtime_error(
            "build cursor check: the side column, battlefield and radar overlap"
        );
    // The pointer moved to the point, then the frame's cursor pass, as the
    // main loop runs it, which must leave `cursor` shown.
    const auto expect = [&](float x, float y, uint8_t cursor, const char* where) {
        update_pointer(x, y);
        tick_and_draw_cursor();
        if (cursor_index_ != cursor)
            throw std::runtime_error(
                std::string("build cursor check: cursor ") + std::to_string(cursor_index_) + ' ' +
                where + ", not " + std::to_string(cursor)
            );
    };
    expect(column_x, column_y, normal, "beside the battlefield");
    expect(field_x, field_y, normal, "on the battlefield, come from the side column");
    // The site is centred on the ground under the pointer, and a click there
    // places the building.
    const auto ground = build_cursor_point(field_x, field_y);
    const auto site = build_site_under(field_x, field_y);
    if (!ground || !site || pick_match_cursor() != build)
        throw std::runtime_error("build cursor check: a click on the battlefield places nothing");
    const auto middle_x = (site->cell_x * 2 + site->footprint_x) * (OA_MAP_CELL_PIXELS / 2);
    const auto middle_z = (site->cell_z * 2 + site->footprint_z) * (OA_MAP_CELL_PIXELS / 2);
    if (std::abs(((*ground)[0] >> 16) - middle_x) > OA_MAP_CELL_PIXELS / 2 ||
        std::abs(((*ground)[2] >> 16) - middle_z) > OA_MAP_CELL_PIXELS / 2)
        throw std::runtime_error("build cursor check: the site is not centred on the pointer");
    expect(radar_x, radar_y, build, "over the radar");
    expect(field_x, field_y, build, "on the battlefield, come from the radar");
    expect(column_x, column_y, normal, "beside the battlefield again");
    std::cout << "build cursor check: normal cursor beside the battlefield and kept on it over "
                 "the site's middle; build cursor over the radar and kept on the battlefield "
                 "from it\n";
}

void Runtime::check_build_site_zooms(uint16_t builder, uint16_t type) {
    namespace input = oa::sim::gameplay_input;
    namespace settings = oa::ui::engine_settings;
    if (!cursors_loaded_)
        throw std::runtime_error("build site zoom check: the game's cursors are not loaded");
    const auto& fields = offline_type_fields_.at(type);
    if (fields.runtime_metadata == nullptr)
        throw std::runtime_error("build site zoom check: the building has no placement limits");
    const auto& limits = *fields.runtime_metadata;
    const auto fx = static_cast<int32_t>(spawn_types_[type].footprint_x);
    const auto fz = static_cast<int32_t>(spawn_types_[type].footprint_z);
    const auto& spatial = match_->spatial();
    const auto [map_width, map_height] = shown_map_size();
    const auto saved_layout = match_layout_;
    const auto saved_settings = engine_settings();
    const auto saved_zoom = match_zoom_;
    const auto saved_zoom_target = match_zoom_target_;
    const auto saved_camera_x = match_camera_x_;
    const auto saved_camera_z = match_camera_z_;
    const bool saved_paused = match_paused_;
    const bool saved_pointer_known = match_pointer_known_;
    std::vector<std::string> failures;

    // A clear site whose ground stays clear for `margin` cells all round, so
    // that the pointer finds a clear site there even where one screen pixel
    // spans several cells; the widest margin the map has, searching from its
    // middle rows out.
    std::optional<std::pair<int32_t, int32_t>> open;
    for (int32_t margin = 3; margin >= 0 && !open; --margin)
        for (int32_t step = 0; step < static_cast<int32_t>(spatial.terrain_height) && !open;
             ++step) {
            const auto z = static_cast<int32_t>(spatial.terrain_height) / 2 +
                           ((step & 1) != 0 ? -(step + 1) / 2 : step / 2);
            for (int32_t x = margin; x + fx + margin <= static_cast<int32_t>(spatial.terrain_width);
                 ++x)
                if (site_fault(
                        spatial, limits, x - margin, z - margin, fx + 2 * margin, fz + 2 * margin
                    ) == SiteFault::none) {
                    open = std::pair{x, z};
                    break;
                }
        }
    if (!open)
        throw std::runtime_error("build site zoom check: the map has no clear site");
    const auto open_x = (open->first * 2 + fx) * (OA_MAP_CELL_PIXELS / 2);
    const auto open_z = (open->second * 2 + fz) * (OA_MAP_CELL_PIXELS / 2);
    const oa::sim::unit_movement::Terrain terrain(*selected_tnt_);
    const auto open_ground =
        std::max<int32_t>(terrain.sea_level(), terrain.height(open_x << 16, open_z << 16));

    // The pointer's moves and presses as SDL brings them; a headless run
    // takes them in canvas pixels.
    const auto send = [&](SDL_EventType kind, int x, int y) {
        SDL_Event event{};
        event.type = kind;
        if (kind == SDL_EVENT_MOUSE_MOTION) {
            event.motion.x = static_cast<float>(x);
            event.motion.y = static_cast<float>(y);
        } else {
            event.button.button = SDL_BUTTON_LEFT;
            event.button.down = kind == SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.clicks = 1;
            event.button.x = static_cast<float>(x);
            event.button.y = static_cast<float>(y);
        }
        bool running = true;
        dispatch_event(event, running);
    };
    // Frames until the zoom the wheel set is reached, the match held so
    // that the view moves only with the zoom; then one drawn in play, since
    // a held match's title covers the battlefield's middle.
    const auto settle = [&] {
        match_paused_ = true;
        for (int frame = 0; frame < 600 && match_zoom_ != match_zoom_target_; ++frame) {
            frame_time_ns_ += 1'000'000'000ULL / 60ULL;
            move_match_camera();
        }
        match_paused_ = false;
        render_match_surface();
        if (match_zoom_ != match_zoom_target_)
            failures.push_back(
                "the zoom did not reach the wheel's " + std::to_string(match_zoom_target_)
            );
    };
    // Wheel steps out about the battlefield's centre, as a player zooms out.
    const auto wheel_out = [&](int steps) {
        for (int step = 0; step < steps; ++step) {
            handle_match_zoom(
                -1.0F,
                static_cast<float>(match_layout_.left + match_layout_.battlefield_width() / 2),
                static_cast<float>(match_layout_.top + match_layout_.battlefield_height() / 2),
                true
            );
            settle();
        }
    };
    // The cursor the frame draws, drawn alone on a blank canvas: where its
    // point lands, from where its picture lands.
    const std::array<uint8_t, 3> blank{1, 2, 3};
    const auto drawn_cursor_point = [&]() -> std::optional<std::pair<int, int>> {
        renderer::Surface canvas;
        canvas.width = static_cast<uint32_t>(match_layout_.width);
        canvas.height = static_cast<uint32_t>(match_layout_.height);
        canvas.rgb.resize(static_cast<std::size_t>(canvas.width) * canvas.height * 3U);
        for (std::size_t at = 0; at < canvas.rgb.size(); at += 3)
            std::copy(
                blank.begin(), blank.end(), canvas.rgb.begin() + static_cast<std::ptrdiff_t>(at)
            );
        std::swap(canvas, surface_);
        tick_and_draw_cursor();
        std::swap(canvas, surface_);
        if (cursor_image_ == nullptr)
            return std::nullopt;
        const auto rendered = oa::formats::gaf::render_normal(*cursor_image_);
        if (!rendered.ok())
            return std::nullopt;
        const auto& picture = *rendered.frame;
        // The first covered pixel of the cursor's picture, row by row, and
        // the first pixel the draw changed on the canvas.
        std::optional<std::pair<int, int>> covered;
        for (uint32_t row = 0; row < picture.height && !covered; ++row)
            for (uint32_t column = 0; column < picture.width && !covered; ++column)
                if (picture.coverage[static_cast<std::size_t>(row) * picture.width + column] != 0)
                    covered = std::pair{static_cast<int>(column), static_cast<int>(row)};
        std::optional<std::pair<int, int>> changed;
        for (uint32_t row = 0; row < canvas.height && !changed; ++row)
            for (uint32_t column = 0; column < canvas.width && !changed; ++column) {
                const auto at = (static_cast<std::size_t>(row) * canvas.width + column) * 3U;
                if (!std::equal(
                        blank.begin(),
                        blank.end(),
                        canvas.rgb.begin() + static_cast<std::ptrdiff_t>(at)
                    ))
                    changed = std::pair{static_cast<int>(column), static_cast<int>(row)};
            }
        if (!covered || !changed)
            return std::nullopt;
        return std::pair{
            changed->first - covered->first + picture.origin_x,
            changed->second - covered->second + picture.origin_y
        };
    };
    // The battlefield frame with the site's outline and without it; the
    // rectangle the outline's colours changed, in canvas pixels.
    const auto drawn_outline = [&]() -> std::optional<std::array<int64_t, 4>> {
        match_command_ = MatchCommand::none;
        render_match_surface();
        const auto without = match_world_cpu_;
        match_command_ = MatchCommand::build;
        pending_build_type_ = type;
        render_match_surface();
        const auto& with = match_world_cpu_;
        const std::array<std::array<uint8_t, 3>, 3> colours{
            ui_color_rgb(kBuildSiteClearColor),
            ui_color_rgb(kBuildSiteRefusedColor),
            ui_color_rgb(kBuildSiteOverOwnUnitsColor)
        };
        std::optional<std::array<int64_t, 4>> box;
        for (uint32_t row = 0; row < with.height; ++row)
            for (uint32_t column = 0; column < with.width; ++column) {
                const auto at = (static_cast<std::size_t>(row) * with.width + column) * 3U;
                const std::array<uint8_t, 3> pixel{
                    with.rgb[at], with.rgb[at + 1], with.rgb[at + 2]
                };
                if (pixel[0] == without.rgb[at] && pixel[1] == without.rgb[at + 1] &&
                    pixel[2] == without.rgb[at + 2])
                    continue;
                if (std::find(colours.begin(), colours.end(), pixel) == colours.end())
                    continue;
                const int64_t x = match_layout_.left + static_cast<int64_t>(column);
                const int64_t y = match_layout_.top + static_cast<int64_t>(row);
                if (!box)
                    box = std::array<int64_t, 4>{x, y, x, y};
                else
                    *box = {
                        std::min((*box)[0], x),
                        std::min((*box)[1], y),
                        std::max((*box)[2], x),
                        std::max((*box)[3], y)
                    };
            }
        return box;
    };

    std::string report;
    int placed = 0;
    int refused = 0;
    // One pointer place: the cursor kept and its point on the pointer, the
    // outline drawn around the site under the pointer and centred on it,
    // and the click placing the building at that site or nowhere.
    const auto try_pointer = [&](const std::string& view, int x, int y, bool must_place) {
        const auto name = view + " at " + std::to_string(x) + ',' + std::to_string(y);
        match_command_ = MatchCommand::build;
        pending_build_type_ = type;
        send(SDL_EVENT_MOUSE_MOTION, x, y);
        const auto ground = build_cursor_point(static_cast<float>(x), static_cast<float>(y));
        const auto site = build_site_under(static_cast<float>(x), static_cast<float>(y));
        if (!ground || !site) {
            failures.push_back("no site under the pointer " + name);
            return;
        }
        if (must_place && !site->legal) {
            failures.push_back(
                "the pointer " + name + " on clear ground found a refused site " +
                std::to_string(site->cell_x) + ',' + std::to_string(site->cell_z)
            );
            return;
        }
        const auto point = drawn_cursor_point();
        if (cursor_index_ != static_cast<uint8_t>(input::OrderCursor::normal) || !point ||
            point->first != x || point->second != y)
            failures.push_back(
                "cursor " + std::to_string(cursor_index_) + " drawn with its point at " +
                (point ? std::to_string(point->first) + ',' + std::to_string(point->second)
                       : std::string("none")) +
                ", not the normal cursor's point on the pointer " + name
            );
        // Where the outline must be: the footprint of the site the pointer
        // finds, at the site's height, as the view draws the map.
        auto drawn_view = live_viewport(match_camera_x_, match_camera_z_);
        align_order_overlay_viewport(drawn_view);
        const auto corner = [&](int32_t cell_x, int32_t cell_z) {
            return project_match_point(
                drawn_view,
                {static_cast<uint32_t>(cell_x * OA_MAP_CELL_PIXELS) << 16,
                 static_cast<uint32_t>(site->world[1]),
                 static_cast<uint32_t>(cell_z * OA_MAP_CELL_PIXELS) << 16}
            );
        };
        const auto top_left = corner(site->cell_x, site->cell_z);
        const auto bottom_right =
            corner(site->cell_x + site->footprint_x, site->cell_z + site->footprint_z);
        const auto box = drawn_outline();
        const std::array<int64_t, 4> expected{
            top_left.x, top_left.y, bottom_right.x, bottom_right.y
        };
        if (!box || *box != expected)
            failures.push_back(
                "the outline " + name + " is drawn " +
                (box ? std::to_string((*box)[0]) + ',' + std::to_string((*box)[1]) + " to " +
                           std::to_string((*box)[2]) + ',' + std::to_string((*box)[3])
                     : std::string("nowhere")) +
                ", not around the site " + std::to_string(site->cell_x) + ',' +
                std::to_string(site->cell_z) + " at " + std::to_string(expected[0]) + ',' +
                std::to_string(expected[1]) + " to " + std::to_string(expected[2]) + ',' +
                std::to_string(expected[3])
            );
        // The outline's colour says whether the site may be built on.
        if (box) {
            const auto at =
                (static_cast<std::size_t>((*box)[1] - match_layout_.top) * match_world_cpu_.width +
                 static_cast<std::size_t>((*box)[0] - match_layout_.left)) *
                3U;
            const std::array<uint8_t, 3> colour{
                match_world_cpu_.rgb[at], match_world_cpu_.rgb[at + 1], match_world_cpu_.rgb[at + 2]
            };
            const auto wanted = !site->legal ? kBuildSiteRefusedColor
                                : site->over_own_units && ui_rules().build_tools.enabled
                                    ? kBuildSiteOverOwnUnitsColor
                                    : kBuildSiteClearColor;
            if (colour != ui_color_rgb(wanted))
                failures.push_back(
                    "the outline " + name + " is not the " + (site->legal ? "clear" : "refused") +
                    " colour"
                );
        }
        // The site's middle is the ground under the pointer moved onto the
        // cells, at most half a cell each way, and lifted by its height
        // above that ground.
        const auto zoom = static_cast<double>(match_zoom());
        const double lift =
            std::abs(static_cast<double>((site->world[1] >> 16) - ((*ground)[1] >> 16))) / 2.0;
        const double across = (OA_MAP_CELL_PIXELS / 2.0) * zoom + 2.0;
        const double down = (OA_MAP_CELL_PIXELS / 2.0 + lift) * zoom + 2.0;
        const double middle_x = static_cast<double>(expected[0] + expected[2]) / 2.0;
        const double middle_y = static_cast<double>(expected[1] + expected[3]) / 2.0;
        if (std::abs(middle_x - x) > across || std::abs(middle_y - y) > down)
            failures.push_back(
                "the outline " + name + " is centred at " + std::to_string(middle_x) + ',' +
                std::to_string(middle_y) + ", not on the pointer"
            );
        const auto orders_before = [&] {
            std::size_t count = 0;
            for (const auto* order = match_->orders(builder).primary; order != nullptr;
                 order = order->next)
                ++count;
            return count;
        }();
        send(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
        send(SDL_EVENT_MOUSE_BUTTON_UP, x, y);
        std::optional<oa::sim::match_runtime::Match::QueuedCommandView> first;
        std::size_t orders_after = 0;
        match_->visit_primary_queue(builder, [&](const auto& queued) {
            if (!first)
                first = queued;
            ++orders_after;
        });
        if (!site->legal) {
            if (orders_after != orders_before)
                failures.push_back("a click on the refused site " + name + " placed the building");
            ++refused;
        } else if (
            !first || first->kind != oa::sim::match_runtime::mobile_build_kind ||
            first->build_type != type || first->destination[0] != site->world[0] ||
            first->destination[2] != site->world[2]
        )
            failures.push_back(
                "a click " + name + " did not order MobileBuild at the site the outline showed"
            );
        else
            ++placed;
        match_->stop_orders(builder);
    };

    // The game's own screen, and a larger window with HUD scaling's chrome
    // twice the size and at the game's size.
    struct Window {
        int width{};
        int height{};
        bool hud_scaling{};
    };

    for (const auto& window :
         {Window{kCanvasWidth, kCanvasHeight, true},
          Window{1920, 1080, true},
          Window{1920, 1080, false}}) {
        auto chosen = saved_settings;
        chosen.hud_scaling = window.hud_scaling;
        apply_engine_settings(chosen);
        match_layout_ = lay_out_match(window.width, window.height);
        const auto bf_w = match_layout_.battlefield_width();
        const auto bf_h = match_layout_.battlefield_height();
        // The farthest any Maximum zoom out choice lets the view go.
        auto farthest_choice = settings::ZoomOutLimit::automatic;
        float farthest = 1.0F;
        for (const auto choice : settings::zoom_out_limits) {
            const float floor = least_battlefield_zoom(
                choice, detail_zoom_floor(), map_width, map_height, bf_w, bf_h
            );
            if (floor < farthest) {
                farthest = floor;
                farthest_choice = choice;
            }
        }
        chosen.max_zoom_out = farthest_choice;
        apply_engine_settings(chosen);
        // The wheel's steps from zoom 1 to the farthest.
        const int steps = static_cast<int>(std::ceil(
            std::log(1.0 / static_cast<double>(farthest)) /
                std::log(static_cast<double>(kZoomWheelFactor)) -
            1.0e-6
        ));
        // Zoom 1 with the clear ground at the battlefield's centre.
        match_zoom_ = match_zoom_target_ = kDefaultBattlefieldZoom;
        set_camera_position(open_x - visible_map_width() / 2, open_z - visible_map_height() / 2, 0);
        view_hold_ = {};
        match_command_ = MatchCommand::build;
        pending_build_type_ = type;
        settle();
        // The pointer comes from the side column, where the build button
        // left the normal cursor.
        send(SDL_EVENT_MOUSE_MOTION, match_layout_.left / 2, match_layout_.top + bf_h / 2);
        tick_and_draw_cursor();
        const auto size = std::to_string(window.width) + 'x' + std::to_string(window.height) +
                          " chrome " + std::to_string(match_layout_.scale).substr(0, 4);
        std::string zooms;
        int turned = 0;
        for (const int reach : {0, steps / 2, steps}) {
            wheel_out(reach - turned);
            turned = reach;
            const auto view = size + " zoom " + std::to_string(match_zoom());
            zooms += std::string(zooms.empty() ? "" : ", ") + std::to_string(match_zoom());
            // The clear ground, where a click places the building, then
            // points across the battlefield wherever it shows the map.
            auto drawn_view = live_viewport(match_camera_x_, match_camera_z_);
            align_order_overlay_viewport(drawn_view);
            const auto open_point = project_match_point(
                drawn_view,
                {static_cast<uint32_t>(open_x) << 16,
                 static_cast<uint32_t>(open_ground) << 16,
                 static_cast<uint32_t>(open_z) << 16}
            );
            try_pointer(view, open_point.x, open_point.y, true);
            for (const auto& [share_x, share_y] :
                 {std::pair{0.27, 0.31}, std::pair{0.63, 0.72}, std::pair{0.81, 0.19}}) {
                const auto x = match_layout_.left + static_cast<int>(bf_w * share_x);
                const auto y = match_layout_.top + static_cast<int>(bf_h * share_y);
                const auto shown =
                    map_pixel_drawn_at(live_viewport(match_camera_x_, match_camera_z_), {x, y});
                if (!shown || shown->x < 0 || shown->y < 0 || shown->x >= map_width ||
                    shown->y >= map_height)
                    continue;
                try_pointer(view, x, y, false);
            }
        }
        report += std::string(report.empty() ? "" : "; ") + size + " at zooms " + zooms;
    }

    apply_engine_settings(saved_settings);
    match_layout_ = saved_layout;
    match_zoom_ = saved_zoom;
    match_zoom_target_ = saved_zoom_target;
    match_camera_x_ = saved_camera_x;
    match_camera_z_ = saved_camera_z;
    view_hold_ = {};
    match_paused_ = saved_paused;
    match_pointer_known_ = saved_pointer_known;
    match_command_ = MatchCommand::build;
    pending_build_type_ = type;
    render_match_surface();
    if (!failures.empty()) {
        std::string message = "build site zoom check:";
        for (const auto& failure : failures)
            message += "\n  " + failure;
        throw std::runtime_error(message);
    }
    std::cout << "build site zoom check: normal cursor's point on the pointer, outline centred "
                 "on it and the click placing there over "
              << report << "; " << placed << " placed, " << refused << " refused\n";
}

void Runtime::check_build_site_edges(uint16_t type) {
    // Map pixels the pointer stands inside an edge of the battlefield.
    constexpr int pointer_inset = 2;
    const auto [map_width, map_height] = shown_map_size();
    const auto saved_camera_x = match_camera_x_;
    const auto saved_camera_z = match_camera_z_;
    const auto saved_zoom = match_zoom_;
    const auto saved_zoom_target = match_zoom_target_;
    const fs::path report_directory = "local/reports";
    fs::create_directories(report_directory);
    std::vector<std::string> failures;
    std::string report;
    const auto set_zoom = [&](float zoom) {
        match_zoom_ = zoom;
        match_zoom_target_ = zoom;
    };
    // Draws the frame with and without the ghost for the pointer at
    // (`pointer_x`, `pointer_y`) on the battlefield, the camera at (`camera_x`,
    // `camera_z`) and `zoom`, and compares them. The site must start left of
    // the map when `over_left` is set and above it when `over_top` is.
    const auto try_view = [&](const std::string& where,
                              int32_t camera_x,
                              int32_t camera_z,
                              int pointer_x,
                              int pointer_y,
                              float zoom,
                              bool over_left,
                              bool over_top) {
        // The zoom in tenths where it is not whole: 1, 1.5, 2.
        const auto tenths = static_cast<int>(std::lround(zoom * 10.0F));
        const auto zoom_name = std::to_string(tenths / 10) +
                               (tenths % 10 != 0 ? '.' + std::to_string(tenths % 10) : "");
        const auto name = where + " at zoom " + zoom_name;
        set_zoom(zoom);
        match_camera_x_ = camera_x;
        match_camera_z_ = camera_z;
        const auto canvas_x = static_cast<float>(match_layout_.left + pointer_x);
        const auto canvas_y = static_cast<float>(match_layout_.top + pointer_y);
        update_pointer(canvas_x, canvas_y);
        match_command_ = MatchCommand::none;
        render_match_surface();
        const auto without = match_world_cpu_;
        match_command_ = MatchCommand::build;
        pending_build_type_ = type;
        render_match_surface();
        const auto with = match_world_cpu_;
        const auto site = build_site_under(canvas_x, canvas_y);
        if (!site) {
            failures.push_back("no site under the pointer " + name);
            return;
        }
        std::string snapshot = "native-build-ghost-" + where + "-zoom-" + zoom_name + ".ppm";
        for (auto& letter : snapshot)
            if (letter == ' ')
                letter = '-';
        renderer::Surface frame;
        compose_match_frame(frame);
        write_ppm(report_directory / snapshot, frame);
        report += std::string(report.empty() ? "" : ", ") + name + " site " +
                  std::to_string(site->cell_x) + ',' + std::to_string(site->cell_z);
        if ((over_left && site->cell_x >= 0) || (over_top && site->cell_z >= 0))
            failures.push_back(
                "the site " + name + " starts at " + std::to_string(site->cell_x) + ',' +
                std::to_string(site->cell_z) + ", not over the edge of the map"
            );
        // The footprint's rectangle on the battlefield frame, from the camera
        // and the zoom alone.
        const auto scale = static_cast<double>(zoom);
        const auto raise = std::lround(static_cast<double>(site->world[1] >> 16) * 0.5 * scale);
        const auto frame_x = [&](int32_t cell) {
            return std::llround((cell * OA_MAP_CELL_PIXELS - match_camera_x_) * scale);
        };
        const auto frame_y = [&](int32_t cell) {
            return std::llround((cell * OA_MAP_CELL_PIXELS - match_camera_z_) * scale) - raise;
        };
        const auto left = frame_x(site->cell_x);
        const auto right = frame_x(site->cell_x + site->footprint_x);
        const auto top = frame_y(site->cell_z);
        const auto bottom = frame_y(site->cell_z + site->footprint_z);
        const auto band = kBuildSiteOutlineCount * build_site_outline_width(zoom);
        const auto colour =
            ui_color_rgb(site->legal ? kBuildSiteClearColor : kBuildSiteRefusedColor);
        const auto changed = [&](int64_t x, int64_t y) {
            if (x < 0 || y < 0 || x >= static_cast<int64_t>(with.width) ||
                y >= static_cast<int64_t>(with.height))
                return false;
            const auto at =
                (static_cast<std::size_t>(y) * with.width + static_cast<std::size_t>(x)) * 3U;
            return with.rgb[at] != without.rgb[at] || with.rgb[at + 1] != without.rgb[at + 1] ||
                   with.rgb[at + 2] != without.rgb[at + 2];
        };
        std::size_t drawn = 0;
        std::optional<std::pair<int64_t, int64_t>> stray;
        for (int64_t y = 0; y < static_cast<int64_t>(with.height); ++y)
            for (int64_t x = 0; x < static_cast<int64_t>(with.width); ++x) {
                if (!changed(x, y))
                    continue;
                ++drawn;
                const auto at =
                    (static_cast<std::size_t>(y) * with.width + static_cast<std::size_t>(x)) * 3U;
                const std::array<uint8_t, 3> pixel{
                    with.rgb[at], with.rgb[at + 1], with.rgb[at + 2]
                };
                // How far inside the rectangle the pixel lies, with one pixel
                // of slack for rounding at each side of the band.
                const auto depth = std::min({x - left, right - x, y - top, bottom - y});
                if (!stray && (pixel != colour || depth < -1 || depth > band))
                    stray = std::pair{x, y};
            }
        // A refused site stands at height 0, so over the bottom edge of high
        // ground its rectangle can fall wholly below the battlefield.
        const bool on_battlefield = right >= 0 && left < static_cast<int64_t>(with.width) &&
                                    bottom >= 0 && top < static_cast<int64_t>(with.height);
        if (on_battlefield && drawn == 0)
            failures.push_back("no outline drawn " + name);
        if (stray)
            failures.push_back(
                "the ghost " + name + " changed " + std::to_string(stray->first) + ',' +
                std::to_string(stray->second) + ", outside its outline " + std::to_string(left) +
                ',' + std::to_string(top) + " to " + std::to_string(right) + ',' +
                std::to_string(bottom)
            );
        if (left < 0 || top < 0 || right >= static_cast<int64_t>(with.width) ||
            bottom >= static_cast<int64_t>(with.height))
            return;
        // A whole rectangle shows the whole band: count it from outside the
        // top edge's middle down to the centre, and from outside the left
        // edge's middle across to the centre.
        const auto count_changed = [&](int64_t x, int64_t y, int64_t step_x, int64_t step_y) {
            int count = 0;
            for (; x <= (left + right) / 2 && y <= (top + bottom) / 2; x += step_x, y += step_y)
                if (changed(x, y))
                    ++count;
            return count;
        };
        const auto top_depth = count_changed((left + right) / 2, top - 2, 0, 1);
        const auto left_depth = count_changed(left - 2, (top + bottom) / 2, 1, 0);
        if (top_depth != band || left_depth != band)
            failures.push_back(
                "the outline " + name + " is " + std::to_string(top_depth) +
                " pixels deep at the top and " + std::to_string(left_depth) + " at the left, not " +
                std::to_string(band)
            );
    };
    // The view at `column` and `row` of the map (0 its left or top edge, 1 its
    // middle, 2 its right or bottom edge), the pointer as far into the
    // battlefield.
    const auto try_grid = [&](const char* where, int column, int row, float zoom) {
        set_zoom(zoom);
        const auto inset = static_cast<int>(std::lround(pointer_inset * zoom));
        const auto along = [&](int step, int extent) {
            return step == 0 ? inset : step == 1 ? extent / 2 : extent - 1 - inset;
        };
        try_view(
            where,
            column * std::max(0, map_width - visible_map_width()) / 2,
            row * std::max(0, map_height - visible_map_height()) / 2,
            along(column, match_layout_.battlefield_width()),
            along(row, match_layout_.battlefield_height()),
            zoom,
            column == 0,
            false
        );
    };

    // Over the top left corner the footprint starts left of and above the
    // map whatever the ground there, and its rectangle is placed left of and
    // above the battlefield.
    set_zoom(1.0F);
    match_command_ = MatchCommand::build;
    pending_build_type_ = type;
    const auto corner = pending_build_site({pointer_inset << 16, 0, pointer_inset << 16});
    // The first cell of a footprint `size` cells wide centred on the pointer;
    // a footprint of 0 counts as 2.
    const auto corner_cell = [&](int16_t size) {
        const auto cells = static_cast<double>(size > 0 ? size : 2);
        return static_cast<int32_t>(std::floor(
            (pointer_inset - (cells - 1.0) * OA_MAP_CELL_PIXELS / 2.0) / OA_MAP_CELL_PIXELS
        ));
    };
    const auto corner_x = corner_cell(spawn_types_[type].footprint_x);
    const auto corner_z = corner_cell(spawn_types_[type].footprint_z);
    if (!corner || corner->legal || corner->cell_x != corner_x || corner->cell_z != corner_z)
        failures.push_back(
            "the site at the top left corner is " +
            (corner ? std::to_string(corner->cell_x) + ',' + std::to_string(corner->cell_z)
                    : std::string("none")) +
            ", not a refused " + std::to_string(corner_x) + ',' + std::to_string(corner_z)
        );
    const auto corner_view = live_viewport(0, 0);
    const auto above_left = project_match_point(
        corner_view,
        {static_cast<uint32_t>(corner_x * OA_MAP_CELL_PIXELS) << 16,
         0,
         static_cast<uint32_t>(corner_z * OA_MAP_CELL_PIXELS) << 16}
    );
    if (above_left.x != corner_view.destination_x + corner_x * OA_MAP_CELL_PIXELS ||
        above_left.y != corner_view.destination_y + corner_z * OA_MAP_CELL_PIXELS)
        failures.push_back(
            "the site at the top left corner is placed at " + std::to_string(above_left.x) + ',' +
            std::to_string(above_left.y) + " on the frame"
        );

    try_grid("top left", 0, 0, 1.0F);
    try_grid("top", 1, 0, 1.0F);
    try_grid("top right", 2, 0, 1.0F);
    try_grid("left", 0, 1, 1.0F);
    try_grid("right", 2, 1, 1.0F);
    try_grid("bottom left", 0, 2, 1.0F);
    try_grid("bottom", 1, 2, 1.0F);
    try_grid("bottom right", 2, 2, 1.0F);
    for (const auto zoom : {1.0F, 1.5F, 2.0F, 3.0F})
        try_grid("middle", 1, 1, zoom);
    // Each outline is one pixel wide up to zoom 1 and as wide as the zoom,
    // rounded to the nearest pixel, zoomed in.
    for (const auto& [zoom, width] :
         {std::pair{0.5F, 1},
          std::pair{1.0F, 1},
          std::pair{1.3F, 1},
          std::pair{1.5F, 2},
          std::pair{2.0F, 2},
          std::pair{2.5F, 3},
          std::pair{3.0F, 3}})
        if (build_site_outline_width(zoom) != width)
            failures.push_back(
                "each outline is " + std::to_string(build_site_outline_width(zoom)) +
                " pixels wide at zoom " + std::to_string(zoom) + ", not " + std::to_string(width)
            );

    // The pointer reaches a site over the top edge only where the ground
    // along it is low enough; the first such place, a cell apart, is tried
    // when the map has one.
    set_zoom(1.0F);
    const auto top_inset = pointer_inset;
    std::optional<std::pair<int32_t, int>> low_top;
    const auto camera_step = std::max(1, visible_map_width() / 2);
    for (int32_t camera_x = 0; !low_top && camera_x + visible_map_width() <= map_width;
         camera_x += camera_step)
        for (int pointer_x = top_inset;
             !low_top && pointer_x < match_layout_.battlefield_width() - top_inset;
             pointer_x += OA_MAP_CELL_PIXELS) {
            match_camera_x_ = camera_x;
            match_camera_z_ = 0;
            const auto site = build_site_under(
                static_cast<float>(match_layout_.left + pointer_x),
                static_cast<float>(match_layout_.top + top_inset)
            );
            if (site && site->cell_z < 0)
                low_top = std::pair{camera_x, pointer_x};
        }
    if (low_top)
        try_view(
            "top low ground", low_top->first, 0, low_top->second, top_inset, 1.0F, false, true
        );
    else
        report += ", no site over the top edge within the pointer's reach";

    set_zoom(saved_zoom);
    match_zoom_target_ = saved_zoom_target;
    match_camera_x_ = saved_camera_x;
    match_camera_z_ = saved_camera_z;
    render_match_surface();
    if (!failures.empty()) {
        std::string message = "build ghost edge check:";
        for (const auto& failure : failures)
            message += "\n  " + failure;
        throw std::runtime_error(message);
    }
    std::cout << "build ghost edge check: " << spawn_type_names_.at(type)
              << " outline stays inside its footprint over " << report << '\n';
}

} // namespace oa::app
