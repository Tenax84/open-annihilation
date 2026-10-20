// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The installed mutators. The list is read from the player's Mutators folder
// on first use, and again after every mutator package is installed or
// removed. The scripts in those folders are never opened.

#include "oa/app/runtime.hpp"

#include "oa/data/mutator_library/library.hpp"

#include <memory>
#include <string>
#include <utility>

namespace oa::app {

struct Runtime::MutatorLibraryState {
    oa::data::mutator_library::Listing listing{};
};

void Runtime::destroy_mutator_library_state(MutatorLibraryState* state) noexcept {
    delete state;
}

const oa::data::mutator_library::Listing& Runtime::installed_mutators() const {
    if (!mutator_library_state_) {
        std::unique_ptr<MutatorLibraryState, void (*)(MutatorLibraryState*) noexcept> made{
            new MutatorLibraryState, destroy_mutator_library_state
        };
        made->listing = oa::data::mutator_library::list_installed(
            user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name)
        );
        mutator_library_state_ = std::move(made);
    }
    return mutator_library_state_->listing;
}

void Runtime::reload_installed_mutators() const {
    if (mutator_library_state_ == nullptr)
        return;
    mutator_library_state_->listing = oa::data::mutator_library::list_installed(
        user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name)
    );
}

} // namespace oa::app
