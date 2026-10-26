// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The simulation hash the run plays under, for an extension, and the check
// that the hash follows the profile and a Developer Mode override.
#include "oa/app/presence_facts.hpp"
#include "oa/app/runtime.hpp"

#include "engine_settings_state.hpp"

#include "oa/base/sha256.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/data/mod_profile/overrides.hpp"
#include "oa/data/mod_profile/value.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/loadout.hpp"

#include <algorithm>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

namespace {

namespace profiles = oa::data::mod_profile;

/// Resolves the plain baseline and returns its sim hash, as text.
///
/// @return 64 lower-case hexadecimal digits
/// @throws std::runtime_error when the baseline does not resolve
std::string resolve_baseline_hash() {
    const std::string text = profiles::base_game_profile_text();
    const auto resolved = profiles::resolve_profile(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()),
        profiles::base_game_id
    );
    if (!resolved.resolution)
        throw std::runtime_error("the plain baseline does not resolve");
    return profiles::digest_text(resolved.resolution->profile.sim_hash);
}

/// The plain baseline's sim hash, as text, resolved once for the process.
///
/// The baseline is made from the rule table built into the program alone.
/// The mod played, a game mode, Developer Mode and a soft restart for a mod
/// switch change which profile simulation_hash reads, never this. A resolve
/// that throws is tried again on the next call.
///
/// @return 64 lower-case hexadecimal digits
/// @throws std::runtime_error when the baseline does not resolve
const std::string& baseline_hash() {
    static const std::string hash = resolve_baseline_hash();
    return hash;
}

} // namespace

oa::base::sha256::Digest profile_sim_hash(const Runtime& runtime) {
    // The hash simulation_hash chose before mutators. This function is the
    // friend, so the profile fields stay here. played_sim_hash and
    // run_mutators both start from this digest.
    if (const auto* mod = runtime.mod_profile())
        return mod->sim_hash;
    if (runtime.engine_settings_ && runtime.engine_settings_->layered)
        return runtime.engine_settings_->base_sim_hash;
    if (runtime.options_.mod_profile)
        return runtime.options_.mod_profile->sim_hash;
    const std::optional<oa::base::sha256::Digest> parsed =
        oa::base::sha256::parse_hex(baseline_hash());
    if (!parsed)
        throw std::runtime_error("the plain baseline does not resolve");
    return *parsed;
}

oa::base::sha256::Digest played_sim_hash(const Runtime& runtime) {
    return oa::data::mutators::fold_sim_hash(
        profile_sim_hash(runtime), runtime.match_mutator_digest()
    );
}

std::string simulation_hash(const Runtime& runtime) {
    return profiles::digest_text(played_sim_hash(runtime));
}

RunMutators run_mutators(const Runtime& runtime) {
    // The same profile and the same digest played_sim_hash folds, read once
    // so the two calls cannot disagree.
    const oa::base::sha256::Digest profile = profile_sim_hash(runtime);
    const std::optional<oa::base::sha256::Digest> digest = runtime.match_mutator_digest();
    RunMutators told;
    told.simulation_hash_without = profiles::digest_text(profile);
    if (!digest)
        return told;
    const oa::data::mutators::Loadout* loadout = runtime.match_loadout();
    if (loadout == nullptr)
        return told;
    told.ids.reserve(loadout->entries.size());
    for (const oa::data::mutators::LoadoutEntry& entry : loadout->entries)
        told.ids.push_back(entry.id);
    told.count = static_cast<uint32_t>(told.ids.size());
    return told;
}

void Runtime::check_simulation_hash() {
    // The command line names the mutator. Keep it, then choose none, so the
    // hash below is the one a list that has not been built leaves unchanged.
    const auto chosen_now = chosen_match_mutators();
    const std::vector<oa::data::mutators::ResolvedMutator> kept(
        chosen_now.begin(), chosen_now.end()
    );
    if (kept.empty())
        throw std::runtime_error("the command line must name a mutator");
    choose_match_mutators({});

    const std::string played = simulation_hash(*this);
    if (played.size() != 64)
        throw std::runtime_error("the simulation hash is not 64 hexadecimal digits");
    const auto expect_no_mutators = [this](const std::string& hash) {
        const RunMutators told = run_mutators(*this);
        if (told.count != 0 || !told.ids.empty() || told.simulation_hash_without != hash ||
            simulation_hash(*this) != told.simulation_hash_without)
            throw std::runtime_error(
                "with no mutators run_mutators did not report the simulation hash alone"
            );
    };
    expect_no_mutators(played);
    const std::string baseline = baseline_hash();
    if (!options_.mod_profile) {
        if (played != baseline)
            throw std::runtime_error("with no mod the simulation hash is not the base game's");
    } else {
        if (played != profiles::digest_text(options_.mod_profile->sim_hash))
            throw std::runtime_error("under a mod the simulation hash is not that profile's");
        if (played == baseline)
            throw std::runtime_error("a mod that changes the simulation kept the base game's hash");
    }

    const auto lists = [](const std::vector<std::string>& names, std::string_view id) {
        return std::find(names.begin(), names.end(), id) != names.end();
    };
    const auto before = presence_facts(*this);
    if (before.developer_mode || !before.overrides.empty())
        throw std::runtime_error("presence facts started in Developer Mode");
    if (static_cast<bool>(options_.mod_profile) != before.rules_differ_from_base)
        throw std::runtime_error(
            options_.mod_profile ? "a changed mod did not differ from 3.1c in the presence facts"
                                 : "with no mod the presence facts said the rules differ from 3.1c"
        );
    if (profiles::digest_text(before.sim_hash) != played)
        throw std::runtime_error("the presence simulation hash is not the one the game plays");

    const auto back_to_menu = [this] {
        if (match_)
            leave_match();
        load(Screen::main_menu);
    };

    start_benchmark_skirmish_as_chosen();
    if (simulation_hash(*this) != played || presence_facts(*this) != before)
        throw std::runtime_error("a skirmish with no mutators changed the simulation hash");
    if (match_loadout() != nullptr || match_mutator_digest())
        throw std::runtime_error("a skirmish with no mutators recorded a loadout");
    back_to_menu();

    choose_match_mutators(kept);
    if (simulation_hash(*this) != played || presence_facts(*this) != before)
        throw std::runtime_error("a list chosen but not yet built changed the simulation hash");
    expect_no_mutators(played);

    start_benchmark_skirmish_as_chosen();
    const std::optional<oa::base::sha256::Digest> digest = match_mutator_digest();
    const oa::data::mutators::Loadout* loadout = match_loadout();
    if (!digest || loadout == nullptr)
        throw std::runtime_error("the mutators did not record a loadout");
    const std::optional<oa::base::sha256::Digest> first = oa::base::sha256::parse_hex(played);
    if (!first)
        throw std::runtime_error("the simulation hash is not hexadecimal");
    const std::string folded =
        profiles::digest_text(oa::data::mutators::fold_sim_hash(*first, digest));
    const std::string during = simulation_hash(*this);
    if (during != folded)
        throw std::runtime_error(
            "the simulation hash is not the profile hash folded with the mutator digest"
        );
    if (during == played)
        throw std::runtime_error("mutators left the simulation hash unchanged");
    const RunMutators told = run_mutators(*this);
    if (told.simulation_hash_without != played || told.simulation_hash_without == during ||
        simulation_hash(*this) != during)
        throw std::runtime_error("run_mutators did not report the hash from before the mutators");
    if (told.count == 0 || static_cast<std::size_t>(told.count) != loadout->entries.size() ||
        told.ids.size() != loadout->entries.size())
        throw std::runtime_error("run_mutators did not list the loadout");
    for (std::size_t index = 0; index < told.ids.size(); ++index) {
        if (told.ids[index] != loadout->entries[index].id)
            throw std::runtime_error("run_mutators listed the mutators out of order");
    }
    const auto played_facts = presence_facts(*this);
    if (profiles::digest_text(played_facts.sim_hash) != during ||
        !played_facts.rules_differ_from_base)
        throw std::runtime_error(
            "the presence facts did not report the folded hash as differing from 3.1c"
        );
    std::string read_error;
    const std::optional<oa::data::mutators::Loadout> read_back = oa::data::mutators::parse_loadout(
        oa::data::mutators::canonical_loadout(*loadout), &read_error
    );
    if (!read_back || *read_back != *loadout)
        throw std::runtime_error("the match loadout did not read back: " + read_error);
    back_to_menu();
    if (simulation_hash(*this) != played || presence_facts(*this) != before)
        throw std::runtime_error("leaving the match left the mutator digest in the hash");
    if (match_loadout() != nullptr || match_mutator_digest())
        throw std::runtime_error("leaving the match kept the loadout");
    expect_no_mutators(played);

    const auto original = engine_settings();
    auto settings = original;
    settings.developer_mode = true;
    settings.hack_overrides.push_back(
        profiles::HackOverride{"ai.attack-wave-size", true, {{"units", profiles::make_integer(40)}}}
    );
    apply_engine_settings(settings);
    const std::string changed = simulation_hash(*this);
    if (changed == played)
        throw std::runtime_error(
            "a simulation-changing Developer Mode override left the simulation hash unchanged"
        );
    expect_no_mutators(changed);
    const auto overridden = presence_facts(*this);
    if (!overridden.developer_mode || overridden.overrides.size() != 1 ||
        overridden.overrides[0].hack != "ai.attack-wave-size" || !overridden.overrides[0].on ||
        !lists(overridden.game_hacks, "ai.attack-wave-size") || !overridden.rules_differ_from_base)
        throw std::runtime_error("the attack-wave override did not reach the presence facts");

    settings.hack_overrides.push_back(profiles::HackOverride{"ui.megamap", true, {}});
    apply_engine_settings(settings);
    if (simulation_hash(*this) != changed)
        throw std::runtime_error("a view hack changed the simulation hash");
    const auto viewed = presence_facts(*this);
    if (!lists(viewed.view_hacks, "ui.megamap") || lists(viewed.game_hacks, "ui.megamap"))
        throw std::runtime_error("the megamap override was not a view hack in the presence facts");

    // Putting the settings back plays the profile the run started with, and
    // the kept baseline still agrees with a fresh resolve.
    apply_engine_settings(original);
    if (simulation_hash(*this) != played)
        throw std::runtime_error("turning Developer Mode off did not bring the hash back");
    expect_no_mutators(played);
    if (presence_facts(*this) != before)
        throw std::runtime_error(
            "turning Developer Mode off did not bring the presence facts back"
        );
    if (baseline_hash() != resolve_baseline_hash())
        throw std::runtime_error("the kept baseline hash differs from a fresh resolve");
    std::cout << "presence facts check: passed\n";
    std::cout << "simulation hash check: passed\n";
}

} // namespace oa::app
