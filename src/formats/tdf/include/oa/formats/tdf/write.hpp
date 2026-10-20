// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Writes a parsed TDF document back to text.
#pragma once

#include "oa/formats/tdf.hpp"

#include <optional>
#include <string>

namespace oa::formats::tdf {

/// Writes a document as TDF text.
///
/// A document no edit has touched is written as its comment-stripped text,
/// unchanged. A block nobody edited, under a block someone did, is written
/// with its parsed body byte for byte, so its body hash does not move. An
/// edited block, and any block above it, is written in canonical layout with
/// its keys in file order and case variants oldest first. The result is
/// refused when it would be longer than max_input_bytes, because it could
/// not be read back.
///
/// @param document document to write, or null
/// @return the text, or nullopt when there is nothing to write or the text would be too long
[[nodiscard]] std::optional<std::string> write_text(const Document* document);

} // namespace oa::formats::tdf
