// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A rule file longer than the limit is refused before it is read. The
// statements themselves are read by the parser.

#include "oa/formats/soft_mutator.hpp"

#include <utility>

namespace oa::formats::soft_mutator {

ParseResult parse_rules(std::span<const uint8_t> text) {
    if (text.size() > max_input_bytes) {
        ParseResult refused;
        Diagnostic item;
        item.severity = Severity::error;
        item.code = DiagnosticCode::too_large;
        item.line = 1;
        item.message = "rule file is larger than 1 MiB";
        refused.diagnostics.push_back(std::move(item));
        return refused;
    }
    return {};
}

} // namespace oa::formats::soft_mutator
