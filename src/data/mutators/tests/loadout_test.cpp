// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The loadout record: its canonical text, the digests computed outside the
// engine, and the text of an entry's options.

#include "oa/data/mutators/loadout.hpp"

#include "oa/base/sha256.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/pipeline.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {

namespace fs = std::filesystem;
namespace mutators = oa::data::mutators;
namespace oamod = oa::formats::oamod;
namespace oamut = oa::formats::oamut;
namespace sha = oa::base::sha256;

/// The canonical text of the fixed two-entry loadout, one of them a folder.
constexpr std::string_view kCanonical =
    R"({"mutators":[{"clashes":"replace","folder":false,"id":"ridge-pace","options":{"boost":false,"pace":1.5,"temper":"hot"},"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","version":"1.2"},{"clashes":"replace","folder":true,"id":"ridge-kit","options":{},"sha256":"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210","version":"3"}],"oaloadout":1})";

/// Prints a failed requirement before counting it.
///
/// @param condition the requirement
/// @param message what failed
void expect(bool condition, const std::string& message) {
    if (!condition)
        std::fprintf(stderr, "loadout: %s\n", message.c_str());
    OA_CHECK(condition);
}

/// Reads a number from its canonical text.
///
/// @param text the number
/// @return the number
oamod::Number number_of(std::string_view text) {
    oamod::Number number{};
    if (oamod::parse_number(text, number) != oamod::NumberStatus::ok)
        throw std::runtime_error("not a number: " + std::string{text});
    return number;
}

/// Reads a digest from lower-case hex.
///
/// @param hex 64 hex digits
/// @return the digest
sha::Digest digest_of_hex(std::string_view hex) {
    const std::optional<sha::Digest> digest = sha::parse_hex(hex);
    if (!digest)
        throw std::runtime_error("not a digest");
    return *digest;
}

/// Returns 32 bytes of one value.
///
/// @param byte the byte repeated through the digest
/// @return the digest
sha::Digest filled_digest(uint8_t byte) {
    sha::Digest digest{};
    digest.fill(byte);
    return digest;
}

/// Writes a digest as hex for a failure message.
///
/// @param digest the digest
/// @return 64 lower-case hex digits
std::string hex_of(const sha::Digest& digest) {
    const auto digits = sha::to_hex(digest);
    return std::string(digits.begin(), digits.end());
}

/// A number option whose value is the given canonical number.
///
/// @param id the option id
/// @param text the canonical number
/// @return the option
mutators::LoadoutOption number_option(std::string_view id, std::string_view text) {
    mutators::LoadoutOption option;
    option.id = std::string{id};
    option.type = oamut::OptionType::number;
    option.value.number = true;
    option.value.number_value = number_of(text);
    option.value.text = oamod::canonical_number_text(option.value.number_value);
    return option;
}

/// A toggle option.
///
/// @param id the option id
/// @param enabled the toggle's value
/// @return the option
mutators::LoadoutOption toggle_option(std::string_view id, bool enabled) {
    mutators::LoadoutOption option;
    option.id = std::string{id};
    option.type = oamut::OptionType::toggle;
    option.toggle = enabled;
    return option;
}

/// A string choice.
///
/// @param id the option id
/// @param text the choice's text
/// @return the option
mutators::LoadoutOption choice_option(std::string_view id, std::string_view text) {
    mutators::LoadoutOption option;
    option.id = std::string{id};
    option.type = oamut::OptionType::choice;
    option.value.text = std::string{text};
    return option;
}

/// The fixed two-entry loadout the literal and the digests use.
///
/// @return the loadout, options in id byte order
mutators::Loadout sample_loadout() {
    mutators::LoadoutEntry pace;
    pace.id = "ridge-pace";
    pace.version = "1.2";
    pace.package =
        digest_of_hex("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    pace.options.push_back(toggle_option("boost", false));
    pace.options.push_back(number_option("pace", "1.5"));
    pace.options.push_back(choice_option("temper", "hot"));

    mutators::LoadoutEntry kit;
    kit.id = "ridge-kit";
    kit.version = "3";
    kit.folder = true;
    kit.package = digest_of_hex("fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210");

    mutators::Loadout loadout;
    loadout.entries.push_back(std::move(pace));
    loadout.entries.push_back(std::move(kit));
    return loadout;
}

/// The fixed layer the layer digest and the mutator digest use.
///
/// One served file, one hidden file and one mask.
///
/// @return the layer
oa::MutatorLayerSpec fixed_layer() {
    oa::MutatorLayerEntry served;
    served.path = "units/ridge.fbi";
    served.owner = "ridge-pace";
    served.kind = oa::MutatorSourceKind::loose;
    oa::MutatorLayerEntry hidden;
    hidden.path = "weapons/old.tdf";
    hidden.owner = "ridge-kit";
    hidden.kind = oa::MutatorSourceKind::hidden;
    oa::MutatorMask mask;
    mask.pattern = "ai/*.txt";
    mask.owner = "ridge-pace";

    oa::MutatorLayerSpec spec;
    spec.label = "mutators";
    spec.entries.push_back(std::move(served));
    spec.entries.push_back(std::move(hidden));
    spec.masks.push_back(std::move(mask));
    return spec;
}

/// The bytes the fixed layer serves.
///
/// @param path the path as the layer stores it
/// @return `unit-a` for the served file, empty otherwise
std::vector<uint8_t> fixed_served(std::string_view path) {
    if (path == "units/ridge.fbi")
        return {'u', 'n', 'i', 't', '-', 'a'};
    return {};
}

/// Checks the literal canonical form and that it reads back equal.
void canonical_form() {
    const mutators::Loadout loadout = sample_loadout();
    const std::string written = mutators::canonical_loadout(loadout);
    if (written != kCanonical)
        std::fprintf(stderr, "loadout canonical:\n%s\n", written.c_str());
    expect(written == kCanonical, "the canonical form equals the literal");

    mutators::Loadout reordered = loadout;
    std::swap(reordered.entries[0].options[0], reordered.entries[0].options[2]);
    expect(
        mutators::canonical_loadout(reordered) == written,
        "option order does not change the canonical text"
    );

    std::string error = "untouched";
    const std::optional<mutators::Loadout> read = mutators::parse_loadout(written, &error);
    expect(read.has_value(), "the canonical form reads back");
    if (read)
        expect(*read == loadout, "the canonical form reads back equal");
    expect(error == "untouched", "a loadout that reads leaves the error untouched");
    expect(mutators::parse_loadout(written, nullptr).has_value(), "a null error still reads");

    const mutators::Loadout empty;
    const std::string empty_text = mutators::canonical_loadout(empty);
    expect(empty_text == R"({"mutators":[],"oaloadout":1})", "an empty loadout has one text");
    const std::optional<mutators::Loadout> read_empty = mutators::parse_loadout(empty_text, &error);
    expect(read_empty.has_value() && *read_empty == empty, "an empty loadout reads back");
}

/// Checks the digests against values computed outside the engine.
void digests() {
    const mutators::Loadout loadout = sample_loadout();
    // python3 - <<'PY'
    // import hashlib
    // text = '{"mutators":[{"clashes":"replace","folder":false,"id":"ridge-pace","options":{"boost":false,"pace":1.5,"temper":"hot"},"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","version":"1.2"},{"clashes":"replace","folder":true,"id":"ridge-kit","options":{},"sha256":"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210","version":"3"}],"oaloadout":1}'
    // print(hashlib.sha256(text.encode()).hexdigest())
    // PY
    const sha::Digest loadout_pin =
        digest_of_hex("6e25afc8642291d65eb72bedb934432c87522677246f5c62bdc51d4462c58fee");
    const sha::Digest got_loadout = mutators::loadout_digest(loadout);
    if (got_loadout != loadout_pin)
        std::fprintf(stderr, "loadout digest %s\n", hex_of(got_loadout).c_str());
    expect(got_loadout == loadout_pin, "loadout_digest matches the independent digest");

    mutators::Loadout swapped = loadout;
    std::swap(swapped.entries[0], swapped.entries[1]);
    expect(
        mutators::loadout_digest(swapped) != got_loadout, "reordering entries changes the digest"
    );

    // python3 - <<'PY'
    // import hashlib
    // message = bytearray(b"oa-mutator-layer-1\n")
    // message += b"units/ridge.fbi\0F" + hashlib.sha256(b"unit-a").digest()
    // message += b"weapons/old.tdf\0H"
    // message += b"M" + b"ai/*.txt" + b"\0"
    // print(hashlib.sha256(message).hexdigest())
    // PY
    bool hidden_read = false;
    const auto served = [&hidden_read](std::string_view path) {
        if (path == "weapons/old.tdf")
            hidden_read = true;
        return fixed_served(path);
    };
    const oa::MutatorLayerSpec layer = fixed_layer();
    const sha::Digest layer_pin =
        digest_of_hex("0f193230fd2df02e7fee7ea8c2a5da796ecb40da04d817c4ae1dc177307d6d95");
    const sha::Digest got_layer = mutators::layer_digest(layer, served);
    if (got_layer != layer_pin)
        std::fprintf(stderr, "layer digest %s\n", hex_of(got_layer).c_str());
    expect(got_layer == layer_pin, "layer_digest matches the independent digest");
    expect(!hidden_read, "a hidden entry is not read");

    oa::MutatorLayerSpec reversed = layer;
    std::swap(reversed.entries[0], reversed.entries[1]);
    reversed.label = "other";
    reversed.entries[0].owner = "other";
    reversed.entries[1].owner = "other";
    expect(
        mutators::layer_digest(reversed, fixed_served) == got_layer,
        "layer_digest ignores entry order, owners and the label"
    );

    oa::MutatorLayerSpec folded = layer;
    folded.entries[0].path = "Units\\RIDGE.fbi";
    folded.entries[1].path = "Weapons\\OLD.TDF";
    folded.masks[0].pattern = "AI\\*.TXT";
    const auto folded_served = [](std::string_view path) -> std::vector<uint8_t> {
        if (path == "Units\\RIDGE.fbi")
            return {'u', 'n', 'i', 't', '-', 'a'};
        return {};
    };
    expect(
        mutators::layer_digest(folded, folded_served) == got_layer,
        "layer_digest folds case and slashes"
    );

    const auto other_byte = [](std::string_view) -> std::vector<uint8_t> {
        return {'u', 'n', 'i', 't', '-', 'b'};
    };
    expect(
        mutators::layer_digest(layer, other_byte) != got_layer,
        "one served byte changes the layer digest"
    );
    oa::MutatorLayerSpec no_hidden = layer;
    no_hidden.entries.pop_back();
    expect(
        mutators::layer_digest(no_hidden, fixed_served) != got_layer,
        "a hidden entry changes the layer digest"
    );
    oa::MutatorLayerSpec no_mask = layer;
    no_mask.masks.clear();
    expect(
        mutators::layer_digest(no_mask, fixed_served) != got_layer,
        "a mask changes the layer digest"
    );

    // python3 - <<'PY'
    // import hashlib
    // text = b'{"mutators":[{"clashes":"replace","folder":false,"id":"ridge-pace","options":{"boost":false,"pace":1.5,"temper":"hot"},"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","version":"1.2"},{"clashes":"replace","folder":true,"id":"ridge-kit","options":{},"sha256":"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210","version":"3"}],"oaloadout":1}'
    // layer = bytes.fromhex("0f193230fd2df02e7fee7ea8c2a5da796ecb40da04d817c4ae1dc177307d6d95")
    // print(hashlib.sha256(b"oa-mutators-1\n" + text + layer).hexdigest())
    // PY
    const sha::Digest mutator_pin =
        digest_of_hex("3368faef830f6ac5f02909bc9ba97b7a565a10572725cb291093cec67d40bee5");
    const sha::Digest got_mutator = mutators::mutator_digest(loadout, got_layer);
    if (got_mutator != mutator_pin)
        std::fprintf(stderr, "mutator digest %s\n", hex_of(got_mutator).c_str());
    expect(got_mutator == mutator_pin, "mutator_digest matches the independent digest");

    const sha::Digest sim = filled_digest(0x11);
    expect(
        mutators::fold_sim_hash(sim, std::nullopt) == sim, "no digest leaves the sim hash unchanged"
    );
    // python3 - <<'PY'
    // import hashlib
    // print(hashlib.sha256(b"oa-sim-hash-mutators-1\n" + bytes([0x11]) * 32 + bytes([0x22]) * 32).hexdigest())
    // PY
    const sha::Digest fold_pin =
        digest_of_hex("c296eda44c723205c958fd659373cde84d35935d6dd7a96b6dedb91e28b56c0c");
    const sha::Digest folded_hash = mutators::fold_sim_hash(sim, filled_digest(0x22));
    expect(folded_hash != sim, "a digest changes the sim hash");
    if (folded_hash != fold_pin)
        std::fprintf(stderr, "folded hash %s\n", hex_of(folded_hash).c_str());
    expect(folded_hash == fold_pin, "fold_sim_hash matches the independent digest");

    mutators::Loadout added = loadout;
    added.entries[0].clashes = oamut::ClashMode::add;
    const std::string added_text = mutators::canonical_loadout(added);
    expect(
        added_text.find("{\"mutators\":[{\"clashes\":\"add\",\"folder\":false,") == 0,
        "an added mutator writes clashes add"
    );
    expect(
        added_text.find("\"clashes\":\"replace\",\"folder\":true") != std::string::npos,
        "the other entry stays replace"
    );
    // python3 - <<'PY'
    // import hashlib
    // text = '{"mutators":[{"clashes":"add","folder":false,"id":"ridge-pace","options":{"boost":false,"pace":1.5,"temper":"hot"},"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","version":"1.2"},{"clashes":"replace","folder":true,"id":"ridge-kit","options":{},"sha256":"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210","version":"3"}],"oaloadout":1}'
    // print(hashlib.sha256(text.encode()).hexdigest())
    // layer = bytes.fromhex("0f193230fd2df02e7fee7ea8c2a5da796ecb40da04d817c4ae1dc177307d6d95")
    // print(hashlib.sha256(b"oa-mutators-1\n" + text.encode() + layer).hexdigest())
    // PY
    const sha::Digest add_loadout_pin =
        digest_of_hex("1e3d26a32403c2e73104763887577a5e1e3260d467175384e7b03d7737d3256e");
    const sha::Digest add_mutator_pin =
        digest_of_hex("daefb0c197b4b102e224ff8085a0b5a55292416103a73bc271a32f61ecbec325");
    const sha::Digest got_add = mutators::loadout_digest(added);
    if (got_add != add_loadout_pin)
        std::fprintf(stderr, "add loadout digest %s\n", hex_of(got_add).c_str());
    expect(got_add == add_loadout_pin, "an add-mode loadout matches its independent digest");
    expect(got_add != got_loadout, "the clash mode changes the loadout digest");
    const sha::Digest got_add_mutator = mutators::mutator_digest(added, got_layer);
    if (got_add_mutator != add_mutator_pin)
        std::fprintf(stderr, "add mutator digest %s\n", hex_of(got_add_mutator).c_str());
    expect(got_add_mutator == add_mutator_pin, "an add-mode mutator digest matches");
    expect(!(added == loadout), "entries that differ only in clashes are unequal");
    std::string add_error;
    const std::optional<mutators::Loadout> read_add =
        mutators::parse_loadout(added_text, &add_error);
    expect(
        read_add.has_value() && *read_add == added, "an add-mode loadout reads back: " + add_error
    );
}

/// Checks that a refusal names what was wrong.
///
/// @param text the text
/// @param needle a word the error must hold
void refuses(std::string_view text, std::string_view needle) {
    std::string error;
    const std::optional<mutators::Loadout> read = mutators::parse_loadout(text, &error);
    expect(!read.has_value(), std::string{needle} + " is refused");
    expect(error.find(needle) != std::string::npos, "the error names " + std::string{needle});
    expect(!mutators::parse_loadout(text, nullptr).has_value(), "a null error still refuses");
}

/// Checks the texts parse_loadout refuses, and the two limits' boundaries.
void refusals() {
    std::string missing{kCanonical};
    const std::string member = "\"clashes\":\"replace\",";
    const std::size_t member_at = missing.find(member);
    expect(member_at != std::string::npos, "the fixture names clashes");
    if (member_at != std::string::npos)
        missing.erase(member_at, member.size());
    refuses(missing, "clashes");

    std::string bad{kCanonical};
    const std::string replace_mode = "\"clashes\":\"replace\"";
    const std::size_t mode_at = bad.find(replace_mode);
    expect(mode_at != std::string::npos, "the fixture's clash mode is replace");
    if (mode_at != std::string::npos)
        bad.replace(mode_at, replace_mode.size(), "\"clashes\":\"Add\"");
    refuses(bad, "clashes");

    refuses(std::string{kCanonical}.replace(kCanonical.rfind(":1}"), 3, ":2}"), "oaloadout");
    refuses(R"({"extra":false,"mutators":[],"oaloadout":1})", "extra");

    std::string upper{kCanonical};
    const std::size_t letter = upper.find("abcdef");
    expect(letter != std::string::npos, "the fixture has a hex letter");
    upper[letter] = 'A';
    refuses(upper, "sha256");

    std::string spaced{kCanonical};
    const std::size_t colon = spaced.find(':');
    spaced.insert(colon + 1, " ");
    refuses(spaced, "canonical");
    refuses(std::string{kCanonical} + "x", "trailing");

    mutators::Loadout many;
    for (int index = 0; index < 33; ++index) {
        mutators::LoadoutEntry entry;
        entry.id = "m";
        entry.version = "1";
        many.entries.push_back(std::move(entry));
    }
    const std::string too_many = mutators::canonical_loadout(many);
    expect(
        too_many.size() <= mutators::loadout_text_most_bytes, "33 entries fit in the text limit"
    );
    refuses(too_many, "32");
    many.entries.pop_back();
    std::string limit_error;
    const std::optional<mutators::Loadout> thirty_two =
        mutators::parse_loadout(mutators::canonical_loadout(many), &limit_error);
    expect(thirty_two.has_value() && thirty_two->entries.size() == 32, "32 entries are read");

    const std::string over(mutators::loadout_text_most_bytes + 1, 'x');
    refuses(over, "65529");
    std::string boundary_error;
    const std::string boundary(mutators::loadout_text_most_bytes, 'x');
    expect(
        !mutators::parse_loadout(boundary, &boundary_error).has_value(),
        "the limit of junk is refused"
    );
    expect(boundary_error.find("65529") == std::string::npos, "the limit is inside the size limit");
}

/// Checks default_option for a toggle, a choice and a number.
void defaults() {
    oamut::Option toggle;
    toggle.id = "armed";
    toggle.type = oamut::OptionType::toggle;
    toggle.toggle_default = true;
    const mutators::LoadoutOption armed = mutators::default_option(toggle);
    expect(armed == toggle_option("armed", true), "a toggle default is its toggle");

    oamut::Option choice;
    choice.id = "temper";
    choice.type = oamut::OptionType::choice;
    oamut::ChoiceValue slow;
    slow.text = "slow";
    oamut::ChoiceValue hot;
    hot.text = "hot";
    oamut::ChoiceValue two;
    two.number = true;
    two.number_value = number_of("2");
    two.text = "2";
    choice.values = {slow, two, hot};
    choice.default_index = 1;
    const mutators::LoadoutOption temper = mutators::default_option(choice);
    expect(temper.type == oamut::OptionType::choice, "a choice default keeps the choice type");
    expect(
        temper.value.number && temper.value.text == "2", "a numeric choice default is that number"
    );

    choice.default_index = 2;
    expect(
        mutators::default_option(choice).value.text == "hot",
        "a string choice default is that string"
    );

    oamut::Option rate;
    rate.id = "rate";
    rate.type = oamut::OptionType::number;
    rate.minimum = number_of("0");
    rate.maximum = number_of("10");
    rate.step = number_of("0.25");
    rate.number_default = number_of("2.25");
    const mutators::LoadoutOption made = mutators::default_option(rate);
    expect(made == number_option("rate", "2.25"), "a number default keeps its canonical text");
    expect(made.value.text == "2.25", "a fractional step's default text is canonical");
}

/// Wraps one option in a one-entry loadout.
///
/// @param option the option
/// @return the loadout
mutators::Loadout one_option(const mutators::LoadoutOption& option) {
    mutators::LoadoutEntry entry;
    entry.id = "ridge-rate";
    entry.version = "1";
    entry.options.push_back(option);
    mutators::Loadout loadout;
    loadout.entries.push_back(std::move(entry));
    return loadout;
}

/// Checks that a loadout's canonical text reads back to an equal record.
///
/// @param loadout the loadout
/// @param what the case, for the failure message
void reads_back_equal(const mutators::Loadout& loadout, const std::string& what) {
    const std::string text = mutators::canonical_loadout(loadout);
    std::string error;
    const std::optional<mutators::Loadout> read = mutators::parse_loadout(text, &error);
    expect(read.has_value(), what + " reads back: " + error);
    if (!read)
        return;
    expect(*read == loadout, what + " reads back equal");
    expect(loadout == *read, what + " is equal both ways round");
    expect(!(*read != loadout), what + " is not unequal");
    expect(
        mutators::loadout_digest(*read) == mutators::loadout_digest(loadout),
        what + " keeps its digest"
    );
}

/// Checks that record equality is the canonical text's equality.
void equality() {
    oamut::Option rate;
    rate.id = "rate";
    rate.type = oamut::OptionType::number;
    rate.minimum = number_of("0");
    rate.maximum = number_of("10");
    rate.step = number_of("0.25");
    rate.number_default = number_of("1.0");
    const mutators::LoadoutOption one_point_zero = mutators::default_option(rate);
    expect(
        !one_point_zero.value.number_value.integer, "a 1.0 default is not written as an integer"
    );
    const mutators::Loadout from_decimal = one_option(one_point_zero);
    expect(
        mutators::canonical_loadout(from_decimal).find("\"rate\":1}") != std::string::npos,
        "a 1.0 default is written as 1"
    );
    reads_back_equal(from_decimal, "a 1.0 number default");

    oamut::Option gear;
    gear.id = "gear";
    gear.type = oamut::OptionType::choice;
    oamut::ChoiceValue two;
    two.number = true;
    two.number_value = number_of("2");
    two.text = "2";
    oamut::ChoiceValue three = two;
    three.number_value = number_of("3");
    three.text = "3";
    gear.values = {two, three};
    gear.default_index = 1;
    const mutators::LoadoutOption numeric_choice = mutators::default_option(gear);
    reads_back_equal(one_option(numeric_choice), "a numeric choice");
    expect(
        numeric_choice == number_option("gear", "3"),
        "a numeric choice equals a number option of the same number"
    );

    oamut::Option temper;
    temper.id = "temper";
    temper.type = oamut::OptionType::choice;
    oamut::ChoiceValue slow;
    slow.text = "slow";
    oamut::ChoiceValue hot;
    hot.text = "hot";
    temper.values = {slow, hot};
    temper.default_index = 1;
    reads_back_equal(one_option(mutators::default_option(temper)), "a string choice");
    reads_back_equal(sample_loadout(), "the two-entry loadout");

    // Stored fields the canonical text does not write: the integer mark and
    // a number's own text.
    mutators::LoadoutOption stale = number_option("rate", "1");
    stale.value.number_value.integer = false;
    stale.value.text = "1.0";
    expect(stale == number_option("rate", "1"), "1 and 1.0 are one number");
    expect(one_option(stale) == from_decimal, "two records with one text are equal");

    mutators::Loadout listed = sample_loadout();
    std::swap(listed.entries[0].options[0], listed.entries[0].options[2]);
    expect(
        mutators::canonical_loadout(listed) == mutators::canonical_loadout(sample_loadout()),
        "the option order leaves the text alone"
    );
    expect(listed == sample_loadout(), "the option order leaves the record equal");

    // Records whose texts differ stay unequal.
    expect(
        !(number_option("rate", "1.25") == number_option("rate", "1")),
        "another number is another option"
    );
    expect(
        !(number_option("pace", "1") == number_option("rate", "1")), "another id is another option"
    );
    expect(
        !(toggle_option("boost", true) == toggle_option("boost", false)), "a toggle's value counts"
    );
    expect(
        !(choice_option("temper", "hot") == choice_option("temper", "slow")),
        "a choice's text counts"
    );
    expect(
        !(choice_option("gear", "3") == number_option("gear", "3")),
        "a string choice never equals a number"
    );
    expect(
        !(choice_option("boost", "true") == toggle_option("boost", true)),
        "a string choice never equals a toggle"
    );
    mutators::Loadout swapped = sample_loadout();
    std::swap(swapped.entries[0], swapped.entries[1]);
    expect(!(swapped == sample_loadout()), "the entry order counts");
    mutators::Loadout folder_changed = sample_loadout();
    folder_changed.entries[1].folder = false;
    expect(!(folder_changed == sample_loadout()), "the folder mark counts");

    // Equality and the canonical text agree for every pair.
    std::vector<mutators::Loadout> records;
    records.push_back(from_decimal);
    records.push_back(one_option(stale));
    records.push_back(one_option(number_option("rate", "1.25")));
    records.push_back(one_option(choice_option("rate", "1")));
    records.push_back(one_option(numeric_choice));
    records.push_back(one_option(number_option("gear", "3")));
    records.push_back(one_option(toggle_option("rate", true)));
    records.push_back(sample_loadout());
    records.push_back(listed);
    records.push_back(swapped);
    records.push_back(mutators::Loadout{});
    for (const mutators::Loadout& left : records) {
        for (const mutators::Loadout& right : records) {
            const bool same_text =
                mutators::canonical_loadout(left) == mutators::canonical_loadout(right);
            expect((left == right) == same_text, "equality follows the canonical text");
        }
    }
}

/// Checks option_text and options_text for each type, a fraction and no options.
void option_texts() {
    expect(mutators::option_text(toggle_option("armed", true)) == "true", "a toggle on is true");
    expect(
        mutators::option_text(toggle_option("armed", false)) == "false", "a toggle off is false"
    );
    expect(mutators::option_text(number_option("pace", "1.5")) == "1.5", "a number is its text");
    mutators::LoadoutOption whole = number_option("rate", "1.0");
    whole.value.text = "1.0";
    expect(mutators::option_text(whole) == "1", "a number is its canonical text");
    mutators::LoadoutOption stale_choice;
    stale_choice.id = "gear";
    stale_choice.type = oamut::OptionType::choice;
    stale_choice.value.number = true;
    stale_choice.value.number_value = number_of("2");
    stale_choice.value.text = "2.0";
    expect(mutators::option_text(stale_choice) == "2", "a numeric choice is its canonical number");
    expect(mutators::option_text(choice_option("temper", "hot")) == "hot", "a choice is its text");
    expect(mutators::option_text(choice_option("temper", "")).empty(), "an empty choice is empty");

    mutators::LoadoutEntry armed;
    armed.options.push_back(toggle_option("armed", true));
    expect(mutators::options_text(armed) == "armed=true", "a toggle on is true");
    armed.options[0].toggle = false;
    expect(mutators::options_text(armed) == "armed=false", "a toggle off is false");

    mutators::LoadoutEntry pace;
    pace.options.push_back(number_option("pace", "1.5"));
    expect(mutators::options_text(pace) == "pace=1.5", "a number uses its canonical text");

    mutators::LoadoutEntry temper;
    temper.options.push_back(choice_option("temper", "hot"));
    expect(mutators::options_text(temper) == "temper=hot", "a choice uses the manifest text");

    oamut::Option rate;
    rate.id = "rate";
    rate.type = oamut::OptionType::number;
    rate.step = number_of("0.25");
    rate.number_default = number_of("2.25");
    mutators::LoadoutEntry fractional;
    fractional.options.push_back(mutators::default_option(rate));
    expect(
        mutators::options_text(fractional) == "rate=2.25", "a fractional step is canonical text"
    );

    mutators::LoadoutEntry mixed;
    mixed.options.push_back(choice_option("temper", "hot"));
    mixed.options.push_back(toggle_option("boost", false));
    mixed.options.push_back(number_option("pace", "1.5"));
    expect(
        mutators::options_text(mixed) == "boost=false,pace=1.5,temper=hot",
        "names are joined in byte order"
    );
    const mutators::Loadout sample = sample_loadout();
    std::string joined;
    for (const mutators::LoadoutOption& option : sample.entries[0].options) {
        if (!joined.empty())
            joined += ',';
        joined += option.id + "=" + mutators::option_text(option);
    }
    expect(
        mutators::options_text(sample.entries[0]) == joined, "options_text joins each option_text"
    );
    mutators::LoadoutEntry geared;
    geared.options.push_back(stale_choice);
    expect(mutators::options_text(geared) == "gear=2", "options_text writes the canonical number");

    expect(mutators::options_text(mutators::LoadoutEntry{}).empty(), "no options write nothing");

    mutators::Loadout gear;
    mutators::LoadoutEntry entry;
    entry.id = "ridge-gear";
    entry.version = "1";
    mutators::LoadoutOption numeric;
    numeric.id = "gear";
    numeric.type = oamut::OptionType::choice;
    numeric.value.number = true;
    numeric.value.number_value = number_of("2");
    numeric.value.text = "2";
    entry.options.push_back(numeric);
    gear.entries.push_back(entry);
    const std::string text = mutators::canonical_loadout(gear);
    expect(text.find("\"gear\":2") != std::string::npos, "a numeric choice is written as a number");
    const std::optional<mutators::Loadout> read = mutators::parse_loadout(text, nullptr);
    expect(
        read.has_value() && read->entries[0].options[0].type == oamut::OptionType::number,
        "a JSON number reads back as a number option"
    );
    expect(read.has_value() && *read == gear, "a numeric choice reads back equal");
}

/// The directory this run owns.
struct Scratch {
    fs::path root;

    Scratch() : root(oa::test::make_scratch_directory("data-mutators-loadout")) {}

    ~Scratch() {
        std::error_code error;
        fs::remove_all(root, error);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

/// Returns a path's UTF-8 spelling, for a message.
///
/// @param path the path
/// @return its UTF-8 spelling
std::string utf8_of(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

/// Writes a file, creating its parent directories.
///
/// @param path the host path
/// @param text the bytes
void write_text(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream)
        throw std::runtime_error("cannot write " + utf8_of(path));
}

/// Fills a folder with the two files the folder digest is pinned to.
///
/// @param folder the folder
/// @param nested_first true to create the nested file before the top file
void plant_files(const fs::path& folder, bool nested_first) {
    if (nested_first) {
        write_text(folder / "nested" / "b.txt", "beta");
        write_text(folder / "a.txt", "alpha");
    } else {
        write_text(folder / "a.txt", "alpha");
        write_text(folder / "nested" / "b.txt", "beta");
    }
}

/// Checks the folder digest, including a link where one can be made.
///
/// @param scratch the scratch directory
void folder(const Scratch& scratch) {
    const fs::path first = scratch.root / "first";
    const fs::path second = scratch.root / "second";
    plant_files(first, true);
    plant_files(second, false);
    std::string error;
    const std::optional<sha::Digest> from_first = mutators::folder_digest(first, &error);
    const std::optional<sha::Digest> from_second = mutators::folder_digest(second, &error);
    expect(from_first.has_value() && from_second.has_value(), "both folders have a digest");
    if (from_first && from_second)
        expect(*from_first == *from_second, "file creation order does not change the digest");
    // python3 - <<'PY'
    // import hashlib
    // files = [("a.txt", b"alpha"), ("nested/b.txt", b"beta")]
    // message = bytearray()
    // for path, data in sorted(files):
    //     message += path.encode() + b"\0" + len(data).to_bytes(8, "little")
    //     message += hashlib.sha256(data).digest()
    // print(hashlib.sha256(message).hexdigest())
    // PY
    const sha::Digest pin =
        digest_of_hex("5ead7eed495e90765dc574a01c34c84ffd95d84c8ee23b83d13475149abc854c");
    if (from_first && *from_first != pin)
        std::fprintf(stderr, "folder digest %s\n", hex_of(*from_first).c_str());
    expect(
        from_first.has_value() && *from_first == pin, "folder_digest matches the independent digest"
    );

    // The names oa-tool mutator pack leaves out do not change the digest.
    // A dot name it keeps, and __MACOSX below the first part, do.
    const fs::path noisy = scratch.root / "noisy";
    plant_files(noisy, false);
    write_text(noisy / ".DS_Store", "finder");
    write_text(noisy / "nested" / ".ds_store", "nested");
    write_text(noisy / "._a.txt", "appledouble");
    write_text(noisy / ".git" / "config", "repo");
    write_text(noisy / ".oa-origin.yaml", "origin");
    write_text(noisy / ".backup" / "old.txt", "backup");
    write_text(noisy / "__MACOSX" / "a.txt", "resource");
    std::string noisy_error;
    const std::optional<sha::Digest> from_noisy = mutators::folder_digest(noisy, &noisy_error);
    expect(
        from_noisy.has_value() && *from_noisy == pin,
        "names the packer leaves out do not change the digest: " + noisy_error
    );
    std::error_code git_link_error;
    fs::create_symlink(noisy / "a.txt", noisy / ".git" / "linked.txt", git_link_error);
    if (!git_link_error) {
        const std::optional<sha::Digest> with_git_link =
            mutators::folder_digest(noisy, &noisy_error);
        expect(
            with_git_link.has_value() && *with_git_link == pin,
            "a link inside a left-out folder is not walked: " + noisy_error
        );
    }
    write_text(noisy / ".notes", "kept");
    const std::optional<sha::Digest> with_notes = mutators::folder_digest(noisy, &noisy_error);
    expect(
        with_notes.has_value() && from_noisy.has_value() && *with_notes != *from_noisy,
        "a dot name the packer keeps changes the digest: " + noisy_error
    );
    write_text(noisy / "nested" / "__MACOSX" / "x", "kept-mac");
    const std::optional<sha::Digest> with_nested_mac = mutators::folder_digest(noisy, &noisy_error);
    expect(
        with_nested_mac.has_value() && with_notes.has_value() && *with_nested_mac != *with_notes,
        "__MACOSX below the first part is hashed: " + noisy_error
    );

    write_text(first / "a.txt", "alphe");
    const std::optional<sha::Digest> changed = mutators::folder_digest(first, &error);
    expect(
        changed.has_value() && from_first.has_value() && *changed != *from_first,
        "one byte changes the folder digest"
    );

    const fs::path linked = scratch.root / "linked";
    plant_files(linked, false);
    std::error_code link_error;
    fs::create_symlink(linked / "a.txt", linked / "linked.txt", link_error);
    std::error_code status_error;
    if (link_error || !fs::is_symlink(fs::symlink_status(linked / "linked.txt", status_error))) {
        std::fprintf(stderr, "loadout: skipped the symbolic link case\n");
    } else {
        std::string link_message;
        expect(
            !mutators::folder_digest(linked, &link_message).has_value(),
            "a symbolic link is refused"
        );
        expect(link_message.find("linked.txt") != std::string::npos, "the error names the link");
    }

    const fs::path closed = scratch.root / "closed";
    write_text(closed / "secret.txt", "secret");
    std::error_code permission_error;
    fs::permissions(closed / "secret.txt", fs::perms::none, permission_error);
    std::ifstream probe(closed / "secret.txt", std::ios::binary);
    if (permission_error || probe) {
        std::fprintf(stderr, "loadout: skipped the unreadable file case\n");
        probe.close();
    } else {
        std::string closed_message;
        expect(
            !mutators::folder_digest(closed, &closed_message).has_value(),
            "an unreadable file is refused"
        );
        expect(
            closed_message.find("secret.txt") != std::string::npos,
            "the error names the unreadable file"
        );
    }
    probe.close();
    fs::permissions(closed / "secret.txt", fs::perms::owner_write, permission_error);
}

/// Checks the kept folder digests: the same digest as folder_digest, kept
/// while no entry changes, and made again when one is added, removed or
/// resized. An edit that keeps a file's size and write time keeps the old
/// digest, which shows the folder was not read again.
///
/// @param scratch the scratch directory
void kept_folder_digests(const Scratch& scratch) {
    const fs::path kept = scratch.root / "kept";
    plant_files(kept, false);
    mutators::FolderDigests digests;
    std::string error;
    const std::optional<sha::Digest> first = digests.digest(kept, &error);
    const std::optional<sha::Digest> made = mutators::folder_digest(kept, &error);
    expect(
        first.has_value() && made.has_value() && *first == *made,
        "a kept digest is the folder digest: " + error
    );

    const fs::path file = kept / "nested" / "b.txt";
    std::error_code time_error;
    const fs::file_time_type written = fs::last_write_time(file, time_error);
    expect(!time_error, "the file's write time can be read");
    write_text(file, "BETA");
    fs::last_write_time(file, written, time_error);
    if (time_error) {
        std::fprintf(stderr, "loadout: skipped the kept digest's same-time case\n");
    } else {
        const uint64_t reads = digests.reads();
        const std::optional<sha::Digest> same_stamp = digests.digest(kept, &error);
        expect(
            same_stamp.has_value() && first.has_value() && *same_stamp == *first &&
                digests.reads() == reads,
            "an edit that keeps the size and the write time keeps the digest, unread"
        );
    }

    write_text(file, "beta, longer");
    const std::optional<sha::Digest> resized = digests.digest(kept, &error);
    const std::optional<sha::Digest> resized_made = mutators::folder_digest(kept, &error);
    expect(
        resized.has_value() && resized_made.has_value() && *resized == *resized_made &&
            first.has_value() && *resized != *first,
        "a resized file makes the digest again"
    );

    write_text(kept / "c.txt", "gamma");
    const std::optional<sha::Digest> added = digests.digest(kept, &error);
    expect(
        added.has_value() && resized.has_value() && *added != *resized &&
            *added == *mutators::folder_digest(kept, &error),
        "an added file makes the digest again"
    );
    fs::remove(kept / "c.txt");
    const std::optional<sha::Digest> removed = digests.digest(kept, &error);
    expect(
        removed.has_value() && resized.has_value() && *removed == *resized,
        "a removed file makes the digest again"
    );

    const fs::path linked = scratch.root / "kept-linked";
    plant_files(linked, false);
    std::error_code link_error;
    fs::create_symlink(linked / "a.txt", linked / "linked.txt", link_error);
    std::error_code status_error;
    if (link_error || !fs::is_symlink(fs::symlink_status(linked / "linked.txt", status_error))) {
        std::fprintf(stderr, "loadout: skipped the kept digest's symbolic link case\n");
    } else {
        std::string link_message;
        expect(
            !digests.digest(linked, &link_message).has_value() &&
                link_message.find("linked.txt") != std::string::npos,
            "a refused folder is refused, and named, through the cache"
        );
        fs::remove(linked / "linked.txt");
        expect(
            digests.digest(linked, &link_message).has_value(),
            "a refusal is not kept once the link is gone"
        );
    }
}

/// Checks the folder digest of a name outside ASCII, which is hashed as UTF-8.
///
/// Skipped where the file system does not keep the name as written, as one
/// that normalises names does.
///
/// @param scratch the scratch directory
void unicode_folder(const Scratch& scratch) {
    const fs::path unicode = scratch.root / "unicode";
    const fs::path nested = unicode / fs::path(std::u8string(u8"\u00e9t\u00e9"));
    const fs::path file = nested / fs::path(std::u8string(u8"caf\u00e9.txt"));
    bool kept = false;
    try {
        write_text(unicode / "a.txt", "alpha");
        write_text(file, "coffee");
        for (const fs::directory_entry& entry : fs::directory_iterator(nested)) {
            if (entry.path().filename().u8string() == std::u8string(u8"caf\u00e9.txt"))
                kept = true;
        }
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "loadout: %s\n", failure.what());
    }
    if (!kept) {
        std::fprintf(stderr, "loadout: skipped the non-ASCII name case\n");
        return;
    }
    // python3 - <<'PY'
    // import hashlib
    // files = [("a.txt", b"alpha"), ("\u00e9t\u00e9/caf\u00e9.txt", b"coffee")]
    // message = bytearray()
    // for path, data in sorted(files, key=lambda item: item[0].encode()):
    //     message += path.encode() + b"\0" + len(data).to_bytes(8, "little")
    //     message += hashlib.sha256(data).digest()
    // print(hashlib.sha256(message).hexdigest())
    // PY
    const sha::Digest pin =
        digest_of_hex("d877ba29ee048796ed10c2746264ec3bb77b8ae2587543c7cfc646ffaa6535b5");
    std::string error;
    const std::optional<sha::Digest> digest = mutators::folder_digest(unicode, &error);
    if (digest && *digest != pin)
        std::fprintf(stderr, "unicode folder digest %s\n", hex_of(*digest).c_str());
    expect(
        digest.has_value() && *digest == pin,
        "a non-ASCII name is hashed as its UTF-8 bytes: " + error
    );
}

/// Checks that a pipe is refused as not a regular file, where pipes exist.
///
/// @param scratch the scratch directory
void pipe_folder(const Scratch& scratch) {
#if defined(_WIN32)
    (void)scratch;
    std::fprintf(stderr, "loadout: skipped the pipe case\n");
#else
    const fs::path piped = scratch.root / "piped";
    write_text(piped / "a.txt", "alpha");
    const fs::path fifo = piped / "pipe";
    if (::mkfifo(fifo.c_str(), 0600) != 0) {
        std::fprintf(stderr, "loadout: skipped the pipe case\n");
        return;
    }
    std::string error;
    expect(!mutators::folder_digest(piped, &error).has_value(), "a pipe is refused");
    expect(
        error == "pipe is not a regular file", "a pipe is named as not a regular file: " + error
    );
    std::string root_error;
    expect(!mutators::folder_digest(fifo, &root_error).has_value(), "a pipe is not a folder");
    expect(
        root_error.find("is not a folder") != std::string::npos,
        "a pipe given as the folder is named as not a folder: " + root_error
    );
#endif
}

} // namespace

namespace {
void applied_only();
void chosen_list(const Scratch& scratch);
void resolves_installed(const Scratch& scratch);
void hand_made_folder(const Scratch& scratch);
void clash_mode_is_part_of_the_loadout(const Scratch& scratch);
} // namespace

int main() {
    canonical_form();
    digests();
    refusals();
    defaults();
    equality();
    option_texts();
    {
        const Scratch scratch;
        folder(scratch);
        kept_folder_digests(scratch);
        unicode_folder(scratch);
        pipe_folder(scratch);
        applied_only();
        chosen_list(scratch);
        resolves_installed(scratch);
        hand_made_folder(scratch);
        clash_mode_is_part_of_the_loadout(scratch);
    }
    const int status = oa::test::check_exit_status();
    if (status == 0)
        std::puts("loadout: ok");
    return status;
}

namespace {

/// One numeric choice value.
///
/// @param text the number, as the manifest writes it
/// @return the value
oamut::ChoiceValue number_choice(std::string_view text) {
    oamut::ChoiceValue value;
    value.number = true;
    value.number_value = number_of(text);
    value.text = oamod::canonical_number_text(value.number_value);
    return value;
}

/// One string choice value.
///
/// @param text the identifier
/// @return the value
oamut::ChoiceValue text_choice(std::string text) {
    oamut::ChoiceValue value;
    value.text = std::move(text);
    return value;
}

/// A manifest with one toggle, one number, one numeric choice and one string choice.
///
/// @param id the mutator's id
/// @param version the version text
/// @return the manifest
oamut::Manifest pace_manifest(std::string id, std::string_view version) {
    oamut::Manifest manifest;
    manifest.format = oamut::format_version;
    manifest.id = std::move(id);
    manifest.name = "Ridge Pace";
    const std::optional<oamut::Version> parsed = oamut::parse_version(version);
    if (!parsed)
        throw std::runtime_error("not a version");
    manifest.version = *parsed;
    manifest.author.name = "Ridge Maker";
    manifest.tags = {"gameplay"};
    manifest.works_with.push_back(oamut::Condition{"*", {}, {}});
    manifest.layers = {oamut::Layer::units};

    oamut::Option boost;
    boost.id = "boost";
    boost.type = oamut::OptionType::toggle;
    boost.label = "Boost";
    boost.toggle_default = false;
    manifest.options.push_back(std::move(boost));

    oamut::Option health;
    health.id = "health";
    health.type = oamut::OptionType::number;
    health.label = "Health";
    health.minimum = number_of("0");
    health.maximum = number_of("30000");
    health.step = number_of("1000");
    health.number_default = number_of("4000");
    manifest.options.push_back(std::move(health));

    oamut::Option pace;
    pace.id = "pace";
    pace.type = oamut::OptionType::choice;
    pace.label = "Pace";
    pace.values = {number_choice("1.5"), number_choice("4")};
    pace.default_index = 0;
    manifest.options.push_back(std::move(pace));

    oamut::Option temper;
    temper.id = "temper";
    temper.type = oamut::OptionType::choice;
    temper.label = "Temper";
    temper.values = {text_choice("hot"), text_choice("cold")};
    temper.default_index = 0;
    manifest.options.push_back(std::move(temper));
    return manifest;
}

/// A manifest with no options.
///
/// @param id the mutator's id
/// @param version the version text
/// @return the manifest
oamut::Manifest plain_manifest(std::string id, std::string_view version) {
    oamut::Manifest manifest;
    manifest.format = oamut::format_version;
    manifest.id = std::move(id);
    manifest.name = "Ridge Kit";
    const std::optional<oamut::Version> parsed = oamut::parse_version(version);
    if (!parsed)
        throw std::runtime_error("not a version");
    manifest.version = *parsed;
    manifest.author.name = "Ridge Maker";
    manifest.tags = {"gameplay"};
    manifest.works_with.push_back(oamut::Condition{"*", {}, {}});
    manifest.layers = {oamut::Layer::units};
    return manifest;
}

/// Writes a package folder and, when `sha` is set, a file origin naming it.
///
/// @param folder the package folder
/// @param manifest the manifest
/// @param sha the package file's SHA-256, or empty when the package is named by path
void write_package(const fs::path& folder, const oamut::Manifest& manifest, std::string_view sha) {
    write_text(folder / "oamut.yaml", oamut::write_manifest(manifest));
    write_text(folder / "mutator.lua", "return {}\n");
    if (sha.empty())
        return;
    write_text(
        folder / ".oa-origin.yaml",
        "oa-origin: 1\n"
        "origin: file\n"
        "sha256: \"" +
            std::string(sha) +
            "\"\n"
            "installed: \"2000-01-01\"\n"
    );
}

/// Resolves a choice that must be accepted.
///
/// @param text the choice
/// @param mutators the Mutators folder
/// @return the package
mutators::ResolvedMutator resolved_text(std::string_view text, const fs::path& mutators) {
    std::string error;
    const std::optional<mutators::MutatorChoice> choice = mutators::parse_choice(text, &error);
    expect(choice.has_value(), "the choice parses: " + error);
    if (!choice)
        return {};
    const std::optional<mutators::ResolvedMutator> resolved =
        mutators::resolve_choice(*choice, mutators, &error);
    expect(resolved.has_value(), "the choice resolves: " + error);
    if (!resolved)
        return {};
    return *resolved;
}

/// The option of an id, or null.
///
/// @param options the options
/// @param id the option's id
/// @return the option
const mutators::LoadoutOption*
find_option(const std::vector<mutators::LoadoutOption>& options, std::string_view id) {
    for (const mutators::LoadoutOption& option : options) {
        if (option.id == id)
            return &option;
    }
    return nullptr;
}

/// Tells whether both options are the same member.
///
/// @param left one option
/// @param right the other
/// @return true when LoadoutOption compares them equal
bool same_option(const mutators::LoadoutOption& left, const mutators::LoadoutOption& right) {
    return left == right;
}

/// Checks that a resolved list becomes the canonical loadout, and that a path
/// carries the folder digest.
///
/// @param scratch the scratch directory
void chosen_list(const Scratch& scratch) {
    constexpr std::string_view pace_sha =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const fs::path mutators = scratch.root / "Mutators";
    write_package(mutators / "ridge-pace", pace_manifest("ridge-pace", "1.2"), pace_sha);
    const fs::path kit = scratch.root / "loose" / "ridge-kit";
    write_package(kit, plain_manifest("ridge-kit", "3"), {});

    const mutators::ResolvedMutator pace = resolved_text("ridge-pace:temper=cold", mutators);
    const mutators::ResolvedMutator by_path = resolved_text(fs::absolute(kit).string(), mutators);
    expect(by_path.folder, "a package folder named by path sets folder");
    std::string digest_error;
    const std::optional<sha::Digest> folder = mutators::folder_digest(kit, &digest_error);
    expect(folder.has_value() && by_path.package == *folder, "the path carries the folder digest");
    expect(!pace.folder, "an installed id is not a folder");
    expect(pace.package == digest_of_hex(pace_sha), "an installed id keeps the recorded SHA-256");

    const std::vector<mutators::ResolvedMutator> list{pace, by_path};
    const mutators::Loadout made = mutators::loadout_of(list, oamut::ClashMode::replace);
    expect(made.entries.size() == 2, "loadout_of lists every mutator");
    expect(
        made.entries.size() == 2 && made.entries[0].id == "ridge-pace" &&
            made.entries[1].id == "ridge-kit",
        "loadout_of keeps the list's order"
    );
    if (made.entries.size() == 2) {
        const std::vector<mutators::LoadoutOption>& options = made.entries[0].options;
        expect(options.size() == 4, "loadout_of fills every option");
        expect(
            options.size() == 4 && options[0].id == "boost" && options[1].id == "health" &&
                options[2].id == "pace" && options[3].id == "temper",
            "options are stored in id byte order"
        );
        const oamut::Manifest manifest = pace_manifest("ridge-pace", "1.2");
        for (const oamut::Option& option : manifest.options) {
            const mutators::LoadoutOption* found = find_option(options, option.id);
            const mutators::LoadoutOption fallback = mutators::default_option(option);
            expect(found != nullptr, "every manifest option is in the loadout");
            if (found == nullptr)
                continue;
            if (option.id == "temper") {
                expect(found->value.text == "cold", "the named option keeps the player's value");
            } else {
                expect(same_option(*found, fallback), "an unnamed option keeps its default");
            }
        }
    }

    const std::string literal =
        std::string(
            R"({"mutators":[{"clashes":"replace","folder":false,"id":"ridge-pace","options":{"boost":false,"health":4000,"pace":1.5,"temper":"cold"},"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","version":"1.2"},{"clashes":"replace","folder":true,"id":"ridge-kit","options":{},"sha256":")"
        ) +
        hex_of(by_path.package) + R"(","version":"3"}],"oaloadout":1})";
    const std::string canonical = mutators::canonical_loadout(made);
    if (canonical != literal)
        std::fprintf(stderr, "loadout canonical: %s\n", canonical.c_str());
    expect(canonical == literal, "the canonical form equals the literal");

    if (!made.entries.empty()) {
        const std::string options = mutators::options_text(made.entries[0]);
        const mutators::ResolvedMutator again = resolved_text("ridge-pace:" + options, mutators);
        expect(again.options.size() == pace.options.size(), "options_text resolves every option");
        for (const mutators::LoadoutOption& option : pace.options) {
            const mutators::LoadoutOption* found = find_option(again.options, option.id);
            expect(
                found != nullptr && same_option(*found, option),
                "options_text reads back " + option.id
            );
        }
    }
}

/// Checks that only the mutators a result applied stay, in order.
void applied_only() {
    mutators::Loadout chosen;
    for (const char* id : {"ridge-one", "ridge-two", "ridge-three", "ridge-four", "ridge-five"}) {
        mutators::LoadoutEntry entry;
        entry.id = id;
        entry.version = "1";
        chosen.entries.push_back(std::move(entry));
    }
    mutators::PipelineResult result;
    const auto outcome = [](const char* id, mutators::Outcome kind) {
        mutators::MutatorResult made;
        made.id = id;
        made.version = "1";
        made.outcome = kind;
        return made;
    };
    result.mutators.push_back(outcome("ridge-one", mutators::Outcome::applied));
    result.mutators.push_back(outcome("ridge-two", mutators::Outcome::failed));
    result.mutators.push_back(outcome("ridge-three", mutators::Outcome::applied));
    result.mutators.push_back(outcome("ridge-four", mutators::Outcome::excluded));
    result.mutators.push_back(outcome("ridge-five", mutators::Outcome::incompatible));
    const mutators::Loadout kept = mutators::applied_loadout(chosen, result);
    expect(kept.entries.size() == 2, "applied_loadout drops the mutator that did not apply");
    expect(
        kept.entries.size() == 2 && kept.entries[0].id == "ridge-one" &&
            kept.entries[1].id == "ridge-three",
        "applied_loadout keeps the applied mutators in order"
    );
}

/// Checks that a loadout finds its installed packages, and refuses the rest.
///
/// @param scratch the scratch directory
void resolves_installed(const Scratch& scratch) {
    constexpr std::string_view pace_sha =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    constexpr std::string_view plain_sha =
        "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
    const fs::path mutators = scratch.root / "installed";
    write_package(mutators / "ridge-pace", pace_manifest("ridge-pace", "1.2"), pace_sha);
    write_package(mutators / "ridge-plain", plain_manifest("ridge-plain", "3"), plain_sha);

    const mutators::ResolvedMutator pace = resolved_text("ridge-pace:health=2000", mutators);
    const mutators::ResolvedMutator plain = resolved_text("ridge-plain", mutators);
    const mutators::Loadout made =
        mutators::loadout_of(std::vector<mutators::ResolvedMutator>{pace, plain});
    std::string error;
    const std::optional<std::vector<mutators::ResolvedMutator>> found =
        mutators::resolve_loadout(made, mutators, &error);
    expect(found.has_value(), "resolve_loadout finds the installed packages: " + error);
    if (found && found->size() == 2) {
        expect(
            (*found)[0].manifest.id == "ridge-pace" && (*found)[1].manifest.id == "ridge-plain",
            "resolve_loadout keeps the loadout's order"
        );
        expect(
            !(*found)[0].folder && (*found)[0].package == digest_of_hex(pace_sha),
            "resolve_loadout keeps the package SHA-256"
        );
        const mutators::LoadoutOption* health = find_option((*found)[0].options, "health");
        const mutators::LoadoutOption* original = find_option(pace.options, "health");
        expect(
            health != nullptr && original != nullptr && same_option(*health, *original),
            "resolve_loadout types the options"
        );
    } else {
        expect(false, "resolve_loadout returned both packages");
    }

    mutators::Loadout missing = made;
    missing.entries[0].id = "ridge-missing";
    expect(!mutators::resolve_loadout(missing, mutators, &error), "a missing id is refused");
    expect(
        error == "\"ridge-missing\" is not an installed mutator",
        "the missing id is named: " + error
    );

    mutators::Loadout other_version = made;
    other_version.entries[0].version = "9";
    expect(
        !mutators::resolve_loadout(other_version, mutators, &error), "another version is refused"
    );
    expect(
        error == "\"ridge-pace\" is version 1.2, not 9",
        "the other version names the entry: " + error
    );

    mutators::Loadout other_sha = made;
    other_sha.entries[0].package = filled_digest(0xab);
    expect(!mutators::resolve_loadout(other_sha, mutators, &error), "another SHA-256 is refused");
    expect(
        error == "\"ridge-pace\" is a different package",
        "the other SHA-256 names the entry: " + error
    );

    mutators::Loadout folder = made;
    folder.entries[1].folder = true;
    expect(!mutators::resolve_loadout(folder, mutators, &error), "a folder entry is refused");
    expect(error == "\"ridge-plain\" is a package folder", "the folder entry is named: " + error);

    // One listing resolves several choices as the folder does.
    const oa::data::mutator_library::Listing listing =
        oa::data::mutator_library::list_installed(mutators);
    for (const std::string_view text : {"ridge-pace:health=2000", "ridge-plain"}) {
        const std::optional<mutators::MutatorChoice> choice = mutators::parse_choice(text, &error);
        expect(choice.has_value(), "the choice reads: " + error);
        if (!choice)
            continue;
        const std::optional<mutators::ResolvedMutator> from_listing =
            mutators::resolve_choice(*choice, listing, &error);
        const std::optional<mutators::ResolvedMutator> from_folder =
            mutators::resolve_choice(*choice, mutators, &error);
        expect(
            from_listing.has_value() && from_folder.has_value() &&
                from_listing->manifest.id == from_folder->manifest.id &&
                from_listing->package == from_folder->package &&
                from_listing->options.size() == from_folder->options.size(),
            "a listing resolves " + std::string(text) + " as its folder does"
        );
    }
    const std::optional<mutators::MutatorChoice> absent = mutators::parse_choice("ridge-absent");
    expect(
        absent.has_value() && !mutators::resolve_choice(*absent, listing, &error) &&
            error == "\"ridge-absent\" is not an installed mutator",
        "a listing names an id it lacks: " + error
    );
}

/// Checks a hand-made folder, a different folder digest and a bad SHA-256.
///
/// @param scratch the scratch directory
void hand_made_folder(const Scratch& scratch) {
    const fs::path mutators = scratch.root / "handmade";
    write_package(mutators / "ridge-hand", plain_manifest("ridge-hand", "1.0"), {});
    const mutators::ResolvedMutator hand = resolved_text("ridge-hand", mutators);
    expect(hand.folder, "a hand-made folder sets folder");
    std::string digest_error;
    const std::optional<sha::Digest> digest =
        mutators::folder_digest(mutators / "ridge-hand", &digest_error);
    expect(
        digest.has_value() && hand.package == *digest,
        "a hand-made folder keeps the folder digest: " + digest_error
    );

    const mutators::Loadout made =
        mutators::loadout_of(std::vector<mutators::ResolvedMutator>{hand});
    std::string error;
    const std::optional<std::vector<mutators::ResolvedMutator>> found =
        mutators::resolve_loadout(made, mutators, &error);
    expect(found.has_value(), "the same folder digest resolves: " + error);
    if (found && found->size() == 1) {
        expect(
            (*found)[0].folder && (*found)[0].package == hand.package,
            "playback keeps the folder digest"
        );
    } else {
        expect(false, "resolve_loadout returned the hand-made package");
    }

    mutators::Loadout other = made;
    if (!other.entries.empty())
        other.entries[0].package = filled_digest(0x11);
    expect(
        !mutators::resolve_loadout(other, mutators, &error), "a different folder digest is refused"
    );
    expect(
        error == "\"ridge-hand\" is a package folder",
        "the different folder digest is named as a package folder: " + error
    );

    write_package(mutators / "ridge-bad", plain_manifest("ridge-bad", "1.0"), "abcd");
    std::string bad_error;
    const std::optional<mutators::MutatorChoice> choice =
        mutators::parse_choice("ridge-bad", &bad_error);
    expect(choice.has_value(), "the short SHA-256 choice parses: " + bad_error);
    if (choice) {
        expect(
            !mutators::resolve_choice(*choice, mutators, &bad_error).has_value(),
            "a SHA-256 that is not 64 hexadecimal digits is refused"
        );
        expect(
            bad_error ==
                "\"ridge-bad\" records a package SHA-256 that is not 64 hexadecimal digits",
            "the short SHA-256 is named: " + bad_error
        );
    }
}

/// The mode in force is recorded, and a loadout read back keeps that mode.
///
/// @param scratch the scratch directory
void clash_mode_is_part_of_the_loadout(const Scratch& scratch) {
    mutators::ResolvedMutator manifest_add;
    manifest_add.manifest = plain_manifest("ridge-manifest", "1.0");
    manifest_add.manifest.clashes = oamut::ClashMode::add;
    manifest_add.package = filled_digest(0x21);
    const mutators::Loadout from_manifest = mutators::loadout_of(
        std::vector<mutators::ResolvedMutator>{manifest_add}, oamut::ClashMode::replace
    );
    expect(from_manifest.entries.size() == 1, "a manifest mode makes one entry");
    if (!from_manifest.entries.empty()) {
        expect(
            from_manifest.entries[0].clashes == oamut::ClashMode::add,
            "a manifest of add wins over a default of replace"
        );
    }
    expect(
        mutators::canonical_loadout(from_manifest).find("\"clashes\":\"add\"") != std::string::npos,
        "the manifest mode is in the canonical text"
    );

    mutators::ResolvedMutator setting_add;
    setting_add.manifest = plain_manifest("ridge-setting", "1.0");
    setting_add.package = filled_digest(0x22);
    const mutators::Loadout from_setting = mutators::loadout_of(
        std::vector<mutators::ResolvedMutator>{setting_add}, oamut::ClashMode::add
    );
    expect(
        !from_setting.entries.empty() && from_setting.entries[0].clashes == oamut::ClashMode::add,
        "no manifest key and a default of add records add"
    );
    expect(
        mutators::canonical_loadout(from_setting).find("\"clashes\":\"add\"") != std::string::npos,
        "the preference default is in the canonical text"
    );

    mutators::ResolvedMutator player;
    player.manifest = plain_manifest("ridge-player", "1.0");
    player.manifest.clashes = oamut::ClashMode::add;
    player.clashes = oamut::ClashMode::replace;
    player.package = filled_digest(0x23);
    const mutators::Loadout from_player =
        mutators::loadout_of(std::vector<mutators::ResolvedMutator>{player}, oamut::ClashMode::add);
    expect(
        !from_player.entries.empty() && from_player.entries[0].clashes == oamut::ClashMode::replace,
        "the player's choice wins"
    );
    expect(
        mutators::canonical_loadout(from_player).find("\"clashes\":\"replace\"") !=
            std::string::npos,
        "the player's choice is in the canonical text"
    );

    constexpr std::string_view sha =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const fs::path mutators = scratch.root / "clash-mode";
    oamut::Manifest written = plain_manifest("ridge-recorded", "1.0");
    written.clashes = oamut::ClashMode::add;
    write_package(mutators / "ridge-recorded", written, sha);
    const mutators::ResolvedMutator resolved = resolved_text("ridge-recorded", mutators);
    expect(!resolved.clashes.has_value(), "a choice without a mode leaves the player unset");
    const mutators::Loadout recorded = mutators::loadout_of(
        std::vector<mutators::ResolvedMutator>{resolved}, oamut::ClashMode::replace
    );
    expect(
        !recorded.entries.empty() && recorded.entries[0].clashes == oamut::ClashMode::add,
        "the recorded entry keeps the manifest mode"
    );
    std::string error;
    const std::optional<std::vector<mutators::ResolvedMutator>> played =
        mutators::resolve_loadout(recorded, mutators, &error);
    expect(played.has_value(), "resolve_loadout reads the recorded mode: " + error);
    if (played && played->size() == 1) {
        expect(
            (*played)[0].clashes.has_value() && *(*played)[0].clashes == oamut::ClashMode::add,
            "resolve_loadout of an add entry returns add"
        );
    } else {
        expect(false, "resolve_loadout returned the recorded package");
    }
}

} // namespace
