// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/tdf/write.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::formats::tdf {
namespace {

/// A text being built, refused once it would pass max_input_bytes.
struct Sink {
    std::string text;
    bool full = false;

    /// Appends bytes unless the result would be too long to parse.
    ///
    /// @param bytes bytes to append
    void add(std::string_view bytes) {
        if (full)
            return;
        if (bytes.size() > max_input_bytes || text.size() > max_input_bytes - bytes.size()) {
            full = true;
            return;
        }
        text.append(bytes.data(), bytes.size());
    }

    /// Appends a NUL-terminated string.
    ///
    /// @param bytes string; null appends nothing
    void add(const char* bytes) {
        if (bytes != nullptr)
            add(std::string_view(bytes));
    }

    /// Appends one character.
    ///
    /// @param c character
    void add_char(char c) { add(std::string_view(&c, 1)); }

    /// Appends a run of tab characters.
    ///
    /// @param count tabs to append
    void tabs(uint32_t count) {
        for (uint32_t tab = 0; tab < count; ++tab)
            add_char('\t');
    }
};

/// Reports whether a block or one below it has been edited.
///
/// @param block block, or null
/// @return true when the block must be written in canonical layout
[[nodiscard]] bool block_dirty(const Block* block) {
    if (block == nullptr)
        return false;
    if (block->edited)
        return true;
    for (uint32_t child_index = 0; child_index < block->child_count; ++child_index) {
        if (block_dirty(block->children[child_index]))
            return true;
    }
    return false;
}

/// Writes a block's keys in file order, one per line, at a tab depth.
///
/// Case variants come out oldest first, so a re-read sees the same newest.
///
/// @param[in,out] sink text being built
/// @param block block whose keys are written
/// @param depth tab columns before each key; 0 for the root
void write_keys(Sink& sink, const Block* block, uint32_t depth) {
    std::vector<uint32_t> places(block->property_count);
    for (uint32_t place = 0; place < block->property_count; ++place)
        places[place] = place;
    std::sort(places.begin(), places.end(), [&](uint32_t earlier, uint32_t later) {
        return block->properties[earlier].order < block->properties[later].order;
    });
    for (const uint32_t place : places) {
        sink.tabs(depth);
        sink.add(block->properties[place].key);
        sink.add_char('=');
        sink.add(block->properties[place].value);
        sink.add(";\r\n");
    }
}

/// Writes one section at a depth. Top-level sections are depth 1.
///
/// @param[in,out] sink text being built
/// @param block section to write
/// @param depth section depth, at least 1
void write_block(Sink& sink, const Block* block, uint32_t depth) {
    sink.tabs(depth - 1u);
    sink.add_char('[');
    sink.add(block->name);
    sink.add("]\r\n");
    sink.tabs(depth);
    if (!block_dirty(block)) {
        sink.add_char('{');
        if (block->body_text != nullptr && block->body_length != 0)
            sink.add(std::string_view(block->body_text, block->body_length));
        sink.add("}\r\n");
        return;
    }
    sink.add("{\r\n");
    write_keys(sink, block, depth);
    for (uint32_t child_index = 0; child_index < block->child_count; ++child_index)
        write_block(sink, block->children[child_index], depth + 1u);
    sink.tabs(depth);
    sink.add("}\r\n");
}

/// Writes a dirty root: its keys at column 0, then its children.
///
/// @param[in,out] sink text being built
/// @param root root block
void write_root(Sink& sink, const Block* root) {
    write_keys(sink, root, 0);
    for (uint32_t child_index = 0; child_index < root->child_count; ++child_index)
        write_block(sink, root->children[child_index], 1u);
}

} // namespace

std::optional<std::string> write_text(const Document* document) {
    if (document == nullptr || document->root == nullptr)
        return std::nullopt;
    if (!block_dirty(document->root)) {
        if (document->text == nullptr)
            return std::string{};
        if (document->text_length > max_input_bytes)
            return std::nullopt;
        return std::string(document->text, document->text_length);
    }
    Sink sink;
    write_root(sink, document->root);
    if (sink.full)
        return std::nullopt;
    return std::move(sink.text);
}

} // namespace oa::formats::tdf
