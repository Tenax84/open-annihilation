// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle room's mutator lines and the host's block, over the room's
// rectangles. No game data: place_block, the chat lines, the waiting line
// and one translated lookup.

#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/netgame/unicode_chat.hpp"
#include "oa/ui/frontend_multiplayer/mutator_room.hpp"
#include "oa/ui/frontend_state/oa_button_place.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mp = oa::ui::frontend_multiplayer;
namespace place = oa::ui::frontend_state;

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++failures;
    }
}

/// Scales one frame pixel into points. 1/1 is the 640 by 480 frame.
int32_t scaled(int32_t value, int32_t numerator, int32_t denominator) {
    return value * numerator / denominator;
}

place::PlaceRect scale_rect(place::PlaceRect rect, int32_t numerator, int32_t denominator) {
    return {
        scaled(rect.x, numerator, denominator),
        scaled(rect.y, numerator, denominator),
        scaled(rect.width, numerator, denominator),
        scaled(rect.height, numerator, denominator)
    };
}

/// Tells whether two rectangles share an interior point.
bool meets(const place::PlaceRect& first, const place::PlaceRect& second) {
    return first.x < second.x + second.width && second.x < first.x + first.width &&
           first.y < second.y + second.height && second.y < first.y + first.height;
}

/// The battle room's controls, in frame pixels, apart from Select Map.
///
/// Only controls: the heading over the player rows is the picture's, and
/// nothing stops a block lifted that far.
std::vector<place::PlaceRect> room_obstacles() {
    std::vector<place::PlaceRect> obstacles;
    for (int32_t row = 0; row < 10; ++row)
        obstacles.push_back({9, 69 + row * 20, 486, 20});
    const place::PlaceRect column[] = {
        {510, 12, 120, 20},
        {510, 94, 120, 20},
        {510, 134, 120, 20},
        {510, 175, 120, 20},
        {510, 215, 120, 20},
        {510, 255, 120, 20},
        {510, 295, 120, 20},
        {510, 335, 120, 20},
        {510, 374, 120, 20},
        {510, 396, 120, 20},
        {510, 416, 120, 20},
    };
    for (const place::PlaceRect& one : column)
        obstacles.push_back(one);
    obstacles.push_back({110, 308, 191, 13});
    obstacles.push_back({21, 342, 414, 96});
    obstacles.push_back({21, 441, 294, 20});
    obstacles.push_back({522, 437, 96, 31});
    obstacles.push_back({508, 437, 125, 30});
    obstacles.push_back({9, 456, 447, 20});
    return obstacles;
}

/// Places two lines above Select Map and reports what was kept.
void place_above_map(
    int32_t numerator, int32_t denominator, std::size_t kept_lines, const char* label
) {
    const place::PlaceRect map = scale_rect({327, 306, 120, 20}, numerator, denominator);
    std::vector<place::PlaceRect> obstacles = room_obstacles();
    for (place::PlaceRect& obstacle : obstacles)
        obstacle = scale_rect(obstacle, numerator, denominator);
    const place::PlaceRect room = scale_rect({0, 0, 640, 480}, numerator, denominator);
    // Compact metrics, and a button wider than the kit's least width.
    const place::BlockSizes sizes{96, 17, 12, 200, 80, 4, std::size_t{2}, 0};
    const place::BlockPlace block = mp::place_over_anchor(map, obstacles, room, sizes);
    expect(block.lines.size() == kept_lines, label);
    const auto last = block.lines.empty() ? block.button : block.lines.back();
    expect(last.y + last.height + sizes.gap >= map.y - 1, "the block sits just over Select Map");
    expect(
        block.button.x + block.button.width == map.x + map.width,
        "the button's right edge is Select Map's"
    );
    expect(block.button.y + block.button.height <= map.y, "the button is above Select Map");
    for (const place::PlaceRect& line : block.lines) {
        expect(line.x + line.width == map.x + map.width, "a line's right edge is Select Map's");
        expect(line.y + line.height <= map.y, "a line is above Select Map");
        expect(!meets(line, map), "a line meets Select Map");
    }
    expect(!meets(block.button, map), "the button meets Select Map");
    const auto clear = [&](const place::PlaceRect& part) {
        for (const place::PlaceRect& obstacle : obstacles)
            if (meets(part, obstacle))
                return false;
        return true;
    };
    expect(clear(block.button), "the button meets a control of the room");
    for (const place::PlaceRect& line : block.lines)
        expect(clear(line), "a line meets a control of the room");
}

bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

bool any_contains(std::span<const std::string> lines, std::string_view needle) {
    for (const std::string& line : lines)
        if (contains(line, needle))
            return true;
    return false;
}

void expect_fits(std::span<const std::string> lines) {
    for (const std::string& line : lines) {
        expect(line.size() <= 71, "a chat line passes 71 bytes");
        expect(line != "*** ", "a chat line is an empty continuation");
        expect(!line.empty(), "a chat line is empty");
    }
}

void chat_cases() {
    const std::vector<std::string> none;
    const std::vector<std::string> gatehouse{"Gatehouse"};
    const std::vector<std::string> both{"Gatehouse", "Ridge"};
    const std::vector<std::string> on = mp::mutator_chat_lines("Ada", none, both);
    expect(any_contains(on, "Ada turned on mutators:"), "turning on names the host");
    expect(any_contains(on, "Gatehouse"), "turning on names Gatehouse");
    expect(any_contains(on, "Ridge"), "turning on names Ridge");
    expect(
        any_contains(on, "Open Annihilation only"),
        "going from none to some says the game is Open Annihilation only"
    );
    expect_fits(on);

    const std::vector<std::string> changed = mp::mutator_chat_lines("Ada", gatehouse, both);
    expect(any_contains(changed, "turned on mutators:"), "a change says the mutators turned on");
    expect(
        !any_contains(changed, "Open Annihilation only"),
        "a change of two lists says the game is Open Annihilation only"
    );
    expect(
        !any_contains(changed, "turned mutators off"),
        "a change of two lists says the mutators turned off"
    );
    expect_fits(changed);

    const std::vector<std::string> off = mp::mutator_chat_lines("Ada", gatehouse, none);
    expect(off.size() == 1, "turning off posts one line");
    expect(any_contains(off, "Ada turned mutators off."), "turning off names the host");
    expect_fits(off);

    // "*** " + "Ada" + " turned on mutators: " is 28 bytes, leaving 43.
    // Four names of 9 and the commas between them are 42. The fifth spills.
    const std::vector<std::string> long_list{
        "Northgate", "Ridgewell", "Gatehouse", "Keepstone", "Ridgetops"
    };
    const std::vector<std::string> split = mp::mutator_chat_lines("Ada", none, long_list);
    expect(!split.empty(), "a long list posts a line");
    expect(
        !split.empty() && split.front().starts_with("*** Ada turned on mutators:"),
        "the first line is the turned-on line"
    );
    expect_fits(split);
    bool continued = false;
    for (std::size_t index = 1; index < split.size(); ++index) {
        expect(split[index].starts_with("*** "), "a continuation starts with *** ");
        if (contains(split[index], "Ridgetops"))
            continued = true;
    }
    expect(continued, "the name that does not fit continues on its own line");
    for (const std::string& name : long_list)
        expect(any_contains(split, name), "a long list leaves a name out");

    expect(mp::mutator_chat_lines("Ada", both, both).empty(), "the same list posts nothing");
    expect(mp::mutator_chat_lines("Ada", none, none).empty(), "no list posts nothing");

    // "*** " + 30 + " turned on mutators: " + 16 is 71, so the turned-on line
    // just holds a 16-byte name.
    const std::string host(30, 'A');
    const std::vector<std::string> exact =
        mp::mutator_chat_lines(host, none, std::vector<std::string>{"Verylongmutators"});
    expect(
        !exact.empty() && exact.front() == "*** " + host + " turned on mutators: Verylongmutators",
        "a name that just fits stays on the turned-on line"
    );
    expect_fits(exact);

    // One byte more is 72: the turned-on line goes alone, and the name
    // starts the next line, whole.
    const std::vector<std::string> tight =
        mp::mutator_chat_lines(host, none, std::vector<std::string>{"Verylongmutatorss"});
    expect(
        tight.size() == 3 && tight[0] == "*** " + host + " turned on mutators:",
        "a name that does not fit leaves the turned-on line alone"
    );
    expect(
        tight.size() == 3 && tight[1] == "*** Verylongmutatorss",
        "a name that does not fit starts the next line"
    );
    expect(any_contains(tight, "Open Annihilation only"), "the Open Annihilation line follows");
    expect_fits(tight);
}

void lack_and_reason() {
    const mp::PlayerLack lacks[] = {
        {"Ana", "Gatehouse"},
        {"Bea", "Ridge"},
    };
    const std::vector<std::string> lines = mp::lacking_lines(lacks);
    expect(lines.size() == 2, "each lack posts a line");
    expect(
        lines.size() == 2 && contains(lines[0], "Ana") && contains(lines[0], "Gatehouse"),
        "the first lack names Ana and Gatehouse"
    );
    expect(
        lines.size() == 2 && contains(lines[1], "Bea") && contains(lines[1], "Ridge"),
        "the second lack names Bea and Ridge"
    );
    expect(mp::start_reason(lacks[0]) == "Waiting for Ana: Gatehouse", "the waiting line");

    const place::PlaceRect start{522, 437, 96, 31};
    const place::PlaceRect message{21, 441, 294, 20};
    const std::span<const place::PlaceRect> message_span(&message, 1);
    const place::PlaceRect box = mp::reason_box(start, message_span, 12, 400);
    expect(
        box.x == 315 && box.y == 441 && box.width == 203 && box.height == 12,
        "MESSAGE shortens the waiting line"
    );
    const place::PlaceRect unmet = mp::reason_box(start, message_span, 12, 200);
    expect(unmet.x == 318 && unmet.width == 200, "an edge that only meets does not shorten");

    // LOUNGE2's doors (battlestart) start 14 pixels left of START, and
    // SYNCHING shares START's place: the line ends 4 before the doors.
    const place::PlaceRect room_parts[] = {
        message, {522, 437, 96, 31}, {508, 437, 125, 30}, {9, 456, 447, 20}, {21, 342, 414, 96}
    };
    const place::PlaceRect doors = mp::reason_box(start, room_parts, 12, 400);
    expect(
        doors.x == 315 && doors.y == 441 && doors.width == 189 && doors.height == 12,
        "the waiting line ends before START's doors"
    );
}

void translated() {
    oa::data::languages::InterfaceText catalogue;
    expect(
        catalogue.add(
            "[Waiting for {player}: {name}]\n{\nde=Wartet auf {player}: {name};\n}\n"
            "[Getting {name}: {percent}%]\n{\nde=Hole {name}: {percent}%;\n}\n"
            "[Ready once {name} is here]\n{\nde=Bereit, sobald {name} da ist;\n}\n"
        ),
        "the catalogue reads"
    );
    const oa::data::languages::Language* german = oa::data::languages::find_by_tag("de");
    expect(german != nullptr, "German is known");
    if (german == nullptr)
        return;
    oa::data::languages::set_interface_language(&catalogue, *german);
    const mp::PlayerLack lack{"Ana", "Gatehouse"};
    const std::string waiting = mp::start_reason(lack);
    const std::string downloading =
        mp::get_mutators_text(mp::GetMutators::downloading, "Ridge", 60, {}, {});
    const std::string ready = mp::guest_ready_line("Ridge");
    oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());
    expect(waiting == "Wartet auf Ana: Gatehouse", "the waiting line is translated");
    expect(downloading == "Hole Ridge: 60%", "the download line is translated");
    expect(ready == "Bereit, sobald Ridge da ist", "the ready line is translated");
}

} // namespace

/// place_block alone lifts a two-line block past the player rows at 640 by
/// 480. place_over_anchor drops the second line instead.
void lift_is_refused() {
    const place::PlaceRect map{327, 306, 120, 20};
    const std::vector<place::PlaceRect> obstacles = room_obstacles();
    const place::BlockSizes sizes{96, 17, 12, 200, 80, 4, std::size_t{2}, 0};
    const place::BlockPlace lifted = place::place_block(map, obstacles, {0, 0, 640, 480}, sizes);
    expect(lifted.button.y < 69, "place_block alone lifts the block past the player rows");
    const place::BlockPlace kept = mp::place_over_anchor(map, obstacles, {0, 0, 640, 480}, sizes);
    expect(kept.button.y > 269, "place_over_anchor keeps the block under the player rows");
}

/// Two buttons on one row, right-aligned to Select Map, for the guest's block.
place::BlockSizes guest_sizes() {
    place::BlockSizes sizes{52, 17, 12, 200, 80, 4, std::size_t{1}, 0};
    sizes.button_widths = {64, 96};
    sizes.button_gap = 5;
    return sizes;
}

/// GET MAP's button and line at 1:1, in the layer's points, over the map name.
std::vector<place::PlaceRect> get_map_box() {
    return {{205, 283, 96, 17}, {110, 327, 191, 12}};
}

bool same_rect(const place::PlaceRect& rect, int32_t x, int32_t y, int32_t width, int32_t height) {
    return rect.x == x && rect.y == y && rect.width == width && rect.height == height;
}

bool clear_of(const place::BlockPlace& block, const std::vector<place::PlaceRect>& obstacles) {
    for (const place::PlaceRect& button : block.buttons) {
        for (const place::PlaceRect& obstacle : obstacles)
            if (meets(button, obstacle))
                return false;
    }
    for (const place::PlaceRect& line : block.lines) {
        for (const place::PlaceRect& obstacle : obstacles)
            if (meets(line, obstacle))
                return false;
    }
    return true;
}

void expect_row(const place::BlockPlace& block, int32_t right, const char* label) {
    expect(block.buttons.size() == 2, label);
    if (block.buttons.size() != 2)
        return;
    const place::PlaceRect& left = block.buttons.front();
    const place::PlaceRect& shown = block.buttons.back();
    expect(left.x + left.width + 5 == shown.x, "the buttons stand a gap apart");
    expect(
        same_rect(block.button, shown.x, shown.y, shown.width, shown.height),
        "the rightmost button is the row"
    );
    expect(shown.x + shown.width == right, "the row's right edge is Select Map's");
    expect(left.y == shown.y && left.height == shown.height, "the buttons share a row");
}

/// The guest's row over LOUNGE2, with GET MAP's box present and absent.
void guest_row() {
    const place::BlockSizes sizes = guest_sizes();
    const place::PlaceRect map{327, 306, 120, 20};
    const std::vector<place::PlaceRect> obstacles = room_obstacles();
    const place::PlaceRect room{0, 0, 640, 480};
    const place::BlockPlace clear = mp::place_over_anchor(map, obstacles, room, sizes);
    expect_row(clear, map.x + map.width, "640 keeps two buttons");
    expect(clear.button.y > 269, "640 without GET MAP stays under the player rows");
    expect(
        clear.button.y + clear.button.height <= map.y, "640 without GET MAP stays above Select Map"
    );
    expect(clear.lines.size() == 1, "640 without GET MAP keeps its line");
    expect(clear_of(clear, obstacles), "640 without GET MAP meets a control of the room");
    expect(same_rect(clear.buttons.front(), 282, 272, 64, 17), "640 without GET MAP places VIEW…");
    expect(same_rect(clear.button, 351, 272, 96, 17), "640 without GET MAP places GET MUTATORS");

    std::vector<place::PlaceRect> crowded = obstacles;
    const std::vector<place::PlaceRect> box = get_map_box();
    crowded.insert(crowded.end(), box.begin(), box.end());
    const place::BlockPlace beside = mp::place_over_anchor(map, crowded, room, sizes);
    expect_row(beside, map.x + map.width, "640 with GET MAP keeps two buttons");
    expect(beside.lines.empty(), "640 with GET MAP drops the line that cannot sit clear");
    expect(beside.button.y < 69, "640 with GET MAP may sit above the player rows");
    expect(clear_of(beside, crowded), "640 with GET MAP meets GET MAP or a control");
    expect(same_rect(beside.button, 351, 48, 96, 17), "640 with GET MAP places the row");

    const place::PlaceRect wide_map = scale_rect(map, 3, 2);
    std::vector<place::PlaceRect> wide_obstacles = room_obstacles();
    for (place::PlaceRect& obstacle : wide_obstacles)
        obstacle = scale_rect(obstacle, 3, 2);
    const place::PlaceRect wide_room = scale_rect(room, 3, 2);
    const place::BlockPlace wide =
        mp::place_over_anchor(wide_map, wide_obstacles, wide_room, sizes);
    expect_row(wide, wide_map.x + wide_map.width, "1280 keeps two buttons");
    expect(wide.lines.size() == 1, "1280 without GET MAP keeps its line");
    expect(clear_of(wide, wide_obstacles), "1280 without GET MAP meets a control");
    expect(same_rect(wide.button, 574, 425, 96, 17), "1280 places GET MUTATORS");

    std::vector<place::PlaceRect> wide_box = get_map_box();
    for (place::PlaceRect& part : wide_box)
        part = scale_rect(part, 3, 2);
    std::vector<place::PlaceRect> wide_crowded = wide_obstacles;
    wide_crowded.insert(wide_crowded.end(), wide_box.begin(), wide_box.end());
    const place::BlockPlace wide_beside =
        mp::place_over_anchor(wide_map, wide_crowded, wide_room, sizes);
    expect_row(wide_beside, wide_map.x + wide_map.width, "1280 with GET MAP keeps two buttons");
    expect(wide_beside.lines.size() == 1, "1280 with GET MAP keeps its line");
    expect(clear_of(wide_beside, wide_crowded), "1280 with GET MAP meets GET MAP or a control");
    expect(same_rect(wide_beside.button, 574, 425, 96, 17), "1280 with GET MAP places the row");
}

void expect_state(
    std::initializer_list<mp::MutatorFetch> states, mp::GetMutators want, const char* label
) {
    const std::vector<mp::MutatorFetch> listed(states);
    expect(mp::get_mutators_state(listed) == want, label);
}

void state_and_lines() {
    using Fetch = mp::MutatorFetch;
    expect_state({}, mp::GetMutators::none, "an empty list shows the summary");
    expect_state(
        {Fetch::have, Fetch::have}, mp::GetMutators::none, "a list already here shows the summary"
    );
    expect_state(
        {Fetch::have, Fetch::get}, mp::GetMutators::get, "get is the first that is not here"
    );
    expect_state(
        {Fetch::have, Fetch::other_release_get},
        mp::GetMutators::get,
        "another release that can be fetched is get"
    );
    expect_state({Fetch::gone}, mp::GetMutators::gone, "gone names itself");
    expect_state(
        {Fetch::other_release_gone}, mp::GetMutators::gone, "another release that is gone is gone"
    );
    expect_state({Fetch::downloading}, mp::GetMutators::downloading, "downloading names itself");
    expect_state({Fetch::failed}, mp::GetMutators::failed, "failed names itself");
    expect_state(
        {Fetch::downloads_off}, mp::GetMutators::downloads_off, "downloads off names itself"
    );
    expect_state(
        {Fetch::waiting_for_catalogue, Fetch::get},
        mp::GetMutators::none,
        "a catalogue still being read stops the scan"
    );
    expect_state(
        {Fetch::get, Fetch::downloading},
        mp::GetMutators::get,
        "the first item that is not here decides"
    );

    expect(
        mp::get_mutators_text(mp::GetMutators::none, "Ridge", 40, "cut", "Core").empty(),
        "none has no line"
    );
    expect(
        mp::get_mutators_text(mp::GetMutators::get, "Ridge", 40, "cut", "Core").empty(),
        "get has no line"
    );
    expect(
        mp::get_mutators_text(mp::GetMutators::downloading, "Ridge", 60, {}, "Core") ==
            "Getting Ridge: 60%",
        "the download line names the percent"
    );
    expect(
        mp::get_mutators_text(mp::GetMutators::downloading, "Ridge", 150, {}, {}) ==
            "Getting Ridge: 100%",
        "a percent past 100 is kept at 100"
    );
    expect(
        mp::get_mutators_text(mp::GetMutators::downloading, "Ridge", -4, {}, {}) ==
            "Getting Ridge: 0%",
        "a percent below 0 is kept at 0"
    );
    expect(
        mp::get_mutators_text(mp::GetMutators::failed, "Ridge", 0, "the file was cut", "Core") ==
            "Ridge: the file was cut",
        "a failure says why"
    );
    expect(
        mp::get_mutators_text(
            mp::GetMutators::failed, "Ridge Tough", 0, "the server could not be reached", {}
        ) == "Ridge Tough: unreachable",
        "a server that cannot be reached says unreachable"
    );
    expect(
        mp::get_mutators_text(mp::GetMutators::gone, "Ridge", 0, {}, {}) == "Ridge: not offered.",
        "gone says the registry does not offer it"
    );
    expect(
        mp::get_mutators_text(mp::GetMutators::downloads_off, "Ridge", 0, {}, "Core") ==
            "Core: downloads off.",
        "downloads off names the registry and keeps off"
    );
    expect(
        mp::get_mutators_text(mp::GetMutators::downloads_off, "Ridge", 0, {}, {}) ==
            "Downloads are off.",
        "an empty registry still says downloads are off"
    );
    expect(
        mp::guest_ready_line("Ridge") == "Ready once Ridge is here",
        "the ready line names the mutator"
    );
    for (const mp::GetMutators state : {
             mp::GetMutators::none,
             mp::GetMutators::get,
             mp::GetMutators::downloading,
             mp::GetMutators::failed,
             mp::GetMutators::gone,
             mp::GetMutators::downloads_off,
         }) {
        const bool shown = state == mp::GetMutators::get || state == mp::GetMutators::failed;
        expect(
            mp::get_mutators_button_shown(state) == shown, "the button shows for get and failed"
        );
    }
}

void guest_chat() {
    const std::vector<std::string> none;
    const std::vector<mp::GuestLack> ridge{{"Ridge", "Core"}};
    const std::vector<std::string> first = mp::guest_lack_lines(none, ridge);
    expect(first.size() == 1, "one lack that fits posts one line");
    expect(
        first.size() == 1 &&
            first[0] == "*** You do not have Ridge. GET MUTATORS downloads it from Core.",
        "one lack names the mutator and its registry"
    );
    expect_fits(first);
    const std::vector<std::string> held{"Ridge"};
    expect(mp::guest_lack_lines(held, ridge).empty(), "the same lack posts nothing");
    const std::vector<mp::GuestLack> same_name{{"Ridge", "Other"}};
    expect(
        mp::guest_lack_lines(held, same_name).empty(),
        "a new registry for the same name posts nothing"
    );
    expect(mp::guest_lack_lines(held, {}).empty(), "nothing lacking posts nothing");

    // No registry offers it: no download is promised.
    const std::vector<mp::GuestLack> unoffered{{"Ridge", ""}};
    const std::vector<std::string> alone = mp::guest_lack_lines(none, unoffered);
    expect(
        alone.size() == 1 && alone[0] == "*** You do not have Ridge.",
        "a lack no registry offers names the mutator alone"
    );
    expect(!any_contains(alone, "GET MUTATORS"), "a lack no registry offers promises no download");
    const std::vector<mp::GuestLack> unoffered_both{{"Ridge", ""}, {"Keep", "Core"}};
    const std::vector<std::string> alone_both = mp::guest_lack_lines(none, unoffered_both);
    expect(
        alone_both.size() == 1 && alone_both[0] == "*** You do not have Ridge, Keep.",
        "several lacks the first registry does not offer name the mutators alone"
    );

    const std::vector<mp::GuestLack> both{{"Ridge", "Core"}, {"Keep", "Other"}};
    const std::vector<std::string> changed = mp::guest_lack_lines(held, both);
    expect(changed.size() == 1, "two short lacks fit one line");
    expect(
        changed.size() == 1 &&
            changed[0] == "*** You do not have Ridge, Keep. GET MUTATORS downloads them from Core.",
        "several lacks use the first registry, and download them"
    );
    expect_fits(changed);
    expect(!any_contains(changed, "Other"), "a later registry is not named");

    const std::vector<mp::GuestLack> swapped{{"Keep", "Core"}, {"Ridge", "Core"}};
    const std::vector<std::string> before{"Ridge", "Keep"};
    expect(!mp::guest_lack_lines(before, swapped).empty(), "a change of order posts again");

    // The key is the id. A name and a registry that arrive after the id
    // was posted do not post the line again, and the first line uses the
    // name known then.
    const std::vector<mp::GuestLack> by_id{{"ridge-tough", "", "ridge"}};
    const std::vector<std::string> id_first = mp::guest_lack_lines(none, by_id);
    expect(
        id_first.size() == 1 && id_first[0] == "*** You do not have ridge-tough.",
        "the first lack line uses the name known then"
    );
    const std::vector<mp::GuestLack> named{{"Ridge Tough", "Core Prime", "ridge"}};
    expect(
        mp::guest_lack_lines(std::vector<std::string>{"ridge"}, named).empty(),
        "a later name for the same id posts nothing"
    );
    expect(
        !mp::guest_lack_lines(
             std::vector<std::string>{"ridge"}, std::vector<mp::GuestLack>{{"Keep", "Core", "keep"}}
        )
             .empty(),
        "another id posts again"
    );

    const std::vector<mp::GuestLack> long_one{{"Stonewatch Keepers", "Core"}};
    const std::vector<std::string> split = mp::guest_lack_lines(none, long_one);
    expect(split.size() == 2, "a sentence that does not fit splits after its period");
    expect(
        split.size() == 2 && split[0] == "*** You do not have Stonewatch Keepers." &&
            split[1] == "*** GET MUTATORS downloads it from Core.",
        "the rest of the sentence starts a second line"
    );
    expect_fits(split);

    const std::vector<mp::GuestLack> continued{{"Ridge", "Core"}, {"Stonewatch Keepers", "Core"}};
    const std::vector<std::string> packed = mp::guest_lack_lines(none, continued);
    expect(packed.size() == 2, "a second name that does not fit the sentence continues");
    expect(
        packed.size() == 2 &&
            packed[0] == "*** You do not have Ridge. GET MUTATORS downloads them from Core." &&
            packed[1] == "*** Stonewatch Keepers",
        "the name that does not fit starts its own line"
    );
    expect_fits(packed);
}

/// Chinese writes its full stop with no space after it. A lack no registry
/// offers reads whole, and a sentence that does not fit splits after that
/// full stop.
void guest_chat_wide_stop() {
    oa::data::languages::InterfaceText catalogue;
    expect(
        catalogue.add(
            "[*** You do not have {name}.]\n{\nzh-Hans=*** 你没有{name}。;\n}\n"
            "[*** You do not have {names}.]\n{\nzh-Hans=*** 你没有{names}。;\n}\n"
            "[*** You do not have {name}. GET MUTATORS downloads it from {registry}.]\n"
            "{\nzh-Hans=*** 你没有{name}。获取变异器从{registry}下载它。;\n}\n"
            "[*** You do not have {names}. GET MUTATORS downloads them from {registry}.]\n"
            "{\nzh-Hans=*** 你没有{names}。获取变异器从{registry}下载它们。;\n}\n"
        ),
        "the Chinese catalogue reads"
    );
    const oa::data::languages::Language* chinese = oa::data::languages::find_by_tag("zh-Hans");
    expect(chinese != nullptr, "Simplified Chinese is known");
    if (chinese == nullptr)
        return;
    oa::data::languages::set_interface_language(&catalogue, *chinese);
    const std::vector<std::string> none;
    const std::vector<std::string> alone =
        mp::guest_lack_lines(none, std::vector<mp::GuestLack>{{"Ridge", ""}});
    const std::vector<std::string> alone_both =
        mp::guest_lack_lines(none, std::vector<mp::GuestLack>{{"Ridge", ""}, {"Keep", ""}});
    const std::vector<std::string> offered =
        mp::guest_lack_lines(none, std::vector<mp::GuestLack>{{"Ridge", "Core"}});
    const std::vector<std::string> split = mp::guest_lack_lines(
        none, std::vector<mp::GuestLack>{{"Stonewatch Keepers", "Core Prime Registry"}}
    );
    oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());
    expect(
        alone.size() == 1 && alone[0] == "*** 你没有Ridge。",
        "a Chinese lack no registry offers reads whole"
    );
    expect(!any_contains(alone, "从下载"), "a Chinese lack does not download from nothing");
    expect(
        alone_both.size() == 1 && alone_both[0] == "*** 你没有Ridge, Keep。",
        "Chinese lacks no registry offers read whole"
    );
    expect(
        offered.size() == 1 && offered[0] == "*** 你没有Ridge。获取变异器从Core下载它。",
        "a Chinese lack names its registry"
    );
    expect(
        split.size() == 2 && split[0] == "*** 你没有Stonewatch Keepers。" &&
            split[1] == "*** 获取变异器从Core Prime Registry下载它。",
        "a Chinese sentence that does not fit splits after its full stop"
    );
    expect_fits(split);
}

/// Width of an ASCII line in the small game font, in points.
///
/// The advances are that font's, one per byte from space through tilde.
/// A byte outside that range counts as far too wide.
int32_t small_font_width(std::string_view text) {
    static constexpr int32_t advances[] = {7,  4, 5, 11, 5, 9, 6, 4, 5, 5, 7, 5, 4, 5, 3, 6,
                                           6,  5, 6, 6,  7, 6, 6, 6, 5, 5, 3, 3, 7, 5, 7, 6,
                                           11, 7, 6, 6,  6, 5, 5, 6, 6, 3, 5, 6, 5, 7, 7, 6,
                                           6,  6, 6, 6,  6, 6, 7, 8, 6, 6, 5, 4, 6, 4, 7, 7,
                                           3,  6, 6, 6,  6, 6, 5, 6, 6, 4, 4, 6, 3, 8, 6, 6,
                                           6,  6, 6, 6,  5, 6, 6, 9, 5, 6, 6, 6, 4, 6, 8};
    int32_t width = 0;
    for (const unsigned char byte : text) {
        if (byte < 32 || byte > 126)
            return 10000;
        width += advances[byte - 32];
    }
    return width;
}

/// At 640 by 480 the state line is one row. The two lines the room used to
/// cut must keep their last word and fit that row, and the 179-point box
/// the compact room gives them.
void state_lines_fit_at_640() {
    const place::PlaceRect map{327, 306, 120, 20};
    const std::vector<place::PlaceRect> obstacles = room_obstacles();
    const place::PlaceRect room{0, 0, 640, 480};
    const place::BlockSizes sizes{96, 17, 12, 200, 80, 4, std::size_t{2}, 0};
    const place::BlockPlace block = mp::place_over_anchor(map, obstacles, room, sizes);
    expect(block.lines.size() == 1, "640x480 keeps one state line");
    if (block.lines.size() != 1)
        return;
    const int32_t box = block.lines.front().width;
    const int32_t budget = box < 179 ? box : 179;
    const std::string downloads =
        mp::get_mutators_text(mp::GetMutators::downloads_off, "Ridge", 0, {}, "Core Prime");
    const std::string failed = mp::get_mutators_text(
        mp::GetMutators::failed, "Ridge Tough", 0, "the server could not be reached", {}
    );
    expect(contains(downloads, "off"), "downloads off keeps off at 640x480");
    expect(contains(failed, "unreachable"), "the unreachable line keeps its meaning at 640x480");
    expect(!contains(downloads, "..."), "downloads off is not cut with an ellipsis");
    expect(!contains(failed, "..."), "the unreachable line is not cut with an ellipsis");
    expect(small_font_width(downloads) <= budget, "downloads off fits the 640 state line");
    expect(small_font_width(failed) <= budget, "unreachable fits the 640 state line");
}

/// The French unranked line, as the catalogue says it.
constexpr std::string_view kFrenchUnranked =
    "*** Cette partie est seulement Open Annihilation, et non classée.";

/// é in the 3.1c font is one code-page byte, and a line wider than the chat
/// list breaks at a space onto a line that starts "*** ".
void french_line_fits_the_code_page() {
    oa::data::languages::InterfaceText catalogue;
    expect(
        catalogue.add(
            "[*** This game is Open Annihilation only, and unranked.]\n"
            "{\nfr=*** Cette partie est seulement Open Annihilation, et non classée.;\n}\n"
        ),
        "the French unranked line is a catalogue word"
    );
    const oa::data::languages::Language* french = oa::data::languages::find_by_tag("fr");
    expect(french != nullptr, "French is a language");
    if (french == nullptr)
        return;
    oa::data::languages::set_interface_language(&catalogue, *french);
    const std::string line(
        oa::data::languages::interface_text(
            "*** This game is Open Annihilation only, and unranked."
        )
    );
    oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());
    expect(line == kFrenchUnranked, "the French unranked line is the catalogue's");
    const std::string bytes = oa::netgame::chat_code_page(line);
    expect(
        bytes.find('\xE9') != std::string::npos, "é is the code page's E9 with Unicode chat off"
    );
    expect(bytes.find("\xC3\xA9") == std::string::npos, "é is not left as two UTF-8 bytes");

    // The chat list's text starts two pixels in and stops at the gadget's
    // far edge: 414 wide in the battle room, so 412 pixels of glyphs.
    constexpr int32_t room = 412;
    const auto measure = [](std::string_view text) -> int32_t {
        return static_cast<int32_t>(oa::netgame::chat_code_page(text).size() * 7);
    };
    expect(measure(line) > room, "the French line is wider than the chat list");
    const std::vector<std::string> fitted = mp::fit_chat_line(line, room, measure);
    expect(fitted.size() >= 2, "the French line wraps onto another chat line");
    std::string joined;
    for (const std::string& piece : fitted) {
        expect(measure(piece) <= room, "each wrapped chat line fits the list");
        expect(piece.starts_with("*** "), "a wrapped chat line keeps the mark");
        if (!joined.empty())
            joined.push_back(' ');
        joined += piece.starts_with("*** ") ? piece.substr(4) : piece;
    }
    expect(joined == std::string(kFrenchUnranked).substr(4), "the wrap keeps every word");
}

/// A Simplified Chinese lack has no spaces. The chat row breaks it between
/// characters, keeps every character, and leaves a "{name}" whole.
void chinese_line_breaks_between_characters() {
    oa::data::languages::InterfaceText catalogue;
    expect(
        catalogue.add(
            "[*** You do not have {name}.]\n"
            "{\nzh-Hans=*** 你没有{name}所以这一行没有空格必须在字之间换行建造完成单位已就绪;\n}\n"
        ),
        "the Chinese lack line is a catalogue word"
    );
    const oa::data::languages::Language* chinese = oa::data::languages::find_by_tag("zh-Hans");
    expect(chinese != nullptr, "Simplified Chinese is known");
    if (chinese == nullptr)
        return;
    oa::data::languages::set_interface_language(&catalogue, *chinese);
    std::string line(oa::data::languages::interface_text("*** You do not have {name}."));
    oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());
    const std::string place = "{name}";
    const auto at = line.find(place);
    expect(at != std::string::npos, "the Chinese line still names {name}");
    if (at == std::string::npos)
        return;
    line.replace(at, place.size(), "建造完成");
    const auto measure = [](std::string_view text) -> int32_t {
        int32_t width = 0;
        for (std::size_t index = 0; index < text.size();) {
            const unsigned char lead = static_cast<unsigned char>(text[index]);
            std::size_t bytes = 1;
            if ((lead & 0xE0U) == 0xC0U)
                bytes = 2;
            else if ((lead & 0xF0U) == 0xE0U)
                bytes = 3;
            else if ((lead & 0xF8U) == 0xF0U)
                bytes = 4;
            else if ((lead & 0xC0U) == 0x80U) {
                return 10000;
            }
            if (index + bytes > text.size())
                return 10000;
            width += bytes == 1 ? 7 : 14;
            index += bytes;
        }
        return width;
    };
    expect(measure(line) > 70, "the Chinese line is wider than the row");
    const std::vector<std::string> fitted = mp::fit_chat_line(line, 70, measure);
    expect(fitted.size() >= 2, "the Chinese line breaks between characters");
    std::string joined;
    for (const std::string& piece : fitted) {
        expect(measure(piece) <= 70, "each Chinese piece fits the row");
        expect(piece.starts_with("*** "), "a Chinese piece keeps the mark");
        joined += piece.substr(4);
    }
    expect(joined == line.substr(4), "the break keeps every character");
}

/// The chat list's measure at 640x480, near enough: 7 an ASCII character,
/// 12 a Chinese one, which these lines write in three bytes. Any other byte,
/// such as a cut UTF-8 sequence, is far too wide.
int32_t list_width(std::string_view text) {
    int32_t width = 0;
    for (std::size_t at = 0; at < text.size();) {
        const auto byte = [&](std::size_t offset) {
            return static_cast<unsigned char>(text[at + offset]);
        };
        if (byte(0) < 0x80) {
            width += 7;
            ++at;
            continue;
        }
        if ((byte(0) & 0xF0U) != 0xE0U || at + 3 > text.size() || (byte(1) & 0xC0U) != 0x80U ||
            (byte(2) & 0xC0U) != 0x80U)
            return 10000;
        width += 12;
        at += 3;
    }
    return width;
}

/// A Chinese line in the 412 points of the list at 640x480 is cut by the
/// ring's 71 bytes before it reaches the edge: three bytes a character. Each
/// line stays within both, so none loses its end, and none breaks inside a
/// Latin word or starts with "，" or "。". The first line is narrower than
/// the list and still too long to store whole.
void chinese_line_keeps_the_ring_bytes() {
    constexpr int32_t room = 412;
    const std::string_view lines[] = {
        "*** Visitor没有能够在每一局游戏开始之前自动建造完成的防御工事。",
        "*** Visitor使用的是Total Annihilation 3.1c，该版本无法使用变异器。",
        "*** 你没有建造完成所以这一行没有空格必须在字之间换行建造完成单位已就绪，"
        "主机正在等待所有玩家。",
    };
    for (const std::string_view line : lines) {
        expect(line.size() > 71, "the Chinese line is longer than a stored chat line");
        const std::vector<std::string> fitted = mp::fit_chat_line(line, room, list_width);
        expect(fitted.size() >= 2, "the long Chinese line breaks");
        std::string joined;
        std::string previous;
        for (const std::string& piece : fitted) {
            expect(piece.size() <= 71, "each Chinese line keeps within the ring's 71 bytes");
            expect(list_width(piece) <= room, "each Chinese line fits the list");
            expect(piece.starts_with("*** "), "each Chinese line keeps the mark");
            const std::string_view words = std::string_view(piece).substr(4);
            expect(
                !words.starts_with("，") && !words.starts_with("。"),
                "no Chinese line starts with a comma or a full stop"
            );
            const auto latin = [](char letter) {
                return (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z');
            };
            expect(
                previous.empty() || words.empty() || !latin(previous.back()) ||
                    !latin(words.front()),
                "a Latin word is not broken"
            );
            previous = piece;
            joined += words;
        }
        std::string whole(line.substr(4));
        std::erase(whole, ' ');
        std::erase(joined, ' ');
        expect(joined == whole, "the Chinese break keeps every character");
    }
}

/// Beside START at 640x480 the waiting line has 189 points. Each language's
/// line for Visitor and Ridge Tough fits that row, so none is cut short.
void waiting_lines_fit_at_640() {
    oa::data::languages::InterfaceText catalogue;
    expect(
        catalogue.add(
            "[Waiting for {player}: {name}]\n"
            "{\n"
            "de=Wartet auf {player}: {name};\n"
            "fr=Attend {player} : {name};\n"
            "es=Esperando a {player}: {name};\n"
            "it=In attesa di {player}: {name};\n"
            "}\n"
        ),
        "the waiting line is a catalogue word"
    );
    const mp::PlayerLack lack{"Visitor", "Ridge Tough"};
    const char* tags[] = {"en", "de", "fr", "es", "it"};
    for (const char* tag : tags) {
        const oa::data::languages::Language* language = tag[0] == 'e' && tag[1] == 'n'
                                                            ? &oa::data::languages::english()
                                                            : oa::data::languages::find_by_tag(tag);
        expect(language != nullptr, "the waiting line's language is known");
        if (language == nullptr)
            continue;
        oa::data::languages::set_interface_language(&catalogue, *language);
        const std::string waiting = mp::start_reason(lack);
        oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());
        expect(small_font_width(waiting) <= 189, "the waiting line fits beside START at 640x480");
        expect(!contains(waiting, "..."), "the waiting line is not cut short");
    }
}

int main() {
    place_above_map(1, 1, 1, "640x480 keeps one line");
    lift_is_refused();
    place_above_map(3, 2, 2, "1.5 points a frame pixel keeps two lines");
    chat_cases();
    lack_and_reason();
    translated();
    guest_row();
    state_and_lines();
    state_lines_fit_at_640();
    guest_chat();
    guest_chat_wide_stop();
    french_line_fits_the_code_page();
    chinese_line_breaks_between_characters();
    chinese_line_keeps_the_ring_bytes();
    waiting_lines_fit_at_640();
    if (failures != 0)
        std::fprintf(stderr, "%d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
