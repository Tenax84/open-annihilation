// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/tdf/edit.hpp"
#include "tdf_detail.hpp"

#include <cstring>
#include <new>
#include <string_view>

namespace oa::formats::tdf {
namespace {

/// Tells whether a character is a line break the reader trims.
///
/// @param c character
/// @return true for CR or LF
[[nodiscard]] bool is_line_break(char c) noexcept {
    return c == '\n' || c == '\r';
}

/// Tells whether text holds a comment opener the reader would blank.
///
/// @param text text to scan
/// @return true when `//` or `/*` occurs
[[nodiscard]] bool has_comment_mark(std::string_view text) noexcept {
    for (std::size_t index = 0; index + 1 < text.size(); ++index) {
        if (text[index] == '/' && (text[index + 1] == '/' || text[index + 1] == '*'))
            return true;
    }
    return false;
}

/// Trims spaces, tabs and line breaks from both ends, as the reader does.
///
/// @param text text to trim
/// @return the interior
[[nodiscard]] std::string_view trim_separators(std::string_view text) noexcept {
    std::size_t begin = 0;
    while (begin < text.size() && detail::is_separator(text[begin]))
        ++begin;
    std::size_t end = text.size();
    while (end > begin && detail::is_separator(text[end - 1]))
        --end;
    return text.substr(begin, end - begin);
}

/// Folds one ASCII letter to lower case.
///
/// @param c character
/// @return the folded character
[[nodiscard]] unsigned char fold_lower(unsigned char c) noexcept {
    return static_cast<unsigned char>(c - 'A') < 26u ? static_cast<unsigned char>(c + 0x20) : c;
}

/// Compares a view with a NUL-terminated key, ignoring ASCII case.
///
/// @param left view, which may hold an embedded NUL
/// @param right NUL-terminated key
/// @return negative, zero or positive as left sorts before, with or after right
[[nodiscard]] int compare_view(std::string_view left, const char* right) noexcept {
    std::size_t index = 0;
    for (;;) {
        const auto left_byte = index < left.size() ? static_cast<unsigned char>(left[index])
                                                   : static_cast<unsigned char>(0);
        const auto right_byte = static_cast<unsigned char>(*right);
        if (left_byte == 0 || right_byte == 0) {
            if (left_byte == right_byte)
                return 0;
            const unsigned char folded_left = fold_lower(left_byte);
            const unsigned char folded_right = fold_lower(right_byte);
            return folded_left < folded_right ? -1 : 1;
        }
        const unsigned char folded_left = fold_lower(left_byte);
        const unsigned char folded_right = fold_lower(right_byte);
        if (folded_left != folded_right)
            return folded_left < folded_right ? -1 : 1;
        ++index;
        ++right;
    }
}

/// Finds the first property whose key is not less than a key, ignoring ASCII case.
///
/// @param block block searched
/// @param key key looked for
/// @return the property index, or property_count when every key is less
[[nodiscard]] uint32_t lower_bound_view(const Block* block, std::string_view key) noexcept {
    uint32_t first = 0;
    uint32_t last = block->property_count;
    while (first != last) {
        const uint32_t middle = first + (last - first) / 2u;
        if (compare_view(key, block->properties[middle].key) > 0)
            first = middle + 1;
        else
            last = middle;
    }
    return first;
}

/// Tells whether a trimmed key cannot be written and read back.
///
/// @param text trimmed key
/// @return true when the key is empty or holds a character the reader would not keep
[[nodiscard]] bool key_rejected(std::string_view text) noexcept {
    if (text.empty())
        return true;
    for (const char c : text) {
        if (c == '=' || c == ';' || c == '[' || c == ']' || c == '{' || c == '}' || c == '\0' ||
            is_line_break(c))
            return true;
    }
    return has_comment_mark(text);
}

/// Tells whether a trimmed value cannot be written and read back.
///
/// @param text trimmed value
/// @return true when the value is too long or holds a character the reader would not keep
[[nodiscard]] bool value_rejected(std::string_view text) noexcept {
    if (text.size() > max_edited_value_bytes)
        return true;
    for (const char c : text) {
        if (c == ';' || c == '\0' || is_line_break(c))
            return true;
    }
    return has_comment_mark(text);
}

/// Tells whether a trimmed section name cannot be written and read back.
///
/// @param text trimmed name
/// @return true when the name holds a character the reader would not keep
[[nodiscard]] bool name_rejected(std::string_view text) noexcept {
    for (const char c : text) {
        if (c == '[' || c == ']' || c == '{' || c == '}' || c == '\0' || is_line_break(c))
            return true;
    }
    return has_comment_mark(text);
}

/// Finds how deep a block sits, with the root at depth 0.
///
/// @param node block being visited
/// @param target block looked for
/// @param depth depth of node
/// @param[out] found depth of target, when found
/// @return true when target is node or one of its descendants
[[nodiscard]] bool
depth_of(const Block* node, const Block* target, uint32_t depth, uint32_t* found) noexcept {
    if (node == nullptr)
        return false;
    if (node == target) {
        *found = depth;
        return true;
    }
    for (uint32_t child_index = 0; child_index < node->child_count; ++child_index) {
        if (depth_of(node->children[child_index], target, depth + 1u, found))
            return true;
    }
    return false;
}

/// Reports whether a block or one below it has been edited.
///
/// @param block block, or null
/// @return true when an edit has marked this block or a descendant
[[nodiscard]] bool any_edited(const Block* block) noexcept {
    if (block == nullptr)
        return false;
    if (block->edited)
        return true;
    for (uint32_t child_index = 0; child_index < block->child_count; ++child_index) {
        if (any_edited(block->children[child_index]))
            return true;
    }
    return false;
}

/// Stores a status when the caller asked for one.
///
/// @param[out] status status to set; may be null
/// @param outcome status to store
void report(EditStatus* status, EditStatus outcome) noexcept {
    if (status != nullptr)
        *status = outcome;
}

} // namespace

const char* edit_status_message(EditStatus status) noexcept {
    switch (status) {
    case EditStatus::ok:
        return "ok";
    case EditStatus::bad_key:
        return "key is empty or would not read back";
    case EditStatus::bad_value:
        return "value would not read back or is too long";
    case EditStatus::bad_name:
        return "section name would not read back";
    case EditStatus::no_such_child:
        return "no child at that index";
    case EditStatus::out_of_memory:
        return "TDF arena exhausted";
    }
    return "unknown";
}

Block* editable_root(Document* document) noexcept {
    return document != nullptr ? document->root : nullptr;
}

Block* editable_child(Block* block, uint32_t index) noexcept {
    if (block == nullptr || index >= block->child_count)
        return nullptr;
    return block->children[index];
}

Block* editable_child(Block* block, std::string_view name) noexcept {
    if (block == nullptr)
        return nullptr;
    for (uint32_t child_index = 0; child_index < block->child_count; ++child_index) {
        if (compare_view(name, block->children[child_index]->name) == 0)
            return block->children[child_index];
    }
    return nullptr;
}

EditStatus
set_value(Document* document, Block* block, std::string_view key, std::string_view value) noexcept {
    if (document == nullptr || block == nullptr)
        return EditStatus::bad_key;
    const std::string_view trimmed_key = trim_separators(key);
    const std::string_view trimmed_value = trim_separators(value);
    if (key_rejected(trimmed_key))
        return EditStatus::bad_key;
    if (value_rejected(trimmed_value))
        return EditStatus::bad_value;

    const uint32_t position = lower_bound_view(block, trimmed_key);
    if (position < block->property_count &&
        compare_view(trimmed_key, block->properties[position].key) == 0) {
        char* stored =
            detail::arena_copy(&document->arena, trimmed_value.data(), trimmed_value.size());
        if (stored == nullptr)
            return EditStatus::out_of_memory;
        block->properties[position].value = stored;
        block->edited = true;
        return EditStatus::ok;
    }

    char* stored_key = detail::arena_copy(&document->arena, trimmed_key.data(), trimmed_key.size());
    char* stored_value =
        stored_key != nullptr
            ? detail::arena_copy(&document->arena, trimmed_value.data(), trimmed_value.size())
            : nullptr;
    if (stored_value == nullptr)
        return EditStatus::out_of_memory;
    if (!detail::grow(
            &document->arena, &block->properties, block->property_count, &block->property_capacity
        ))
        return EditStatus::out_of_memory;
    std::memmove(
        &block->properties[position + 1],
        &block->properties[position],
        sizeof(Property) * (block->property_count - position)
    );
    block->properties[position].key = stored_key;
    block->properties[position].value = stored_value;
    block->properties[position].order = block->next_order++;
    ++block->property_count;
    block->edited = true;
    return EditStatus::ok;
}

EditStatus remove_key(Document*, Block* block, std::string_view key) noexcept {
    if (block == nullptr)
        return EditStatus::ok;
    const std::string_view trimmed_key = trim_separators(key);
    const uint32_t first = lower_bound_view(block, trimmed_key);
    uint32_t last = first;
    while (last < block->property_count &&
           compare_view(trimmed_key, block->properties[last].key) == 0)
        ++last;
    if (first == last)
        return EditStatus::ok;
    std::memmove(
        &block->properties[first],
        &block->properties[last],
        sizeof(Property) * (block->property_count - last)
    );
    block->property_count -= last - first;
    block->edited = true;
    return EditStatus::ok;
}

Block*
add_child(Document* document, Block* parent, std::string_view name, EditStatus* status) noexcept {
    if (document == nullptr || parent == nullptr || document->root == nullptr) {
        report(status, EditStatus::bad_name);
        return nullptr;
    }
    const std::string_view trimmed_name = trim_separators(name);
    if (name_rejected(trimmed_name)) {
        report(status, EditStatus::bad_name);
        return nullptr;
    }
    uint32_t depth = 0;
    if (!depth_of(document->root, parent, 0, &depth) || depth >= max_nesting_depth) {
        report(status, EditStatus::bad_name);
        return nullptr;
    }

    auto* child = static_cast<Block*>(detail::arena_alloc(&document->arena, sizeof(Block)));
    char* stored_name =
        child != nullptr
            ? detail::arena_copy(&document->arena, trimmed_name.data(), trimmed_name.size())
            : nullptr;
    if (stored_name == nullptr) {
        report(status, EditStatus::out_of_memory);
        return nullptr;
    }
    new (child) Block{};
    child->name = stored_name;
    if (!detail::grow(
            &document->arena, &parent->children, parent->child_count, &parent->child_capacity
        )) {
        report(status, EditStatus::out_of_memory);
        return nullptr;
    }
    parent->children[parent->child_count++] = child;
    parent->edited = true;
    report(status, EditStatus::ok);
    return child;
}

EditStatus remove_child(Document*, Block* parent, uint32_t index) noexcept {
    if (parent == nullptr || index >= parent->child_count)
        return EditStatus::no_such_child;
    std::memmove(
        &parent->children[index],
        &parent->children[index + 1],
        sizeof(Block*) * (parent->child_count - index - 1u)
    );
    --parent->child_count;
    parent->edited = true;
    return EditStatus::ok;
}

const char* property_value_at(const Block* block, int32_t index) noexcept {
    if (block == nullptr || index < 0 || static_cast<uint32_t>(index) >= block->property_count)
        return nullptr;
    return block->properties[index].value;
}

bool document_edited(const Document* document) noexcept {
    return document != nullptr && any_edited(document->root);
}

} // namespace oa::formats::tdf
