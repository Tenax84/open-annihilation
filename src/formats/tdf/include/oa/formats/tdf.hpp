// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// TDF/FBI/GUI text documents, parsed the way the game parses them.
//
// A document is a tree of blocks. Each block has a name, an ordered list of
// child blocks and a property list kept sorted by case-insensitive key. Each
// key also records its place in the file, so a later write can keep that
// order. All storage lives in one bounded arena owned by the document.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace oa::formats::tdf {

// Upper bounds applied before any allocation.
inline constexpr uint32_t max_input_bytes = 16u * 1024u * 1024u;
inline constexpr uint32_t max_arena_bytes = 64u * 1024u * 1024u;
inline constexpr uint32_t max_nesting_depth = 64;

struct Property {
    char* key{};
    char* value{};
    uint32_t order{}; ///< place in the block; a repeated identical key keeps its first place
};

/// One block of a TDF document.
///
/// `properties` stay sorted by case-insensitive key. `body_hash` describes
/// the body text the block was parsed from. An edit marks the block and
/// leaves that hash stale until the written text is parsed again.
struct Block {
    char* name{};
    Block** children{};
    uint32_t child_count{};
    uint32_t child_capacity{};
    Property* properties{}; // sorted by case-insensitive key
    uint32_t property_count{};
    uint32_t property_capacity{};
    uint32_t
        body_hash{}; ///< hash of the parsed body; stale after an edit until a rewrite is parsed
    const char*
        body_text{}; ///< parsed body, after '{' up to '}'; empty when add_child made the block
    uint32_t body_length{}; ///< bytes of body_text
    uint32_t next_order{};  ///< place the next new key takes
    bool edited{};          ///< set when an edit changes this block
};

struct ArenaChunk;

struct Arena {
    ArenaChunk* head{};
    uint32_t used_bytes{};
};

// A parsed TDF document: the root plus a cursor used by the section-stepping
// helpers. A null cursor means "at the root".
struct Document {
    Block* root{};
    const Block* cursor{};
    bool from_archive{}; // file was read from an HPI rather than a loose file
    Arena arena;
    const char* text{};     ///< comment-stripped copy of the parsed text, in the arena
    uint32_t text_length{}; ///< bytes of text, not counting the terminating NUL
};

enum class ParseStatus : uint8_t {
    ok,
    sub_record_close_missing, // "sub-record name is never closed by ']'"
    sub_record_open_missing,  // "no '{' follows the sub-record name"
    equals_missing,           // "entry lacks '=' between key and value"
    semicolon_missing,        // "entry lacks its closing ';'"
    unexpected_end,           // "end of file inside a sub-record"
    too_deep,
    too_large,
    out_of_memory,
};

struct ParseError {
    ParseStatus status{};
    uint32_t offset{};     // byte offset into the comment-stripped text
    char block_name[64]{}; // block being parsed when the error was raised
};

/// Returns a short English description of a parse status.
///
/// @param status parse status
/// @return a static string
[[nodiscard]] const char* parse_status_message(ParseStatus status) noexcept;
/// Describes a parse failure: its status, the byte it was found at and the
/// block being parsed.
///
/// @param error failure parse_text reported
/// @return e.g. "entry lacks its closing ';' at byte 12 in [Coral20]"
[[nodiscard]] std::string describe(const ParseError& error);

/// Makes a document empty with no arena storage.
///
/// @param[out] document document to initialize
void document_init(Document* document) noexcept;
/// Frees a document's arena and clears its root and cursor.
///
/// @param[in,out] document document to empty; it can be parsed into again
void document_free(Document* document) noexcept;

/// Replaces a document's contents with a parse of a text.
///
/// The text is copied and NUL-terminated, its comments are stripped, and the
/// root block is parsed. On failure the document is left empty.
///
/// @param[in,out] document document that receives the tree
/// @param text TDF text; need not be NUL-terminated
/// @param length bytes of text, at most max_input_bytes
/// @param from_archive recorded in Document::from_archive
/// @param[out] error status, offset and block name of a failure; may be null
/// @return true when the text parsed
[[nodiscard]] bool parse_text(
    Document* document, const char* text, uint32_t length, bool from_archive, ParseError* error
) noexcept;

/// Blanks // and /* */ comments with spaces in place.
///
/// The newline ending a line comment survives; an unterminated block comment
/// blanks everything to the end.
///
/// @param[in,out] text NUL-terminated text
void strip_comments(char* text) noexcept;

/// Hashes a byte range the way block bodies are hashed.
///
/// @param data first byte
/// @param length bytes hashed; non-positive hashes nothing
/// @return byte lanes: sum, xor, sum of (index ^ byte) and xor of (index + byte),
///         lowest lane first, with index taken modulo 256
[[nodiscard]] uint32_t buffer_hash(const uint8_t* data, int32_t length) noexcept;

/// Compares two strings ignoring ASCII case, folding to lower case.
///
/// @param left first NUL-terminated string
/// @param right second NUL-terminated string
/// @return negative, zero or positive as left sorts before, with or after right
[[nodiscard]] int compare_nocase(const char* left, const char* right) noexcept;

/// Parses a TDF integer the way the game does.
///
/// @param text NUL-terminated value
/// @return the value; leading space and one sign are accepted and overflow wraps modulo 2^32
[[nodiscard]] int32_t parse_int(const char* text) noexcept;
/// Parses a TDF floating-point value the way the game does.
///
/// Only [sign] digits [. digits] [(e|E|d|D) [sign] digits] is accepted.
///
/// @param text NUL-terminated value
/// @return the value, or 0 when no digits are present
[[nodiscard]] double parse_double(const char* text) noexcept;

/// Finds the first child block whose name matches, ignoring ASCII case.
///
/// @param block parent block, or null
/// @param name child name
/// @return the child, or null
[[nodiscard]] const Block* find_child(const Block* block, const char* name) noexcept;
/// Looks up a field value, ignoring ASCII case.
///
/// @param block block searched, or null
/// @param key field name
/// @return the value of the newest case variant of key, or null when missing
[[nodiscard]] const char* find_value(const Block* block, const char* key) noexcept;
/// Reads an integer field (see parse_int).
///
/// @param block block searched, or null
/// @param key field name
/// @param fallback value returned when the field is missing
/// @return the value
[[nodiscard]] int32_t get_int(const Block* block, const char* key, int32_t fallback) noexcept;
/// Reads a floating-point field (see parse_double).
///
/// @param block block searched, or null
/// @param key field name
/// @param fallback value returned when the field is missing
/// @return the value
[[nodiscard]] double get_double(const Block* block, const char* key, double fallback) noexcept;
/// Reads a field as 16.16 fixed point.
///
/// @param block block searched, or null
/// @param key field name
/// @param fallback value returned as given, unscaled, when the field is missing
/// @return the low 32 bits of the value times 65536, truncated toward zero to 64 bits
[[nodiscard]] int32_t get_fixed(const Block* block, const char* key, int32_t fallback) noexcept;
/// Copies a string field into a buffer.
///
/// @param block block searched, or null
/// @param key field name
/// @param[out] out buffer; padded with NULs like strncpy and always terminated
/// @param size bytes of out; 0 copies nothing and returns false
/// @param fallback copied, bounded by size, when the field is missing, or null for none
/// @return true when the field was found
bool get_string(
    const Block* block, const char* key, char* out, std::size_t size, const char* fallback
) noexcept;

/// Returns a block's number of child blocks.
///
/// @param block block, or null
/// @return the count; 0 for null
[[nodiscard]] uint32_t child_count(const Block* block) noexcept;
/// Returns one child block by position.
///
/// @param block block, or null
/// @param index child position, in file order
/// @return the child, or null past the end
[[nodiscard]] const Block* child_at(const Block* block, uint32_t index) noexcept;
/// Returns a block's number of fields.
///
/// @param block block, or null
/// @return the count; 0 for null
[[nodiscard]] uint32_t property_count(const Block* block) noexcept;
/// Returns the key at a position in case-insensitive key order.
///
/// @param block block, or null
/// @param index property position
/// @return the key, or null when index is negative or past the end
[[nodiscard]] const char* property_key_at(const Block* block, int32_t index) noexcept;

/// Moves the cursor back to the root.
///
/// @param[in,out] document document whose cursor is cleared
void reset_cursor(Document* document) noexcept;
/// Returns the section the cursor is on.
///
/// @param document document
/// @return the block, or null at the root
[[nodiscard]] const Block* cursor(const Document* document) noexcept;
/// Moves the cursor to a block.
///
/// @param[in,out] document document whose cursor is set
/// @param block block of this document, or null for the root
void set_cursor(Document* document, const Block* block) noexcept;
/// Moves the cursor to a named child of the cursor (or of the root).
///
/// @param[in,out] document document whose cursor moves
/// @param name child name, matched ignoring ASCII case
/// @return true when found; a miss resets the cursor to the root
bool select_section(Document* document, const char* name) noexcept;
/// Moves the cursor to the index-th child of the cursor (or of the root).
///
/// @param[in,out] document document whose cursor moves
/// @param index child position, in file order
/// @return true when the child exists; a miss resets the cursor to the root
bool step_entry(Document* document, uint32_t index) noexcept;

/// A Document that frees its tree when it goes out of scope.
///
/// Moving one hands its tree over and leaves the source empty; blocks keep
/// their addresses across the move.
class OwnedDocument {
  public:

    /// Makes an empty document.
    OwnedDocument() noexcept;
    /// Frees the tree.
    ~OwnedDocument();
    /// Takes another document's tree.
    ///
    /// @param[in,out] other document left empty
    OwnedDocument(OwnedDocument&& other) noexcept;
    /// Frees this tree and takes another document's.
    ///
    /// @param[in,out] other document left empty
    /// @return this document
    OwnedDocument& operator=(OwnedDocument&& other) noexcept;
    OwnedDocument(const OwnedDocument&) = delete;
    OwnedDocument& operator=(const OwnedDocument&) = delete;

    /// Replaces the tree with a parse of a text, as parse_text does.
    ///
    /// @param text TDF text; one longer than max_input_bytes fails as too_large
    /// @param[out] error status, offset and block name of a failure; may be null
    /// @return true when the text parsed; on failure the document is empty
    [[nodiscard]] bool parse(std::string_view text, ParseError* error = nullptr) noexcept;

    /// Returns the root block, whose children are the top-level sections.
    ///
    /// @return the root, or null while empty
    [[nodiscard]] const Block* root() const noexcept { return document_.root; }

    /// Returns the document, for the cursor helpers.
    ///
    /// @return the document
    [[nodiscard]] Document* get() noexcept { return &document_; }

    /// Returns the document.
    ///
    /// @return the document
    [[nodiscard]] const Document* get() const noexcept { return &document_; }

  private:

    Document document_{};
};

} // namespace oa::formats::tdf
