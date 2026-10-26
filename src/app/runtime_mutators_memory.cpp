// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The last mutator set of each setup, and the crash record beside the
// preferences file. A set is the canonical loadout text. A file the player
// broke is logged and is never fatal.

#include "oa/app/runtime.hpp"

#include "oa/app/mutators_memory.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/ui/engine_settings.hpp"

#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace mutators = oa::data::mutators;
namespace settings = oa::ui::engine_settings;

static_assert(
    mutators_memory::preference_key(mutators_memory::Setup::skirmish) ==
    settings::key::mutators_skirmish
);
static_assert(
    mutators_memory::preference_key(mutators_memory::Setup::multiplayer) ==
    settings::key::mutators_multiplayer
);
static_assert(
    mutators_memory::preference_key(mutators_memory::Setup::campaign) ==
    settings::key::mutators_campaign
);

struct Runtime::MutatorMemory {
    mutators_memory::Sentinel sentinel = mutators_memory::Sentinel::disabled();
    std::optional<mutators_memory::Crash> crash{};
};

void Runtime::destroy_mutator_memory(MutatorMemory* memory) noexcept {
    delete memory;
}

std::string Runtime::mutator_names(const mutators::Loadout& loadout) {
    std::string names;
    for (const mutators::LoadoutEntry& entry : loadout.entries) {
        if (!names.empty())
            names += ", ";
        names += entry.id;
        names += ' ';
        names += entry.version;
    }
    return names;
}

std::string Runtime::mutator_list_text(const std::vector<std::string>& names) {
    std::string joined;
    for (const std::string& name : names) {
        if (!joined.empty())
            joined += ", ";
        joined += name;
    }
    return joined;
}

std::optional<mutators::Loadout> Runtime::remembered_mutators(mutators_memory::Setup setup) const {
    if (options_.no_mutators)
        return std::nullopt;
    const auto found = preference_values_.find(std::string(mutators_memory::preference_key(setup)));
    if (found == preference_values_.end())
        return std::nullopt;
    return mutators::parse_loadout(found->second, nullptr);
}

void Runtime::remember_mutators(mutators_memory::Setup setup, const mutators::Loadout& loadout) {
    if (options_.no_mutators || loadout.entries.empty())
        return;
    try {
        preference_values_[std::string(mutators_memory::preference_key(setup))] =
            mutators::canonical_loadout(loadout);
        preferences_dirty_ = true;
        save_preferences();
    } catch (const std::exception& error) {
        if (mutator_sets_failure_logged_)
            return;
        mutator_sets_failure_logged_ = true;
        std::cerr << "open-annihilation: mutators: the last set could not be stored: "
                  << error.what() << '\n';
    } catch (...) {
        if (mutator_sets_failure_logged_)
            return;
        mutator_sets_failure_logged_ = true;
        std::cerr << "open-annihilation: mutators: the last set could not be stored\n";
    }
}

void Runtime::forget_mutators(mutators_memory::Setup setup) {
    // The crash question calls this after the player answers, including a
    // run that refused mutators: the flag keeps a set from being read or
    // remembered, and FORGET IT still erases one the crash record named.
    const std::string key{mutators_memory::preference_key(setup)};
    if (preference_values_.erase(key) == 0)
        return;
    try {
        preferences_dirty_ = true;
        save_preferences();
    } catch (const std::exception& error) {
        if (mutator_sets_failure_logged_)
            return;
        mutator_sets_failure_logged_ = true;
        std::cerr << "open-annihilation: mutators: the last set could not be stored: "
                  << error.what() << '\n';
    } catch (...) {
        if (mutator_sets_failure_logged_)
            return;
        mutator_sets_failure_logged_ = true;
        std::cerr << "open-annihilation: mutators: the last set could not be stored\n";
    }
}

void Runtime::start_with_mutators(
    mutators_memory::Setup setup, std::vector<mutators::ResolvedMutator> chosen
) {
    const mutators::Loadout loadout = mutators::loadout_of(chosen);
    if (options_.no_mutators) {
        note_mutators_refused(mutator_names(loadout));
        mutator_setup_.reset();
        choose_match_mutators({});
        return;
    }
    if (chosen.empty()) {
        // A setup that starts without a set plays the run's own list,
        // --mutator's or a launch file's, normally none; never an
        // earlier setup's set.
        mutator_setup_.reset();
        take_command_line_mutators();
        return;
    }
    remember_mutators(setup, loadout);
    mutator_setup_ = setup;
    choose_match_mutators(std::move(chosen));
}

std::optional<mutators_memory::Crash> Runtime::take_mutator_crash() {
    if (mutator_memory_ == nullptr)
        return std::nullopt;
    std::optional<mutators_memory::Crash> taken = std::move(mutator_memory_->crash);
    mutator_memory_->crash.reset();
    return taken;
}

bool Runtime::mutators_refused_for_run() const noexcept {
    return options_.no_mutators;
}

void Runtime::open_mutator_sentinel() {
    std::unique_ptr<MutatorMemory, void (*)(MutatorMemory*) noexcept> made{
        new MutatorMemory, destroy_mutator_memory
    };
    if (preference_path_.empty()) {
        mutator_memory_ = std::move(made);
        return;
    }
    try {
        fs::path folder = preference_path_.parent_path();
        if (folder.empty())
            folder = ".";
        made->sentinel = mutators_memory::Sentinel::open_folder(folder);
        if (const std::optional<mutators_memory::Crash>& leftover = made->sentinel.leftover()) {
            made->crash = *leftover;
            if (leftover->loadout.entries.empty()) {
                std::cerr << "open-annihilation: mutators: the last game ended in a crash; its "
                             "mutators could not be read\n";
            } else {
                std::cerr << "open-annihilation: mutators: the last game ended in a crash with "
                          << mutator_names(leftover->loadout) << '\n';
            }
            made->sentinel.clear();
        }
        mutator_memory_ = std::move(made);
    } catch (const std::exception& error) {
        made->sentinel = mutators_memory::Sentinel::disabled();
        made->crash.reset();
        mutator_memory_ = std::move(made);
        std::cerr << "open-annihilation: mutators: the crash record could not be read: "
                  << error.what() << '\n';
    } catch (...) {
        made->sentinel = mutators_memory::Sentinel::disabled();
        made->crash.reset();
        mutator_memory_ = std::move(made);
        std::cerr << "open-annihilation: mutators: the crash record could not be read\n";
    }
}

void Runtime::arm_mutator_sentinel(
    std::optional<mutators_memory::Setup> setup, const mutators::Loadout& loadout
) {
    if (mutator_memory_ == nullptr || loadout.entries.empty())
        return;
    mutators_memory::Crash crash;
    crash.setup = setup;
    crash.loadout = loadout;
    mutator_memory_->sentinel.write(crash);
}

void Runtime::clear_mutator_sentinel() {
    if (mutator_memory_ == nullptr)
        return;
    mutator_memory_->sentinel.clear();
}

void Runtime::note_mutators_refused(std::string_view named) {
    if (mutator_refusal_logged_ || named.empty())
        return;
    mutator_refusal_logged_ = true;
    std::cerr << "open-annihilation: mutators: --no-mutators refused " << named << '\n';
}

} // namespace oa::app
