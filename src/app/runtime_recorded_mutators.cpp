// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutators a match is playing, written into a recording and read back
// before a replay's match is built. The player's own list is put back when
// that match ends.

#include "oa/app/runtime.hpp"

#include "oa/base/sha256.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/data/persist/save_mutators.hpp"
#include "oa/data/saves/content_record.hpp"
#include "oa/session/demo/recording.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace mutators = oa::data::mutators;
namespace saves = oa::data::saves;

static_assert(
    mutators::loadout_text_most_bytes == oa::session::demo::recording_loadout_most_bytes,
    "a loadout parse_loadout accepts fits in a recording"
);
static_assert(mutators::loadout_text_most_bytes <= oa::data::persist::save_mutators_most_bytes);

namespace {

constexpr std::string_view kUnreadable = "The mutators this was made with cannot be read.";
constexpr std::string_view kMissing = "This needs a mutator you do not have: {detail}";
constexpr std::string_view kPlayedNone = "This was made with mutators, and this run plays none.";

/// The sentence for a mutator this machine does not have, with `detail` in place.
///
/// @param detail what was missing, as the loadout's refusal names it
/// @return the sentence
std::string missing_mutator(std::string_view detail) {
    std::string sentence{oa::data::languages::interface_text(kMissing)};
    const std::size_t at = sentence.find("{detail}");
    if (at != std::string::npos)
        sentence.replace(at, std::string_view("{detail}").size(), detail);
    return sentence;
}

} // namespace

struct Runtime::LoadoutOnceHold {
    /// True while a replay or a saved game stands in for the player's list.
    bool pending = false;
    /// True once the match that plays the held list has started.
    bool started = false;
    /// The list the player had chosen.
    std::vector<mutators::ResolvedMutator> kept{};
};

void Runtime::destroy_loadout_once_hold(LoadoutOnceHold* hold) noexcept {
    delete hold;
}

std::string Runtime::played_loadout_text() const {
    const mutators::Loadout* loadout = match_loadout();
    if (loadout == nullptr)
        return {};
    return mutators::canonical_loadout(*loadout);
}

std::vector<saves::ContentMutator> Runtime::played_mutators() const {
    const mutators::Loadout* loadout = match_loadout();
    if (loadout == nullptr)
        return {};
    const std::span<const mutators::ResolvedMutator> chosen = chosen_match_mutators();
    std::vector<saves::ContentMutator> listed;
    listed.reserve(loadout->entries.size());
    for (const mutators::LoadoutEntry& entry : loadout->entries) {
        saves::ContentMutator item;
        const std::array<char, oa::base::sha256::hex_size> hex =
            oa::base::sha256::to_hex(entry.package);
        item.sha256.assign(hex.begin(), hex.end());
        item.id = entry.id;
        item.version = entry.version;
        item.name = entry.id;
        for (const mutators::ResolvedMutator& one : chosen) {
            if (one.manifest.id != entry.id)
                continue;
            if (!one.manifest.name.empty())
                item.name = one.manifest.name;
            break;
        }
        listed.push_back(std::move(item));
    }
    return listed;
}

std::string Runtime::recorded_mutators_refusal(
    std::string_view loadout_text, std::vector<mutators::ResolvedMutator>* resolved
) const {
    std::string parse_error;
    const std::optional<mutators::Loadout> parsed =
        mutators::parse_loadout(loadout_text, &parse_error);
    if (!parsed) {
        std::cerr << "open-annihilation: mutators: the recorded loadout cannot be read: "
                  << parse_error << '\n';
        return std::string(oa::data::languages::interface_text(kUnreadable));
    }
    // A run that plays none refuses a recording that names mutators, before
    // those packages are looked up. N05's saves ask the same question here.
    if (options_.no_mutators && !parsed->entries.empty()) {
        std::cerr << "open-annihilation: mutators: --no-mutators refused the recorded "
                  << mutator_names(*parsed) << '\n';
        return std::string(oa::data::languages::interface_text(kPlayedNone));
    }
    const fs::path folder =
        user_folder() / std::string(oa::data::mutator_library::mutators_folder_name);
    std::string resolve_error;
    std::optional<std::vector<mutators::ResolvedMutator>> found =
        mutators::resolve_loadout(*parsed, folder, &resolve_error);
    if (!found)
        return missing_mutator(resolve_error);
    if (resolved != nullptr)
        *resolved = std::move(*found);
    return {};
}

void Runtime::play_loadout_once(std::vector<mutators::ResolvedMutator> playback) {
    if (loadout_once_hold_ == nullptr) {
        std::unique_ptr<LoadoutOnceHold, void (*)(LoadoutOnceHold*) noexcept> made{
            new LoadoutOnceHold, destroy_loadout_once_hold
        };
        loadout_once_hold_ = std::move(made);
    }
    LoadoutOnceHold& hold = *loadout_once_hold_;
    if (!hold.pending) {
        const std::span<const mutators::ResolvedMutator> chosen = chosen_match_mutators();
        hold.kept.assign(chosen.begin(), chosen.end());
    }
    hold.pending = true;
    hold.started = false;
    choose_match_mutators(std::move(playback));
}

void Runtime::cancel_loadout_once() noexcept {
    if (loadout_once_hold_ == nullptr || !loadout_once_hold_->pending)
        return;
    LoadoutOnceHold& hold = *loadout_once_hold_;
    choose_match_mutators(std::exchange(hold.kept, {}));
    hold.started = false;
    hold.pending = false;
}

void Runtime::start_loadout_once() noexcept {
    if (loadout_once_hold_ == nullptr || !loadout_once_hold_->pending)
        return;
    loadout_once_hold_->started = true;
}

bool Runtime::mutators_held_for_next_match() const noexcept {
    return loadout_once_hold_ != nullptr && loadout_once_hold_->pending &&
           !loadout_once_hold_->started;
}

void Runtime::finish_loadout_once() {
    if (loadout_once_hold_ == nullptr || !loadout_once_hold_->pending)
        return;
    LoadoutOnceHold& hold = *loadout_once_hold_;
    if (!hold.started)
        return;
    choose_match_mutators(std::exchange(hold.kept, {}));
    hold.started = false;
    hold.pending = false;
}

} // namespace oa::app
