// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Reads a legacy rule file. Keywords and section names are matched without
// case. A number is accepted only as the grammar allows (no exponent, no
// leading '+'), and then read by the same reader the game uses for the value.

#include "oa/formats/soft_mutator.hpp"

#include "oa/formats/tdf.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::formats::soft_mutator {
namespace {

// parse_double keeps 511 characters of a token. A longer one is refused,
// so a number that is accepted is the number the grammar named.
constexpr size_t max_number_token = 511;

enum class Directive : uint8_t { folder, extension };
enum class Slot : uint8_t { loose, needs, changes };
enum class Term : uint8_t { semi, newline, brace, eof };
enum class NumberRead : uint8_t { absent, ok, bad };

struct NumberToken {
    NumberRead read{NumberRead::absent};
    size_t end{};
    double value{};
};

struct Raw {
    std::string text;
    Term term{Term::eof};
    bool unterminated{false};
};

struct BlockState {
    std::string header;
    bool saw_needs{false};
    bool saw_changes{false};
    bool promoted{false};
    bool unclosed_reported{false};
    uint32_t header_line{1};
    uint32_t statements{0};
};

[[nodiscard]] bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] bool is_horizontal(unsigned char c) noexcept {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

[[nodiscard]] bool is_ident(unsigned char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

[[nodiscard]] bool is_exponent_mark(char c) noexcept {
    return c == 'e' || c == 'E' || c == 'd' || c == 'D';
}

[[nodiscard]] char ascii_lower(unsigned char c) noexcept {
    if (c >= 'A' && c <= 'Z')
        return static_cast<char>(c - 'A' + 'a');
    return static_cast<char>(c);
}

[[nodiscard]] bool same_word(std::string_view text, std::string_view word) noexcept {
    if (text.size() != word.size())
        return false;
    for (size_t index = 0; index < text.size(); ++index) {
        if (ascii_lower(static_cast<unsigned char>(text[index])) !=
            ascii_lower(static_cast<unsigned char>(word[index])))
            return false;
    }
    return true;
}

void trim_ends(std::string& text) {
    size_t begin = 0;
    while (begin < text.size() && is_horizontal(static_cast<unsigned char>(text[begin])))
        ++begin;
    size_t end = text.size();
    while (end > begin && is_horizontal(static_cast<unsigned char>(text[end - 1])))
        --end;
    if (begin == 0 && end == text.size())
        return;
    text.erase(end);
    text.erase(0, begin);
}

void trim_trailing(std::string& text) {
    size_t end = text.size();
    while (end > 0 && is_horizontal(static_cast<unsigned char>(text[end - 1])))
        --end;
    text.erase(end);
}

[[nodiscard]] std::string quote(std::string_view text) {
    constexpr size_t shown = 80;
    const bool cut = text.size() > shown;
    if (cut)
        text.remove_suffix(text.size() - shown);
    std::string out;
    out.reserve(text.size() + 5);
    out.push_back('\'');
    out.append(text.data(), text.size());
    if (cut)
        out.append("...");
    out.push_back('\'');
    return out;
}

[[nodiscard]] std::string canonical_token(std::string text, bool extension) {
    for (char& c : text) {
        if (c == '\\')
            c = '/';
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte >= 'A' && byte <= 'Z')
            c = static_cast<char>(byte - 'A' + 'a');
    }
    if (extension && !text.empty() && text.front() == '.')
        text.erase(text.begin());
    return text;
}

[[nodiscard]] NumberToken read_number(std::string_view text, size_t start) {
    NumberToken token;
    token.end = start;
    if (start >= text.size())
        return token;
    if (text[start] == '+') {
        size_t cursor = start + 1;
        if (cursor < text.size() && (is_digit(text[cursor]) || text[cursor] == '.')) {
            ++cursor;
            while (cursor < text.size() &&
                   (is_digit(text[cursor]) || text[cursor] == '.' ||
                    is_exponent_mark(text[cursor]) || text[cursor] == '+' || text[cursor] == '-'))
                ++cursor;
            token.read = NumberRead::bad;
            token.end = cursor;
        }
        return token;
    }
    size_t cursor = start;
    if (cursor < text.size() && text[cursor] == '-')
        ++cursor;
    if (cursor < text.size() && text[cursor] == '.') {
        ++cursor;
        const size_t digit_at = cursor;
        while (cursor < text.size() && is_digit(text[cursor]))
            ++cursor;
        if (cursor == digit_at) {
            token.read = NumberRead::bad;
            token.end = cursor;
            return token;
        }
    } else if (cursor < text.size() && is_digit(text[cursor])) {
        while (cursor < text.size() && is_digit(text[cursor]))
            ++cursor;
        if (cursor < text.size() && text[cursor] == '.') {
            ++cursor;
            while (cursor < text.size() && is_digit(text[cursor]))
                ++cursor;
        }
    } else {
        return token;
    }
    if (cursor < text.size() && is_exponent_mark(text[cursor])) {
        ++cursor;
        if (cursor < text.size() && (text[cursor] == '+' || text[cursor] == '-'))
            ++cursor;
        while (cursor < text.size() && is_digit(text[cursor]))
            ++cursor;
        token.read = NumberRead::bad;
        token.end = cursor;
        return token;
    }
    if (cursor - start >= max_number_token) {
        token.read = NumberRead::bad;
        token.end = cursor;
        return token;
    }
    const std::string_view spelling = text.substr(start, cursor - start);
    const std::string terminated(spelling);
    token.value = tdf::parse_double(terminated.c_str());
    token.read = NumberRead::ok;
    token.end = cursor;
    return token;
}

[[nodiscard]] bool whole_number(std::string_view text) {
    const NumberToken token = read_number(text, 0);
    return token.read == NumberRead::ok && token.end == text.size();
}

// A change value that matched no number, scale, clamp, meta or quoted form,
// and does not start like one of those forms, is bare text. An empty value
// is included: it assigns empty text, which is not the quoted removal.
[[nodiscard]] bool bare_text_value(std::string_view text) noexcept {
    if (text.empty())
        return true;
    switch (text.front()) {
    case '"':
    case '*':
    case '^':
    case '!':
    case '(':
    case ',':
    case '-':
        return false;
    default:
        return true;
    }
}

struct Cleaned {
    std::string text;
    bool removed_space{false};
    bool unterminated{false};
};

[[nodiscard]] Cleaned clean_spaces(std::string_view value) {
    Cleaned cleaned;
    size_t index = 0;
    while (index < value.size()) {
        if (value[index] == '"') {
            const size_t end = value.find('"', index + 1);
            if (end == std::string_view::npos) {
                cleaned.unterminated = true;
                const std::string_view rest = value.substr(index);
                cleaned.text.append(rest.data(), rest.size());
                return cleaned;
            }
            const std::string_view piece = value.substr(index, end - index + 1);
            cleaned.text.append(piece.data(), piece.size());
            index = end + 1;
            continue;
        }
        const unsigned char byte = static_cast<unsigned char>(value[index]);
        if (is_horizontal(byte)) {
            cleaned.removed_space = true;
            ++index;
            continue;
        }
        cleaned.text.push_back(value[index]);
        ++index;
    }
    return cleaned;
}

class Parser {
  public:

    explicit Parser(std::span<const uint8_t> text) : text_(text) {}

    [[nodiscard]] ParseResult run() {
        if (text_.size() >= 3 && text_[0] == 0xEFu && text_[1] == 0xBBu && text_[2] == 0xBFu) {
            diagnose(Severity::warning, DiagnosticCode::byte_order_mark, 1, "byte order mark");
            index_ = 3;
        }
        while (!at_end()) {
            skip_ws();
            if (at_end())
                break;
            const int c = peek();
            if (c == '[') {
                parse_block();
                continue;
            }
            if (c == '}') {
                diagnose(
                    Severity::warning,
                    DiagnosticCode::stray_closing_brace,
                    line_,
                    "closing brace outside a block"
                );
                advance();
                continue;
            }
            if (c == '{') {
                diagnose(
                    Severity::error, DiagnosticCode::unexpected_brace, line_, "unexpected brace"
                );
                advance();
                continue;
            }
            if (const std::optional<Directive> directive = directive_here()) {
                parse_directive(*directive);
                continue;
            }
            diagnose(Severity::error, DiagnosticCode::stray_text, line_, "stray text");
            skip_stray();
        }
        return std::move(result_);
    }

  private:

    std::span<const uint8_t> text_;
    size_t index_{0};
    uint32_t line_{1};
    bool sealed_{false};
    std::string folder_{"units"};
    std::string extension_{"fbi"};
    ParseResult result_;
    Block* block_{nullptr};
    BlockState* state_{nullptr};

    [[nodiscard]] bool at_end() const noexcept { return index_ >= text_.size(); }

    [[nodiscard]] int peek() const noexcept {
        if (at_end())
            return -1;
        return static_cast<int>(text_[index_]);
    }

    [[nodiscard]] int peek_at(size_t delta) const noexcept {
        if (index_ + delta >= text_.size())
            return -1;
        return static_cast<int>(text_[index_ + delta]);
    }

    void advance() noexcept {
        if (at_end())
            return;
        const uint8_t c = text_[index_++];
        if (c == '\n') {
            if (line_ < UINT32_MAX)
                ++line_;
        } else if (c == '\r') {
            if (line_ < UINT32_MAX)
                ++line_;
            if (!at_end() && text_[index_] == '\n')
                ++index_;
        }
    }

    void skip_horizontal() noexcept {
        while (!at_end() && is_horizontal(text_[index_]))
            ++index_;
    }

    void skip_ws() {
        while (!at_end()) {
            const int c = peek();
            if (c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\n' || c == '\r') {
                advance();
                continue;
            }
            if (c == '/' && peek_at(1) == '/') {
                advance();
                advance();
                while (!at_end() && peek() != '\n' && peek() != '\r')
                    advance();
                continue;
            }
            if (c == '/' && peek_at(1) == '*') {
                diagnose(
                    Severity::error,
                    DiagnosticCode::block_comment,
                    line_,
                    "block comment is not allowed"
                );
                advance();
                advance();
                continue;
            }
            break;
        }
    }

    void skip_stray() {
        while (!at_end()) {
            const int c = peek();
            if (c == '\n' || c == '\r' || c == '[' || c == '{' || c == '}')
                break;
            advance();
        }
        if (!at_end() && (peek() == '\n' || peek() == '\r'))
            advance();
    }

    void diagnose(Severity severity, DiagnosticCode code, uint32_t line, std::string message) {
        if (sealed_)
            return;
        if (result_.diagnostics.size() >= max_diagnostics) {
            Diagnostic stop;
            stop.severity = Severity::error;
            stop.code = DiagnosticCode::too_many_diagnostics;
            stop.line = line;
            stop.message = "too many diagnostics";
            result_.diagnostics.back() = std::move(stop);
            sealed_ = true;
            return;
        }
        Diagnostic item;
        item.severity = severity;
        item.code = code;
        item.line = line;
        item.message = std::move(message);
        result_.diagnostics.push_back(std::move(item));
    }

    void report_unclosed(uint32_t line) {
        if (state_ != nullptr && state_->unclosed_reported)
            return;
        if (state_ != nullptr)
            state_->unclosed_reported = true;
        diagnose(Severity::error, DiagnosticCode::unclosed_block, line, "block is not closed");
    }

    [[nodiscard]] bool keyword_at(const char* word, size_t length) const noexcept {
        if (index_ + length > text_.size())
            return false;
        for (size_t offset = 0; offset < length; ++offset) {
            if (ascii_lower(text_[index_ + offset]) !=
                ascii_lower(static_cast<unsigned char>(word[offset])))
                return false;
        }
        if (index_ + length < text_.size() && is_ident(text_[index_ + length]))
            return false;
        return true;
    }

    [[nodiscard]] std::optional<Directive> directive_here() const noexcept {
        const char* folder_word = "folder";
        const char* extension_word = "extension";
        const bool folder = keyword_at(folder_word, 6);
        const bool extension = keyword_at(extension_word, 9);
        if (!folder && !extension)
            return std::nullopt;
        const size_t length = folder ? size_t{6} : size_t{9};
        size_t cursor = index_ + length;
        while (cursor < text_.size() && is_horizontal(text_[cursor]))
            ++cursor;
        if (cursor >= text_.size() || text_[cursor] != static_cast<uint8_t>('='))
            return std::nullopt;
        return folder ? Directive::folder : Directive::extension;
    }

    void parse_directive(Directive directive) {
        const uint32_t directive_line = line_;
        const bool folder = directive == Directive::folder;
        const size_t length = folder ? size_t{6} : size_t{9};
        index_ += length;
        skip_horizontal();
        if (!at_end() && peek() == '=')
            advance();
        skip_horizontal();
        std::string text;
        Term term = Term::eof;
        while (!at_end()) {
            const int c = peek();
            if (c == '/' && peek_at(1) == '/') {
                term = Term::newline;
                advance();
                advance();
                while (!at_end() && peek() != '\n' && peek() != '\r')
                    advance();
                break;
            }
            if (c == ';') {
                term = Term::semi;
                break;
            }
            if (c == '\n' || c == '\r') {
                term = Term::newline;
                break;
            }
            if (c == '[' || c == '{' || c == '}') {
                term = Term::brace;
                break;
            }
            text.push_back(static_cast<char>(c));
            advance();
        }
        trim_ends(text);
        if (term == Term::semi) {
            advance();
        } else {
            if (term == Term::newline && !at_end() && (peek() == '\n' || peek() == '\r'))
                advance();
            diagnose(
                Severity::warning,
                DiagnosticCode::missing_semicolon,
                directive_line,
                "missing ';' after " + quote(folder ? "folder" : "extension")
            );
        }
        std::string canonical = canonical_token(std::move(text), !folder);
        if (folder)
            folder_ = std::move(canonical);
        else
            extension_ = std::move(canonical);
    }

    void skip_braced() {
        if (at_end() || peek() != '{')
            return;
        int depth = 0;
        bool in_string = false;
        while (!at_end()) {
            const int c = peek();
            if (in_string) {
                if (c == '"' || c == '\n' || c == '\r')
                    in_string = false;
                advance();
                continue;
            }
            if (c == '"') {
                in_string = true;
                advance();
                continue;
            }
            if (c == '/' && peek_at(1) == '/') {
                advance();
                advance();
                while (!at_end() && peek() != '\n' && peek() != '\r')
                    advance();
                continue;
            }
            if (c == '{') {
                ++depth;
            } else if (c == '}') {
                --depth;
                advance();
                if (depth == 0)
                    return;
                continue;
            }
            advance();
        }
    }

    void parse_block() {
        const uint32_t header_line = line_;
        advance();
        std::string header;
        bool header_closed = false;
        while (!at_end()) {
            const int c = peek();
            if (c == ']') {
                header_closed = true;
                advance();
                break;
            }
            if (c == '\n' || c == '\r')
                break;
            header.push_back(static_cast<char>(c));
            advance();
        }
        trim_ends(header);
        if (!header_closed) {
            diagnose(
                Severity::error, DiagnosticCode::unclosed_block, header_line, "block is not closed"
            );
            if (!at_end() && (peek() == '\n' || peek() == '\r'))
                advance();
            return;
        }
        if (header.size() > max_text_bytes) {
            diagnose(
                Severity::error,
                DiagnosticCode::text_too_long,
                header_line,
                "text is longer than 1024 bytes"
            );
            skip_ws();
            if (!at_end() && peek() == '{')
                skip_braced();
            return;
        }
        Block block;
        BlockState state;
        state.header = header;
        state.header_line = header_line;
        block.target.folder = folder_;
        block.target.extension = extension_;
        block.line = header_line;
        block.selector = Selector::name;
        block.name = header;
        if (same_word(header, "*all*")) {
            block.selector = Selector::all;
            block.name.clear();
        } else if (same_word(header, "*group*")) {
            block.selector = Selector::group;
            block.name.clear();
        }
        block_ = &block;
        state_ = &state;
        skip_ws();
        if (at_end() || peek() != '{') {
            report_unclosed(header_line);
            block_ = nullptr;
            state_ = nullptr;
            return;
        }
        advance();
        const bool closed = parse_region(Slot::loose);
        if (!closed)
            report_unclosed(header_line);
        const bool group_without_needs = block.selector == Selector::group && !state.saw_needs;
        block_ = nullptr;
        state_ = nullptr;
        if (!closed)
            return;
        if (group_without_needs) {
            diagnose(
                Severity::warning,
                DiagnosticCode::group_without_needs,
                header_line,
                "group has no needs, so it changes every record"
            );
        }
        if (result_.rules.blocks.size() >= max_blocks) {
            diagnose(
                Severity::error, DiagnosticCode::too_many_blocks, header_line, "too many blocks"
            );
            return;
        }
        result_.rules.blocks.push_back(std::move(block));
    }

    [[nodiscard]] bool parse_region(Slot slot) {
        while (!at_end()) {
            skip_ws();
            if (at_end())
                return false;
            const int c = peek();
            if (c == '}') {
                advance();
                return true;
            }
            if (c == '{') {
                diagnose(
                    Severity::error, DiagnosticCode::unexpected_brace, line_, "unexpected brace"
                );
                advance();
                continue;
            }
            if (c == '[') {
                if (slot != Slot::loose) {
                    reject_nested_section();
                    continue;
                }
                if (!parse_section())
                    return false;
                continue;
            }
            if (c == 0) {
                diagnose(Severity::error, DiagnosticCode::stray_text, line_, "stray text");
                advance();
                continue;
            }
            parse_statement(slot);
        }
        return false;
    }

    [[nodiscard]] bool parse_section() {
        const uint32_t section_line = line_;
        advance();
        std::string name;
        bool closed = false;
        while (!at_end()) {
            const int c = peek();
            if (c == ']') {
                closed = true;
                advance();
                break;
            }
            if (c == '\n' || c == '\r')
                break;
            name.push_back(static_cast<char>(c));
            advance();
        }
        trim_ends(name);
        if (!closed) {
            report_unclosed(section_line);
            if (!at_end() && (peek() == '\n' || peek() == '\r'))
                advance();
            return true;
        }
        if (same_word(name, "needs") || same_word(name, "changes")) {
            const bool needs = same_word(name, "needs");
            if (block_->selector != Selector::group) {
                drop_loose_changes();
                block_->selector = Selector::group;
                block_->name.clear();
                if (!state_->promoted) {
                    diagnose(
                        Severity::warning,
                        DiagnosticCode::group_without_asterisks,
                        state_->header_line,
                        "header " + quote(state_->header) + " is read as a group"
                    );
                    state_->promoted = true;
                }
            }
            if (needs && state_->saw_needs) {
                diagnose(
                    Severity::warning,
                    DiagnosticCode::repeated_section,
                    section_line,
                    "repeated section " + quote(name)
                );
            }
            if (!needs && state_->saw_changes) {
                diagnose(
                    Severity::warning,
                    DiagnosticCode::repeated_section,
                    section_line,
                    "repeated section " + quote(name)
                );
            }
            if (needs)
                state_->saw_needs = true;
            else
                state_->saw_changes = true;
            skip_ws();
            if (at_end() || peek() != '{') {
                report_unclosed(section_line);
                return true;
            }
            advance();
            return parse_region(needs ? Slot::needs : Slot::changes);
        }
        diagnose(
            Severity::error,
            DiagnosticCode::unknown_section,
            section_line,
            "unknown section " + quote(name)
        );
        skip_ws();
        if (!at_end() && peek() == '{')
            skip_braced();
        return true;
    }

    void reject_nested_section() {
        const uint32_t section_line = line_;
        diagnose(
            Severity::error,
            DiagnosticCode::unknown_section,
            section_line,
            "a section cannot hold a section"
        );
        advance();
        while (!at_end()) {
            const int c = peek();
            if (c == ']') {
                advance();
                break;
            }
            if (c == '\n' || c == '\r')
                break;
            advance();
        }
        skip_ws();
        if (!at_end() && peek() == '{')
            skip_braced();
    }

    void drop_loose_changes() {
        if (block_ == nullptr)
            return;
        for (const Change& change : block_->changes) {
            diagnose(
                Severity::error,
                DiagnosticCode::stray_text,
                change.line,
                "stray text " + quote(change.field)
            );
        }
        block_->changes.clear();
    }

    void parse_statement(Slot slot) {
        const uint32_t statement_line = line_;
        std::string field;
        while (!at_end()) {
            const int c = peek();
            if (c == '=' || c == ';' || c == '\n' || c == '\r' || c == '}' || c == '{' || c == '[')
                break;
            if (c == '/' && (peek_at(1) == '/' || peek_at(1) == '*'))
                break;
            field.push_back(static_cast<char>(c));
            advance();
        }
        trim_ends(field);
        if (at_end() || peek() != '=') {
            diagnose(Severity::error, DiagnosticCode::stray_text, statement_line, "stray text");
            if (!at_end() && (peek() == ';' || peek() == '\n' || peek() == '\r'))
                advance();
            return;
        }
        advance();
        const Raw raw = read_raw();
        if (state_->statements >= max_statements) {
            diagnose(
                Severity::error,
                DiagnosticCode::too_many_statements,
                statement_line,
                "too many statements"
            );
            return;
        }
        ++state_->statements;
        if (slot == Slot::loose && block_->selector == Selector::group) {
            diagnose(
                Severity::error,
                DiagnosticCode::stray_text,
                statement_line,
                field.empty() ? "stray text" : "stray text " + quote(field)
            );
            return;
        }
        if (field.empty()) {
            diagnose(Severity::error, DiagnosticCode::stray_text, statement_line, "stray text");
            return;
        }
        if (field.size() > max_text_bytes) {
            diagnose(
                Severity::error,
                DiagnosticCode::text_too_long,
                statement_line,
                "text is longer than 1024 bytes"
            );
            return;
        }
        if (same_word(field, "folder") || same_word(field, "extension")) {
            diagnose(
                Severity::error,
                DiagnosticCode::directive_in_block,
                statement_line,
                "directive " + quote(field) + " inside a block"
            );
            return;
        }
        if (raw.unterminated) {
            diagnose(
                Severity::error,
                DiagnosticCode::unterminated_string,
                statement_line,
                "unterminated string in " + quote(field)
            );
            return;
        }
        const Cleaned cleaned = clean_spaces(raw.text);
        if (cleaned.unterminated) {
            diagnose(
                Severity::error,
                DiagnosticCode::unterminated_string,
                statement_line,
                "unterminated string in " + quote(field)
            );
            return;
        }
        const bool all_depth = same_word(field, "<alldepth2>");
        const bool kept =
            slot == Slot::needs
                ? classify_need(field, cleaned.text, cleaned.removed_space, statement_line)
                : classify_change(
                      field,
                      cleaned.text,
                      raw.text,
                      cleaned.removed_space,
                      statement_line,
                      all_depth
                  );
        if (kept && raw.term != Term::semi) {
            diagnose(
                Severity::warning,
                DiagnosticCode::missing_semicolon,
                statement_line,
                "missing ';' after " + quote(field)
            );
        }
    }

    [[nodiscard]] Raw read_raw() {
        skip_horizontal();
        Raw raw;
        bool in_string = false;
        while (!at_end()) {
            const int c = peek();
            if (!in_string && c == '/' && peek_at(1) == '/') {
                trim_trailing(raw.text);
                advance();
                advance();
                while (!at_end() && peek() != '\n' && peek() != '\r')
                    advance();
                if (!at_end())
                    advance();
                raw.term = Term::newline;
                return raw;
            }
            if (!in_string && (c == '\n' || c == '\r')) {
                trim_trailing(raw.text);
                advance();
                raw.term = Term::newline;
                return raw;
            }
            if (!in_string && c == ';') {
                trim_trailing(raw.text);
                advance();
                raw.term = Term::semi;
                return raw;
            }
            if (!in_string && c == '}') {
                trim_trailing(raw.text);
                raw.term = Term::brace;
                return raw;
            }
            if (c == '"') {
                in_string = !in_string;
                raw.text.push_back('"');
                advance();
                continue;
            }
            if (in_string && (c == '\n' || c == '\r')) {
                raw.unterminated = true;
                advance();
                raw.term = Term::newline;
                return raw;
            }
            raw.text.push_back(static_cast<char>(c));
            advance();
        }
        if (in_string)
            raw.unterminated = true;
        trim_trailing(raw.text);
        raw.term = Term::eof;
        return raw;
    }

    [[nodiscard]] bool
    classify_need(const std::string& field, const std::string& value, bool spaced, uint32_t line) {
        if (same_word(field, "<alldepth2>")) {
            diagnose(
                Severity::error,
                DiagnosticCode::change_form_in_needs,
                line,
                "change form in needs for " + quote(field)
            );
            return false;
        }
        bool negated = false;
        std::string_view body = value;
        if (!body.empty() && body.front() == '!') {
            negated = true;
            body.remove_prefix(1);
        }
        if (!body.empty() && (body.front() == '*' || body.front() == '^')) {
            diagnose(
                Severity::error,
                DiagnosticCode::change_form_in_needs,
                line,
                "change form in needs for " + quote(field)
            );
            return false;
        }
        if (!body.empty() && body.front() == '"') {
            const size_t end = body.find('"', 1);
            if (end == std::string_view::npos) {
                diagnose(
                    Severity::error,
                    DiagnosticCode::unterminated_string,
                    line,
                    "unterminated string in " + quote(field)
                );
                return false;
            }
            const std::string_view inside = body.substr(1, end - 1);
            const std::string_view rest = body.substr(end + 1);
            if (inside.size() > max_text_bytes) {
                diagnose(
                    Severity::error,
                    DiagnosticCode::text_too_long,
                    line,
                    "text is longer than 1024 bytes"
                );
                return false;
            }
            if (rest == "+") {
                diagnose(
                    Severity::error,
                    DiagnosticCode::change_form_in_needs,
                    line,
                    "change form in needs for " + quote(field)
                );
                return false;
            }
            if (rest.empty() && inside.empty()) {
                diagnose(
                    Severity::warning,
                    DiagnosticCode::empty_text_need,
                    line,
                    "empty text in the need " + quote(field)
                );
                keep_need(field, NeedKind::equals, negated, std::string(), 0, 0, spaced, line);
                return true;
            }
            NeedKind kind = NeedKind::equals;
            if (rest == "*") {
                kind = NeedKind::contains;
            } else if (!rest.empty()) {
                diagnose(
                    Severity::error, DiagnosticCode::bad_need, line, "bad need " + quote(field)
                );
                return false;
            }
            keep_need(field, kind, negated, std::string(inside), 0, 0, spaced, line);
            return true;
        }
        const NumberToken low = read_number(body, 0);
        if (low.read == NumberRead::bad) {
            diagnose(
                Severity::error, DiagnosticCode::bad_number, line, "bad number in " + quote(field)
            );
            return false;
        }
        if (low.read == NumberRead::ok && low.end < body.size() && body[low.end] == ',') {
            const NumberToken high = read_number(body, low.end + 1);
            if (high.read == NumberRead::bad) {
                diagnose(
                    Severity::error,
                    DiagnosticCode::bad_number,
                    line,
                    "bad number in " + quote(field)
                );
                return false;
            }
            if (high.read == NumberRead::ok && high.end == body.size()) {
                keep_need(
                    field,
                    NeedKind::range,
                    negated,
                    std::string(),
                    low.value,
                    high.value,
                    spaced,
                    line
                );
                return true;
            }
        }
        diagnose(Severity::error, DiagnosticCode::bad_need, line, "bad need " + quote(field));
        return false;
    }

    [[nodiscard]] bool classify_change(
        const std::string& field,
        const std::string& value,
        const std::string& written,
        bool spaced,
        uint32_t line,
        bool all_depth
    ) {
        if (!value.empty() && value.front() == '!') {
            diagnose(
                Severity::error,
                DiagnosticCode::need_form_in_changes,
                line,
                "need form in changes for " + quote(field)
            );
            return false;
        }
        if (!value.empty() && value.front() == '"') {
            const size_t end = value.find('"', 1);
            if (end == std::string::npos) {
                diagnose(
                    Severity::error,
                    DiagnosticCode::unterminated_string,
                    line,
                    "unterminated string in " + quote(field)
                );
                return false;
            }
            const std::string inside = value.substr(1, end - 1);
            const std::string rest = value.substr(end + 1);
            if (inside.size() > max_text_bytes) {
                diagnose(
                    Severity::error,
                    DiagnosticCode::text_too_long,
                    line,
                    "text is longer than 1024 bytes"
                );
                return false;
            }
            if (rest == "*") {
                diagnose(
                    Severity::error,
                    DiagnosticCode::need_form_in_changes,
                    line,
                    "need form in changes for " + quote(field)
                );
                return false;
            }
            if (rest == "+") {
                keep_change(
                    field, ChangeKind::append, 0, 0, 0, inside, spaced, false, line, all_depth
                );
                return true;
            }
            if (!rest.empty()) {
                diagnose(
                    Severity::error, DiagnosticCode::bad_change, line, "bad change " + quote(field)
                );
                return false;
            }
            if (inside.empty()) {
                keep_change(
                    field,
                    ChangeKind::remove,
                    0,
                    0,
                    0,
                    std::string(),
                    spaced,
                    false,
                    line,
                    all_depth
                );
                return true;
            }
            const bool quoted = whole_number(inside);
            keep_change(
                field, ChangeKind::assign_text, 0, 0, 0, inside, spaced, quoted, line, all_depth
            );
            return true;
        }
        if (!value.empty() && value.front() == '*') {
            const NumberToken factor = read_number(value, 1);
            if (factor.read != NumberRead::ok) {
                const bool numeric = factor.read == NumberRead::bad ||
                                     (value.size() > 1 && (is_digit(value[1]) || value[1] == '+' ||
                                                           value[1] == '-' || value[1] == '.'));
                diagnose(
                    Severity::error,
                    numeric ? DiagnosticCode::bad_number : DiagnosticCode::bad_change,
                    line,
                    (numeric ? "bad number in " : "bad change ") + quote(field)
                );
                return false;
            }
            if (factor.end == value.size()) {
                keep_change(
                    field,
                    ChangeKind::scale,
                    factor.value,
                    0,
                    0,
                    std::string(),
                    spaced,
                    false,
                    line,
                    all_depth
                );
                return true;
            }
            if (value[factor.end] == '(') {
                const NumberToken low = read_number(value, factor.end + 1);
                if (low.read != NumberRead::ok || low.end >= value.size() ||
                    value[low.end] != ',') {
                    diagnose(
                        Severity::error,
                        low.read == NumberRead::bad ? DiagnosticCode::bad_number
                                                    : DiagnosticCode::bad_change,
                        line,
                        low.read == NumberRead::bad ? "bad number in " + quote(field)
                                                    : "bad change " + quote(field)
                    );
                    return false;
                }
                const NumberToken high = read_number(value, low.end + 1);
                if (high.read != NumberRead::ok || high.end >= value.size() ||
                    value[high.end] != ')' || high.end + 1 != value.size()) {
                    diagnose(
                        Severity::error,
                        high.read == NumberRead::bad ? DiagnosticCode::bad_number
                                                     : DiagnosticCode::bad_change,
                        line,
                        high.read == NumberRead::bad ? "bad number in " + quote(field)
                                                     : "bad change " + quote(field)
                    );
                    return false;
                }
                if (low.value > high.value) {
                    diagnose(
                        Severity::error,
                        DiagnosticCode::clamp_low_above_high,
                        line,
                        "clamp of " + quote(field) + " has its low above its high"
                    );
                    return false;
                }
                keep_change(
                    field,
                    ChangeKind::scale_clamp,
                    factor.value,
                    low.value,
                    high.value,
                    std::string(),
                    spaced,
                    false,
                    line,
                    all_depth
                );
                return true;
            }
            diagnose(
                Severity::error, DiagnosticCode::bad_change, line, "bad change " + quote(field)
            );
            return false;
        }
        if (!value.empty() && value.front() == '^') {
            const NumberToken meta = read_number(value, 1);
            if (meta.read == NumberRead::ok && meta.end == value.size()) {
                keep_change(
                    field,
                    ChangeKind::meta,
                    meta.value,
                    0,
                    0,
                    std::string(),
                    spaced,
                    false,
                    line,
                    all_depth
                );
                return true;
            }
            diagnose(
                Severity::error,
                meta.read == NumberRead::bad ? DiagnosticCode::bad_number
                                             : DiagnosticCode::bad_change,
                line,
                meta.read == NumberRead::bad ? "bad number in " + quote(field)
                                             : "bad change " + quote(field)
            );
            return false;
        }
        const NumberToken first = read_number(value, 0);
        if (first.read == NumberRead::bad) {
            diagnose(
                Severity::error, DiagnosticCode::bad_number, line, "bad number in " + quote(field)
            );
            return false;
        }
        if (first.read == NumberRead::ok && first.end < value.size() && value[first.end] == ',') {
            const NumberToken second = read_number(value, first.end + 1);
            if (second.read == NumberRead::bad) {
                diagnose(
                    Severity::error,
                    DiagnosticCode::bad_number,
                    line,
                    "bad number in " + quote(field)
                );
                return false;
            }
            if (second.read == NumberRead::ok && second.end == value.size()) {
                diagnose(
                    Severity::error,
                    DiagnosticCode::need_form_in_changes,
                    line,
                    "need form in changes for " + quote(field)
                );
                return false;
            }
        }
        if (first.read == NumberRead::ok && first.end == value.size()) {
            keep_change(
                field,
                ChangeKind::assign,
                first.value,
                0,
                0,
                std::string(),
                spaced,
                false,
                line,
                all_depth
            );
            return true;
        }
        if (first.read == NumberRead::ok) {
            diagnose(
                Severity::error, DiagnosticCode::bad_number, line, "bad number in " + quote(field)
            );
            return false;
        }
        // The forms above are tried on the value with its spaces removed.
        // What remains, when it does not start like those forms, is the text
        // as written: inner spaces kept, ends already trimmed.
        if (bare_text_value(written)) {
            if (written.size() > max_text_bytes) {
                diagnose(
                    Severity::error,
                    DiagnosticCode::text_too_long,
                    line,
                    "text is longer than 1024 bytes"
                );
                return false;
            }
            if (written.empty()) {
                diagnose(
                    Severity::warning,
                    DiagnosticCode::empty_value,
                    line,
                    "empty value for " + quote(field)
                );
            } else {
                diagnose(
                    Severity::warning,
                    DiagnosticCode::unquoted_text,
                    line,
                    "unquoted text for " + quote(field)
                );
            }
            keep_change(
                field, ChangeKind::assign_text, 0, 0, 0, written, false, false, line, all_depth
            );
            return true;
        }
        diagnose(Severity::error, DiagnosticCode::bad_change, line, "bad change " + quote(field));
        return false;
    }

    void keep_need(
        const std::string& field,
        NeedKind kind,
        bool negated,
        std::string text,
        double low,
        double high,
        bool spaced,
        uint32_t line
    ) {
        if (spaced) {
            diagnose(
                Severity::warning,
                DiagnosticCode::spaces_in_value,
                line,
                "spaces in the value of " + quote(field)
            );
        }
        if (kind == NeedKind::range && low > high) {
            diagnose(
                Severity::warning,
                DiagnosticCode::empty_range,
                line,
                "range of " + quote(field) + " is empty"
            );
        }
        Need need;
        need.field = field;
        need.kind = kind;
        need.negated = negated;
        need.low = low;
        need.high = high;
        need.text = std::move(text);
        need.line = line;
        block_->needs.push_back(std::move(need));
    }

    void keep_change(
        const std::string& field,
        ChangeKind kind,
        double value,
        double low,
        double high,
        std::string text,
        bool spaced,
        bool quoted,
        uint32_t line,
        bool all_depth
    ) {
        if (spaced) {
            diagnose(
                Severity::warning,
                DiagnosticCode::spaces_in_value,
                line,
                "spaces in the value of " + quote(field)
            );
        }
        if (quoted) {
            diagnose(
                Severity::warning,
                DiagnosticCode::quoted_number,
                line,
                "quoted number for " + quote(field)
            );
        }
        Change change;
        change.field = field;
        change.all_depth2 = all_depth;
        change.kind = kind;
        change.value = value;
        change.low = low;
        change.high = high;
        change.text = std::move(text);
        change.line = line;
        block_->changes.push_back(std::move(change));
    }
};

} // namespace

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
    Parser parser(text);
    return parser.run();
}

} // namespace oa::formats::soft_mutator
