// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/tdf.hpp"
#include "oa/formats/tdf/edit.hpp"
#include "oa/formats/tdf/number.hpp"
#include "oa/formats/tdf/write.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace oa::formats::tdf;

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(                                                                          \
                stderr, "%s:%d: %s: CHECK(%s) failed\n", __FILE__, __LINE__, __func__, #condition  \
            );                                                                                     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

struct Parsed {
    OwnedDocument document;

    explicit Parsed(std::string_view text) { CHECK(document.parse(text)); }

    [[nodiscard]] Document* get() noexcept { return document.get(); }

    [[nodiscard]] Block* root() noexcept { return editable_root(document.get()); }

    [[nodiscard]] Block* section(std::string_view name) noexcept {
        return editable_child(root(), name);
    }
};

[[nodiscard]] bool same_properties(const Block* left, const Block* right) {
    if (property_count(left) != property_count(right))
        return false;
    for (uint32_t index = 0; index < property_count(left); ++index) {
        const auto place = static_cast<int32_t>(index);
        const char* left_key = property_key_at(left, place);
        const char* right_key = property_key_at(right, place);
        const char* left_value = property_value_at(left, place);
        const char* right_value = property_value_at(right, place);
        if (left_key == nullptr || right_key == nullptr || left_value == nullptr ||
            right_value == nullptr)
            return false;
        if (std::strcmp(left_key, right_key) != 0 || std::strcmp(left_value, right_value) != 0)
            return false;
        if (left->properties[index].order != right->properties[index].order)
            return false;
    }
    return true;
}

[[nodiscard]] bool same_tree(const Block* left, const Block* right) {
    if (left == nullptr || right == nullptr)
        return left == right;
    if (std::strcmp(left->name, right->name) != 0)
        return false;
    if (!same_properties(left, right))
        return false;
    if (left->child_count != right->child_count)
        return false;
    for (uint32_t index = 0; index < left->child_count; ++index) {
        if (!same_tree(left->children[index], right->children[index]))
            return false;
    }
    return true;
}

[[nodiscard]] std::string body_of(const Block* block) {
    if (block->body_text == nullptr || block->body_length == 0)
        return {};
    return std::string(block->body_text, block->body_length);
}

/// The bytes a clean section is written as: a canonical header around the parsed body.
[[nodiscard]] std::string clean_section(const Block* block, uint32_t depth) {
    std::string text(depth - 1u, '\t');
    text += '[';
    text += block->name;
    text += "]\r\n";
    text.append(depth, '\t');
    text += '{';
    text += body_of(block);
    text += "}\r\n";
    return text;
}

[[nodiscard]] std::string written_text(const Document* document) {
    const auto text = write_text(document);
    CHECK(text.has_value());
    return text ? *text : std::string{};
}

void check_unedited(const char* label, std::string_view source) {
    Parsed parsed(source);
    const auto text = write_text(parsed.get());
    CHECK(text.has_value());
    if (!text)
        return;
    const std::string stored(parsed.get()->text, parsed.get()->text_length);
    if (*text != stored) {
        std::fprintf(stderr, "unedited %s was rewritten\n", label);
        ++failures;
    }
    CHECK(!document_edited(parsed.get()));
}

void check_round_trip(const Document* edited) {
    const auto text = write_text(edited);
    CHECK(text.has_value());
    if (!text)
        return;
    OwnedDocument again;
    CHECK(again.parse(*text));
    CHECK(same_tree(edited->root, again.root()));
}

void unedited_documents_write_the_stored_text() {
    check_unedited(
        "fbi",
        "[UNITINFO]\r\n{\r\n\tUnitName=ARMCOM;\r\n\tSide=ARM;\r\n\tBuildCostEnergy=100;\r\n}\r\n"
    );
    check_unedited(
        "weapons",
        "[ARMCOM_WEAPON]\r\n{\r\n\tID=1;\r\n\tname=First;\r\n}\r\n"
        "[CORCOM_WEAPON]\r\n{\r\n\tID=2;\r\n\t[DAMAGE]\r\n\t{\r\n\t\tdefault=10;\r\n\t}\r\n}\r\n"
        "[KROGOTH_WEAPON]\r\n{\r\n\tID=3;\r\n\tname=Third;\r\n}\r\n"
    );
    check_unedited(
        "gui",
        "[GADGETS]\r\n{\r\n\t[BUTTON]\r\n\t{\r\n\t\t[COMMON]\r\n\t\t{\r\n\t\t\tid=1;\r\n"
        "\t\t}\r\n\t}\r\n}\r\n"
    );
    check_unedited(
        "ota",
        "[GlobalHeader]\r\n{\r\n\tmissionname=Test;\r\n\t[Schema 0]\r\n\t{\r\n"
        "\t\taiprofile=default;\r\n\t}\r\n\t[Schema "
        "1]\r\n\t{\r\n\t\taiprofile=other;\r\n\t}\r\n}\r\n"
    );
    check_unedited("comments", "[A]{ // note\n a=1\nb=2; /* hidden */ c=3;}");
}

void one_unit_edit_keeps_file_order_and_the_newest_variant() {
    Parsed parsed(
        "[UNITINFO]\r\n{\r\n\tUnitName=ARMCOM;\r\n\tSide=ARM;\r\n\tside=CORE;\r\n"
        "\tBuildCostEnergy=100;\r\n}\r\n"
    );
    CHECK(!document_edited(parsed.get()));
    CHECK(
        set_value(parsed.get(), parsed.section("UNITINFO"), "UnitName", "COMMANDER") ==
        EditStatus::ok
    );
    CHECK(document_edited(parsed.get()));
    const std::string text = written_text(parsed.get());
    const char* expected = "[UNITINFO]\r\n"
                           "\t{\r\n"
                           "\tUnitName=COMMANDER;\r\n"
                           "\tSide=ARM;\r\n"
                           "\tside=CORE;\r\n"
                           "\tBuildCostEnergy=100;\r\n"
                           "\t}\r\n";
    CHECK(text == expected);
    check_round_trip(parsed.get());
    OwnedDocument again;
    CHECK(again.parse(text));
    const Block* unit = find_child(again.root(), "UNITINFO");
    CHECK(std::strcmp(find_value(unit, "side"), "CORE") == 0);
    CHECK(std::strcmp(property_key_at(unit, 1), "side") == 0);
    CHECK(std::strcmp(property_key_at(unit, 2), "Side") == 0);
}

void editing_one_weapon_leaves_the_other_sections_byte_for_byte() {
    const char* source = "[ARMCOM_WEAPON]\r\n{\r\n\tID=1;\r\n\tname=First;\r\n}\r\n"
                         "[CORCOM_WEAPON]\r\n{\r\n\tID=2;\r\n\tname=Second;\r\n\t[DAMAGE]\r\n"
                         "\t{\r\n\t\tdefault=10;\r\n\t}\r\n}\r\n"
                         "[KROGOTH_WEAPON]\r\n{\r\n\tID=3;\r\n\tname=Third;\r\n}\r\n";
    Parsed parsed(source);
    Block* first = parsed.section("ARMCOM_WEAPON");
    Block* second = parsed.section("CORCOM_WEAPON");
    Block* third = parsed.section("KROGOTH_WEAPON");
    const std::string first_clean = clean_section(first, 1);
    const std::string third_clean = clean_section(third, 1);
    const uint32_t first_hash = first->body_hash;
    const uint32_t second_hash = second->body_hash;
    const uint32_t third_hash = third->body_hash;
    CHECK(set_value(parsed.get(), second, "name", "Changed") == EditStatus::ok);
    const std::string text = written_text(parsed.get());
    CHECK(text.find(first_clean) != std::string::npos);
    CHECK(text.find(third_clean) != std::string::npos);
    check_round_trip(parsed.get());
    OwnedDocument again;
    CHECK(again.parse(text));
    CHECK(find_child(again.root(), "ARMCOM_WEAPON")->body_hash == first_hash);
    CHECK(find_child(again.root(), "KROGOTH_WEAPON")->body_hash == third_hash);
    CHECK(find_child(again.root(), "CORCOM_WEAPON")->body_hash != second_hash);

    Parsed nested(source);
    Block* damage = editable_child(nested.section("CORCOM_WEAPON"), "DAMAGE");
    const std::string kept_first = clean_section(nested.section("ARMCOM_WEAPON"), 1);
    const std::string kept_third = clean_section(nested.section("KROGOTH_WEAPON"), 1);
    const uint32_t weapon_hash = nested.section("CORCOM_WEAPON")->body_hash;
    CHECK(set_value(nested.get(), damage, "default", "12") == EditStatus::ok);
    const std::string nested_text = written_text(nested.get());
    const char* damage_text = "\t[DAMAGE]\r\n\t\t{\r\n\t\tdefault=12;\r\n\t\t}\r\n";
    CHECK(nested_text.find(damage_text) != std::string::npos);
    CHECK(nested_text.find(kept_first) != std::string::npos);
    CHECK(nested_text.find(kept_third) != std::string::npos);
    OwnedDocument nested_again;
    CHECK(nested_again.parse(nested_text));
    CHECK(find_child(nested_again.root(), "CORCOM_WEAPON")->body_hash != weapon_hash);
    CHECK(
        find_child(nested_again.root(), "ARMCOM_WEAPON")->body_hash ==
        nested.section("ARMCOM_WEAPON")->body_hash
    );
    CHECK(
        find_child(nested_again.root(), "KROGOTH_WEAPON")->body_hash ==
        nested.section("KROGOTH_WEAPON")->body_hash
    );
    const Block* written_damage =
        find_child(find_child(nested_again.root(), "CORCOM_WEAPON"), "DAMAGE");
    CHECK(std::strcmp(find_value(written_damage, "default"), "12") == 0);
}

void editing_one_schema_rewrites_the_header_and_keeps_the_other_schema() {
    const char* source = "[GlobalHeader]\r\n{\r\n\tmissionname=Test;\r\n"
                         "\t[Schema 0]\r\n\t{\r\n\t\taiprofile=default;\r\n"
                         "\t\tSurfaceMetal=1000;\r\n\t}\r\n"
                         "\t[Schema 1]\r\n\t{\r\n\t\taiprofile=other;\r\n\t}\r\n}\r\n"
                         "[Special]\r\n{\r\n\tnote=keep;\r\n}\r\n";
    Parsed parsed(source);
    Block* header = parsed.section("GlobalHeader");
    Block* schema1 = editable_child(header, "Schema 1");
    Block* special = parsed.section("Special");
    const std::string schema1_clean = clean_section(schema1, 2);
    const uint32_t header_hash = header->body_hash;
    const uint32_t schema1_hash = schema1->body_hash;
    const uint32_t special_hash = special->body_hash;
    CHECK(
        set_value(parsed.get(), editable_child(header, "Schema 0"), "aiprofile", "ridge") ==
        EditStatus::ok
    );
    const std::string text = written_text(parsed.get());
    CHECK(text.find(schema1_clean) != std::string::npos);
    CHECK(text.find("\t[Special]\r\n") == std::string::npos);
    CHECK(text.find(clean_section(special, 1)) != std::string::npos);
    OwnedDocument again;
    CHECK(again.parse(text));
    const Block* written_header = find_child(again.root(), "GlobalHeader");
    CHECK(written_header->body_hash != header_hash);
    CHECK(find_child(written_header, "Schema 1")->body_hash == schema1_hash);
    CHECK(find_child(again.root(), "Special")->body_hash == special_hash);
    CHECK(
        std::strcmp(find_value(find_child(written_header, "Schema 0"), "aiprofile"), "ridge") == 0
    );
    check_round_trip(parsed.get());
}

void edits_add_remove_and_report_themselves() {
    Parsed added("[A]{b=1;a=2;}");
    CHECK(set_value(added.get(), added.section("A"), "  c  ", "  3  ") == EditStatus::ok);
    CHECK(document_edited(added.get()));
    CHECK(written_text(added.get()) == "[A]\r\n\t{\r\n\tb=1;\r\n\ta=2;\r\n\tc=3;\r\n\t}\r\n");
    check_round_trip(added.get());

    Parsed removed("[A]{Side=ARM;side=CORE;x=1;}");
    CHECK(!document_edited(removed.get()));
    CHECK(remove_key(removed.get(), removed.section("A"), "SIDE") == EditStatus::ok);
    CHECK(document_edited(removed.get()));
    CHECK(property_count(removed.section("A")) == 1);
    CHECK(std::strcmp(property_key_at(removed.section("A"), 0), "x") == 0);
    CHECK(written_text(removed.get()) == "[A]\r\n\t{\r\n\tx=1;\r\n\t}\r\n");
    Parsed absent("[A]{x=1;}");
    CHECK(remove_key(absent.get(), absent.section("A"), "missing") == EditStatus::ok);
    CHECK(!document_edited(absent.get()));

    Parsed child("[A]{v=1;}");
    EditStatus status = EditStatus::bad_name;
    Block* extra = add_child(child.get(), child.section("A"), " EXTRA ", &status);
    CHECK(extra != nullptr && status == EditStatus::ok);
    CHECK(std::strcmp(extra->name, "EXTRA") == 0);
    CHECK(document_edited(child.get()));
    CHECK(written_text(child.get()) == "[A]\r\n\t{\r\n\tv=1;\r\n\t[EXTRA]\r\n\t\t{}\r\n\t}\r\n");
    check_round_trip(child.get());

    Parsed dropped("[A]{}[B]{v=1;}[C]{}");
    CHECK(remove_child(dropped.get(), dropped.root(), 1) == EditStatus::ok);
    CHECK(document_edited(dropped.get()));
    CHECK(dropped.root()->child_count == 2);
    CHECK(std::strcmp(dropped.root()->children[0]->name, "A") == 0);
    CHECK(std::strcmp(dropped.root()->children[1]->name, "C") == 0);
    check_round_trip(dropped.get());
}

void refusal_leaves_the_document_unchanged(Parsed& parsed, EditStatus status) {
    CHECK(status != EditStatus::ok);
    CHECK(!document_edited(parsed.get()));
    const auto text = write_text(parsed.get());
    CHECK(text.has_value());
    if (text)
        CHECK(*text == std::string(parsed.get()->text, parsed.get()->text_length));
}

void refusals_leave_the_document_unchanged() {
    const char* source = "[A]{v=1;}";
    const std::string_view bad_keys[] = {
        "",
        "   ",
        "a=b",
        "a;b",
        "a[b",
        "a]b",
        "a{b",
        "a}b",
        "a\nb",
        "a\rb",
        "a//b",
        "a/*b",
    };
    for (const std::string_view key : bad_keys) {
        Parsed parsed(source);
        refusal_leaves_the_document_unchanged(
            parsed, set_value(parsed.get(), parsed.section("A"), key, "1")
        );
    }
    {
        Parsed parsed(source);
        const std::string_view key("a\0b", 3);
        refusal_leaves_the_document_unchanged(
            parsed, set_value(parsed.get(), parsed.section("A"), key, "1")
        );
    }
    const std::string_view bad_values[] = {"a;b", "a\nb", "a\rb", "a//b", "a/*b"};
    for (const std::string_view value : bad_values) {
        Parsed parsed(source);
        refusal_leaves_the_document_unchanged(
            parsed, set_value(parsed.get(), parsed.section("A"), "k", value)
        );
    }
    {
        Parsed parsed(source);
        const std::string_view value("a\0b", 3);
        refusal_leaves_the_document_unchanged(
            parsed, set_value(parsed.get(), parsed.section("A"), "k", value)
        );
    }
    {
        Parsed parsed(source);
        const std::string value(max_edited_value_bytes + 1u, 'x');
        refusal_leaves_the_document_unchanged(
            parsed, set_value(parsed.get(), parsed.section("A"), "k", value)
        );
    }
    const std::string_view bad_names[] = {"[", "]", "{", "}", "a\nb", "a\rb", "a//b", "a/*b"};
    for (const std::string_view name : bad_names) {
        Parsed parsed(source);
        EditStatus status = EditStatus::ok;
        CHECK(add_child(parsed.get(), parsed.section("A"), name, &status) == nullptr);
        refusal_leaves_the_document_unchanged(parsed, status);
    }
    {
        Parsed parsed(source);
        EditStatus status = EditStatus::ok;
        const std::string_view name("a\0b", 3);
        CHECK(add_child(parsed.get(), parsed.section("A"), name, &status) == nullptr);
        refusal_leaves_the_document_unchanged(parsed, status);
    }
    {
        Parsed parsed(source);
        Block* parent = parsed.root();
        for (int depth = 0; depth < 64; ++depth) {
            EditStatus status = EditStatus::ok;
            Block* child = add_child(parsed.get(), parent, "n", &status);
            CHECK(child != nullptr && status == EditStatus::ok);
            parent = child;
        }
        const uint32_t before = parent->child_count;
        EditStatus status = EditStatus::ok;
        CHECK(add_child(parsed.get(), parent, "n", &status) == nullptr);
        CHECK(status == EditStatus::bad_name);
        CHECK(parent->child_count == before);
    }
    {
        Parsed parsed(source);
        CHECK(remove_child(parsed.get(), parsed.root(), 1) == EditStatus::no_such_child);
        CHECK(!document_edited(parsed.get()));
        CHECK(parsed.root()->child_count == 1);
    }
    {
        Parsed parsed(source);
        EditStatus status = EditStatus::ok;
        const std::string name(std::size_t{max_arena_bytes} + 1u, 'n');
        CHECK(add_child(parsed.get(), parsed.root(), name, &status) == nullptr);
        CHECK(status == EditStatus::out_of_memory);
        CHECK(!document_edited(parsed.get()));
        CHECK(parsed.root()->child_count == 1);
    }
}

void a_stored_line_break_round_trips_inside_an_edited_block() {
    Parsed parsed("[A]{a=1\nb=2;c=3;}[B]{d=4;}");
    CHECK(std::strcmp(find_value(parsed.section("A"), "a"), "1\nb=2") == 0);
    CHECK(set_value(parsed.get(), parsed.section("A"), "c", "9") == EditStatus::ok);
    const std::string text = written_text(parsed.get());
    const char* expected = "[A]\r\n\t{\r\n\ta=1\nb=2;\r\n\tc=9;\r\n\t}\r\n[B]\r\n\t{d=4;}\r\n";
    CHECK(text == expected);
    check_round_trip(parsed.get());
    OwnedDocument again;
    CHECK(again.parse(text));
    CHECK(std::strcmp(find_value(find_child(again.root(), "A"), "a"), "1\nb=2") == 0);
    CHECK(find_child(again.root(), "B")->body_hash == parsed.section("B")->body_hash);
}

void write_text_refuses_a_document_past_the_parse_limit() {
    Parsed parsed("");
    const std::string name(1024, 'n');
    for (int count = 0; count < 20000; ++count) {
        EditStatus status = EditStatus::ok;
        Block* child = add_child(parsed.get(), parsed.root(), name, &status);
        CHECK(child != nullptr && status == EditStatus::ok);
        if (child == nullptr)
            return;
    }
    CHECK(!write_text(parsed.get()).has_value());
}

void expect_number(double value, const char* text) {
    const auto spelled = format_number(value);
    CHECK(spelled.has_value());
    if (!spelled)
        return;
    if (*spelled != text) {
        std::fprintf(stderr, "format got %s, expected %s\n", spelled->c_str(), text);
        ++failures;
    }
    CHECK(spelled->find_first_of("eE+") == std::string::npos);
}

void expect_fixed(double value, const char* text, int32_t fixed) {
    expect_number(value, text);
    Parsed parsed(std::string("[A]{v=") + text + ";}");
    CHECK(get_fixed(parsed.section("A"), "v", 0) == fixed);
}

void number_golden_table() {
    expect_number(0.0, "0");
    expect_number(-0.0, "0");
    expect_number(1.0, "1");
    expect_number(-1.0, "-1");
    expect_number(30.0, "30");
    expect_number(240.0, "240");
    expect_fixed(1.6 * 1.5, "2.4", 157286);
    expect_number(0.1 + 0.2, "0.3");
    expect_number(0.57 * 100.0, "57");
    CHECK(parse_int("57") == 57);
    expect_number(1.0 / 3.0, "0.333333333333333");
    expect_number(2.0 / 3.0, "0.666666666666667");
    expect_number(0.5, "0.5");
    expect_number(-1.25, "-1.25");
    expect_number(32767.5, "32767.5");
    expect_fixed(157286.0 / 65536.0, "2.399993896484375", 157286);
    expect_fixed(65537.0 / 65536.0, "1.0000152587890625", 65537);
    expect_number(-157286.0 / 65536.0, "-2.399993896484375");
    expect_number(1.0 / 65536.0, "0.0000152587890625");
    expect_number(1.0 / 131072.0, "0.00000762939453125");
    expect_number(1.005, "1.005");
    expect_number(0.66666666 * 30.0, "19.9999998");
    expect_number(1900000.0, "1900000");
    expect_number(1e9, "1000000000");
    expect_number(99999999999.0, "99999999999");
    expect_number(4294967296.0, "4294967296");
    expect_number(123456789012345678.0, "123456789012345680");
    expect_number(1e21, "1000000000000000000000");

    const char* maximum =
        "179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558"
        "632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245"
        "490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168"
        "738177180919299881250404026184124858368";
    expect_number(std::numeric_limits<double>::max(), maximum);
    CHECK(std::strlen(maximum) == 309);

    std::string denorm = "0.";
    denorm.append(323, '0');
    denorm += "494065645841247";
    CHECK(denorm.size() == 340);
    expect_number(std::numeric_limits<double>::denorm_min(), denorm.c_str());

    CHECK(!format_number(std::numeric_limits<double>::quiet_NaN()).has_value());
    CHECK(!format_number(std::numeric_limits<double>::infinity()).has_value());
    CHECK(!format_number(-std::numeric_limits<double>::infinity()).has_value());
}

} // namespace

int main() {
    unedited_documents_write_the_stored_text();
    one_unit_edit_keeps_file_order_and_the_newest_variant();
    editing_one_weapon_leaves_the_other_sections_byte_for_byte();
    editing_one_schema_rewrites_the_header_and_keeps_the_other_schema();
    edits_add_remove_and_report_themselves();
    refusals_leave_the_document_unchanged();
    a_stored_line_break_round_trips_inside_an_edited_block();
    write_text_refuses_a_document_past_the_parse_limit();
    number_golden_table();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("tdf-writer: all checks passed");
    return 0;
}
