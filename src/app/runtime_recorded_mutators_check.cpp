// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-replay-loadout: a recording's loadout is refused, played for one
// match, and the player's own list put back. The command line names
// ridge-tough and ridge-art-a, both installed. ridge-commander-boom is the
// package folder beside the player's folder.

#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"

#include "oa/base/sha256.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/data/saves/content_record.hpp"
#include "oa/ui/frontend_dialogs.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace mutators = oa::data::mutators;

/// The package file named in the installed ridge-art-a origin record.
constexpr std::string_view kArtPackage =
    "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90";

constexpr std::string_view kUnreadable = "The mutators this was made with cannot be read.";
constexpr std::string_view kNeeds = "This needs a mutator you do not have: ";

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("replay loadout check: " + what);
}

/// The ids of the chosen list, in list order.
///
/// @param runtime the runtime
/// @return the ids
std::vector<std::string> chosen_ids(const Runtime& runtime) {
    const auto chosen = runtime.chosen_match_mutators();
    std::vector<std::string> ids;
    ids.reserve(chosen.size());
    for (const mutators::ResolvedMutator& one : chosen)
        ids.push_back(one.manifest.id);
    return ids;
}

/// The ids of the running match's loadout, in list order.
///
/// @param runtime the runtime
/// @return the ids, or an empty list when this match applied none
std::vector<std::string> loadout_ids(const Runtime& runtime) {
    const mutators::Loadout* loadout = runtime.match_loadout();
    if (loadout == nullptr)
        return {};
    std::vector<std::string> ids;
    ids.reserve(loadout->entries.size());
    for (const mutators::LoadoutEntry& entry : loadout->entries)
        ids.push_back(entry.id);
    return ids;
}

/// Throws when the ids differ.
///
/// @param got the ids read
/// @param want the ids expected
/// @param what names the list
void expect_ids(
    const std::vector<std::string>& got, const std::vector<std::string>& want, const char* what
) {
    if (got == want)
        return;
    std::string text = what;
    text += " held";
    for (const std::string& id : got) {
        text += ' ';
        text += id;
    }
    fail(text);
}

/// The refusal sentence, with V06's message after it.
///
/// @param detail the message
/// @return the sentence
std::string needs(std::string_view detail) {
    std::string sentence{kNeeds};
    sentence += detail;
    return sentence;
}

} // namespace

void Runtime::check_replay_loadout() {
    const auto chosen_now = chosen_match_mutators();
    const std::vector<mutators::ResolvedMutator> supplied(chosen_now.begin(), chosen_now.end());
    if (supplied.size() != 2 || supplied[0].manifest.id != "ridge-tough" ||
        supplied[1].manifest.id != "ridge-art-a")
        fail("the command line must name ridge-tough and then ridge-art-a");
    const std::vector<std::string> player{"ridge-tough", "ridge-art-a"};
    const mutators::Loadout art_loadout =
        mutators::loadout_of(std::span<const mutators::ResolvedMutator>(&supplied[1], 1));
    const std::string art_text = mutators::canonical_loadout(art_loadout);
    if (art_loadout.entries.size() != 1)
        fail("ridge-art-a wrote no loadout");
    const std::array<char, oa::base::sha256::hex_size> art_hex =
        oa::base::sha256::to_hex(art_loadout.entries[0].package);
    if (std::string_view(art_hex.data(), art_hex.size()) != kArtPackage)
        fail("ridge-art-a's package is not the origin record");

    const auto refused = [this](std::string_view text, std::string_view sentence) {
        std::vector<mutators::ResolvedMutator> resolved;
        const std::string got = recorded_mutators_refusal(text, &resolved);
        if (got != sentence)
            fail(got.empty() ? "a loadout was accepted" : got);
    };
    refused("x", kUnreadable);
    mutators::Loadout missing = art_loadout;
    missing.entries[0].id = "ridge-missing";
    refused(
        mutators::canonical_loadout(missing), needs("\"ridge-missing\" is not an installed mutator")
    );
    mutators::Loadout other_package = art_loadout;
    const std::optional<oa::base::sha256::Digest> other_sha =
        oa::base::sha256::parse_hex(std::string(oa::base::sha256::hex_size, '1'));
    if (!other_sha)
        fail("the other package sha was not hex");
    other_package.entries[0].package = *other_sha;
    refused(
        mutators::canonical_loadout(other_package), needs("\"ridge-art-a\" is a different package")
    );
    mutators::Loadout other_version = art_loadout;
    other_version.entries[0].version = "9.9";
    refused(
        mutators::canonical_loadout(other_version), needs("\"ridge-art-a\" is version 1.0, not 9.9")
    );
    mutators::Loadout as_folder = art_loadout;
    as_folder.entries[0].folder = true;
    refused(mutators::canonical_loadout(as_folder), needs("\"ridge-art-a\" is a package folder"));

    std::vector<mutators::ResolvedMutator> resolved;
    if (!recorded_mutators_refusal(art_text, &resolved).empty() || resolved.size() != 1 ||
        resolved[0].manifest.id != "ridge-art-a")
        fail("ridge-art-a's own loadout was refused");
    if (!recorded_mutators_refusal(art_text, nullptr).empty())
        fail("a null resolved was refused");

    if (!played_loadout_text().empty() || !played_mutators().empty())
        fail("the menu reported mutators");

    const auto back_to_menu = [this] {
        if (match_)
            leave_match();
        load(Screen::main_menu);
    };
    const auto restore_player = [this, &supplied] { choose_match_mutators(supplied); };

    choose_match_mutators({});
    start_benchmark_skirmish_as_chosen();
    if (!played_loadout_text().empty() || !played_mutators().empty() || match_loadout() != nullptr)
        fail("a skirmish with no mutators reported one");
    back_to_menu();
    restore_player();

    choose_match_mutators({supplied[1]});
    start_benchmark_skirmish_as_chosen();
    const std::string played = played_loadout_text();
    std::string read_error;
    const std::optional<mutators::Loadout> read_back = mutators::parse_loadout(played, &read_error);
    const mutators::Loadout* running = match_loadout();
    if (running == nullptr || played != mutators::canonical_loadout(*running) || !read_back ||
        !(*read_back == *running))
        fail("the played loadout did not read back");
    const std::vector<oa::data::saves::ContentMutator> noted = played_mutators();
    if (noted.size() != 1 || noted[0].sha256 != kArtPackage || noted[0].id != "ridge-art-a" ||
        noted[0].version != "1.0" || noted[0].name != "Ridge Art A")
        fail("the note's mutator was not ridge-art-a");
    back_to_menu();
    restore_player();

    play_loadout_once({supplied[1]});
    start_benchmark_skirmish_as_chosen();
    expect_ids(loadout_ids(*this), {"ridge-art-a"}, "the replay");
    expect_ids(chosen_ids(*this), {"ridge-art-a"}, "the replay's choice");
    back_to_menu();
    expect_ids(chosen_ids(*this), player, "the list after the replay");

    play_loadout_once({supplied[1]});
    start_benchmark_skirmish();
    expect_ids(loadout_ids(*this), player, "a setup Start over a one-match list");
    back_to_menu();
    expect_ids(chosen_ids(*this), player, "the list after a setup Start");

    play_loadout_once({});
    start_benchmark_skirmish_as_chosen();
    if (assets_.mutator_layer_mounted() || match_loadout() != nullptr)
        fail("an empty recording mounted mutators");
    back_to_menu();
    expect_ids(chosen_ids(*this), player, "the list after an empty recording");

    restore_player();
    start_benchmark_skirmish_as_chosen();
    play_loadout_once({supplied[1]});
    back_to_menu();
    start_benchmark_skirmish_as_chosen();
    expect_ids(loadout_ids(*this), {"ridge-art-a"}, "the match after leaving");
    back_to_menu();
    expect_ids(chosen_ids(*this), player, "the list after that match");

    const fs::path boom_folder = user_folder().parent_path() / "ridge-commander-boom";
    std::string boom_error;
    const std::optional<mutators::MutatorChoice> boom_choice =
        mutators::parse_choice(path_to_utf8(boom_folder), &boom_error);
    if (!boom_choice)
        fail(boom_error.empty() ? "ridge-commander-boom was not a choice" : boom_error);
    const fs::path mutators_folder =
        user_folder() / std::string(oa::data::mutator_library::mutators_folder_name);
    const std::optional<mutators::ResolvedMutator> boom =
        mutators::resolve_choice(*boom_choice, mutators_folder, &boom_error);
    if (!boom || boom->manifest.id != "ridge-commander-boom")
        fail(boom_error.empty() ? "ridge-commander-boom did not resolve" : boom_error);
    play_loadout_once({*boom});
    bool entered = false;
    try {
        start_benchmark_skirmish_as_chosen();
        entered = true;
    } catch (const std::exception&) {
        entered = false;
    }
    if (entered)
        fail("ridge-commander-boom started a match");
    cancel_loadout_once();
    expect_ids(chosen_ids(*this), player, "the choice after a cancelled start");
    while (oa::ui::frontend_dialogs::dialog_count() != 0)
        oa::ui::frontend_dialogs::close_dialog();
    back_to_menu();
    start_benchmark_skirmish_as_chosen();
    expect_ids(loadout_ids(*this), player, "the match after the cancelled start");
    expect_ids(chosen_ids(*this), player, "the choice after the cancelled start");
    back_to_menu();

    play_loadout_once({supplied[1]});
    cancel_loadout_once();
    expect_ids(chosen_ids(*this), player, "the choice after a cancel before the start");
    start_benchmark_skirmish_as_chosen();
    expect_ids(loadout_ids(*this), player, "the match after a cancel before the start");
    back_to_menu();
    std::cout << "replay loadout check: passed\n";
}

} // namespace oa::app
