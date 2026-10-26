// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The presence card: what the card of an OA player's badge in the battle
// room says (presence_card), how the kit's hover card shows it
// (presence_card_look), where it lies by the badge (presence_card_place,
// presence_card_point), and when it opens and closes (PresenceCardTrigger).
// All of it is pure: the app's host of the card on the OA layer
// (src/app/runtime_presence_card.cpp) hands it the lobby, the badges, the
// pointer and the keys, and draws what it gives with the kit.
#pragma once

#include "oa/netgame/presence.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace oa::ui::frontend_multiplayer {

/// What this machine has of the map pack the host plays, as the card's Map
/// line says it.
enum class PackState {
    unknown,       ///< not known here: the line names the pack alone
    installed,     ///< installed here: "✓ installed", the line in lime
    missing,       ///< not installed here: "· not installed"
    other_release, ///< another release of it is installed here: "· another release here", in amber
};

/// The colour a line's value is drawn in.
enum class CardTone : uint8_t {
    plain, ///< the text colour
    amber, ///< the lock colour: something differs
    lime,  ///< the accent: all is well
};

/// Whether one player has one mutator of the host's set, as the card says it.
enum class CardMutatorHave : uint8_t {
    has,           ///< installed at the host's release: "{item} ✓"
    missing,       ///< not installed: "{item}: not installed"
    other_release, ///< another release is installed: "{item}: another release here"
    unknown,       ///< not known yet: the row is left out, and does not say "not installed"
};

/// One mutator of the host's set, as a card row names it.
struct CardMutator {
    std::string name{};     ///< the mutator's name, or its id
    CardMutatorHave have{}; ///< what this player has of it
};

/// One line of the card: a label and its value, both already translated.
struct PresenceCardLine {
    std::string label{}; ///< "Engine", "Platform", "Mod", "Rules", "Mutators", "Game", "View",
                         ///< "Dev mode" or "Map"
    std::string value{}; ///< what the player's presence record says
    CardTone tone{};     ///< the value's colour
};

/// What the card of one player's OA badge says, every word translated.
struct PresenceCard {
    std::string title{};                   ///< the player's name in capitals
    std::vector<PresenceCardLine> lines{}; ///< the lines, top to bottom
    std::vector<std::string>
        hacks{};              ///< the first game hacks' ids, at most presence_card_listed_hacks
    std::string hacks_more{}; ///< "· <n> more" for the game hacks not listed; empty for none
};

/// The game hacks the card lists by id; the others are counted in PresenceCard::hacks_more.
inline constexpr std::size_t presence_card_listed_hacks = 3;
/// The card's width at Compact, in points.
inline constexpr int32_t presence_card_compact_width = 240;
/// The card's width at Regular, in points.
inline constexpr int32_t presence_card_regular_width = 280;
/// The card's width at Large, in points.
inline constexpr int32_t presence_card_large_width = 320;
/// The points the card leaves clear of the window's edges on each side.
inline constexpr int32_t presence_card_edge_gap = 8;
/// The most lines one difference line wraps onto. At Compact the German and
/// French "cannot start until" lines take three.
inline constexpr std::size_t presence_card_difference_lines = 3;

/// Returns what the card of a slot's player says, from that player's
/// presence record alone (lobby_presence_record): a setup block's bytes are
/// never read for it.
///
/// The title is the player's name in capitals. A player whose record has
/// not come, an OA build before 0.8 that sends none, gets one line,
/// "Engine: Open Annihilation (before 0.8)". With a record the lines are, in
/// order:
/// - Engine: "Open Annihilation <version>", " (development build)" in place
///   of a version's "-<label>";
/// - Platform, under Engine: "<platform> · <architecture>". A field the
///   record leaves empty is left out, and the line goes when both are empty;
/// - Mod: "<name> <version> · rev <packaging revision>", or "Total
///   Annihilation 3.1c" for a record without a mod. A record that names its
///   bundles says "No Mod" instead when it names no mod, and before Mod it
///   adds Base game and Expansions (music left out; "None" when there is no
///   other expansion). Language follows Mod when the record names one;
/// - Rules: "The host's" on the host's card; otherwise "Differ from the
///   host's" in amber when the sim hashes differ
///   (lobby_sim_hash_differs_from_host), with " (rev <n>)" when
///   the host's record names the same mod at packaging revision n, another;
///   "Same as the host's" when the two sim hashes are known and equal (a
///   3.1c host plays the plain baseline's); "Unknown" otherwise;
/// - Mutators, when `mutators` names a set: one row a mutator, the first
///   labelled "Mutators". "{item} ✓" when that player has it, or, in amber,
///   "{item}: not installed" or "{item}: another release here". A row whose
///   state is still unknown is left out. The host's own card marks every
///   one ✓;
/// - Game: how many hacks change the game; View: how many hacks change only
///   the view; Dev mode: "On", with " · <n> overrides" when it counts any,
///   or "Off". A record that leaves out one of these fields leaves out its
///   line;
/// - Map, on the host's card when its record names a map pack: "<name, or
///   the key> <version>", then by `pack_state` " ✓ installed" in lime,
///   " · another release here" in amber, " · not installed", or nothing
///   when the state is not known.
/// When both records name bundles and they differ, unlabelled amber lines
/// name the difference and say the game cannot start. The Rules line stays
/// the sim-hash comparison above. A player who lacks a mutator of the set
/// then gets amber lines after those: "Different from the host: no {name}."
/// or "Different from the host: another release of {name}.", and "The game
/// cannot start until {player} gets it, or the host turns it off." The
/// host's card has none of those lines.
/// The first presence_card_listed_hacks game hacks' ids follow, and
/// "· <n> more" for the rest. The mutator rows and their difference lines
/// go through the interface catalogue (interface_text). Every other label
/// and fixed word goes through the lobby's translation (lobby_translated).
///
/// @param lobby the battle room
/// @param slot the player's slot, 0..9
/// @param pack_state what this machine has of a map pack; unset counts as unknown
/// @param mutators the host's set and what this player has of each; empty adds no rows
/// @return the card; nothing for a slot without the OA badge (lobby_row_badge)
[[nodiscard]] std::optional<PresenceCard> presence_card(
    Lobby& lobby,
    int32_t slot,
    const std::function<PackState(const netgame::PresenceMapPack&)>& pack_state,
    std::span<const CardMutator> mutators = {}
);

/// Returns the kit's hover card for a presence card.
///
/// The card has the OA mark and the card's title in its header, and nothing
/// at the header's right. It has one
/// row a line (its value in the text colour, the lock colour for amber or
/// the accent for lime), and the hack ids and "· <n> more" as its block. It
/// is presence_card_compact_width, presence_card_regular_width or
/// presence_card_large_width wide by the class, and never wider than the
/// window less twice presence_card_edge_gap. A labelled value or a block
/// line wider than its room, the width less the class's hover_card_padding
/// either side and, for a row, the label column, is cut with "..." at a
/// character (kit::cut_to_width). A difference line, which has no label, wraps
/// instead, onto at most presence_card_difference_lines lines. The last of
/// them is cut with "..." when the rest does not fit, and the lines before it
/// are not. Its size is kit::hover_card_size's.
///
/// @param card what the card says
/// @param size_class the OA layer's size class
/// @param window_points_width the room's width, in the layer's points
/// @param fonts the fonts the card is measured in
/// @return the hover card, its arrow not yet pointed (presence_card_point)
[[nodiscard]] kit::HoverCardLook presence_card_look(
    const PresenceCard& card,
    kit::SizeClass size_class,
    int32_t window_points_width,
    const kit::Fonts& fonts
);

/// Returns where the card lies by its badge: kit::hover_card_rect with the
/// badge itself as the anchor, since that already leaves the arrow's depth
/// between the badge and the card, and the safe area less
/// presence_card_edge_gap on each side as the frame. The card lies below the
/// badge, above it when there is no room below, and inside the window.
///
/// @param badge the badge, in the layer's points
/// @param size the card's size, from kit::hover_card_size
/// @param safe_area the window's safe area, in the layer's points
/// @param arrow_depth the class's hover_card_arrow, the gap hover_card_rect
///     leaves for the arrow; it moves nothing
/// @return the card's rectangle, in the layer's points, its arrow outside it
[[nodiscard]] kit::Rect
presence_card_place(kit::Rect badge, kit::Point size, kit::Rect safe_area, int32_t arrow_depth);

/// Points the card's arrow at its badge: from the card's top edge when the
/// card lies below the badge, from its bottom edge when above, its point at
/// the badge's centre.
///
/// @param[in,out] look the card; its arrow and arrow_at are set
/// @param card the card's rectangle (presence_card_place)
/// @param badge the badge
void presence_card_point(kit::HoverCardLook& look, kit::Rect card, kit::Rect badge) noexcept;

/// Names the open tip's lines for automation, in the order the hover card
/// draws them: each row, then each block line.
///
/// A line is a control of kind area that takes no press and no focus, so
/// the endpoint lists it as a label. Its name is battleroom.tip.line.<n>,
/// from 0; the endpoint adds oa. Its text is the words drawn on that line,
/// the row's value, or "<label>: <value>" when the row has a label, already
/// cut as the card cuts them. The header is not a line. The rectangles are
/// the rows the card draws, in the same points as `card`.
///
/// @param[in,out] list the display list the card is drawn into
/// @param card the card's rectangle, in the list's points
/// @param look the hover card drawn there
void presence_card_controls(kit::DisplayList& list, kit::Rect card, const kit::HoverCardLook& look);

/// The slot that names no badge.
inline constexpr int32_t presence_card_no_slot = -1;

/// When the card opens, for which badge, and when it closes.
///
/// The pointer resting on a badge opens its card once kit::hover_card_due
/// says so, the badge's slot as the control (kit::hover_card_delay_ms). The
/// card stays open while the pointer is over the badge or the card, and
/// closes once it has been over them and is over neither; moving onto
/// another badge closes it, and that badge's card opens after its own wait.
/// The information key toggles the card of the focused row and moves it
/// with the focus; Escape closes it; a finger's tap on a badge toggles that
/// badge's card, and one elsewhere closes it. A card closed by a key or a
/// tap stays closed while the pointer rests on the same badge.
class PresenceCardTrigger {
  public:

    /// Follows the pointer: the badge it is over now, once a frame and on
    /// every move.
    ///
    /// @param slot the slot of the badge under the pointer, or presence_card_no_slot
    /// @param now_ms the time now, in milliseconds
    /// @return true when the open card changed
    bool pointer(int32_t slot, uint64_t now_ms) noexcept;

    /// Takes the information key (F1, a controller's View).
    ///
    /// With no card open it opens the focused row's card; with one open it
    /// moves it to the focused row when that is another badge's, and
    /// otherwise closes it.
    ///
    /// @param focused_slot the focused row's slot when that row has the
    ///     badge, else presence_card_no_slot
    /// @return true when the key is taken; false with no card open and no
    ///     badge focused, when the key goes on
    bool key_info(int32_t focused_slot) noexcept;

    /// Takes Escape (a controller's B).
    ///
    /// @return true when it closed a card, and the key is taken; false
    ///     with none open, when the key goes on
    bool key_close() noexcept;

    /// Takes a finger's tap.
    ///
    /// @param slot the slot of the badge tapped, or presence_card_no_slot
    /// @return true for a tap on a badge, which opens or closes its card and
    ///     is taken; false for a tap elsewhere, which closes the card and
    ///     goes on
    bool tap(int32_t slot) noexcept;

    /// Tells whether the pointer is over the open card, its arrow included;
    /// pointer then decides whether the card stays.
    ///
    /// @param over the pointer is over the card
    void over_card(bool over) noexcept;

    /// Follows the focus: a card the information key opened moves to the
    /// focused row when that is another badge's, and closes when the focus
    /// leaves the badges' rows.
    ///
    /// @param focused_slot the focused row's slot when that row has the
    ///     badge, else presence_card_no_slot
    /// @return true when the open card changed
    bool focus(int32_t focused_slot) noexcept;

    /// Closes the card, as when an OA screen covers it or its player leaves.
    void close() noexcept;

    /// Returns the slot whose card is open.
    ///
    /// @return the slot, or presence_card_no_slot while no card is open
    [[nodiscard]] int32_t open_slot() const noexcept { return open_; }

  private:

    /// What opened the card.
    enum class Opener : uint8_t {
        hover, ///< the pointer resting on the badge
        key,   ///< the information key
        tap,   ///< a finger's tap
    };

    /// Opens a badge's card.
    ///
    /// @param slot the badge's slot
    /// @param by what opened it
    void open(int32_t slot, Opener by) noexcept;

    /// Closes the card as the pointer leaving it does.
    void drop() noexcept;

    /// Closes the card as a key or a tap does: the badge the pointer rests
    /// on opens nothing until the pointer leaves it.
    void shut() noexcept;

    oa::ui::kit::HoverTimer timer_{};     ///< the badge the pointer rests on, and since when
    int32_t open_{presence_card_no_slot}; ///< the slot whose card is open
    Opener opened_by_{Opener::hover};     ///< what opened it
    bool over_card_{};                    ///< the pointer is over the open card
    bool pointer_inside_{};               ///< the pointer has been over the open card or its badge
    int32_t resting_{presence_card_no_slot}; ///< the badge under the pointer, from its last report
    bool held_off_{};                        ///< hover opens nothing on held_slot_
    int32_t held_slot_{presence_card_no_slot}; ///< the badge a key or tap closed the card over
};

} // namespace oa::ui::frontend_multiplayer
