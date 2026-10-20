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

/// Bytes of allocation or of a scan that count as one instruction.
inline constexpr std::size_t bytes_per_charge = 64;

/// One Lua value, laid out as the interpreter's own value.
///
/// The header that names that value is private to the interpreter, so the
/// sandbox keeps this copy. `table_range_limit` is
/// `default_memory_budget / sizeof(TValue)` from it.
union ValueSlot {
    void* pointer;
    lua_CFunction function;
    lua_Integer integer;
    lua_Number number;
    unsigned char byte;
};

struct TableSlot {
    ValueSlot value;
    unsigned char tag;
};

/// Most elements one table call may move or visit.
inline constexpr std::size_t table_range_limit = default_memory_budget / sizeof(TableSlot);

/// Which protected entry the trampoline runs.
enum class PhaseMode : uint8_t { open_libraries, run_chunk, call, host, compile };

struct Sandbox::Control {
    std::size_t memory_cap{default_memory_budget};
    std::size_t memory_in_use{};
    bool allocation_refused{};

    /// The state this block belongs to. Set before the libraries open.
    lua_State* main_state{};
    /// Set while the open measures the string header.
    bool measuring_string_header{};
    /// `nsize - length - 1` of one long string, or 0 until that measurement.
    std::size_t string_header{};
    /// A string past `string_length_limit` was refused. Sticky for the phase.
    bool string_refused{};
    uint64_t string_refused_length{};
    /// Set while a chunk is being loaded, so a literal is blamed on that chunk.
    bool loading_chunk{};
    char string_message[budget_message_bytes]{};

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

/// Replaces `string.find`, `match`, `gmatch` and `gsub` in the table on top
/// of the stack with the counted copies of Lua 5.4.9's matcher.
///
/// They take the same arguments and give the same results and messages as
/// Lua's own; each matcher step counts against the phase allowance, and past
/// it they raise the budget message and mark the allowance spent. `gmatch`'s
/// iterator keeps its place in a userdata value.
///
/// @param state the sandbox state, with the `string` table on top
void install_patterns(lua_State* state);

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

/// The instruction hook. Library code re-arms it when a charge spends the allowance.
extern "C" void script_count_hook(lua_State* state, lua_Debug* activation);

/// How many instruction units `bytes` cost, one per `bytes_per_charge`, rounded up.
///
/// @param bytes requested bytes, not the size the allocator happened to reserve
/// @return the units, 0 when `bytes` is 0
inline uint64_t units_for_bytes(std::size_t bytes) {
    return (static_cast<uint64_t>(bytes) + bytes_per_charge - 1) / bytes_per_charge;
}

/// Adds `units` to the phase count and marks the allowance spent when it is reached.
///
/// Does not raise. The allocator uses this, because it cannot call into the
/// state. The count hook raises on its next check.
///
/// @param control the state being charged
/// @param units instruction units to add; 0 adds nothing
inline void add_charge(HostDetail::Control* control, uint64_t units) {
    if (control == nullptr || units == 0)
        return;
    const uint64_t room = UINT64_MAX - control->instructions_used;
    if (units > room)
        control->instructions_used = UINT64_MAX;
    else
        control->instructions_used += units;
    if (control->instructions_used >= control->instruction_allowance)
        control->allowance_spent = true;
}

/// Adds `units` and raises, as the count hook does, when the allowance is spent.
///
/// A string refusal raises the string message instead. No object with a
/// destructor may be alive across this call.
///
/// @param state the sandbox state
/// @param units instruction units to add
inline void charge(lua_State* state, uint64_t units) {
    HostDetail::Control* control = control_from(state);
    if (control->string_refused) {
        lua_sethook(state, script_count_hook, LUA_MASKCOUNT, 1);
        luaL_error(state, "%s", control->string_message);
    }
    if (control->allowance_spent)
        luaL_error(state, "%s", control->instruction_message);
    add_charge(control, units);
    if (control->allowance_spent) {
        lua_sethook(state, script_count_hook, LUA_MASKCOUNT, 1);
        luaL_error(state, "%s", control->instruction_message);
    }
}

/// Records that `length` bytes would pass the string cap, and where it happened.
///
/// The first refusal sticks. `message` null uses the standard sentence naming
/// `length` and 65536. Later calls do nothing.
///
/// @param control the state that refused the string
/// @param length how many bytes the string would have held
/// @param message the sentence to keep, or null for the standard one
void record_string_limit(HostDetail::Control* control, uint64_t length, const char* message);

/// Records the refusal and raises at the calling line.
///
/// @param state the sandbox state
/// @param length how many bytes the string would have held
/// @param message the sentence to keep; null uses the standard one
/// @return does not return
void raise_string_limit(lua_State* state, uint64_t length, const char* message);

/// Raises when `have + extra` would pass the string cap.
///
/// @param state the sandbox state
/// @param have bytes already in the buffer
/// @param extra bytes about to be added
/// @param message the early sentence naming the function
inline void
reject_long_piece(lua_State* state, std::size_t have, std::size_t extra, const char* message) {
    if (extra > string_length_limit || have > string_length_limit - extra) {
        const uint64_t length = static_cast<uint64_t>(have) + static_cast<uint64_t>(extra);
        raise_string_limit(state, length, message);
    }
}

} // namespace oa::data::script_host
