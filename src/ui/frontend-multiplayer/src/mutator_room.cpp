// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle room's mutator lines. The words are looked up here so the
// interface catalogue sees them.
#include "oa/ui/frontend_multiplayer/mutator_room.hpp"

#include "oa/base/text/line_break.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace oa::ui::frontend_multiplayer {
namespace {

/// One place of a text and its value.
using Field = std::pair<std::string_view, std::string_view>;

/// The chat ring keeps 71 bytes of a line (kChatLineBytes less the terminator).
constexpr std::size_t kLineBytes = kChatLineBytes - 1;

/// Looks a text up and fills each "{name}" with its value.
///
/// @param english the text, in English
/// @param fields each place's name and value
/// @return the text
std::string filled(std::string_view english, std::initializer_list<Field> fields = {}) {
    std::string text(oa::data::languages::interface_text(english));
    for (const auto& [name, value] : fields) {
        const std::string place = "{" + std::string(name) + "}";
        for (std::size_t at = text.find(place); at != std::string::npos;
             at = text.find(place, at + value.size()))
            text.replace(at, place.size(), value);
    }
    return text;
}

/// The turned-on line with one list filled in.
///
/// @param host the host's name
/// @param list the names joined so far
/// @return the line
std::string turned_on(std::string_view host, std::string_view list) {
    return filled("*** {host} turned on mutators: {list}", {{"host", host}, {"list", list}});
}

/// A continuation with one list filled in.
///
/// @param list the names joined so far
/// @return the line
std::string continued(std::string_view list) {
    return filled("*** {list}", {{"list", list}});
}

/// Joins names onto one template until another would pass 71 bytes.
///
/// @param host the host's name, read only by the turned-on line
/// @param items the names still to place
/// @param index the first name still to place; advanced past those taken
/// @param first true for the turned-on line
/// @return the line, or empty when the next name does not fit
std::string take_line(
    std::string_view host, std::span<const std::string> items, std::size_t& index, bool first
) {
    std::string joined;
    const std::size_t start = index;
    for (; index < items.size(); ++index) {
        const std::string candidate = joined.empty() ? items[index] : joined + ", " + items[index];
        const std::string line = first ? turned_on(host, candidate) : continued(candidate);
        if (line.size() > kLineBytes)
            break;
        joined = candidate;
    }
    if (index == start)
        return {};
    return first ? turned_on(host, joined) : continued(joined);
}

/// The turned-on line with no name in it, its trailing space left out.
///
/// @param host the host's name
/// @return the line
std::string turned_on_alone(std::string_view host) {
    std::string line = turned_on(host, {});
    while (!line.empty() && line.back() == ' ')
        line.pop_back();
    return line;
}

/// The turned-on lines for a list, split at name boundaries.
///
/// The turned-on line always comes first. When the first name does not fit
/// beside the host's words, that line goes alone and the names follow.
///
/// @param host the host's name
/// @param items the names
/// @return the lines
std::vector<std::string> pack_on(std::string_view host, std::span<const std::string> items) {
    std::vector<std::string> lines;
    std::size_t index = 0;
    const std::string first = take_line(host, items, index, true);
    lines.push_back(first.empty() ? turned_on_alone(host) : first);
    while (index < items.size()) {
        const std::string line = take_line(host, items, index, false);
        if (line.empty()) {
            // One name cannot fit a line. Leave it out rather than cut it.
            ++index;
            continue;
        }
        lines.push_back(line);
    }
    return lines;
}

/// Tells whether two rectangles share an interior point.
///
/// @param first a rectangle
/// @param second another
/// @return true when they overlap
bool meets(const frontend_state::PlaceRect& first, const frontend_state::PlaceRect& second) {
    return first.width > 0 && first.height > 0 && second.width > 0 && second.height > 0 &&
           first.x < second.x + second.width && second.x < first.x + first.width &&
           first.y < second.y + second.height && second.y < first.y + first.height;
}

/// Tells whether two lists name the same mutators in the same order.
///
/// @param before the names the room had
/// @param after the names the room has
/// @return true when they are the same
bool same_names(std::span<const std::string> before, std::span<const std::string> after) {
    return before.size() == after.size() && std::equal(before.begin(), before.end(), after.begin());
}

/// Looks a text up and fills each place, dropping an empty value with the space before it.
///
/// @param english the text, in English
/// @param fields each place's name and value
/// @return the text
std::string filled_drop(std::string_view english, std::initializer_list<Field> fields) {
    std::string text(oa::data::languages::interface_text(english));
    for (const auto& [name, value] : fields) {
        const std::string place = "{" + std::string(name) + "}";
        for (std::size_t at = text.find(place); at != std::string::npos;
             at = text.find(place, at + value.size())) {
            if (value.empty() && at > 0 && text[at - 1] == ' ') {
                text.erase(at - 1, place.size() + 1);
                at -= 1;
            } else {
                text.replace(at, place.size(), value);
            }
        }
    }
    return text;
}

/// Joins names with ", " from a start, taking `count` of them.
///
/// @param names the names
/// @param start the first
/// @param count how many
/// @return the joined names
std::string join_names(std::span<const std::string> names, std::size_t start, std::size_t count) {
    std::string joined;
    const std::size_t end = std::min(names.size(), start + count);
    for (std::size_t index = start; index < end; ++index) {
        if (!joined.empty())
            joined += ", ";
        joined += names[index];
    }
    return joined;
}

/// The guest's sentence for one joined list.
///
/// Each form is a word of its own in the catalogue, so every language
/// writes it whole: one name or several, with a registry or none.
///
/// @param singular true when the list is the one-name sentence
/// @param joined the names joined so far
/// @param registry the first lack's registry; empty when none offers it
/// @return the line
std::string guest_sentence(bool singular, std::string_view joined, std::string_view registry) {
    // No registry offers it: the first sentence alone, without the promise
    // of a download.
    if (registry.empty())
        return singular ? filled_drop("*** You do not have {name}.", {{"name", joined}})
                        : filled_drop("*** You do not have {names}.", {{"names", joined}});
    return singular
               ? filled_drop(
                     "*** You do not have {name}. GET MUTATORS downloads it from {registry}.",
                     {{"name", joined}, {"registry", registry}}
                 )
               : filled_drop(
                     "*** You do not have {names}. GET MUTATORS downloads them from {registry}.",
                     {{"names", joined}, {"registry", registry}}
                 );
}

/// The full stop Chinese and Japanese write, with no space after it.
constexpr std::string_view kWideFullStop = "。";

/// Splits one sentence that does not fit after its first full stop: ". ",
/// or "。" with no space after it.
///
/// The second part starts "*** ". Nothing is returned when either part
/// would pass 71 bytes or the sentence has no full stop to split on.
///
/// @param sentence the sentence
/// @return the two lines, or nothing
std::vector<std::string> split_sentence(std::string_view sentence) {
    const std::size_t dot = sentence.find(". ");
    const std::size_t wide = sentence.find(kWideFullStop);
    std::size_t head_end = 0;
    std::size_t tail_start = 0;
    if (dot != std::string_view::npos && (wide == std::string_view::npos || dot < wide)) {
        head_end = dot + 1;
        tail_start = dot + 2;
    } else if (wide != std::string_view::npos) {
        head_end = wide + kWideFullStop.size();
        tail_start = head_end;
    } else {
        return {};
    }
    if (tail_start >= sentence.size())
        return {};
    const std::string head(sentence.substr(0, head_end));
    std::string tail(sentence.substr(tail_start));
    if (tail.rfind("*** ", 0) != 0)
        tail.insert(0, "*** ");
    if (head.size() > kLineBytes || tail.size() > kLineBytes || head.empty() || tail.empty())
        return {};
    return {head, tail};
}

/// The guest's lines for one change of what it lacks.
///
/// @param lacking what this machine lacks now
/// @return the lines
std::vector<std::string> guest_lines_of(std::span<const GuestLack> lacking) {
    std::vector<std::string> names;
    names.reserve(lacking.size());
    for (const GuestLack& one : lacking)
        names.push_back(one.name);
    if (names.empty())
        return {};
    const std::string registry = lacking.front().registry;
    const bool singular = names.size() == 1;
    const std::string whole =
        guest_sentence(singular, join_names(names, 0, names.size()), registry);
    if (whole.size() <= kLineBytes)
        return {whole};

    std::size_t fitted = 0;
    for (std::size_t count = 1; count <= names.size(); ++count) {
        const std::string candidate =
            guest_sentence(singular, join_names(names, 0, count), registry);
        if (candidate.size() > kLineBytes)
            break;
        fitted = count;
    }
    std::vector<std::string> lines;
    std::size_t index = 0;
    if (fitted > 0) {
        lines.push_back(guest_sentence(singular, join_names(names, 0, fitted), registry));
        index = fitted;
    } else {
        const std::vector<std::string> parts =
            split_sentence(guest_sentence(singular, names.front(), registry));
        if (!parts.empty()) {
            lines.insert(lines.end(), parts.begin(), parts.end());
            index = 1;
        } else {
            const std::string alone = continued(names.front());
            if (alone.size() <= kLineBytes)
                lines.push_back(alone);
            index = 1;
        }
    }
    while (index < names.size()) {
        const std::string line = take_line({}, names, index, false);
        if (line.empty()) {
            ++index;
            continue;
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

/// A short phrase for a failure the room can show on one line.
///
/// The queue's own sentence is kept for a failure this does not know. The
/// short phrase is what the interface catalogue translates.
///
/// @param problem the queue's sentence, in English
/// @return the phrase the room shows
std::string_view short_problem(std::string_view problem) noexcept {
    if (problem == "the server could not be reached")
        return "unreachable";
    if (problem == "the registry refused the download" || problem == "the package was refused")
        return "refused";
    if (problem == "the registry no longer offers this release")
        return "not offered";
    if (problem == "downloads are off for this registry")
        return "no downloads";
    if (problem == "there is not enough free space")
        return "no space";
    if (problem == "the download was not the file the catalogue promised")
        return "wrong file";
    if (problem == "the server's file is not the size the catalogue promised")
        return "wrong size";
    if (problem == "the check expired")
        return "check expired";
    if (problem == "the package could not be installed")
        return "install failed";
    if (problem == "the download could not be written")
        return "not written";
    if (problem == "the server asked for a check the game cannot do")
        return "cannot check";
    return problem;
}

/// The percent a download line shows, kept between 0 and 100.
///
/// @param percent the percent asked for
/// @return the percent shown
int32_t shown_percent(int32_t percent) noexcept {
    if (percent < 0)
        return 0;
    if (percent > 100)
        return 100;
    return percent;
}

/// The key one lack is posted under: its id, or its name when it has none.
///
/// @param one the lack
/// @return the key
std::string lack_key(const GuestLack& one) {
    return one.id.empty() ? one.name : one.id;
}

} // namespace

std::vector<std::string> mutator_chat_lines(
    std::string_view host, std::span<const std::string> before, std::span<const std::string> after
) {
    if (same_names(before, after))
        return {};
    if (after.empty())
        return {filled("*** {host} turned mutators off.", {{"host", host}})};
    std::vector<std::string> lines = pack_on(host, after);
    if (before.empty())
        lines.push_back(filled("*** This game is Open Annihilation only, and unranked."));
    return lines;
}

std::vector<std::string> lacking_lines(std::span<const PlayerLack> lacks) {
    std::vector<std::string> lines;
    lines.reserve(lacks.size());
    for (const PlayerLack& lack : lacks)
        lines.push_back(filled(
            "*** {player} does not have {name}.", {{"player", lack.player}, {"name", lack.name}}
        ));
    return lines;
}

std::string start_reason(const PlayerLack& lack) {
    return filled("Waiting for {player}: {name}", {{"player", lack.player}, {"name", lack.name}});
}

frontend_state::PlaceRect reason_box(
    const frontend_state::PlaceRect& start,
    std::span<const frontend_state::PlaceRect> others,
    int32_t line_height,
    int32_t width
) {
    if (line_height <= 0 || width <= 0)
        return {};
    // START shares its place with what is drawn on and round it, such as the
    // battle room's SYNCHING and its doors, which start a little to its left.
    // The line ends before all of them.
    int32_t left = start.x;
    for (const frontend_state::PlaceRect& obstacle : others) {
        if (meets(obstacle, start) && obstacle.x < left)
            left = obstacle.x;
    }
    const int32_t span_right = left - 4;
    int32_t edge = span_right - width;
    const int32_t top = start.y + 4;
    const int32_t bottom = top + line_height;
    for (const frontend_state::PlaceRect& obstacle : others) {
        if (obstacle.width <= 0 || obstacle.height <= 0 || meets(obstacle, start))
            continue;
        const int32_t obstacle_right = obstacle.x + obstacle.width;
        const int32_t obstacle_bottom = obstacle.y + obstacle.height;
        const bool crosses_rows = top < obstacle_bottom && obstacle.y < bottom;
        const bool crosses_columns = edge < obstacle_right && obstacle.x < span_right;
        if (!crosses_rows || !crosses_columns)
            continue;
        if (obstacle_right > edge)
            edge = obstacle_right;
    }
    if (edge >= span_right)
        return {};
    return {edge, top, span_right - edge, line_height};
}

frontend_state::BlockPlace place_over_anchor(
    const frontend_state::PlaceRect& anchor,
    std::span<const frontend_state::PlaceRect> others,
    const frontend_state::PlaceRect& room,
    frontend_state::BlockSizes sizes
) {
    for (;;) {
        frontend_state::BlockPlace block = frontend_state::place_block(anchor, others, room, sizes);
        const frontend_state::PlaceRect& last =
            block.lines.empty() ? block.button : block.lines.back();
        const bool under = block.button.y >= anchor.y + anchor.height;
        const bool over =
            block.button.y < anchor.y && last.y + last.height + sizes.gap + 1 >= anchor.y;
        if (under || over || sizes.lines == 0)
            return block;
        --sizes.lines;
    }
}

std::vector<std::string>
guest_lack_lines(std::span<const std::string> before, std::span<const GuestLack> lacking) {
    std::vector<std::string> keys;
    keys.reserve(lacking.size());
    for (const GuestLack& one : lacking)
        keys.push_back(lack_key(one));
    if (keys.empty() || same_names(before, keys))
        return {};
    return guest_lines_of(lacking);
}

std::vector<std::string> fit_chat_line(
    std::string_view line, int32_t width, const std::function<int32_t(std::string_view)>& measure
) {
    // The ring keeps kLineBytes of a stored line (lobby_post_chat), and a
    // longer one loses its end there. A Chinese character takes three bytes
    // and about two Latin letters' width, so a Chinese line meets that limit
    // well before the list's edge.
    const auto fits = [&](std::string_view text) {
        return text.size() <= kLineBytes && measure(text) <= width;
    };
    if (line.empty() || width <= 0 || !measure || fits(line))
        return {std::string(line)};
    constexpr std::string_view mark = "*** ";
    const bool marked = line.starts_with(mark);
    std::string_view rest = marked ? line.substr(mark.size()) : line;
    // A script written without spaces has no word to break at. The text
    // kit's row break keeps every UTF-8 character whole, so a placeholder's
    // value is never split mid-character. A "{name}" still in the line is
    // one Latin word and stays whole unless it is wider than the row.
    if (oa::base::text::has_wide_script(rest)) {
        std::vector<std::string> lines;
        while (!rest.empty()) {
            const std::string prefix = marked ? std::string(mark) : std::string();
            const auto row = oa::base::text::first_row(rest, [&](std::string_view start) {
                return fits(prefix + std::string(start));
            });
            const std::size_t take = row.bytes > 0 ? row.bytes : row.next;
            if (take == 0)
                break;
            lines.push_back(prefix + std::string(rest.substr(0, take)));
            const std::size_t step = row.next > take ? row.next : take;
            if (step > rest.size())
                break;
            rest.remove_prefix(step);
        }
        if (!lines.empty())
            return lines;
    }
    std::vector<std::string_view> words;
    while (!rest.empty()) {
        const std::size_t space = rest.find(' ');
        if (space == std::string_view::npos) {
            words.push_back(rest);
            break;
        }
        if (space > 0)
            words.push_back(rest.substr(0, space));
        rest.remove_prefix(space + 1);
    }
    std::vector<std::string> lines;
    std::string current;
    for (const std::string_view word : words) {
        const std::string next =
            current.empty() ? (marked ? std::string(mark) : std::string()) + std::string(word)
                            : current + " " + std::string(word);
        if (current.empty() || fits(next)) {
            current = next;
            continue;
        }
        lines.push_back(std::move(current));
        current = (marked ? std::string(mark) : std::string()) + std::string(word);
    }
    if (!current.empty())
        lines.push_back(std::move(current));
    if (lines.empty())
        return {std::string(line)};
    return lines;
}

std::string guest_ready_line(std::string_view name) {
    return filled("Ready once {name} is here", {{"name", name}});
}

GetMutators get_mutators_state(std::span<const MutatorFetch> states) noexcept {
    for (const MutatorFetch state : states) {
        switch (state) {
        case MutatorFetch::have:
            continue;
        case MutatorFetch::get:
        case MutatorFetch::other_release_get:
            return GetMutators::get;
        case MutatorFetch::gone:
        case MutatorFetch::other_release_gone:
            return GetMutators::gone;
        case MutatorFetch::waiting_for_catalogue:
            return GetMutators::none;
        case MutatorFetch::downloading:
            return GetMutators::downloading;
        case MutatorFetch::failed:
            return GetMutators::failed;
        case MutatorFetch::downloads_off:
            return GetMutators::downloads_off;
        }
    }
    return GetMutators::none;
}

std::string get_mutators_text(
    GetMutators state,
    std::string_view name,
    int32_t percent,
    std::string_view problem,
    std::string_view registry
) {
    switch (state) {
    case GetMutators::none:
    case GetMutators::get:
        return {};
    case GetMutators::downloading:
        return filled_drop(
            "Getting {name}: {percent}%",
            {{"name", name}, {"percent", std::to_string(shown_percent(percent))}}
        );
    case GetMutators::failed:
        return filled_drop(
            "{name}: {problem}",
            {{"name", name},
             {"problem", oa::data::languages::interface_text(short_problem(problem))}}
        );
    case GetMutators::gone:
        return filled_drop("{name}: not offered.", {{"name", name}});
    case GetMutators::downloads_off:
        if (registry.empty())
            return std::string(oa::data::languages::interface_text("Downloads are off."));
        return filled_drop("{registry}: downloads off.", {{"registry", registry}});
    }
    return {};
}

bool get_mutators_button_shown(GetMutators state) noexcept {
    return state == GetMutators::get || state == GetMutators::failed;
}

} // namespace oa::ui::frontend_multiplayer
