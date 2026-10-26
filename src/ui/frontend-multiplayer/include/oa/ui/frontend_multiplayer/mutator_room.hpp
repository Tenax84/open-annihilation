// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Chat lines and the waiting line for the host's mutators in the battle room.
// Nothing here reads the lobby or posts a line: the caller does.
#pragma once

#include "oa/ui/frontend_multiplayer/host_mutators.hpp"
#include "oa/ui/frontend_state/oa_button_place.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::frontend_multiplayer {

/// The lines a change of the room's mutators posts.
///
/// The same list posts nothing. A list that becomes empty posts
/// "*** {host} turned mutators off." A list that was empty posts the
/// turned-on lines and then "*** This game is Open Annihilation only, and
/// unranked." Any other change posts the turned-on lines only. The names
/// are joined with ", " and split at those boundaries so no line passes 71
/// bytes and no name is cut. Each continuation starts "*** ". A first name
/// that does not fit beside the host's words starts the next line, and the
/// turned-on line goes alone, so a change always says who made it.
///
/// @param host the host's name
/// @param before the names the room had
/// @param after the names the room has
/// @return the lines, in the order they are posted
[[nodiscard]] std::vector<std::string> mutator_chat_lines(
    std::string_view host, std::span<const std::string> before, std::span<const std::string> after
);

/// Breaks one chat line into lines the battle room's chat list can show.
///
/// The list draws one stored line on one row and clips the rest, and the
/// chat ring keeps 71 bytes of a line (kChatLineBytes less the terminator).
/// A typed line that does not fit is posted as several lines; this does the
/// same at spaces. A line holding Chinese or Japanese characters, kana or
/// Hangul breaks as the text kit wraps a description (first_row): between
/// two Chinese or Japanese characters as well, never inside a UTF-8
/// sequence, and a Latin or Hangul word only when it is wider than the row.
/// A line that fits the room and the 71 bytes is returned as itself; every
/// other line is no wider and no longer than that. A line that starts with
/// "*** " starts each further line with "*** ". A single Latin word wider
/// than the room, in a line without those scripts, is kept whole.
///
/// @param line the line, in UTF-8
/// @param width the room, in the measure's units; 0 keeps the line whole
/// @param measure the width of a line as the chat will draw it
/// @return the lines, in order
[[nodiscard]] std::vector<std::string> fit_chat_line(
    std::string_view line, int32_t width, const std::function<int32_t(std::string_view)>& measure
);

/// One mutator a seated player does not have.
struct PlayerLack {
    std::string player{}; ///< the player's name
    std::string name{};   ///< the mutator's name
};

/// One line per lack: "*** {player} does not have {name}."
///
/// @param lacks the lacks, in the order to post
/// @return the lines
[[nodiscard]] std::vector<std::string> lacking_lines(std::span<const PlayerLack> lacks);

/// The line beside START: "Waiting for {player}: {name}".
///
/// @param lack the player and the mutator
/// @return the line
[[nodiscard]] std::string start_reason(const PlayerLack& lack);

/// A small line just before START.
///
/// START's place takes in every other rectangle that overlaps START, such
/// as the battle room's SYNCHING and the doors drawn round START. The right
/// edge is 4 points before that place's left. The top is 4 points under
/// START's top. Any other rectangle the line meets shortens it from the
/// left. An edge that only meets does not. Nothing remains when the line
/// would be empty.
///
/// @param start START's rectangle
/// @param others every other rectangle, those overlapping START included
/// @param line_height the line's height
/// @param width the width asked for, before it is shortened
/// @return the line, or an empty rectangle when nothing remains
[[nodiscard]] frontend_state::PlaceRect reason_box(
    const frontend_state::PlaceRect& start,
    std::span<const frontend_state::PlaceRect> others,
    int32_t line_height,
    int32_t width
);

/// Places the host's block just over or under its anchor, dropping lines rather than lifting it.
///
/// place_block lifts a block past every rectangle it meets. Over Select
/// Map that would carry the block past the battle room's player rows into
/// its heading. Here the lines drop from the last until the block sits just
/// over or just under the anchor. With no line left, place_block's own
/// answer stands.
///
/// @param anchor the gadget the button is anchored to
/// @param others every other rectangle the block must miss
/// @param room the frame the block must stay inside
/// @param sizes the button, the lines asked for and the gap
/// @return the place
[[nodiscard]] frontend_state::BlockPlace place_over_anchor(
    const frontend_state::PlaceRect& anchor,
    std::span<const frontend_state::PlaceRect> others,
    const frontend_state::PlaceRect& room,
    frontend_state::BlockSizes sizes
);

/// One mutator this machine does not have of the host's list.
struct GuestLack {
    std::string name{}; ///< the mutator's name, or its id
    /// The registry that offers the host's release, by its name or id; empty
    /// when none does, and then no download is promised.
    std::string registry{};
    /// The mutator's id, the key of the line. Empty means the name is the
    /// key. A later name for the same id does not post the line again.
    std::string id{};
};

/// The lines a guest posts when what it lacks changes.
///
/// The same keys post nothing. A lack's key is its id, or its name when it
/// has no id, so a name that arrives after the id was posted does not post
/// the line again. Nothing lacking posts nothing. One name posts "*** You
/// do not have {name}. GET MUTATORS downloads it from {registry}." Several
/// post "*** You do not have {names}. GET MUTATORS downloads them from
/// {registry}." with the first lack's registry. With no registry, the first
/// lack offered by none, "*** You do not have {name}." or "*** You do not
/// have {names}." posts alone. Each form is a catalogue word of its own.
/// Names are joined with ", " and split at those boundaries so no line
/// passes 71 bytes and no name is cut. A sentence that does not fit splits
/// after its first full stop, ". " or "。", and the rest starts "*** ". The
/// lines are not sent: the caller posts them in the local chat.
///
/// @param before the keys last posted, in the host's order
/// @param lacking what this machine lacks now, in the host's order
/// @return the lines, in the order they are posted
[[nodiscard]] std::vector<std::string>
guest_lack_lines(std::span<const std::string> before, std::span<const GuestLack> lacking);

/// The line in READY's place: "Ready once {name} is here".
///
/// @param name the mutator READY is waiting on
/// @return the line
[[nodiscard]] std::string guest_ready_line(std::string_view name);

/// What GET MUTATORS shows a guest, from the host's list in the host's order.
///
/// The first item that is not already here decides. `waiting_for_catalogue`
/// decides `none` until that catalogue is read, and a later item is not
/// consulted. An empty list, or one this machine already has, is `none`.
enum class GetMutators : uint8_t {
    none,          ///< no line of its own: the summary names the set
    get,           ///< the button, and the summary
    downloading,   ///< the bar, and "Getting {name}: {percent}%"
    failed,        ///< the button again, and "{name}: {problem}"
    gone,          ///< "{name}: not offered."
    downloads_off, ///< "{registry}: downloads off."
};

/// Decides what GET MUTATORS shows from each item's fetch state.
///
/// The first item that is not `have` decides, in the host's order. `get` and
/// `other_release_get` give `get`. `gone` and `other_release_gone` give
/// `gone`. `waiting_for_catalogue` gives `none`. `downloading`, `failed` and
/// `downloads_off` give the state of the same name. An empty list, or one
/// of `have` only, gives `none`.
///
/// @param states each item's fetch state, in the host's order
/// @return the state
[[nodiscard]] GetMutators get_mutators_state(std::span<const MutatorFetch> states) noexcept;

/// Returns the line GET MUTATORS shows for a state, in the language shown.
///
/// `none` and `get` have no line of their own: the summary names the set.
/// The percent is kept between 0 and 100. A known failure is a short phrase,
/// then translated. A registry left empty says that downloads are off.
///
/// @param state the state
/// @param name the mutator the deciding item names
/// @param percent how much of that download has arrived
/// @param problem why the last try failed, in English
/// @param registry the registry the deciding item used, by name or by id
/// @return the line; empty for none and get
[[nodiscard]] std::string get_mutators_text(
    GetMutators state,
    std::string_view name,
    int32_t percent,
    std::string_view problem,
    std::string_view registry
);

/// Tells whether GET MUTATORS is on the button row.
///
/// @param state the state
/// @return true for get and failed
[[nodiscard]] bool get_mutators_button_shown(GetMutators state) noexcept;

} // namespace oa::ui::frontend_multiplayer
