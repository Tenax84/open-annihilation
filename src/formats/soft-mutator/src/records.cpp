// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/soft_mutator.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oa::formats::soft_mutator {
namespace {

[[nodiscard]] char ascii_lower(unsigned char c) noexcept {
    if (c >= 'A' && c <= 'Z')
        return static_cast<char>(c - 'A' + 'a');
    return static_cast<char>(c);
}

[[nodiscard]] std::string fold_ascii(std::string_view text) {
    std::string folded(text.size(), '\0');
    for (size_t index = 0; index < text.size(); ++index)
        folded[index] = ascii_lower(static_cast<unsigned char>(text[index]));
    return folded;
}

[[nodiscard]] bool same_folded(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(static_cast<unsigned char>(left[index])) !=
            ascii_lower(static_cast<unsigned char>(right[index])))
            return false;
    }
    return true;
}

struct NeedKey {
    std::string field;
    std::string text;
    NeedKind kind{};
    bool negated{};
    double low{};
    double high{};
};

[[nodiscard]] NeedKey need_key(const Need& need) {
    return NeedKey{
        fold_ascii(need.field), fold_ascii(need.text), need.kind, need.negated, need.low, need.high
    };
}

[[nodiscard]] bool operator==(const NeedKey& left, const NeedKey& right) noexcept {
    return left.field == right.field && left.text == right.text && left.kind == right.kind &&
           left.negated == right.negated && left.low == right.low && left.high == right.high;
}

[[nodiscard]] bool operator<(const NeedKey& left, const NeedKey& right) noexcept {
    if (left.field != right.field)
        return left.field < right.field;
    if (left.kind != right.kind)
        return static_cast<uint8_t>(left.kind) < static_cast<uint8_t>(right.kind);
    if (left.negated != right.negated)
        return !left.negated && right.negated;
    if (left.low != right.low)
        return left.low < right.low;
    if (left.high != right.high)
        return left.high < right.high;
    return left.text < right.text;
}

[[nodiscard]] bool same_needs(const std::vector<Need>& left, const std::vector<Need>& right) {
    if (left.size() != right.size())
        return false;
    std::vector<NeedKey> left_keys;
    std::vector<NeedKey> right_keys;
    left_keys.reserve(left.size());
    right_keys.reserve(right.size());
    for (const Need& need : left)
        left_keys.push_back(need_key(need));
    for (const Need& need : right)
        right_keys.push_back(need_key(need));
    std::sort(left_keys.begin(), left_keys.end());
    std::sort(right_keys.begin(), right_keys.end());
    return left_keys == right_keys;
}

[[nodiscard]] bool same_change(const Change& left, const Change& right) noexcept {
    return same_folded(left.field, right.field) && left.all_depth2 == right.all_depth2 &&
           left.kind == right.kind && left.value == right.value && left.low == right.low &&
           left.high == right.high && left.text == right.text;
}

[[nodiscard]] bool same_changes(const std::vector<Change>& left, const std::vector<Change>& right) {
    if (left.size() != right.size())
        return false;
    std::vector<const Change*> left_order;
    std::vector<const Change*> right_order;
    left_order.reserve(left.size());
    right_order.reserve(right.size());
    for (const Change& change : left)
        left_order.push_back(&change);
    for (const Change& change : right)
        right_order.push_back(&change);
    const auto by_field = [](const Change* first, const Change* second) {
        return fold_ascii(first->field) < fold_ascii(second->field);
    };
    std::stable_sort(left_order.begin(), left_order.end(), by_field);
    std::stable_sort(right_order.begin(), right_order.end(), by_field);
    for (size_t index = 0; index < left_order.size(); ++index) {
        if (!same_change(*left_order[index], *right_order[index]))
            return false;
    }
    return true;
}

} // namespace

bool ParseResult::has_errors() const noexcept {
    for (const Diagnostic& item : diagnostics) {
        if (item.severity == Severity::error)
            return true;
    }
    return false;
}

std::string_view code_name(DiagnosticCode code) noexcept {
    switch (code) {
    case DiagnosticCode::missing_semicolon:
        return "missing_semicolon";
    case DiagnosticCode::group_without_asterisks:
        return "group_without_asterisks";
    case DiagnosticCode::quoted_number:
        return "quoted_number";
    case DiagnosticCode::spaces_in_value:
        return "spaces_in_value";
    case DiagnosticCode::empty_range:
        return "empty_range";
    case DiagnosticCode::group_without_needs:
        return "group_without_needs";
    case DiagnosticCode::repeated_section:
        return "repeated_section";
    case DiagnosticCode::byte_order_mark:
        return "byte_order_mark";
    case DiagnosticCode::unquoted_text:
        return "unquoted_text";
    case DiagnosticCode::empty_value:
        return "empty_value";
    case DiagnosticCode::empty_text_need:
        return "empty_text_need";
    case DiagnosticCode::stray_closing_brace:
        return "stray_closing_brace";
    case DiagnosticCode::unknown_section:
        return "unknown_section";
    case DiagnosticCode::unclosed_block:
        return "unclosed_block";
    case DiagnosticCode::unexpected_brace:
        return "unexpected_brace";
    case DiagnosticCode::directive_in_block:
        return "directive_in_block";
    case DiagnosticCode::bad_number:
        return "bad_number";
    case DiagnosticCode::bad_need:
        return "bad_need";
    case DiagnosticCode::bad_change:
        return "bad_change";
    case DiagnosticCode::need_form_in_changes:
        return "need_form_in_changes";
    case DiagnosticCode::change_form_in_needs:
        return "change_form_in_needs";
    case DiagnosticCode::clamp_low_above_high:
        return "clamp_low_above_high";
    case DiagnosticCode::unterminated_string:
        return "unterminated_string";
    case DiagnosticCode::block_comment:
        return "block_comment";
    case DiagnosticCode::stray_text:
        return "stray_text";
    case DiagnosticCode::too_large:
        return "too_large";
    case DiagnosticCode::too_many_blocks:
        return "too_many_blocks";
    case DiagnosticCode::too_many_statements:
        return "too_many_statements";
    case DiagnosticCode::text_too_long:
        return "text_too_long";
    case DiagnosticCode::too_many_diagnostics:
        return "too_many_diagnostics";
    }
    return "unknown";
}

bool equivalent(const Block& left, const Block& right) noexcept {
    if (left.selector != right.selector || !(left.target == right.target))
        return false;
    if (!same_folded(left.name, right.name))
        return false;
    if (!same_needs(left.needs, right.needs))
        return false;
    return same_changes(left.changes, right.changes);
}

bool equivalent(const RuleFile& left, const RuleFile& right) noexcept {
    if (left.blocks.size() != right.blocks.size())
        return false;
    for (size_t index = 0; index < left.blocks.size(); ++index) {
        if (!equivalent(left.blocks[index], right.blocks[index]))
            return false;
    }
    return true;
}

} // namespace oa::formats::soft_mutator
