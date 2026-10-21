// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Clash mode, the private name a clashing record takes, and the comparison
// that decides a record can be shared.

#include "oa/data/mutators/clashes.hpp"

#include "oa/formats/tdf.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace oa::data::mutators {
namespace tdf = oa::formats::tdf;
namespace oamut = oa::formats::oamut;

namespace {

/// How many private suffixes are tried before the name is given up.
constexpr int k_rename_attempts = 100000;

/// Folds ASCII letters to lower case. Other bytes stay as they are.
///
/// @param text the text
/// @return the folded text
std::string fold_ascii(std::string_view text) {
    std::string folded;
    folded.resize(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        const unsigned char lower =
            byte >= 'A' && byte <= 'Z' ? static_cast<unsigned char>(byte - 'A' + 'a') : byte;
        folded[index] = static_cast<char>(lower);
    }
    return folded;
}

/// One field, for an order-independent comparison.
struct FoldedField {
    /// The key, folded. Keys match without ASCII case.
    std::string key;
    /// The value, byte for byte. A missing value is empty.
    std::string value;

    /// Orders by key, then value.
    ///
    /// @param other the other field
    /// @return true when this field sorts first
    bool operator<(const FoldedField& other) const {
        if (key != other.key)
            return key < other.key;
        return value < other.value;
    }

    /// Reports whether both fields are the same key and value.
    ///
    /// @param other the other field
    /// @return true when the folded key and value match
    bool operator==(const FoldedField& other) const {
        return key == other.key && value == other.value;
    }
};

/// One child kept with the name it matches under.
struct FoldedChild {
    /// The section name, folded.
    std::string name;
    /// The child block.
    const tdf::Block* block{};
};

/// The fields of a block, with a weapon's top-level ID left out.
///
/// Keys are folded. Values stay as stored. `map` rewrites a value when set.
///
/// @param block the block
/// @param skip_id true when a top-level weapon ID is not part of the record
/// @param map optional rewrite of each value; null keeps the stored text
/// @return the fields, sorted by folded key then value bytes
std::vector<FoldedField>
folded_fields(const tdf::Block* block, bool skip_id, const RecordFieldMap* map) {
    std::vector<FoldedField> fields;
    if (block == nullptr || block->properties == nullptr)
        return fields;
    fields.reserve(block->property_count);
    for (uint32_t index = 0; index < block->property_count; ++index) {
        const tdf::Property& property = block->properties[index];
        if (property.key == nullptr)
            continue;
        if (skip_id && tdf::compare_nocase(property.key, "ID") == 0)
            continue;
        FoldedField field;
        field.key = fold_ascii(property.key);
        const std::string_view stored = property.value != nullptr ? property.value : "";
        if (map != nullptr)
            field.value = (*map)(property.key, stored);
        else
            field.value.assign(stored);
        fields.push_back(std::move(field));
    }
    std::sort(fields.begin(), fields.end());
    return fields;
}

/// The named children of a block, in file order within one folded name.
///
/// @param block the block
/// @return the children, sorted by folded name and stable for one name
std::vector<FoldedChild> folded_children(const tdf::Block* block) {
    std::vector<FoldedChild> children;
    const uint32_t count = tdf::child_count(block);
    children.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        const tdf::Block* child = tdf::child_at(block, index);
        if (child == nullptr || child->name == nullptr)
            continue;
        FoldedChild held;
        held.name = fold_ascii(child->name);
        held.block = child;
        children.push_back(std::move(held));
    }
    std::stable_sort(
        children.begin(), children.end(), [](const FoldedChild& left, const FoldedChild& right) {
            return left.name < right.name;
        }
    );
    return children;
}

} // namespace

oamut::ClashMode effective_clash(
    std::optional<oamut::ClashMode> player,
    std::optional<oamut::ClashMode> manifest,
    oamut::ClashMode setting
) noexcept {
    if (player.has_value())
        return *player;
    if (manifest.has_value())
        return *manifest;
    return setting;
}

oamut::ClashMode clash_from_setting(std::string_view text) noexcept {
    const std::optional<oamut::ClashMode> parsed = oamut::parse_clash_mode(text);
    if (!parsed.has_value())
        return clash_setting_default;
    return *parsed;
}

bool record_name_ok(std::string_view name) noexcept {
    if (name.empty() || name.size() > record_name_limit)
        return false;
    for (const char character : name) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte == '/' || byte == '\\' || byte == ':' || byte == '.')
            return false;
    }
    return true;
}

std::optional<std::string>
private_record_name(std::string_view name, const std::function<bool(std::string_view)>& taken) {
    for (int number = 2; number <= k_rename_attempts; ++number) {
        const std::string suffix = std::string{"_"} + std::to_string(number);
        if (suffix.size() > record_name_limit)
            return std::nullopt;
        const std::size_t keep = record_name_limit - suffix.size();
        const std::size_t stem = name.size() < keep ? name.size() : keep;
        std::string candidate;
        candidate.reserve(stem + suffix.size());
        candidate.append(name.data(), stem);
        candidate.append(suffix);
        if (!record_name_ok(candidate))
            continue;
        if (!taken(candidate))
            return candidate;
    }
    return std::nullopt;
}

bool records_identical(
    const tdf::Block* left, const tdf::Block* right, bool weapon, const RecordFieldMap* map_right
) {
    if (left == nullptr || right == nullptr)
        return left == right;
    if (folded_fields(left, weapon, nullptr) != folded_fields(right, weapon, map_right))
        return false;
    const std::vector<FoldedChild> left_children = folded_children(left);
    const std::vector<FoldedChild> right_children = folded_children(right);
    if (left_children.size() != right_children.size())
        return false;
    for (std::size_t index = 0; index < left_children.size(); ++index) {
        if (left_children[index].name != right_children[index].name)
            return false;
        if (!records_identical(
                left_children[index].block, right_children[index].block, false, map_right
            ))
            return false;
    }
    return true;
}

bool weapon_reference_key(std::string_view key) noexcept {
    const std::string folded = fold_ascii(key);
    return folded == "weapon1" || folded == "weapon2" || folded == "weapon3" ||
           folded == "explodeas" || folded == "selfdestructas";
}

bool unit_list_key(std::string_view key) noexcept {
    const std::string folded = fold_ascii(key);
    if (folded == "commander")
        return true;
    constexpr std::string_view prefix = "canbuild";
    if (folded.size() < prefix.size() || folded.compare(0, prefix.size(), prefix) != 0)
        return false;
    for (std::size_t index = prefix.size(); index < folded.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(folded[index]);
        if (byte < '0' || byte > '9')
            return false;
    }
    return true;
}

} // namespace oa::data::mutators
