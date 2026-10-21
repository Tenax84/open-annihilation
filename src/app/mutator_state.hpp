// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutators chosen for the next matches, and the result of the one that
// is running (Runtime::MutatorState). The match start and the hack step
// share it; it stays out of runtime.hpp so that header does not include the
// pipeline.
#pragma once

#include "oa/app/runtime.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/pipeline.hpp"

#include <optional>
#include <vector>

namespace oa::app {

struct Runtime::MutatorState {
    /// The mutators chosen for the next matches, top of the list first.
    std::vector<oa::data::mutators::ResolvedMutator> chosen{};
    /// The running match's pipeline result. Empty before a match applies them.
    std::optional<oa::data::mutators::PipelineResult> result{};
};

} // namespace oa::app
