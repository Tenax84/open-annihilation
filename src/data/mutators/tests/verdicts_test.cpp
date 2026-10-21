// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The dry-run verdicts and the lines they are remembered as.

#include "oa/base/sha256.hpp"
#include "oa/data/mutators/verdicts.hpp"
#include "oa/test/check.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace mutators = oa::data::mutators;
namespace sha = oa::base::sha256;

std::vector<uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

mutators::VerdictKey
key_of(std::string_view loadout, std::string_view mod, std::string_view engine) {
    mutators::VerdictKey key;
    key.loadout = *sha::parse_hex(loadout);
    key.mod = *sha::parse_hex(mod);
    key.engine_version = std::string{engine};
    return key;
}

std::string hex_of(const sha::Digest& digest) {
    const auto text = sha::to_hex(digest);
    return std::string{text.data(), text.size()};
}

constexpr std::string_view kLoad =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kMod =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view kOther =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

std::string good_line(std::string_view loadout, std::string_view word) {
    std::string line = "1\t";
    line += loadout;
    line += '\t';
    line += kMod;
    line += "\t1.0.0\t";
    line += word;
    line += "\t0,0,0,0,0,0,0,0,0,0,0,0\t\t";
    if (word == "incompatible" || word == "failed")
        line += "because";
    line += '\n';
    return line;
}

} // namespace

int main() {
    OA_CHECK(mutators::verdict_word(mutators::Verdict::compatible) == "compatible");
    OA_CHECK(mutators::verdict_word(mutators::Verdict::no_effect) == "no-effect");
    OA_CHECK(mutators::verdict_word(mutators::Verdict::incompatible) == "incompatible");
    OA_CHECK(mutators::verdict_word(mutators::Verdict::failed) == "failed");
    OA_CHECK(mutators::parse_verdict_word("compatible") == mutators::Verdict::compatible);
    OA_CHECK(mutators::parse_verdict_word("no-effect") == mutators::Verdict::no_effect);
    OA_CHECK(mutators::parse_verdict_word("incompatible") == mutators::Verdict::incompatible);
    OA_CHECK(mutators::parse_verdict_word("failed") == mutators::Verdict::failed);
    OA_CHECK(!mutators::parse_verdict_word("no_effect"));
    OA_CHECK(!mutators::parse_verdict_word("COMPATIBLE"));
    OA_CHECK(!mutators::parse_verdict_word(""));

    OA_CHECK(!mutators::any(mutators::ChangeCounts{}));
    mutators::ChangeCounts counts;
    counts.files_hidden = 1;
    OA_CHECK(mutators::any(counts));
    counts.files_hidden = 0;
    counts.units_changed = 53;
    OA_CHECK(mutators::any(counts));

    OA_CHECK(mutators::one_line_reason("a\tb\tc") == "a b c");
    OA_CHECK(mutators::one_line_reason("a\nb\rc") == "a b c");
    OA_CHECK(mutators::one_line_reason("  a \t  b  ") == "a b");
    OA_CHECK(mutators::one_line_reason(std::string(300, 'x')) == std::string(200, 'x'));
    const std::string cut = std::string(198, 'a') + "\xe2\x82\xac";
    OA_CHECK(mutators::one_line_reason(cut) == std::string(198, 'a'));
    OA_CHECK(mutators::one_line_reason(cut).size() == 198);
    const std::string space_cut = std::string(199, 'a') + " bcd";
    const auto space_once = mutators::one_line_reason(space_cut);
    OA_CHECK(space_once == std::string(199, 'a'));
    OA_CHECK(space_once.size() == 199);
    OA_CHECK(mutators::one_line_reason(space_once) == space_once);

    mutators::ChangeCounts full;
    full.units_changed = 53;
    full.units_added = 1;
    full.units_removed = 2;
    full.weapons_changed = 3;
    full.weapons_added = 4;
    full.weapons_removed = 5;
    full.features_changed = 6;
    full.features_added = 7;
    full.features_removed = 8;
    full.menu_entries_changed = 9;
    full.files_added = 10;
    full.files_hidden = 11;

    std::vector<mutators::RememberedVerdict> four(4);
    four[0].key = key_of(kLoad, kMod, "1.0.0");
    four[0].record.verdict = mutators::Verdict::compatible;
    four[0].record.counts = full;
    four[0].record.mod_label = "Ridge 28";
    four[1].key = key_of(kOther, kMod, "1.0.0");
    four[1].record.verdict = mutators::Verdict::no_effect;
    four[2].key = key_of(kLoad, kOther, "1.0.0");
    four[2].record.verdict = mutators::Verdict::incompatible;
    four[2].record.reason = "the hook refused a unit";
    four[2].record.mod_label = "Ridge 28";
    four[3].key = key_of(kOther, kOther, "1.0.1");
    four[3].record.verdict = mutators::Verdict::failed;
    four[3].record.reason = "the list was refused";
    four[3].record.mod_label = "Ridge 28";

    const auto written = mutators::write_verdicts(four);
    const auto read_back = mutators::read_verdicts(written);
    OA_CHECK(read_back.size() == 4);
    OA_CHECK(read_back[0].key == four[0].key);
    OA_CHECK(read_back[0].record == four[0].record);
    OA_CHECK(read_back[1].key == four[1].key);
    OA_CHECK(read_back[1].record == four[1].record);
    OA_CHECK(read_back[2].key == four[2].key);
    OA_CHECK(read_back[2].record == four[2].record);
    OA_CHECK(read_back[3].key == four[3].key);
    OA_CHECK(read_back[3].record == four[3].record);

    const std::string one_line =
        "1\taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\t"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\t"
        "1.0.0\tcompatible\t53,1,2,3,4,5,6,7,8,9,10,11\tRidge 28\t\n";
    OA_CHECK(
        std::string(
            written.begin(), written.begin() + static_cast<std::ptrdiff_t>(one_line.size())
        ) == one_line
    );

    std::vector<mutators::RememberedVerdict> many;
    mutators::VerdictKey oldest;
    for (uint32_t index = 0; index < mutators::verdicts_most_lines + 1; ++index) {
        mutators::RememberedVerdict verdict;
        verdict.key.engine_version = "1";
        verdict.key.loadout[0] = static_cast<uint8_t>(index & 0xffu);
        verdict.key.loadout[1] = static_cast<uint8_t>((index >> 8) & 0xffu);
        verdict.record.verdict = mutators::Verdict::no_effect;
        if (index == 0)
            oldest = verdict.key;
        mutators::remember_verdict(many, verdict);
    }
    OA_CHECK(many.size() == mutators::verdicts_most_lines);
    OA_CHECK(mutators::find_verdict(many, oldest) == nullptr);
    OA_CHECK(
        many.back().key.loadout[0] == static_cast<uint8_t>(mutators::verdicts_most_lines & 0xffu)
    );
    OA_CHECK(many.front().key.loadout[0] == 1);

    std::vector<mutators::RememberedVerdict> longer = many;
    mutators::RememberedVerdict extra;
    extra.key.engine_version = "1";
    extra.key.loadout[2] = 1;
    extra.record.verdict = mutators::Verdict::failed;
    extra.record.reason = "late";
    longer.insert(longer.begin(), extra);
    const auto trimmed = mutators::read_verdicts(mutators::write_verdicts(longer));
    OA_CHECK(trimmed.size() == mutators::verdicts_most_lines);
    OA_CHECK(mutators::find_verdict(trimmed, extra.key) == nullptr);
    OA_CHECK(trimmed.front().key == many.front().key);

    mutators::RememberedVerdict again;
    again.key = many.front().key;
    again.record.verdict = mutators::Verdict::incompatible;
    again.record.reason = "replaced";
    mutators::remember_verdict(many, again);
    OA_CHECK(many.back().key == again.key);
    OA_CHECK(many.back().record.verdict == mutators::Verdict::incompatible);
    OA_CHECK(many.back().record.reason == "replaced");
    OA_CHECK(many.front().key != again.key);

    const std::string first = good_line(kLoad, "compatible");
    const std::string second = good_line(kOther, "no-effect");
    const std::string later = good_line(kLoad, "failed");
    const auto dup = mutators::read_verdicts(bytes_of(first + second + later));
    OA_CHECK(dup.size() == 2);
    OA_CHECK(dup[0].record.verdict == mutators::Verdict::failed);
    OA_CHECK(dup[0].record.reason == "because");
    OA_CHECK(dup[1].record.verdict == mutators::Verdict::no_effect);
    OA_CHECK(hex_of(dup[0].key.loadout) == kLoad);

    std::vector<uint8_t> too_big(mutators::verdicts_file_most_bytes + 1, uint8_t{'a'});
    OA_CHECK(mutators::read_verdicts(too_big).empty());
    OA_CHECK(mutators::verdicts_file_name == "mutator-verdicts.tsv");

    const std::string kept_before = good_line(kLoad, "no-effect");
    const std::string kept_after = good_line(kOther, "failed");
    const auto keepers_only = [&](const std::string& bad) {
        const auto got = mutators::read_verdicts(bytes_of(kept_before + bad + kept_after));
        OA_CHECK(got.size() == 2);
        OA_CHECK(got[0].key == key_of(kLoad, kMod, "1.0.0"));
        OA_CHECK(got[0].record.verdict == mutators::Verdict::no_effect);
        OA_CHECK(got[0].record.reason.empty());
        OA_CHECK(got[0].record.mod_label.empty());
        OA_CHECK(!mutators::any(got[0].record.counts));
        OA_CHECK(got[1].key == key_of(kOther, kMod, "1.0.0"));
        OA_CHECK(got[1].record.verdict == mutators::Verdict::failed);
        OA_CHECK(got[1].record.reason == "because");
        OA_CHECK(got[1].record.mod_label.empty());
    };
    keepers_only(
        std::string{"1\t"} + "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
        "\t" + std::string{kMod} + "\tupper.1\tfailed\t7,0,0,0,0,0,0,0,0,0,0,0\t\tZZ\n"
    );
    keepers_only(
        "1\tabc\t" + std::string{kMod} + "\tshort.1\tfailed\t0,0,0,0,0,0,0,0,0,0,0,0\t\tZZ\n"
    );
    keepers_only(
        "1\t" + std::string{kLoad} + "\t" + std::string{kMod} +
        "\thas space\tfailed\t0,0,0,0,0,0,0,0,0,0,0,0\t\tZZ\n"
    );
    keepers_only(
        "1\t" + std::string{kLoad} + "\t" + std::string{kMod} +
        "\tword.1\tnope\t0,0,0,0,0,0,0,0,0,0,0,0\t\tZZ\n"
    );
    keepers_only(
        "1\t" + std::string{kLoad} + "\t" + std::string{kMod} +
        "\tzero.1\tno-effect\t01,0,0,0,0,0,0,0,0,0,0,0\t\t\n"
    );
    keepers_only(
        "1\t" + std::string{kLoad} + "\t" + std::string{kMod} +
        "\tover.1\tno-effect\t4294967296,0,0,0,0,0,0,0,0,0,0,0\t\t\n"
    );
    keepers_only(
        "1\t" + std::string{kLoad} + "\t" + std::string{kMod} +
        "\tcomp.1\tcompatible\t0,0,0,0,0,0,0,0,0,0,0,0\t\tnope\n"
    );
    keepers_only(
        "1\t" + std::string{kLoad} + "\t" + std::string{kMod} +
        "\tquiet.1\tno-effect\t0,0,0,0,0,0,0,0,0,0,0,0\t\tnope\n"
    );
    keepers_only(
        good_line(kLoad, "failed").substr(0, good_line(kLoad, "failed").size() - 1) + "\r\n"
    );
    keepers_only(
        "1\t" + std::string{kLoad} + "\t" + std::string{kMod} +
        "\tutf.1\tfailed\t0,0,0,0,0,0,0,0,0,0,0,0\t\tbad\xff\n"
    );
    const std::string nine = good_line(kLoad, "failed");
    keepers_only(nine.substr(0, nine.size() - 1) + "\tmore\n");
    keepers_only(
        "2\t" + std::string{kLoad} + "\t" + std::string{kMod} +
        "\tform.1\tfailed\t0,0,0,0,0,0,0,0,0,0,0,0\t\tbecause\n"
    );
    keepers_only(std::string(mutators::verdict_line_most_bytes + 1, 'x') + "\n");

    std::string many_lines;
    sha::Digest first_load{};
    sha::Digest second_load{};
    for (uint32_t index = 0; index < mutators::verdicts_most_lines + 1; ++index) {
        sha::Digest load{};
        load[0] = static_cast<uint8_t>(index & 0xffu);
        load[1] = static_cast<uint8_t>((index >> 8) & 0xffu);
        load[2] = 9;
        if (index == 0)
            first_load = load;
        if (index == 1)
            second_load = load;
        many_lines += good_line(hex_of(load), "no-effect");
    }
    const auto newest = mutators::read_verdicts(bytes_of(many_lines));
    OA_CHECK(newest.size() == mutators::verdicts_most_lines);
    sha::Digest last_load{};
    last_load[0] = static_cast<uint8_t>(mutators::verdicts_most_lines & 0xffu);
    last_load[1] = static_cast<uint8_t>((mutators::verdicts_most_lines >> 8) & 0xffu);
    last_load[2] = 9;
    OA_CHECK(newest.front().key.loadout == second_load);
    OA_CHECK(newest.back().key.loadout == last_load);
    OA_CHECK(mutators::find_verdict(newest, key_of(hex_of(first_load), kMod, "1.0.0")) == nullptr);

    const auto* hit = mutators::find_verdict(four, four[2].key);
    OA_CHECK(hit != nullptr);
    OA_CHECK(hit->verdict == mutators::Verdict::incompatible);
    mutators::VerdictKey missing = four[2].key;
    missing.engine_version = "9";
    OA_CHECK(mutators::find_verdict(four, missing) == nullptr);

    OA_CHECK(mutators::read_verdicts(std::span<const uint8_t>{}).empty());
    return oa::test::check_exit_status();
}
