// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Whether one need holds. A field the record leaves out is read here, and
// only here, as the value the loader uses. A presence test does not read
// that value: it asks whether the key is written.
#include "oa/data/mutation_rules/apply.hpp"

#include "oa/data/field_schema.hpp"
#include "oa/formats/tdf/number.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace oa::data::mutation_rules {
namespace tdf = oa::formats::tdf;
namespace fields = oa::data::field_schema;
namespace defs = oa::data::defs;
namespace sm = oa::formats::soft_mutator;

namespace {

/// Which schema a missing field is read from. `none` reads 0 or empty text
/// and does not ask the field schema.
enum class Schema : uint8_t { none, unit, weapon, move_class };

/// Folds one byte to lower case.
///
/// @param byte a byte of a name or value
/// @return the folded byte
char fold_ascii(char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return byte;
}

/// Folds one path byte: ASCII capitals to lower case, and '\' to '/'.
///
/// @param byte a byte of a path
/// @return the folded byte
char fold_path_byte(char byte) {
    if (byte == '\\')
        return '/';
    return fold_ascii(byte);
}

/// Reports whether two texts are the same without regard to ASCII case.
///
/// @param left a text
/// @param right another text
/// @return true when they fold to the same bytes
bool same_text(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (size_t offset = 0; offset < left.size(); ++offset) {
        if (fold_ascii(left[offset]) != fold_ascii(right[offset]))
            return false;
    }
    return true;
}

/// Reports whether `haystack` contains `needle`, ignoring ASCII case.
///
/// An empty needle is contained. The caller does not use that for a field
/// the record leaves out: a presence test is decided before this is called.
///
/// @param haystack the field text
/// @param needle the text the need looks for
/// @return true when the needle occurs
bool contains_text(std::string_view haystack, std::string_view needle) {
    if (needle.size() > haystack.size())
        return false;
    const size_t last = haystack.size() - needle.size();
    for (size_t start = 0; start <= last; ++start) {
        bool matched = true;
        for (size_t offset = 0; offset < needle.size(); ++offset) {
            if (fold_ascii(haystack[start + offset]) != fold_ascii(needle[offset])) {
                matched = false;
                break;
            }
        }
        if (matched)
            return true;
    }
    return false;
}

/// Reports whether `doc_path` is the layout's movement-class file.
///
/// @param doc_path a document path
/// @param layout the layout whose gamedata folder names the file
/// @return true when the path is `<gamedata>/moveinfo.tdf`
bool is_moveinfo(std::string_view doc_path, const defs::DataLayout& layout) {
    const std::string& directory =
        layout.directories[static_cast<size_t>(defs::DataDirectory::gamedata)];
    const std::string expected = directory + "/moveinfo.tdf";
    if (doc_path.size() != expected.size())
        return false;
    for (size_t offset = 0; offset < expected.size(); ++offset) {
        if (fold_path_byte(doc_path[offset]) != fold_path_byte(expected[offset]))
            return false;
    }
    return true;
}

/// The schema a need uses. Units and weapons win over the movement-class path.
///
/// @param kind the target kind
/// @param moveinfo true when the document is the movement-class file
/// @return the schema, or none
Schema schema_for(TargetKind kind, bool moveinfo) {
    if (kind == TargetKind::units)
        return Schema::unit;
    if (kind == TargetKind::weapons)
        return Schema::weapon;
    if (moveinfo)
        return Schema::move_class;
    return Schema::none;
}

/// Maps a schema onto the field schema's record kind.
///
/// @param schema a schema other than none
/// @return the record kind
fields::RecordKind record_kind(Schema schema) {
    if (schema == Schema::unit)
        return fields::RecordKind::unit;
    if (schema == Schema::weapon)
        return fields::RecordKind::weapon;
    return fields::RecordKind::move_class;
}

/// The number a missing field reads as.
///
/// An absent value, and a schema of none, read as 0. A flag reads as 0 or 1.
/// Text is read with the TDF number parser, which is 0 when the text has no digits.
///
/// @param taken the loader's value; absent when the schema does not hold the key
/// @return the number a range need compares
double number_of(const fields::Value& taken) {
    switch (taken.kind) {
    case fields::Value::Kind::absent:
        return 0;
    case fields::Value::Kind::integer:
        return static_cast<double>(taken.integer);
    case fields::Value::Kind::number:
        return taken.number;
    case fields::Value::Kind::boolean:
        return taken.boolean ? 1.0 : 0.0;
    case fields::Value::Kind::text:
        return tdf::parse_double(taken.text.c_str());
    }
    return 0;
}

/// The text a missing field reads as.
///
/// An absent value reads as empty. A number or an integer is spelled with the
/// TDF number spelling. A flag is `0` or `1`. Text is kept as the schema stored it.
///
/// @param taken the loader's value; absent when the schema does not hold the key
/// @return the text an equality or substring need compares
std::string text_of(const fields::Value& taken) {
    switch (taken.kind) {
    case fields::Value::Kind::absent:
        return {};
    case fields::Value::Kind::integer: {
        const std::optional<std::string> spelled =
            tdf::format_number(static_cast<double>(taken.integer));
        return spelled.has_value() ? *spelled : std::string{};
    }
    case fields::Value::Kind::number: {
        const std::optional<std::string> spelled = tdf::format_number(taken.number);
        return spelled.has_value() ? *spelled : std::string{};
    }
    case fields::Value::Kind::boolean:
        return taken.boolean ? std::string("1") : std::string("0");
    case fields::Value::Kind::text:
        return taken.text;
    }
    return {};
}

/// Reports whether the positive form of a need holds.
///
/// @param record the record's block, or null
/// @param schema which defaults a missing field uses
/// @param need the need
/// @return true when the positive comparison holds
bool positive_met(const tdf::Block* record, Schema schema, const sm::Need& need) {
    const char* written = nullptr;
    if (record != nullptr)
        written = tdf::find_value(record, need.field.c_str());
    // A presence test asks whether the key is written. The loader's value
    // is not consulted, so a default of 1 does not count as the key being there.
    if (need.kind == sm::NeedKind::contains && need.text.empty())
        return written != nullptr;

    fields::Value taken;
    if (written == nullptr && schema != Schema::none)
        taken = fields::default_value(record_kind(schema), need.field, record);

    if (need.kind == sm::NeedKind::range) {
        const double number = written != nullptr ? tdf::parse_double(written) : number_of(taken);
        return need.low <= number && number <= need.high;
    }
    std::string owned;
    std::string_view compared;
    if (written != nullptr) {
        compared = written;
    } else {
        owned = text_of(taken);
        compared = owned;
    }
    if (need.kind == sm::NeedKind::equals)
        return same_text(compared, need.text);
    return contains_text(compared, need.text);
}

/// Reports whether a need holds for a schema chosen by the caller.
///
/// @param record the record's block, or null
/// @param schema which defaults a missing field uses
/// @param need the need
/// @return true when the need holds
bool need_holds(const tdf::Block* record, Schema schema, const sm::Need& need) {
    const bool positive = positive_met(record, schema, need);
    return need.negated ? !positive : positive;
}

} // namespace

bool need_met(const tdf::Block* record, TargetKind kind, const sm::Need& need) {
    return need_holds(record, schema_for(kind, false), need);
}

bool need_met(
    const tdf::Block* record,
    TargetKind kind,
    std::string_view doc_path,
    const defs::DataLayout& layout,
    const sm::Need& need
) {
    return need_holds(record, schema_for(kind, is_moveinfo(doc_path, layout)), need);
}

} // namespace oa::data::mutation_rules
