// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Changes a parsed TDF document in its own arena: keys, values and sections.
#pragma once

#include "oa/formats/tdf.hpp"

#include <cstdint>
#include <string_view>

namespace oa::formats::tdf {

/// Longest value an edit stores, in bytes.
inline constexpr uint32_t max_edited_value_bytes = 65536;

/// Why an edit did not change the document, or that it did.
enum class EditStatus : uint8_t {
    ok,            ///< the edit was applied, or the key was already absent
    bad_key,       ///< the key is empty or would not read back as stored
    bad_value,     ///< the value would not read back as stored, or is too long
    bad_name,      ///< the section name would not read back, or the parent is nested too deeply
    no_such_child, ///< the child index is past the end
    out_of_memory, ///< the document arena cannot hold the edit
};

/// Returns a short English description of an edit status.
///
/// @param status edit status
/// @return a static string
[[nodiscard]] const char* edit_status_message(EditStatus status) noexcept;

/// Returns the root block, whose children are the top-level sections.
///
/// @param document document, or null
/// @return the root, or null when document is null or empty
[[nodiscard]] Block* editable_root(Document* document) noexcept;

/// Returns one child block by position, to edit.
///
/// @param block parent block, or null
/// @param index child position, in file order
/// @return the child, or null past the end
[[nodiscard]] Block* editable_child(Block* block, uint32_t index) noexcept;

/// Returns the first child of a name, ignoring ASCII case, to edit.
///
/// @param block parent block, or null
/// @param name child name
/// @return the child, or null when none matches
[[nodiscard]] Block* editable_child(Block* block, std::string_view name) noexcept;

/// Sets a key's value, or appends the key when the block has none of that name.
///
/// The newest case variant is the one updated, and it keeps its spelling. A
/// new key is spelled as given and placed after every existing key. The
/// stored value is trimmed of spaces, tabs and line breaks at both ends, as
/// the reader trims it. The block is marked edited.
///
/// @param[in,out] document document whose arena stores the text
/// @param[in,out] block block to change
/// @param key field name
/// @param value field value, before trimming
/// @return ok, or bad_key, bad_value or out_of_memory; a refusal changes nothing
[[nodiscard]] EditStatus
set_value(Document* document, Block* block, std::string_view key, std::string_view value) noexcept;

/// Removes every case variant of a key.
///
/// A key that is not present is ok and does not mark the block. Removing a
/// key that is present marks the block edited.
///
/// @param document document that owns the block
/// @param[in,out] block block to change
/// @param key field name, matched ignoring ASCII case
/// @return ok, even when the key was absent
[[nodiscard]] EditStatus
remove_key(Document* document, Block* block, std::string_view key) noexcept;

/// Appends an empty section.
///
/// The name is stored trimmed of spaces, tabs and line breaks at both ends,
/// as the reader trims a section name. The new block has no body text. The
/// parent is marked edited.
///
/// @param[in,out] document document whose arena stores the block
/// @param[in,out] parent block that gains the section
/// @param name section name
/// @param[out] status ok, or bad_name or out_of_memory; may be null
/// @return the new block, or null when the edit is refused
[[nodiscard]] Block*
add_child(Document* document, Block* parent, std::string_view name, EditStatus* status) noexcept;

/// Removes the child at an index.
///
/// The parent's other children keep their order. The parent is marked edited.
///
/// @param document document that owns the block
/// @param[in,out] parent block that loses the section
/// @param index child position, in file order
/// @return ok, or no_such_child when the index is past the end; a refusal changes nothing
[[nodiscard]] EditStatus remove_child(Document* document, Block* parent, uint32_t index) noexcept;

/// Returns the value at a position in case-insensitive key order.
///
/// The position is the same one property_key_at uses.
///
/// @param block block, or null
/// @param index property position
/// @return the value, or null when index is negative or past the end
[[nodiscard]] const char* property_value_at(const Block* block, int32_t index) noexcept;

/// Reports whether any block in the document has been edited.
///
/// @param document document, or null
/// @return true when the document holds a block an edit changed
[[nodiscard]] bool document_edited(const Document* document) noexcept;

} // namespace oa::formats::tdf
