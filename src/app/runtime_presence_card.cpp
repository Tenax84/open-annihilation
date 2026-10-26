// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The presence card on the OA layer: one screen, presence-card, that is not
// modal, has no backdrop and is not opaque, kept on the layer while the
// multiplayer screens show (Runtime::keep_presence_card_screen, once a
// frame from OaLayer::tick). Over the battle room it follows the pointer,
// F1 and Escape and a finger's taps on the OA badges
// (multiplayer_presence_badges), and while a card is open draws the kit's
// hover card by its badge at the layer's scale and size class. It takes F1,
// Escape and a tap only as the card's trigger says (PresenceCardTrigger),
// and lets every other input go on to the battle room. Return and the pad's
// A on a focused row go on too: the battle room opens this screen's trigger
// when the row has an OA badge, and presses nothing.

#include "oa_layer.hpp"

#include "oa/app/runtime.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/ui/frontend_multiplayer/get_map.hpp"
#include "oa/ui/frontend_multiplayer/presence_card.hpp"
#include "oa/ui/frontend_multiplayer/room_mutators.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace mp = oa::ui::frontend_multiplayer;
namespace kit = oa::ui::kit;

/// The card's screen's name on the OA layer.
constexpr std::string_view presence_card_screen_name = "presence-card";

/// The most badges the battle room shows: one a slot.
constexpr std::size_t most_badges = static_cast<std::size_t>(mp::kSlotCount);

/// One OA badge, in the layer's points.
struct BadgePlace {
    int32_t slot{};   ///< the row's player slot
    kit::Rect rect{}; ///< the badge
};

/// The card as it shows: its look, its place and the place of the screen
/// that holds it.
struct ShownCard {
    kit::HoverCardLook look{}; ///< the kit's hover card, its arrow pointed
    kit::Rect card{};          ///< the card, in the layer's points
    kit::Rect box{};           ///< the card and its arrow, in the layer's points
};

/// Tells whether two hover cards look the same.
///
/// @param first a card
/// @param second another
/// @return true when every part is the same
bool same_look(const kit::HoverCardLook& first, const kit::HoverCardLook& second) {
    if (first.width != second.width || first.mark != second.mark || first.title != second.title ||
        first.aside != second.aside || first.block != second.block || first.arrow != second.arrow ||
        first.arrow_at != second.arrow_at || first.rows.size() != second.rows.size())
        return false;
    for (std::size_t index = 0; index < first.rows.size(); ++index) {
        const auto& one = first.rows[index];
        const auto& other = second.rows[index];
        if (one.label != other.label || one.value != other.value ||
            one.value_colour != other.value_colour)
            return false;
    }
    return true;
}

/// Tells whether two rectangles are the same.
///
/// @param first a rectangle
/// @param second another
/// @return true when they are
bool same_rect(const kit::Rect& first, const kit::Rect& second) noexcept {
    return first.x == second.x && first.y == second.y && first.width == second.width &&
           first.height == second.height;
}

/// Returns the smallest rectangle holding two others.
///
/// @param first a rectangle
/// @param second another
/// @return the rectangle holding both
kit::Rect holding(const kit::Rect& first, const kit::Rect& second) noexcept {
    const int32_t left = std::min(first.x, second.x);
    const int32_t top = std::min(first.y, second.y);
    const int32_t right = std::max(first.x + first.width, second.x + second.width);
    const int32_t bottom = std::max(first.y + first.height, second.y + second.height);
    return {left, top, right - left, bottom - top};
}

/// Returns where a hover card's arrow is drawn: a triangle arrow_depth
/// deep out of its edge, 2 × arrow_depth − 1 across at its base.
///
/// @param placed the card
/// @param look the card's look, its arrow pointed
/// @param arrow_depth how far the arrow reaches out of the card
/// @return the arrow's rectangle; empty without an arrow
kit::Rect arrow_rect(const kit::Rect& placed, const kit::HoverCardLook& look, int32_t arrow_depth) {
    const int32_t across = 2 * arrow_depth - 1;
    const int32_t left = placed.x + look.arrow_at - (arrow_depth - 1);
    switch (look.arrow) {
    case kit::Side::top:
        return {left, placed.y - arrow_depth, across, arrow_depth};
    case kit::Side::bottom:
        return {left, placed.y + placed.height, across, arrow_depth};
    case kit::Side::none:
    case kit::Side::left:
    case kit::Side::right:
        break;
    }
    return {};
}

/// The presence card as a screen of the OA layer.
///
/// Its points are the layer's: one point is the view's scale in window
/// pixels from the window's top left corner, and its place's corner is that
/// many points along. While a card is open its place holds the card and its
/// arrow; while none is, over a battle room that shows badges, it is one
/// point it leaves clear, so that it still sees the pointer and the keys;
/// elsewhere it does not show.
class PresenceCardScreen final : public LayerScreen {
  public:

    /// Makes the screen, and binds its trigger so Return on a focused row
    /// opens the same card as F1.
    ///
    /// @param layer the layer it shows on, whose fonts it is drawn in and
    ///     which tells it when a modal screen shows
    /// @param pack_state what this machine has of the host's map pack
    /// @param mutators the host's set and what one slot has of each mutator
    PresenceCardScreen(
        OaLayer& layer,
        std::function<mp::PackState(const oa::netgame::PresenceMapPack&)> pack_state,
        std::function<std::vector<mp::CardMutator>(int32_t)> mutators
    )
        : layer_(layer), pack_state_(std::move(pack_state)), mutators_(std::move(mutators)) {
        mp::multiplayer_bind_row_key(&trigger_);
    }

    /// Clears the Return binding this screen set.
    ~PresenceCardScreen() override { mp::multiplayer_bind_row_key(nullptr); }

    /// Returns "presence-card".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return presence_card_screen_name; }

    /// Returns where the screen shows: the card and its arrow while a card
    /// is open; one point at the window's corner while badges show; else
    /// nowhere. Never in a match.
    ///
    /// @param placed_on what the screen is placed on
    /// @return its place, at the view's scale
    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        if (placed_on.match || (badges_.empty() && !shown_))
            return {};
        // At 150% too: each edge the point at the scale, rounded as the
        // kit's smooth canvas rounds it, and input mapped back by the scale.
        const int32_t percent = std::max(placed_on.scale_percent, int32_t{100});
        const kit::Rect held = box();
        const kit::Rect at = kit::to_canvas(held, percent);
        return {{at.x, at.y, at.width, at.height}, held.width, held.height, percent};
    }

    /// Tells that the screen lets what it does not take go on.
    ///
    /// @return false
    [[nodiscard]] bool modal() const override { return false; }

    /// Tells that the screen darkens nothing.
    ///
    /// @return false
    [[nodiscard]] bool backdrop() const override { return false; }

    /// Tells that the screen covers only the card and its arrow.
    ///
    /// @return false
    [[nodiscard]] bool opaque() const override { return false; }

    /// Draws the open card with the kit, from the place's corner; nothing
    /// while no card is open.
    ///
    /// @param canvas where it draws, at the layer's scale
    void draw(const kit::Canvas& canvas) const override { kit::paint(canvas, display_list()); }

    /// Follows the view: the layer's scale, size class, safe area and the
    /// picture's place, by which the badges are mapped to the layer's
    /// points.
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override {
        if (placed_on.match)
            return;
        view_ = placed_on;
        have_view_ = true;
        read_badges();
        show();
    }

    /// Follows the pointer over the badges and the card, and takes a finger's
    /// tap on a badge, and its lifting, as the card's trigger says.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button, 1 for the left
    /// @param at the pointer, in the screen's points
    /// @param finger_reach a finger's reach; 0 for a mouse, whose presses go on
    /// @return none or redraw for a tap the card takes; pass for everything else
    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        const kit::Rect held = box();
        pointer_at_ = kit::Point{at.x + held.x, at.y + held.y};
        have_pointer_ = true;
        const uint64_t before = revision_;
        const int32_t slot = badge_at(pointer_at_);
        follow_pointer();
        LayerAnswer answer = LayerAnswer::pass;
        if (kind == ScreenInputKind::pointer_down && button == SDL_BUTTON_LEFT &&
            finger_reach > 0) {
            if (trigger_.tap(slot)) {
                lift_taken_ = true;
                answer = LayerAnswer::none;
            }
        } else if (
            kind == ScreenInputKind::pointer_up && button == SDL_BUTTON_LEFT && lift_taken_
        ) {
            lift_taken_ = false;
            answer = LayerAnswer::none;
        }
        show();
        if (answer != LayerAnswer::pass && revision_ != before)
            answer = LayerAnswer::redraw;
        return answer;
    }

    /// Takes F1 and Escape as the card's trigger says.
    ///
    /// @param pressed the key
    /// @return none or redraw when the card took it; pass otherwise
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        bool taken = false;
        if (pressed == kit::Key::info)
            taken = trigger_.key_info(focused_badge());
        else if (pressed == kit::Key::escape)
            taken = trigger_.key_close();
        if (!taken)
            return LayerAnswer::pass;
        const uint64_t before = revision_;
        show();
        return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
    }

    /// Lets the wheel go on.
    ///
    /// @return pass
    LayerAnswer wheel(kit::Point /*at*/, float /*notches*/) override { return LayerAnswer::pass; }

    /// Returns the open card, from the place's corner: the hover card the
    /// player sees, and one named control for each of its lines.
    ///
    /// @return the list; empty while no card is open
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList list;
        if (!shown_)
            return list;
        const kit::Rect held = shown_->box;
        kit::Rect placed = shown_->card;
        placed.x -= held.x;
        placed.y -= held.y;
        kit::add_hover_card(list, placed, shown_->look);
        mp::presence_card_controls(list, placed, shown_->look);
        return list;
    }

    /// Returns no hover, press or focus: the card has no control.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override { return {}; }

    /// Brings the card up to date once a frame: closes the screen when the
    /// multiplayer screens go, closes the card while a modal screen shows,
    /// follows the focus and the resting pointer, and builds the card again
    /// from the lobby, so that a record that comes shows at once and a
    /// player who leaves takes the card with them.
    ///
    /// @return close when the multiplayer screens no longer show; redraw
    ///     when the card changed; otherwise none
    LayerAnswer tick() override {
        if (!mp::multiplayer_showing())
            return LayerAnswer::close;
        const uint64_t before = revision_;
        read_badges();
        if (layer_.modal_shown(false)) {
            trigger_.close();
        } else {
            (void)trigger_.focus(focused_badge());
            if (have_pointer_)
                follow_pointer();
        }
        show();
        return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
    }

    /// Returns a number that changes with the card.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Has nothing of its own to close.
    void close(bool /*by_key*/) override {}

  private:

    /// Returns the place of the screen, in the layer's points: the card and
    /// its arrow, or the one clear point at the corner.
    ///
    /// @return the place
    [[nodiscard]] kit::Rect box() const noexcept {
        return shown_ ? shown_->box : kit::Rect{0, 0, 1, 1};
    }

    /// Reads where the battle room draws its OA badges, mapped from the
    /// 640 by 480 frame through the picture's place in the window to the
    /// layer's points, the edges outward.
    void read_badges() {
        badges_.clear();
        if (!have_view_ || view_.picture_size.x <= 0 || view_.picture_size.y <= 0)
            return;
        std::array<mp::PresenceBadgeSpot, most_badges> spots{};
        const int32_t count =
            mp::multiplayer_presence_badges(spots.data(), static_cast<int32_t>(spots.size()));
        // Window pixels to points at the view's scale, 150% among them.
        const int64_t percent = std::max(view_.scale_percent, int32_t{100});
        const auto& picture = view_.picture;
        const auto across = [&](int32_t column) {
            return picture.x + static_cast<int64_t>(column) * picture.width / view_.picture_size.x;
        };
        const auto down = [&](int32_t row) {
            return picture.y + static_cast<int64_t>(row) * picture.height / view_.picture_size.y;
        };
        const auto floor_points = [percent](int64_t pixels) {
            const int64_t scaled = pixels * 100;
            return static_cast<int32_t>(
                scaled >= 0 ? scaled / percent : -((-scaled + percent - 1) / percent)
            );
        };
        const auto ceil_points = [percent](int64_t pixels) {
            const int64_t scaled = pixels * 100;
            return static_cast<int32_t>(
                scaled >= 0 ? (scaled + percent - 1) / percent : -(-scaled / percent)
            );
        };
        for (int32_t index = 0; index < count && index < static_cast<int32_t>(spots.size());
             ++index) {
            const auto& spot = spots[static_cast<std::size_t>(index)];
            const int32_t left = floor_points(across(spot.x));
            const int32_t top = floor_points(down(spot.y));
            const int32_t right = ceil_points(across(spot.x + spot.width));
            const int32_t bottom = ceil_points(down(spot.y + spot.height));
            badges_.push_back({spot.slot, {left, top, right - left, bottom - top}});
        }
    }

    /// Returns the badge under a point.
    ///
    /// @param point the point, in the layer's points
    /// @return its slot, or presence_card_no_slot
    [[nodiscard]] int32_t badge_at(kit::Point point) const noexcept {
        for (const BadgePlace& badge : badges_)
            if (kit::contains(badge.rect, point))
                return badge.slot;
        return mp::presence_card_no_slot;
    }

    /// Returns the focused row's slot when that row shows a badge.
    ///
    /// @return the slot, or presence_card_no_slot
    [[nodiscard]] int32_t focused_badge() const {
        const int32_t slot = mp::multiplayer_focused_player_slot();
        for (const BadgePlace& badge : badges_)
            if (badge.slot == slot)
                return slot;
        return mp::presence_card_no_slot;
    }

    /// Hands the trigger where the pointer is now: over which badge, and
    /// whether over the card.
    void follow_pointer() {
        trigger_.over_card(shown_ && kit::contains(shown_->box, pointer_at_));
        (void)trigger_.pointer(badge_at(pointer_at_), SDL_GetTicks());
    }

    /// Builds the open card again from the lobby, closing it when its badge
    /// or its player is gone, and counts a change.
    void show() {
        std::optional<ShownCard> next;
        const int32_t slot = trigger_.open_slot();
        const kit::Fonts* fonts = layer_.screen_fonts();
        if (slot != mp::presence_card_no_slot && fonts != nullptr && have_view_) {
            const auto badge =
                std::find_if(badges_.begin(), badges_.end(), [slot](const BadgePlace& place) {
                    return place.slot == slot;
                });
            std::optional<mp::PresenceCard> model;
            if (badge != badges_.end()) {
                const std::vector<mp::CardMutator> mutators = mutators_(slot);
                model = mp::presence_card(mp::multiplayer_lobby(), slot, pack_state_, mutators);
            }
            if (model) {
                const kit::SizeClass size_class = view_.frame.size_class;
                const int32_t arrow_depth = kit::metrics_of(size_class).hover_card_arrow;
                ShownCard shown;
                shown.look =
                    mp::presence_card_look(*model, size_class, view_.frame.area.width, *fonts);
                const kit::Point size = kit::hover_card_size(*fonts, shown.look);
                shown.card =
                    mp::presence_card_place(badge->rect, size, view_.frame.area, arrow_depth);
                mp::presence_card_point(shown.look, shown.card, badge->rect);
                shown.box = holding(shown.card, arrow_rect(shown.card, shown.look, arrow_depth));
                next = std::move(shown);
            } else {
                trigger_.close();
            }
        }
        const bool same =
            next.has_value() == shown_.has_value() &&
            (!next || (same_rect(next->card, shown_->card) && same_rect(next->box, shown_->box) &&
                       same_look(next->look, shown_->look)));
        if (same)
            return;
        shown_ = std::move(next);
        ++revision_;
    }

    OaLayer& layer_;                    ///< the layer it shows on
    mp::PresenceCardTrigger trigger_{}; ///< when the card opens and closes
    /// What this machine has of the host's map pack (Runtime::local_map_pack).
    std::function<mp::PackState(const oa::netgame::PresenceMapPack&)> pack_state_{};
    /// The host's set and what one slot has of each mutator.
    std::function<std::vector<mp::CardMutator>(int32_t)> mutators_{};
    LayerView view_{};                 ///< the view it was last laid out for
    bool have_view_{};                 ///< view_ holds a view of the front end
    std::vector<BadgePlace> badges_{}; ///< the badges the battle room shows now
    kit::Point pointer_at_{};          ///< the pointer, in the layer's points, as last seen
    bool have_pointer_{};              ///< the pointer has been seen
    bool lift_taken_{};                ///< a tap was taken; its lifting is taken too
    std::optional<ShownCard> shown_{}; ///< the open card; none while no card is open
    uint64_t revision_{};              ///< counts the changes to the card
};

} // namespace

void Runtime::keep_presence_card_screen() {
    if (screen_ == Screen::match || !mp::multiplayer_showing())
        return;
    OaLayer& layer = oa_layer();
    if (layer.find(presence_card_screen_name) != nullptr)
        return;
    // The host's card says whether this machine has the host's map pack: its
    // release, by the record's SHA-256, another release, or none of it.
    layer.push(
        std::make_unique<PresenceCardScreen>(
            layer,
            [this](const oa::netgame::PresenceMapPack& pack) {
                const mp::HostPack host = mp::host_pack_of(pack);
                const mp::LocalPack local = local_map_pack(host.id, host.sha256);
                if (!local.installed)
                    return mp::PackState::missing;
                return local.sha256 == host.sha256 ? mp::PackState::installed
                                                   : mp::PackState::other_release;
            },
            [this](int32_t slot) {
                std::vector<mp::CardMutator> rows;
                mp::Lobby& lobby = mp::multiplayer_lobby();
                // The room's list as this machine holds it: the host its own
                // chosen list, a guest the host's list as its records carry it.
                std::vector<std::pair<std::string, std::string>> listed;
                if (mp::lobby_hosts(lobby)) {
                    for (const oa::data::mutators::ResolvedMutator& one : room_mutators()) {
                        listed.emplace_back(
                            one.manifest.id,
                            one.manifest.name.empty() ? one.manifest.id : one.manifest.name
                        );
                    }
                } else {
                    oa::netgame::PresenceLoadoutAssembly assembly;
                    if (!mp::multiplayer_room_mutators(&assembly) ||
                        !oa::netgame::presence_assembly_whole(assembly))
                        return rows;
                    const std::vector<HostMutatorFetch> fetched = host_mutators_fetch();
                    const oa::data::mutator_library::Listing& listing = installed_mutators();
                    for (std::size_t index = 0; index < assembly.mutators.size(); ++index) {
                        const oa::netgame::PresenceMutator& item = assembly.mutators[index];
                        const oa::data::mutator_library::InstalledMutator* found =
                            oa::data::mutator_library::find(listing, item.id);
                        const std::string_view installed =
                            found != nullptr ? std::string_view(found->manifest.name)
                                             : std::string_view();
                        const std::string_view catalogue =
                            index < fetched.size() ? std::string_view(fetched[index].offer.name)
                                                   : std::string_view();
                        listed.emplace_back(
                            item.id, mp::mutator_shown_name(installed, catalogue, item.id)
                        );
                    }
                }
                const bool hosts_slot = mp::lobby_host_slot(lobby) == static_cast<uint8_t>(slot);
                rows.reserve(listed.size());
                for (std::size_t position = 0; position < listed.size(); ++position) {
                    mp::CardMutator row;
                    row.name = listed[position].second;
                    const mp::MutatorHave have =
                        hosts_slot
                            ? mp::MutatorHave::has
                            : mp::lobby_mutator_have(lobby, slot, static_cast<uint8_t>(position));
                    switch (have) {
                    case mp::MutatorHave::has:
                        row.have = mp::CardMutatorHave::has;
                        break;
                    case mp::MutatorHave::other_release:
                        row.have = mp::CardMutatorHave::other_release;
                        break;
                    case mp::MutatorHave::missing:
                        row.have = mp::CardMutatorHave::missing;
                        break;
                    case mp::MutatorHave::unknown:
                        row.have = mp::CardMutatorHave::unknown;
                        break;
                    }
                    rows.push_back(std::move(row));
                }
                return rows;
            }
        )
    );
}

} // namespace oa::app
