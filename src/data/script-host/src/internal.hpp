// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The sandbox's own state. One control block belongs to one state, and the
// allocator's pointer to it does not move when the sandbox object does.
#pragma once

#include "oa/data/script_host/script_host.hpp"

#include "lua.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oa::data::script_host {

inline constexpr std::size_t path_buffer_bytes = 80;
inline constexpr std::size_t message_buffer_bytes = 512;
inline constexpr std::size_t chunk_buffer_bytes = 160;
inline constexpr std::size_t scratch_buffer_bytes = 3072;
inline constexpr std::size_t budget_message_bytes = 160;

/// Which protected entry the trampoline runs.
enum class PhaseMode : uint8_t { open_libraries, run_chunk, call, host, compile };

struct Sandbox::Control {
    std::size_t memory_cap{default_memory_budget};
    std::size_t memory_in_use{};
    bool allocation_refused{};

    uint64_t instruction_allowance{default_instruction_budget};
    uint64_t instructions_used{};
    bool allowance_spent{};
    bool sandbox_failed{};

    ErrorKind pending_kind{ErrorKind::none};
    ErrorKind raised_kind{ErrorKind::none};
    bool location_locked{};
    int32_t error_line{};
    char instruction_message[budget_message_bytes]{};
    char memory_message[budget_message_bytes]{};
    char error_message[message_buffer_bytes]{};
    char error_chunk[chunk_buffer_bytes]{};
    char traceback_text[max_traceback_bytes]{};
    char scratch[scratch_buffer_bytes]{};

    lua_CFunction require_function{};

    int loading_count{};
    char loading_paths[max_require_depth][path_buffer_bytes]{};
    char module_path[path_buffer_bytes]{};
    char chunk_at_path[path_buffer_bytes]{};

    std::vector<uint8_t> read_buffer;
    std::vector<std::string> printed;
    std::size_t printed_bytes{};
    std::size_t lines_left_out{};
    bool print_closed{};
    bool omission_stored{};

    SourceHooks sources{};
    std::string package_id;
    /// Copied from `OpenOptions::random_seed` before the libraries open.
    uint64_t random_seed{};
    std::string phase;
    std::string chunk_name_storage;

    PhaseMode mode{PhaseMode::open_libraries};
    const uint8_t* chunk_data{};
    std::size_t chunk_size{};
    /// Name passed to the loader for the chunk now being loaded. Not owned.
    const char* active_chunk_name{};
    CallHooks call_hooks{};
    bool has_read{};
    void* host_context{};
    void (*host_work)(void* context, lua_State* state){};
    void* install_context{};
    void (*install)(void* context, lua_State* state){};
};

/// The private control block, named for this module's sources.
struct HostDetail {
    using Control = Sandbox::Control;
};

/// Appends text, keeping a terminating NUL inside `end`.
///
/// @param cursor first byte to write
/// @param end one past the last byte of the buffer
/// @param text bytes to copy; null copies nothing
/// @return the new cursor, on the terminating NUL
inline char* append_text(char* cursor, char* end, const char* text) {
    if (cursor == nullptr || end == nullptr || cursor >= end)
        return cursor;
    if (text == nullptr)
        return cursor;
    while (*text != '\0' && cursor + 1 < end)
        *cursor++ = *text++;
    *cursor = '\0';
    return cursor;
}

/// Appends `length` bytes, keeping a terminating NUL inside `end`.
///
/// @param cursor first byte to write
/// @param end one past the last byte of the buffer
/// @param text bytes to copy
/// @param length how many bytes to copy
/// @return the new cursor, on the terminating NUL
inline char* append_bytes(char* cursor, char* end, const char* text, std::size_t length) {
    if (cursor == nullptr || end == nullptr || cursor >= end)
        return cursor;
    if (text == nullptr)
        length = 0;
    for (std::size_t index = 0; index < length && cursor + 1 < end; ++index)
        *cursor++ = text[index];
    *cursor = '\0';
    return cursor;
}

/// Appends a decimal number, keeping a terminating NUL inside `end`.
///
/// @param cursor first byte to write
/// @param end one past the last byte of the buffer
/// @param value the number to write
/// @return the new cursor, on the terminating NUL
inline char* append_u64(char* cursor, char* end, uint64_t value) {
    char digits[20];
    int count = 0;
    do {
        digits[count++] = static_cast<char>('0' + static_cast<int>(value % 10));
        value /= 10;
    } while (value != 0 && count < static_cast<int>(sizeof digits));
    while (count > 0 && cursor != nullptr && end != nullptr && cursor + 1 < end)
        *cursor++ = digits[--count];
    if (cursor != nullptr && end != nullptr && cursor < end)
        *cursor = '\0';
    return cursor;
}

/// Copies bytes into a fixed buffer and terminates it.
///
/// @param destination buffer to fill
/// @param capacity bytes available at `destination`, including the NUL
/// @param text bytes to copy; null copies nothing
/// @param length how many bytes `text` holds
inline void
copy_bounded(char* destination, std::size_t capacity, const char* text, std::size_t length) {
    if (destination == nullptr || capacity == 0)
        return;
    if (text == nullptr)
        text = "";
    if (length >= capacity)
        length = capacity - 1;
    for (std::size_t index = 0; index < length; ++index)
        destination[index] = text[index];
    destination[length] = '\0';
}

/// Returns the control block the state's allocator holds.
///
/// @param state a state this host opened
/// @return that state's control block
inline HostDetail::Control* control_from(lua_State* state) {
    void* user = nullptr;
    lua_getallocf(state, &user);
    return static_cast<HostDetail::Control*>(user);
}

/// Opens the shared libraries and the replacements around them.
///
/// @param state the state being opened
void open_libraries(lua_State* state);

/// Finds a pattern, counting each matcher step against the phase allowance.
///
/// Same arguments and results as Lua's `string.find`, including a plain
/// search. Past the allowance it raises the budget message and marks the
/// allowance spent.
///
/// @param state the sandbox state
/// @return how many values were pushed
extern "C" int find(lua_State* state);

/// Matches a pattern, counting each matcher step against the phase allowance.
///
/// Same arguments and results as Lua's `string.match`. Past the allowance it
/// raises the budget message and marks the allowance spent.
///
/// @param state the sandbox state
/// @return how many values were pushed
extern "C" int match(lua_State* state);

/// Returns a `string.gmatch` iterator whose steps count against the allowance.
///
/// The iterator keeps its place in a userdata value. Past the allowance a
/// step raises the budget message and marks the allowance spent.
///
/// @param state the sandbox state
/// @return how many values were pushed
extern "C" int gmatch(lua_State* state);

/// Replaces matches, counting each matcher step against the phase allowance.
///
/// Same arguments and results as Lua's `string.gsub`. Past the allowance it
/// raises the budget message and marks the allowance spent.
///
/// @param state the sandbox state
/// @return how many values were pushed
extern "C" int gsub(lua_State* state);

/// Opens the math library. May raise.
///
/// @param state the state being opened
void open_math_library(lua_State* state);

/// Installs `pairs` for number, string and boolean keys, in a fixed order.
/// May raise.
///
/// @param state the state being opened
void install_pairs(lua_State* state);

/// Adds the package `require` and an empty module cache.
///
/// @param state the state being opened
void install_require(lua_State* state);

/// Records a loader error already on the stack and keeps that location.
///
/// @param control the state that tried to load
/// @param state the state, with the loader's message at the top
void remember_load_error(HostDetail::Control* control, lua_State* state);

} // namespace oa::data::script_host
