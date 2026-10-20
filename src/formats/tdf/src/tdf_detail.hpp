// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Arena and property-table helpers shared by the parser and the editor.
// Not a public header: documents store nothing outside this arena.
#pragma once

#include "oa/formats/tdf.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace oa::formats::tdf {

struct ArenaChunk {
    ArenaChunk* next{};
    uint32_t capacity{};
    uint32_t used{};
    // Storage follows the header.
};

namespace detail {

inline constexpr uint32_t arena_chunk_bytes = 64u * 1024u;
inline constexpr uint32_t arena_alignment = alignof(std::max_align_t);

/// Rounds a size up to the arena's alignment.
///
/// @param value size in bytes
/// @return the aligned size
[[nodiscard]] inline uint32_t align_up(uint32_t value) noexcept {
    return (value + arena_alignment - 1u) & ~(arena_alignment - 1u);
}

/// Returns the aligned size of an arena chunk header.
///
/// @return bytes before the chunk's storage
[[nodiscard]] inline uint32_t chunk_header_bytes() noexcept {
    return align_up(static_cast<uint32_t>(sizeof(ArenaChunk)));
}

/// Allocates bytes from a document arena.
///
/// @param[in,out] arena arena to take the bytes from
/// @param size bytes to allocate
/// @return the bytes, or null when the arena is exhausted
inline void* arena_alloc(Arena* arena, std::size_t size) noexcept {
    if (size > max_arena_bytes)
        return nullptr;
    const auto need = align_up(static_cast<uint32_t>(size));
    ArenaChunk* chunk = arena->head;
    if (chunk == nullptr || chunk->capacity - chunk->used < need) {
        const auto capacity = need > arena_chunk_bytes ? need : arena_chunk_bytes;
        if (arena->used_bytes + capacity > max_arena_bytes)
            return nullptr;
        auto* raw = static_cast<unsigned char*>(std::malloc(chunk_header_bytes() + capacity));
        if (raw == nullptr)
            return nullptr;
        chunk = reinterpret_cast<ArenaChunk*>(raw);
        chunk->next = arena->head;
        chunk->capacity = capacity;
        chunk->used = 0;
        arena->head = chunk;
        arena->used_bytes += capacity;
    }
    auto* base = reinterpret_cast<unsigned char*>(chunk) + chunk_header_bytes();
    void* result = base + chunk->used;
    chunk->used += need;
    return result;
}

/// Frees every chunk of an arena.
///
/// @param[in,out] arena arena left empty
inline void arena_free(Arena* arena) noexcept {
    ArenaChunk* chunk = arena->head;
    while (chunk != nullptr) {
        ArenaChunk* next = chunk->next;
        std::free(chunk);
        chunk = next;
    }
    arena->head = nullptr;
    arena->used_bytes = 0;
}

/// Copies bytes into the arena and terminates them.
///
/// @param[in,out] arena arena the copy is allocated from
/// @param begin first byte
/// @param length bytes to copy
/// @return the NUL-terminated copy, or null when the arena is exhausted
inline char* arena_copy(Arena* arena, const char* begin, std::size_t length) noexcept {
    auto* copy = static_cast<char*>(arena_alloc(arena, length + 1));
    if (copy == nullptr)
        return nullptr;
    if (length != 0)
        std::memcpy(copy, begin, length);
    copy[length] = '\0';
    return copy;
}

/// Tells whether a character is a separator the reader trims.
///
/// @param c character
/// @return true for a space, tab or line break
[[nodiscard]] inline bool is_separator(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/// Copies a token with separators trimmed from both ends.
///
/// @param[in,out] arena arena the copy is allocated from
/// @param from first character of the token
/// @param to one past its last character
/// @return the NUL-terminated copy, or null when the arena is exhausted
inline char* copy_trimmed_token(Arena* arena, const char* from, const char* to) noexcept {
    while (*from != '\0' && is_separator(*from))
        ++from;
    if (from >= to)
        return arena_copy(arena, "", 0);
    const char* end = to;
    while (is_separator(end[-1]))
        --end;
    return arena_copy(arena, from, static_cast<std::size_t>(end - from));
}

/// Grows a pointer table stored in the arena, leaving the old one behind.
///
/// @param[in,out] arena arena the new table is allocated from
/// @param[in,out] items table; replaced when it grows
/// @param count entries in use
/// @param[in,out] capacity entries allocated
/// @return false when the arena is exhausted
template <typename T>
bool grow(Arena* arena, T** items, uint32_t count, uint32_t* capacity) noexcept {
    if (count < *capacity)
        return true;
    const uint32_t next = *capacity == 0 ? 8u : *capacity * 2u;
    auto* grown = static_cast<T*>(arena_alloc(arena, sizeof(T) * next));
    if (grown == nullptr)
        return false;
    if (count != 0)
        std::memcpy(grown, *items, sizeof(T) * count);
    *items = grown;
    *capacity = next;
    return true;
}

/// Finds the first property whose key is not less than a key, ignoring ASCII case.
///
/// @param block block searched
/// @param key key looked for
/// @return the property index, or property_count when every key is less
[[nodiscard]] inline uint32_t lower_bound_key(const Block* block, const char* key) noexcept {
    uint32_t first = 0;
    uint32_t last = block->property_count;
    while (first != last) {
        const uint32_t middle = first + (last - first) / 2u;
        if (compare_nocase(block->properties[middle].key, key) < 0)
            first = middle + 1;
        else
            last = middle;
    }
    return first;
}

/// Returns the value slot for a key, inserting an empty one when no stored key is byte-identical.
///
/// Keys differing only in case get their own slot, placed ahead of the older
/// one so lookups see the newest. A new slot takes the next file place; a
/// repeated byte-identical key keeps the place of its first occurrence.
///
/// @param[in,out] arena arena the property table grows in
/// @param[in,out] block block whose sorted property table is searched
/// @param key key, already copied into the arena
/// @return the value slot, or null when the arena is exhausted
inline char** find_or_add_property(Arena* arena, Block* block, char* key) noexcept {
    const uint32_t position = lower_bound_key(block, key);
    if (position != block->property_count && std::strcmp(block->properties[position].key, key) == 0)
        return &block->properties[position].value;
    if (!grow(arena, &block->properties, block->property_count, &block->property_capacity))
        return nullptr;
    std::memmove(
        &block->properties[position + 1],
        &block->properties[position],
        sizeof(Property) * (block->property_count - position)
    );
    block->properties[position].key = key;
    block->properties[position].value = nullptr;
    block->properties[position].order = block->next_order++;
    ++block->property_count;
    return &block->properties[position].value;
}

} // namespace detail
} // namespace oa::formats::tdf
