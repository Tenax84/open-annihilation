// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The legacy rule reader: every need and change form, the lenient forms the
// old files rely on, every diagnostic, and the records two spellings share.
// With --corpus DIR it reads that folder instead, which is how the real
// files are checked without being copied into the engine.

#include "oa/formats/soft_mutator.hpp"
#include "oa/formats/tdf.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace sm = oa::formats::soft_mutator;
namespace fs = std::filesystem;

[[nodiscard]] sm::ParseResult parse(std::string_view text) {
    return sm::parse_rules(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size())
    );
}

[[nodiscard]] bool has_code(const sm::ParseResult& result, sm::DiagnosticCode code) {
    for (const sm::Diagnostic& item : result.diagnostics) {
        if (item.code == code)
            return true;
    }
    return false;
}

[[nodiscard]] bool
has_code_at(const sm::ParseResult& result, sm::DiagnosticCode code, uint32_t line) {
    for (const sm::Diagnostic& item : result.diagnostics) {
        if (item.code == code && item.line == line)
            return true;
    }
    return false;
}

[[nodiscard]] int count_code(const sm::ParseResult& result, sm::DiagnosticCode code) {
    int found = 0;
    for (const sm::Diagnostic& item : result.diagnostics) {
        if (item.code == code)
            ++found;
    }
    return found;
}

[[nodiscard]] const sm::Change* change_named(const sm::Block& block, std::string_view field) {
    for (const sm::Change& change : block.changes) {
        if (change.field == field)
            return &change;
    }
    return nullptr;
}

[[nodiscard]] bool kept_nine(const sm::ParseResult& result) {
    for (const sm::Block& block : result.rules.blocks) {
        const sm::Change* kept = change_named(block, "Kept");
        if (kept != nullptr && kept->kind == sm::ChangeKind::assign && kept->value == 9)
            return true;
    }
    return false;
}

void expect_kept(const char* text, sm::DiagnosticCode code, uint32_t line) {
    const sm::ParseResult result = parse(text);
    OA_CHECK(result.has_errors());
    OA_CHECK(has_code_at(result, code, line));
    OA_CHECK(kept_nine(result));
}

void needs() {
    const sm::ParseResult result = parse(
        "[*Group*]\n"
        "{\n"
        "[Needs]\n"
        "{\n"
        "Band=1,2;\n"
        "Outside=!3,4;\n"
        "Side=\"Core\";\n"
        "Other=!\"Arm\";\n"
        "Category=\"KBOT\"*;\n"
        "Hidden=!\"CTRL\"*;\n"
        "Present=\"\"*;\n"
        "Absent=!\"\"*;\n"
        "Cold=-5,10;\n"
        "Half=.5,1;\n"
        "Wide=0,99999999999;\n"
        "Backwards=5,1;\n"
        "Spaced=1, 2;\n"
        "Mark=\"AI \"*;\n"
        "}\n"
        "[Changes]\n"
        "{\n"
        "Kept=1;\n"
        "}\n"
        "}\n"
    );
    OA_CHECK(!result.has_errors());
    OA_CHECK(result.rules.blocks.size() == 1);
    if (result.rules.blocks.size() != 1)
        return;
    const sm::Block& block = result.rules.blocks[0];
    OA_CHECK(block.selector == sm::Selector::group);
    OA_CHECK(block.name.empty());
    OA_CHECK(block.needs.size() == 14);
    if (block.needs.size() != 14)
        return;
    const sm::Need& band = block.needs[0];
    OA_CHECK(band.field == "Band" && band.kind == sm::NeedKind::range && !band.negated);
    OA_CHECK(band.low == 1 && band.high == 2 && band.line == 5);
    const sm::Need& outside = block.needs[1];
    OA_CHECK(
        outside.kind == sm::NeedKind::range && outside.negated && outside.low == 3 &&
        outside.high == 4
    );
    const sm::Need& side = block.needs[2];
    OA_CHECK(side.kind == sm::NeedKind::equals && !side.negated && side.text == "Core");
    const sm::Need& other = block.needs[3];
    OA_CHECK(other.kind == sm::NeedKind::equals && other.negated && other.text == "Arm");
    const sm::Need& category = block.needs[4];
    OA_CHECK(
        category.kind == sm::NeedKind::contains && !category.negated && category.text == "KBOT"
    );
    const sm::Need& hidden = block.needs[5];
    OA_CHECK(hidden.kind == sm::NeedKind::contains && hidden.negated && hidden.text == "CTRL");
    const sm::Need& present = block.needs[6];
    OA_CHECK(present.kind == sm::NeedKind::contains && !present.negated && present.text.empty());
    const sm::Need& absent = block.needs[7];
    OA_CHECK(absent.kind == sm::NeedKind::contains && absent.negated && absent.text.empty());
    const sm::Need& cold = block.needs[8];
    OA_CHECK(cold.low == -5 && cold.high == 10);
    const sm::Need& half = block.needs[9];
    OA_CHECK(half.low == oa::formats::tdf::parse_double(".5") && half.high == 1);
    const sm::Need& wide = block.needs[10];
    OA_CHECK(wide.high == oa::formats::tdf::parse_double("99999999999"));
    const sm::Need& backwards = block.needs[11];
    OA_CHECK(backwards.low == 5 && backwards.high == 1 && backwards.line == 16);
    OA_CHECK(has_code_at(result, sm::DiagnosticCode::empty_range, 16));
    const sm::Need& spaced = block.needs[12];
    OA_CHECK(spaced.low == 1 && spaced.high == 2 && spaced.line == 17);
    OA_CHECK(has_code_at(result, sm::DiagnosticCode::spaces_in_value, 17));
    const sm::Need& mark = block.needs[13];
    OA_CHECK(mark.kind == sm::NeedKind::contains && mark.text == "AI ");
    OA_CHECK(count_code(result, sm::DiagnosticCode::missing_semicolon) == 0);
}

void changes() {
    const sm::ParseResult result = parse(
        "[*All*]\n"
        "{\n"
        "Plain=5;\n"
        "Words=\"text\";\n"
        "Gone=\"\";\n"
        "Extra=\"tail\"+;\n"
        "Factor=*0.333333333;\n"
        "Clamped=*5.0(10,30000);\n"
        "Locked=^0.001;\n"
        "<AllDepth2>=*0.5;\n"
        "Quoted=\"850\";\n"
        "Bolt=\"LIGHTNING \";\n"
        "Tiny=*.50;\n"
        "Trail=5.;\n"
        "Negative=-3;\n"
        "<alldepth2>=*2;\n"
        "}\n"
    );
    OA_CHECK(result.rules.blocks.size() == 1);
    if (result.rules.blocks.size() != 1)
        return;
    const sm::Block& block = result.rules.blocks[0];
    OA_CHECK(block.selector == sm::Selector::all);
    OA_CHECK(block.changes.size() == 14);
    if (block.changes.size() != 14)
        return;
    OA_CHECK(block.changes[0].kind == sm::ChangeKind::assign && block.changes[0].value == 5);
    OA_CHECK(
        block.changes[1].kind == sm::ChangeKind::assign_text && block.changes[1].text == "text"
    );
    OA_CHECK(block.changes[2].kind == sm::ChangeKind::remove && block.changes[2].text.empty());
    OA_CHECK(block.changes[3].kind == sm::ChangeKind::append && block.changes[3].text == "tail");
    OA_CHECK(block.changes[4].kind == sm::ChangeKind::scale);
    OA_CHECK(block.changes[4].value == oa::formats::tdf::parse_double("0.333333333"));
    OA_CHECK(block.changes[5].kind == sm::ChangeKind::scale_clamp);
    OA_CHECK(block.changes[5].value == oa::formats::tdf::parse_double("5.0"));
    OA_CHECK(block.changes[5].low == 10 && block.changes[5].high == 30000);
    OA_CHECK(block.changes[6].kind == sm::ChangeKind::meta);
    OA_CHECK(block.changes[6].value == oa::formats::tdf::parse_double("0.001"));
    OA_CHECK(block.changes[7].kind == sm::ChangeKind::scale && block.changes[7].all_depth2);
    OA_CHECK(block.changes[7].field == "<AllDepth2>" && block.changes[7].value == 0.5);
    OA_CHECK(
        block.changes[8].kind == sm::ChangeKind::assign_text && block.changes[8].text == "850"
    );
    OA_CHECK(block.changes[8].line == 11);
    OA_CHECK(has_code_at(result, sm::DiagnosticCode::quoted_number, 11));
    for (const sm::Diagnostic& item : result.diagnostics) {
        if (item.code == sm::DiagnosticCode::quoted_number)
            OA_CHECK(item.severity == sm::Severity::warning);
    }
    OA_CHECK(
        block.changes[9].kind == sm::ChangeKind::assign_text &&
        block.changes[9].text == "LIGHTNING "
    );
    OA_CHECK(block.changes[10].kind == sm::ChangeKind::scale && block.changes[10].value == 0.5);
    OA_CHECK(block.changes[11].kind == sm::ChangeKind::assign && block.changes[11].value == 5);
    OA_CHECK(block.changes[12].kind == sm::ChangeKind::assign && block.changes[12].value == -3);
    OA_CHECK(block.changes[13].kind == sm::ChangeKind::scale && block.changes[13].all_depth2);
    OA_CHECK(block.changes[13].field == "<alldepth2>" && block.changes[13].value == 2);
    OA_CHECK(!result.has_errors());
}

void selectors() {
    const sm::ParseResult result = parse(
        "[*All*]{A=1;}\n"
        "[*all*]{B=1;}\n"
        "[*Group*]{[Needs]{C=1,1;}[Changes]{D=1;}}\n"
        "[*group*]{[changes]{F=1;}[needs]{E=2,2;}}\n"
        "[RIDGECOM]{G=1;}\n"
        "[RIDGECOM]{H=2;}\n"
        "[RIDGEFAB]{I=3;}\n"
    );
    OA_CHECK(!result.has_errors());
    OA_CHECK(result.rules.blocks.size() == 7);
    if (result.rules.blocks.size() != 7)
        return;
    OA_CHECK(
        result.rules.blocks[0].selector == sm::Selector::all && result.rules.blocks[0].name.empty()
    );
    OA_CHECK(result.rules.blocks[1].selector == sm::Selector::all);
    OA_CHECK(result.rules.blocks[2].selector == sm::Selector::group);
    OA_CHECK(
        result.rules.blocks[2].needs.size() == 1 && result.rules.blocks[2].changes.size() == 1
    );
    OA_CHECK(result.rules.blocks[3].selector == sm::Selector::group);
    OA_CHECK(
        result.rules.blocks[3].needs.size() == 1 && result.rules.blocks[3].needs[0].field == "E"
    );
    OA_CHECK(
        result.rules.blocks[3].changes.size() == 1 && result.rules.blocks[3].changes[0].field == "F"
    );
    OA_CHECK(
        result.rules.blocks[4].selector == sm::Selector::name &&
        result.rules.blocks[4].name == "RIDGECOM"
    );
    OA_CHECK(
        result.rules.blocks[5].name == "RIDGECOM" && result.rules.blocks[5].changes[0].field == "H"
    );
    OA_CHECK(result.rules.blocks[6].name == "RIDGEFAB");
    OA_CHECK(result.rules.blocks[4].line < result.rules.blocks[5].line);
}

void groups_without_asterisks() {
    const sm::ParseResult fortress = parse(
        "[Group]\n"
        "{\n"
        "[needs]\n"
        "{\n"
        "Solar=-1,0;\n"
        "}\n"
        "[changes]\n"
        "{\n"
        "Solar=0;\n"
        "}\n"
        "}\n"
    );
    OA_CHECK(fortress.rules.blocks.size() == 1);
    if (fortress.rules.blocks.size() == 1) {
        OA_CHECK(fortress.rules.blocks[0].selector == sm::Selector::group);
        OA_CHECK(fortress.rules.blocks[0].name.empty());
        OA_CHECK(fortress.rules.blocks[0].needs.size() == 1);
        OA_CHECK(fortress.rules.blocks[0].changes.size() == 1);
        OA_CHECK(fortress.rules.blocks[0].changes[0].value == 0);
    }
    OA_CHECK(has_code_at(fortress, sm::DiagnosticCode::group_without_asterisks, 1));
    OA_CHECK(!fortress.has_errors());

    const sm::ParseResult named = parse(
        "[RIDGECOM]\n"
        "{\n"
        "[Needs]\n"
        "{\n"
        "Band=1,1;\n"
        "}\n"
        "[Changes]\n"
        "{\n"
        "Speed=2;\n"
        "}\n"
        "}\n"
    );
    OA_CHECK(named.rules.blocks.size() == 1);
    if (named.rules.blocks.size() == 1) {
        OA_CHECK(named.rules.blocks[0].selector == sm::Selector::group);
        OA_CHECK(named.rules.blocks[0].name.empty());
        OA_CHECK(named.rules.blocks[0].needs.size() == 1);
        OA_CHECK(named.rules.blocks[0].changes[0].field == "Speed");
    }
    OA_CHECK(has_code(named, sm::DiagnosticCode::group_without_asterisks));
}

void directives() {
    const sm::ParseResult ordered = parse(
        "[RIDGECOM]{A=1;}\n"
        "folder=weapons; extension=tdf;\n"
        "[RIDGEFAB]{B=2;}\n"
    );
    OA_CHECK(ordered.rules.blocks.size() == 2);
    if (ordered.rules.blocks.size() == 2) {
        OA_CHECK(ordered.rules.blocks[0].target.folder == "units");
        OA_CHECK(ordered.rules.blocks[0].target.extension == "fbi");
        OA_CHECK(ordered.rules.blocks[1].target.folder == "weapons");
        OA_CHECK(ordered.rules.blocks[1].target.extension == "tdf");
        OA_CHECK((ordered.rules.blocks[0].target == sm::Target{"units", "fbi"}));
    }
    OA_CHECK(!ordered.has_errors());

    const sm::ParseResult corpses = parse("folder=Features\\Corpses;\n[*All*]{A=1;}\n");
    OA_CHECK(corpses.rules.blocks.size() == 1);
    if (corpses.rules.blocks.size() == 1)
        OA_CHECK(corpses.rules.blocks[0].target.folder == "features/corpses");

    const sm::ParseResult suffix = parse("extension=SIDEData.TDF;\n[*All*]{A=1;}\n");
    OA_CHECK(suffix.rules.blocks.size() == 1);
    if (suffix.rules.blocks.size() == 1)
        OA_CHECK(suffix.rules.blocks[0].target.extension == "sidedata.tdf");

    const sm::ParseResult dotted = parse("extension=.FBI;\n[*All*]{A=1;}\n");
    OA_CHECK(dotted.rules.blocks.size() == 1);
    if (dotted.rules.blocks.size() == 1)
        OA_CHECK(dotted.rules.blocks[0].target.extension == "fbi");

    const sm::ParseResult inside = parse("[RIDGECOM]{folder=weapons;A=1;}\n[RIDGEFAB]{B=2;}\n");
    OA_CHECK(has_code(inside, sm::DiagnosticCode::directive_in_block));
    OA_CHECK(inside.rules.blocks.size() == 2);
    if (inside.rules.blocks.size() == 2) {
        OA_CHECK(inside.rules.blocks[0].target.folder == "units");
        OA_CHECK(inside.rules.blocks[1].target.folder == "units");
        OA_CHECK(change_named(inside.rules.blocks[0], "A") != nullptr);
        OA_CHECK(change_named(inside.rules.blocks[0], "folder") == nullptr);
    }
}

void layout() {
    const sm::ParseResult bare = parse("[RIDGECOM]\n{\nMaxDamage=5000\nSpeed=2\n}\n");
    OA_CHECK(has_code_at(bare, sm::DiagnosticCode::missing_semicolon, 3));
    OA_CHECK(has_code_at(bare, sm::DiagnosticCode::missing_semicolon, 4));
    OA_CHECK(count_code(bare, sm::DiagnosticCode::missing_semicolon) == 2);
    OA_CHECK(bare.rules.blocks.size() == 1 && bare.rules.blocks[0].changes.size() == 2);

    const sm::ParseResult joined = parse("[RIDGECOM]{A=1;}[RIDGEFAB]{B=2;}");
    OA_CHECK(joined.rules.blocks.size() == 2);
    OA_CHECK(count_code(joined, sm::DiagnosticCode::missing_semicolon) == 0);
    if (joined.rules.blocks.size() == 2) {
        OA_CHECK(joined.rules.blocks[0].name == "RIDGECOM");
        OA_CHECK(joined.rules.blocks[1].name == "RIDGEFAB");
    }

    const sm::ParseResult split = parse("[RIDGECOM]\n\n{\nA=1;\n}\n");
    OA_CHECK(split.rules.blocks.size() == 1 && !split.has_errors());

    const sm::ParseResult one_line = parse("[RIDGECOM]{A=1;B=2;}");
    OA_CHECK(one_line.rules.blocks.size() == 1 && one_line.rules.blocks[0].changes.size() == 2);

    const sm::ParseResult noted = parse("[RIDGECOM]// the ridge gate\n{\nA=1; // stays\n}\n");
    OA_CHECK(noted.rules.blocks.size() == 1 && !noted.has_errors());
    if (noted.rules.blocks.size() == 1)
        OA_CHECK(
            noted.rules.blocks[0].changes.size() == 1 && noted.rules.blocks[0].changes[0].line == 3
        );

    const sm::ParseResult crlf = parse("[*All*]\r\n{\r\nA=1\r\n}\r\n");
    OA_CHECK(has_code_at(crlf, sm::DiagnosticCode::missing_semicolon, 3));
    const sm::ParseResult lf = parse("[*All*]\n{\nA=1\n}\n");
    OA_CHECK(has_code_at(lf, sm::DiagnosticCode::missing_semicolon, 3));
    const sm::ParseResult cr = parse("[*All*]\r{\rA=1\r}\r");
    OA_CHECK(has_code_at(cr, sm::DiagnosticCode::missing_semicolon, 3));

    const sm::ParseResult closed_by_brace = parse("[RIDGECOM]{A=1}");
    OA_CHECK(has_code_at(closed_by_brace, sm::DiagnosticCode::missing_semicolon, 1));
    OA_CHECK(closed_by_brace.rules.blocks.size() == 1);

    const sm::ParseResult spaced = parse("[*All*]{Worker=*7 (2,15);}");
    OA_CHECK(has_code(spaced, sm::DiagnosticCode::spaces_in_value));
    OA_CHECK(spaced.rules.blocks.size() == 1 && spaced.rules.blocks[0].changes.size() == 1);
    if (spaced.rules.blocks[0].changes.size() == 1) {
        const sm::Change& change = spaced.rules.blocks[0].changes[0];
        OA_CHECK(change.kind == sm::ChangeKind::scale_clamp);
        OA_CHECK(change.value == 7 && change.low == 2 && change.high == 15);
    }

    const sm::ParseResult no_needs = parse("[*Group*]{[Changes]{A=1;}}");
    OA_CHECK(has_code(no_needs, sm::DiagnosticCode::group_without_needs));
    OA_CHECK(no_needs.rules.blocks.size() == 1 && no_needs.rules.blocks[0].needs.empty());
    OA_CHECK(no_needs.rules.blocks[0].changes.size() == 1);

    const sm::ParseResult repeated = parse(
        "[*Group*]\n"
        "{\n"
        "[Needs]\n"
        "{A=1,1;}\n"
        "[Needs]\n"
        "{B=2,2;}\n"
        "[Changes]\n"
        "{C=1;}\n"
        "}\n"
    );
    OA_CHECK(has_code_at(repeated, sm::DiagnosticCode::repeated_section, 5));
    OA_CHECK(repeated.rules.blocks.size() == 1 && repeated.rules.blocks[0].needs.size() == 2);

    const std::string marked = std::string("\xEF\xBB\xBF") + "[*All*]{A=1;}";
    const sm::ParseResult bom = parse(marked);
    OA_CHECK(has_code_at(bom, sm::DiagnosticCode::byte_order_mark, 1));
    OA_CHECK(!bom.has_errors());
    OA_CHECK(bom.rules.blocks.size() == 1);
}

void errors() {
    expect_kept("[RIDGECOM]{[Foo]{A=1;}Kept=9;}", sm::DiagnosticCode::unknown_section, 1);
    expect_kept("[RIDGECOM]\n[RIDGEFAB]{Kept=9;}", sm::DiagnosticCode::unclosed_block, 1);
    expect_kept("{\n[RIDGECOM]{Kept=9;}", sm::DiagnosticCode::unexpected_brace, 1);
    expect_kept("[RIDGECOM]{folder=weapons;Kept=9;}", sm::DiagnosticCode::directive_in_block, 1);
    expect_kept("[RIDGECOM]{A=1e2;Kept=9;}", sm::DiagnosticCode::bad_number, 1);
    expect_kept("[RIDGECOM]{A=+1;Kept=9;}", sm::DiagnosticCode::bad_number, 1);
    expect_kept("[*Group*]{[Needs]{A=hello;}[Changes]{Kept=9;}}", sm::DiagnosticCode::bad_need, 1);
    expect_kept("[RIDGECOM]{A=(2);Kept=9;}", sm::DiagnosticCode::bad_change, 1);
    // A value that starts like a number keeps R01's reading when it is malformed.
    expect_kept("[RIDGECOM]{A=-x;Kept=9;}", sm::DiagnosticCode::bad_change, 1);
    expect_kept("[RIDGECOM]{A=1,2;Kept=9;}", sm::DiagnosticCode::need_form_in_changes, 1);
    expect_kept("[RIDGECOM]{A=\"x\"*;Kept=9;}", sm::DiagnosticCode::need_form_in_changes, 1);
    expect_kept(
        "[*Group*]{[Needs]{A=*2;}[Changes]{Kept=9;}}", sm::DiagnosticCode::change_form_in_needs, 1
    );
    expect_kept(
        "[*Group*]{[Needs]{A=\"\"+;}[Changes]{Kept=9;}}",
        sm::DiagnosticCode::change_form_in_needs,
        1
    );
    expect_kept(
        "[*Group*]{[Needs]{<AllDepth2>=1,2;}[Changes]{Kept=9;}}",
        sm::DiagnosticCode::change_form_in_needs,
        1
    );
    expect_kept("[RIDGECOM]{A=*2(5,1);Kept=9;}", sm::DiagnosticCode::clamp_low_above_high, 1);
    expect_kept("[RIDGECOM]{A=\"hi\nKept=9;}", sm::DiagnosticCode::unterminated_string, 1);
    expect_kept("/*\n[RIDGECOM]{Kept=9;}", sm::DiagnosticCode::block_comment, 1);
    expect_kept("hello\n[RIDGECOM]{Kept=9;}", sm::DiagnosticCode::stray_text, 1);

    const sm::ParseResult exponent = parse("[RIDGECOM]{A=1e2;Kept=9;}");
    OA_CHECK(exponent.rules.blocks.size() == 1);
    if (exponent.rules.blocks.size() == 1)
        OA_CHECK(change_named(exponent.rules.blocks[0], "A") == nullptr);

    std::string huge(sm::max_input_bytes + 1, 'x');
    const sm::ParseResult too_large = parse(huge);
    OA_CHECK(too_large.rules.blocks.empty());
    OA_CHECK(too_large.diagnostics.size() == 1);
    OA_CHECK(has_code(too_large, sm::DiagnosticCode::too_large));

    std::string many_blocks;
    many_blocks.reserve(4097u * 16u);
    for (int index = 0; index < 4096; ++index)
        many_blocks += "[A]{B=1;}";
    many_blocks += "[RIDGEFAB]{Kept=9;}\nhello\n";
    const sm::ParseResult too_many_blocks = parse(many_blocks);
    OA_CHECK(too_many_blocks.rules.blocks.size() == 4096);
    OA_CHECK(has_code(too_many_blocks, sm::DiagnosticCode::too_many_blocks));
    OA_CHECK(has_code(too_many_blocks, sm::DiagnosticCode::stray_text));
    OA_CHECK(!kept_nine(too_many_blocks));

    std::string many_statements = "[*All*]{";
    for (int index = 0; index < 4097; ++index)
        many_statements += "A=1;";
    many_statements += "}[RIDGEFAB]{Kept=9;}";
    const sm::ParseResult too_many_statements = parse(many_statements);
    OA_CHECK(has_code(too_many_statements, sm::DiagnosticCode::too_many_statements));
    OA_CHECK(kept_nine(too_many_statements));
    OA_CHECK(too_many_statements.rules.blocks.size() == 2);
    if (too_many_statements.rules.blocks.size() == 2)
        OA_CHECK(too_many_statements.rules.blocks[0].changes.size() == 4096);

    const std::string long_field(1025, 'Q');
    const sm::ParseResult long_name = parse("[RIDGECOM]{" + long_field + "=1;Kept=9;}");
    OA_CHECK(has_code(long_name, sm::DiagnosticCode::text_too_long));
    OA_CHECK(kept_nine(long_name));

    std::string noisy;
    for (int index = 0; index < 1001; ++index)
        noisy += "???\n";
    noisy += "[RIDGECOM]{Kept=9;}";
    const sm::ParseResult noisy_result = parse(noisy);
    OA_CHECK(noisy_result.diagnostics.size() == sm::max_diagnostics);
    OA_CHECK(!noisy_result.diagnostics.empty());
    if (!noisy_result.diagnostics.empty()) {
        OA_CHECK(noisy_result.diagnostics.back().code == sm::DiagnosticCode::too_many_diagnostics);
        OA_CHECK(noisy_result.diagnostics.back().severity == sm::Severity::error);
    }
    OA_CHECK(kept_nine(noisy_result));
}

void malformed() {
    const sm::ParseResult empty = parse("");
    OA_CHECK(empty.rules.blocks.empty());
    OA_CHECK(empty.diagnostics.empty());
    OA_CHECK(!empty.has_errors());

    const sm::ParseResult comments = parse("// only\n\n// comments\n");
    OA_CHECK(comments.rules.blocks.empty());
    OA_CHECK(comments.diagnostics.empty());

    const sm::ParseResult open = parse("[RIDGECOM]{A=\"hi");
    OA_CHECK(has_code(open, sm::DiagnosticCode::unterminated_string));
    OA_CHECK(open.rules.blocks.empty());

    const sm::ParseResult lone = parse("[\n[RIDGEFAB]{Kept=9;}");
    OA_CHECK(has_code(lone, sm::DiagnosticCode::unclosed_block));
    OA_CHECK(kept_nine(lone));

    const std::string nul("\0", 1);
    const sm::ParseResult nul_only = parse(nul);
    OA_CHECK(has_code(nul_only, sm::DiagnosticCode::stray_text));
    OA_CHECK(nul_only.rules.blocks.empty());
    const sm::ParseResult nul_then = parse(nul + "[RIDGECOM]{Kept=9;}");
    OA_CHECK(has_code(nul_then, sm::DiagnosticCode::stray_text));
    OA_CHECK(kept_nine(nul_then));

    OA_CHECK(sm::code_name(sm::DiagnosticCode::missing_semicolon) == "missing_semicolon");
    OA_CHECK(sm::code_name(sm::DiagnosticCode::too_many_diagnostics) == "too_many_diagnostics");

    std::string deep = "[*Group*]{";
    deep.reserve(deep.size() + 100000u * 8u);
    for (int index = 0; index < 100000; ++index)
        deep += "[Needs]{";
    const sm::ParseResult deep_result = parse(deep);
    OA_CHECK(deep_result.has_errors());
    OA_CHECK(has_code_at(deep_result, sm::DiagnosticCode::unknown_section, 1));

    const sm::ParseResult nested =
        parse("[*Group*]{[Needs]{A=1,1;[Changes]{B=2;}}}[RIDGECOM]{Kept=9;}");
    OA_CHECK(has_code(nested, sm::DiagnosticCode::unknown_section));
    OA_CHECK(kept_nine(nested));
    bool section_message = false;
    for (const sm::Diagnostic& item : nested.diagnostics) {
        if (item.code == sm::DiagnosticCode::unknown_section &&
            item.message.find("cannot hold a section") != std::string::npos)
            section_message = true;
    }
    OA_CHECK(section_message);
    OA_CHECK(nested.rules.blocks.size() == 2);
    if (nested.rules.blocks.size() == 2) {
        OA_CHECK(nested.rules.blocks[0].selector == sm::Selector::group);
        OA_CHECK(nested.rules.blocks[0].needs.size() == 1);
        OA_CHECK(nested.rules.blocks[0].needs[0].field == "A");
        OA_CHECK(change_named(nested.rules.blocks[0], "B") == nullptr);
        OA_CHECK(nested.rules.blocks[1].name == "RIDGECOM");
    }
}

void loose_in_group() {
    const sm::ParseResult before = parse("[*Group*]{\nA=5;\n[Needs]{C=1,1;}\n[Changes]{B=2;}\n}\n");
    OA_CHECK(has_code_at(before, sm::DiagnosticCode::stray_text, 2));
    OA_CHECK(before.rules.blocks.size() == 1);
    if (before.rules.blocks.size() == 1) {
        OA_CHECK(before.rules.blocks[0].selector == sm::Selector::group);
        OA_CHECK(before.rules.blocks[0].needs.size() == 1);
        OA_CHECK(before.rules.blocks[0].needs[0].field == "C");
        OA_CHECK(before.rules.blocks[0].changes.size() == 1);
        OA_CHECK(before.rules.blocks[0].changes[0].field == "B");
        OA_CHECK(change_named(before.rules.blocks[0], "A") == nullptr);
    }

    const sm::ParseResult after = parse("[*Group*]{\n[Needs]{C=1,1;}\n[Changes]{B=2;}\nA=5;\n}\n");
    OA_CHECK(has_code_at(after, sm::DiagnosticCode::stray_text, 4));
    OA_CHECK(after.rules.blocks.size() == 1);
    if (after.rules.blocks.size() == 1) {
        OA_CHECK(after.rules.blocks[0].needs.size() == 1);
        OA_CHECK(after.rules.blocks[0].changes.size() == 1);
        OA_CHECK(after.rules.blocks[0].changes[0].field == "B");
        OA_CHECK(change_named(after.rules.blocks[0], "A") == nullptr);
    }

    const sm::ParseResult promoted =
        parse("[RIDGECOM]{\nA=1;\n[Needs]{C=1,1;}\n[Changes]{B=2;}\n}\n");
    OA_CHECK(has_code_at(promoted, sm::DiagnosticCode::group_without_asterisks, 1));
    OA_CHECK(has_code_at(promoted, sm::DiagnosticCode::stray_text, 2));
    OA_CHECK(promoted.rules.blocks.size() == 1);
    if (promoted.rules.blocks.size() == 1) {
        OA_CHECK(promoted.rules.blocks[0].selector == sm::Selector::group);
        OA_CHECK(promoted.rules.blocks[0].name.empty());
        OA_CHECK(promoted.rules.blocks[0].needs.size() == 1);
        OA_CHECK(promoted.rules.blocks[0].needs[0].field == "C");
        OA_CHECK(promoted.rules.blocks[0].changes.size() == 1);
        OA_CHECK(promoted.rules.blocks[0].changes[0].field == "B");
        OA_CHECK(change_named(promoted.rules.blocks[0], "A") == nullptr);
    }
}

[[nodiscard]] const sm::Need* need_named(const sm::Block& block, std::string_view field) {
    for (const sm::Need& need : block.needs) {
        if (need.field == field)
            return &need;
    }
    return nullptr;
}

void lenient_forms() {
    const sm::ParseResult word = parse("[RIDGECOM]{Blast=ridgeburst;}\n");
    OA_CHECK(!word.has_errors());
    OA_CHECK(has_code_at(word, sm::DiagnosticCode::unquoted_text, 1));
    OA_CHECK(word.diagnostics.size() == 1);
    OA_CHECK(word.diagnostics[0].severity == sm::Severity::warning);
    OA_CHECK(word.rules.blocks.size() == 1 && word.rules.blocks[0].changes.size() == 1);
    if (word.rules.blocks.size() == 1 && word.rules.blocks[0].changes.size() == 1) {
        const sm::Change& change = word.rules.blocks[0].changes[0];
        OA_CHECK(change.field == "Blast");
        OA_CHECK(change.kind == sm::ChangeKind::assign_text);
        OA_CHECK(change.text == "ridgeburst");
        OA_CHECK(change.value == 0);
        OA_CHECK(change.line == 1);
    }

    const sm::ParseResult words = parse("[RIDGECOM]{Note=weight RIDGE 0;}\n");
    OA_CHECK(!words.has_errors());
    OA_CHECK(has_code_at(words, sm::DiagnosticCode::unquoted_text, 1));
    OA_CHECK(!has_code(words, sm::DiagnosticCode::spaces_in_value));
    OA_CHECK(words.rules.blocks.size() == 1 && words.rules.blocks[0].changes.size() == 1);
    if (words.rules.blocks[0].changes.size() == 1) {
        const sm::Change& change = words.rules.blocks[0].changes[0];
        OA_CHECK(change.kind == sm::ChangeKind::assign_text);
        OA_CHECK(change.text == "weight RIDGE 0");
    }

    const sm::ParseResult gaps = parse("[RIDGECOM]{Note=ridge  gate;}\n");
    OA_CHECK(gaps.rules.blocks.size() == 1 && gaps.rules.blocks[0].changes.size() == 1);
    if (gaps.rules.blocks[0].changes.size() == 1)
        OA_CHECK(gaps.rules.blocks[0].changes[0].text == "ridge  gate");

    const sm::ParseResult noted = parse("[RIDGECOM]{Note=weight RIDGE 0; // stays out\n}\n");
    OA_CHECK(noted.rules.blocks.size() == 1 && noted.rules.blocks[0].changes.size() == 1);
    if (noted.rules.blocks[0].changes.size() == 1) {
        OA_CHECK(noted.rules.blocks[0].changes[0].text == "weight RIDGE 0");
        OA_CHECK(noted.rules.blocks[0].changes[0].text.find("stays") == std::string::npos);
    }
    const sm::ParseResult commented = parse("[RIDGECOM]{Note=ridge gate // stays out\n}\n");
    OA_CHECK(has_code_at(commented, sm::DiagnosticCode::missing_semicolon, 1));
    OA_CHECK(commented.rules.blocks.size() == 1 && commented.rules.blocks[0].changes.size() == 1);
    if (commented.rules.blocks[0].changes.size() == 1)
        OA_CHECK(commented.rules.blocks[0].changes[0].text == "ridge gate");

    const sm::ParseResult beside = parse("[RIDGECOM]{\nBlast=ridgeburst;\nSpeed=2;\n}\n");
    OA_CHECK(!beside.has_errors());
    OA_CHECK(has_code_at(beside, sm::DiagnosticCode::unquoted_text, 2));
    OA_CHECK(count_code(beside, sm::DiagnosticCode::unquoted_text) == 1);
    OA_CHECK(beside.rules.blocks.size() == 1 && beside.rules.blocks[0].changes.size() == 2);
    if (beside.rules.blocks[0].changes.size() == 2) {
        const sm::Change& blast = beside.rules.blocks[0].changes[0];
        const sm::Change& speed = beside.rules.blocks[0].changes[1];
        OA_CHECK(blast.kind == sm::ChangeKind::assign_text && blast.text == "ridgeburst");
        OA_CHECK(blast.line == 2);
        OA_CHECK(speed.kind == sm::ChangeKind::assign && speed.value == 2 && speed.text.empty());
        OA_CHECK(speed.line == 3);
    }

    const sm::ParseResult empty = parse("[RIDGECOM]{Mark=;}\n");
    OA_CHECK(!empty.has_errors());
    OA_CHECK(has_code_at(empty, sm::DiagnosticCode::empty_value, 1));
    OA_CHECK(!has_code(empty, sm::DiagnosticCode::unquoted_text));
    OA_CHECK(empty.rules.blocks.size() == 1 && empty.rules.blocks[0].changes.size() == 1);
    if (empty.rules.blocks[0].changes.size() == 1) {
        const sm::Change& change = empty.rules.blocks[0].changes[0];
        OA_CHECK(change.kind == sm::ChangeKind::assign_text);
        OA_CHECK(change.text.empty());
        OA_CHECK(change.field == "Mark");
    }
    const sm::ParseResult removed = parse("[RIDGECOM]{Mark=\"\";}\n");
    OA_CHECK(!has_code(removed, sm::DiagnosticCode::empty_value));
    OA_CHECK(removed.rules.blocks.size() == 1 && removed.rules.blocks[0].changes.size() == 1);
    if (removed.rules.blocks[0].changes.size() == 1)
        OA_CHECK(removed.rules.blocks[0].changes[0].kind == sm::ChangeKind::remove);

    const sm::ParseResult needs = parse(
        "[*Group*]\n"
        "{\n"
        "[Needs]\n"
        "{\n"
        "Time=\"\";\n"
        "Weapon=!\"\";\n"
        "Present=\"\"*;\n"
        "Absent=!\"\"*;\n"
        "}\n"
        "[Changes]\n"
        "{\n"
        "Kept=1;\n"
        "}\n"
        "}\n"
    );
    OA_CHECK(!needs.has_errors());
    OA_CHECK(has_code_at(needs, sm::DiagnosticCode::empty_text_need, 5));
    OA_CHECK(has_code_at(needs, sm::DiagnosticCode::empty_text_need, 6));
    OA_CHECK(count_code(needs, sm::DiagnosticCode::empty_text_need) == 2);
    OA_CHECK(!has_code(needs, sm::DiagnosticCode::change_form_in_needs));
    OA_CHECK(needs.rules.blocks.size() == 1);
    if (needs.rules.blocks.size() == 1) {
        const sm::Block& block = needs.rules.blocks[0];
        OA_CHECK(block.needs.size() == 4);
        const sm::Need* time = need_named(block, "Time");
        const sm::Need* weapon = need_named(block, "Weapon");
        const sm::Need* present = need_named(block, "Present");
        const sm::Need* absent = need_named(block, "Absent");
        OA_CHECK(time != nullptr && weapon != nullptr && present != nullptr && absent != nullptr);
        if (time != nullptr && weapon != nullptr && present != nullptr && absent != nullptr) {
            OA_CHECK(time->kind == sm::NeedKind::equals && !time->negated && time->text.empty());
            OA_CHECK(time->line == 5);
            OA_CHECK(
                weapon->kind == sm::NeedKind::equals && weapon->negated && weapon->text.empty()
            );
            OA_CHECK(weapon->line == 6);
            OA_CHECK(
                present->kind == sm::NeedKind::contains && !present->negated &&
                present->text.empty()
            );
            OA_CHECK(
                absent->kind == sm::NeedKind::contains && absent->negated && absent->text.empty()
            );
        }
    }

    const sm::ParseResult after_last = parse("[RIDGECOM]{A=1;}\n}\n");
    OA_CHECK(!after_last.has_errors());
    OA_CHECK(has_code_at(after_last, sm::DiagnosticCode::stray_closing_brace, 2));
    OA_CHECK(after_last.diagnostics.size() == 1);
    OA_CHECK(after_last.diagnostics[0].severity == sm::Severity::warning);
    OA_CHECK(after_last.rules.blocks.size() == 1);
    if (after_last.rules.blocks.size() == 1) {
        OA_CHECK(after_last.rules.blocks[0].name == "RIDGECOM");
        OA_CHECK(after_last.rules.blocks[0].changes.size() == 1);
    }

    const sm::ParseResult after_middle = parse("[RIDGECOM]{A=1;}\n}\n[RIDGEFAB]{B=2;}\n}\n");
    OA_CHECK(!after_middle.has_errors());
    OA_CHECK(has_code_at(after_middle, sm::DiagnosticCode::stray_closing_brace, 2));
    OA_CHECK(has_code_at(after_middle, sm::DiagnosticCode::stray_closing_brace, 4));
    OA_CHECK(count_code(after_middle, sm::DiagnosticCode::stray_closing_brace) == 2);
    OA_CHECK(after_middle.rules.blocks.size() == 2);
    if (after_middle.rules.blocks.size() == 2) {
        OA_CHECK(after_middle.rules.blocks[0].name == "RIDGECOM");
        OA_CHECK(after_middle.rules.blocks[1].name == "RIDGEFAB");
        OA_CHECK(after_middle.rules.blocks[1].changes.size() == 1);
        OA_CHECK(after_middle.rules.blocks[1].changes[0].value == 2);
    }

    const sm::ParseResult leading = parse("}\n[RIDGECOM]{Kept=9;}\n");
    OA_CHECK(!leading.has_errors());
    OA_CHECK(has_code_at(leading, sm::DiagnosticCode::stray_closing_brace, 1));
    OA_CHECK(kept_nine(leading));

    // A '}' in the body ends that body. It is not ignored, so a statement
    // written after it is outside the block and stays an error. A '{' in
    // the body, and a '{' at the top level, stay errors as well.
    const sm::ParseResult ended_early = parse("[RIDGECOM]{\n}\nKept=9;\n}\n");
    OA_CHECK(ended_early.has_errors());
    OA_CHECK(has_code_at(ended_early, sm::DiagnosticCode::stray_text, 3));
    OA_CHECK(has_code_at(ended_early, sm::DiagnosticCode::stray_closing_brace, 4));
    OA_CHECK(!kept_nine(ended_early));
    OA_CHECK(ended_early.rules.blocks.size() == 1);
    if (ended_early.rules.blocks.size() == 1)
        OA_CHECK(ended_early.rules.blocks[0].changes.empty());

    const sm::ParseResult inner_open = parse("[RIDGECOM]{{\nKept=9;\n}\n");
    OA_CHECK(inner_open.has_errors());
    OA_CHECK(has_code_at(inner_open, sm::DiagnosticCode::unexpected_brace, 1));
    OA_CHECK(!has_code(inner_open, sm::DiagnosticCode::stray_closing_brace));
    OA_CHECK(kept_nine(inner_open));

    const sm::ParseResult top_open = parse("{\n[RIDGECOM]{Kept=9;}\n");
    OA_CHECK(top_open.has_errors());
    OA_CHECK(has_code_at(top_open, sm::DiagnosticCode::unexpected_brace, 1));
    OA_CHECK(!has_code(top_open, sm::DiagnosticCode::stray_closing_brace));
    OA_CHECK(kept_nine(top_open));

    const sm::ParseResult clamped = parse("[*All*]{Worker=*7 (2,15);}\n");
    OA_CHECK(has_code(clamped, sm::DiagnosticCode::spaces_in_value));
    OA_CHECK(!has_code(clamped, sm::DiagnosticCode::unquoted_text));
    OA_CHECK(clamped.rules.blocks.size() == 1 && clamped.rules.blocks[0].changes.size() == 1);
    if (clamped.rules.blocks[0].changes.size() == 1) {
        const sm::Change& change = clamped.rules.blocks[0].changes[0];
        OA_CHECK(change.kind == sm::ChangeKind::scale_clamp);
        OA_CHECK(change.value == 7 && change.low == 2 && change.high == 15);
    }

    // '*x' has no number token, so it stays a bad change. A '*' followed by
    // a malformed number stays a bad number.
    const sm::ParseResult star_x = parse("[RIDGECOM]{A=*x;Kept=9;}\n");
    OA_CHECK(star_x.has_errors());
    OA_CHECK(has_code_at(star_x, sm::DiagnosticCode::bad_change, 1));
    OA_CHECK(!has_code(star_x, sm::DiagnosticCode::bad_number));
    OA_CHECK(!has_code(star_x, sm::DiagnosticCode::unquoted_text));
    OA_CHECK(kept_nine(star_x));
    if (star_x.rules.blocks.size() == 1)
        OA_CHECK(change_named(star_x.rules.blocks[0], "A") == nullptr);

    const sm::ParseResult star_bad = parse("[RIDGECOM]{A=*1e2;Kept=9;}\n");
    OA_CHECK(has_code_at(star_bad, sm::DiagnosticCode::bad_number, 1));
    OA_CHECK(!has_code(star_bad, sm::DiagnosticCode::unquoted_text));
    OA_CHECK(kept_nine(star_bad));

    const sm::ParseResult bare_rule = parse("[RIDGECOM]{Note=ridge gate;}\n");
    const sm::ParseResult quoted_rule = parse("[RIDGECOM]{Note=\"ridge gate\";}\n");
    OA_CHECK(bare_rule.rules.blocks.size() == 1 && quoted_rule.rules.blocks.size() == 1);
    OA_CHECK(sm::equivalent(bare_rule.rules, quoted_rule.rules));
    OA_CHECK(sm::equivalent(bare_rule.rules.blocks[0], quoted_rule.rules.blocks[0]));
    const sm::ParseResult other_text = parse("[RIDGECOM]{Note=\"ridge  gate\";}\n");
    OA_CHECK(!sm::equivalent(bare_rule.rules, other_text.rules));

    OA_CHECK(sm::code_name(sm::DiagnosticCode::unquoted_text) == "unquoted_text");
    OA_CHECK(sm::code_name(sm::DiagnosticCode::empty_value) == "empty_value");
    OA_CHECK(sm::code_name(sm::DiagnosticCode::empty_text_need) == "empty_text_need");
    OA_CHECK(sm::code_name(sm::DiagnosticCode::stray_closing_brace) == "stray_closing_brace");
}

void same_rules() {
    const sm::ParseResult spaced = parse(
        "[*Group*]\n"
        "{\n"
        "[Needs]\n"
        "{\n"
        "Side=\"Core\";\n"
        "Count=1,2;\n"
        "}\n"
        "[Changes]\n"
        "{\n"
        "MaxDamage=5;\n"
        "Speed=*2;\n"
        "}\n"
        "}\n"
    );
    const sm::ParseResult tight =
        parse("[*GROUP*]{[changes]{speed=*2;maxdamage=5;}[needs]{count=1,2;side=\"core\";}}");
    OA_CHECK(sm::equivalent(spaced.rules, tight.rules));
    OA_CHECK(sm::equivalent(spaced.rules.blocks[0], tight.rules.blocks[0]));

    const sm::ParseResult other_number =
        parse("[*Group*]{[Needs]{Side=\"Core\";Count=1,2;}[Changes]{MaxDamage=6;Speed=*2;}}");
    OA_CHECK(!sm::equivalent(spaced.rules, other_number.rules));

    const sm::ParseResult negated =
        parse("[*Group*]{[Needs]{Side=\"Core\";Count=!1,2;}[Changes]{MaxDamage=5;Speed=*2;}}");
    OA_CHECK(!sm::equivalent(spaced.rules, negated.rules));

    const sm::ParseResult order = parse("[RIDGECOM]{MaxDamage=5;MaxDamage=6;}");
    const sm::ParseResult swapped = parse("[RIDGECOM]{MaxDamage=6;MaxDamage=5;}");
    OA_CHECK(!sm::equivalent(order.rules, swapped.rules));

    const sm::ParseResult text_case = parse("[RIDGECOM]{Name=\"Core\";}");
    const sm::ParseResult text_other = parse("[RIDGECOM]{Name=\"core\";}");
    OA_CHECK(!sm::equivalent(text_case.rules, text_other.rules));
}

[[nodiscard]] std::vector<uint8_t> read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    input.seekg(0, std::ios::end);
    const std::streamoff end = input.tellg();
    input.seekg(0);
    if (end < 0)
        return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(end));
    if (!bytes.empty())
        input.read(
            reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
    return bytes;
}

[[nodiscard]] int corpus_main(const char* dir) {
    std::error_code error;
    fs::directory_iterator cursor(fs::path(dir), error);
    std::vector<fs::path> paths;
    if (!error) {
        const fs::directory_iterator end;
        for (; cursor != end; cursor.increment(error)) {
            if (error)
                break;
            std::error_code file_error;
            if (!cursor->is_regular_file(file_error) || file_error)
                continue;
            if (cursor->path().extension() != ".sm")
                continue;
            paths.push_back(cursor->path());
        }
    }
    std::sort(paths.begin(), paths.end(), [](const fs::path& first, const fs::path& second) {
        return first.filename().generic_string() < second.filename().generic_string();
    });
    if (paths.empty()) {
        std::fprintf(stderr, "no rule files in %s\n", dir);
        std::printf("0 files, 0 blocks, 0 warnings, 0 errors\n");
        return 1;
    }
    unsigned long long blocks = 0;
    unsigned long long warnings = 0;
    unsigned long long error_count = 0;
    for (const fs::path& path : paths) {
        const std::string name = path.filename().string();
        std::ifstream probe(path, std::ios::binary);
        if (!probe) {
            std::printf("%s: 0 blocks, 0 warnings, 1 errors\n", name.c_str());
            ++error_count;
            continue;
        }
        const std::vector<uint8_t> bytes = read_file(path);
        const sm::ParseResult result = sm::parse_rules(bytes);
        unsigned long long file_warnings = 0;
        unsigned long long file_errors = 0;
        for (const sm::Diagnostic& item : result.diagnostics) {
            if (item.severity == sm::Severity::warning)
                ++file_warnings;
            else
                ++file_errors;
        }
        const auto file_blocks = static_cast<unsigned long long>(result.rules.blocks.size());
        std::printf(
            "%s: %llu blocks, %llu warnings, %llu errors\n",
            name.c_str(),
            file_blocks,
            file_warnings,
            file_errors
        );
        for (const sm::Diagnostic& item : result.diagnostics) {
            const char* severity = item.severity == sm::Severity::warning ? "warning" : "error";
            const std::string_view code = sm::code_name(item.code);
            std::printf(
                "%s %.*s:%u\n",
                severity,
                static_cast<int>(code.size()),
                code.data(),
                static_cast<unsigned>(item.line)
            );
        }
        blocks += file_blocks;
        warnings += file_warnings;
        error_count += file_errors;
    }
    const auto files = static_cast<unsigned long long>(paths.size());
    std::printf(
        "%llu files, %llu blocks, %llu warnings, %llu errors\n",
        files,
        blocks,
        warnings,
        error_count
    );
    return error_count == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--corpus")
        return corpus_main(argv[2]);
    if (argc != 1) {
        std::fprintf(stderr, "usage: oa-formats-soft-mutator-test [--corpus DIR]\n");
        return 2;
    }
    needs();
    changes();
    selectors();
    groups_without_asterisks();
    directives();
    layout();
    errors();
    malformed();
    loose_in_group();
    lenient_forms();
    same_rules();
    return oa::test::check_exit_status();
}
