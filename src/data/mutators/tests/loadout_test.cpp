// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The loadout record: its canonical text, the digests computed outside the
// engine, and the text of an entry's options.

#include "oa/data/mutators/loadout.hpp"

#include "oa/base/sha256.hpp"
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

namespace {

namespace fs = std::filesystem;
namespace mutators = oa::data::mutators;
namespace oamod = oa::formats::oamod;
namespace oamut = oa::formats::oamut;
namespace sha = oa::base::sha256;

/// The canonical text of the fixed two-entry loadout, one of them a folder.
constexpr std::string_view kCanonical =
    R"({"mutators":[{"folder":false,"id":"ridge-pace","options":{"boost":false,"pace":1.5,"temper":"hot"},"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","version":"1.2"},{"folder":true,"id":"ridge-kit","options":{},"sha256":"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210","version":"3"}],"oaloadout":1})";

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
    // text = '{"mutators":[{"folder":false,"id":"ridge-pace","options":{"boost":false,"pace":1.5,"temper":"hot"},"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","version":"1.2"},{"folder":true,"id":"ridge-kit","options":{},"sha256":"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210","version":"3"}],"oaloadout":1}'
    // print(hashlib.sha256(text.encode()).hexdigest())
    // PY
    const sha::Digest loadout_pin =
        digest_of_hex("e1e65f3c4c6b16fe1f28baaa5bdb304d50233f0b8c97e5c1a4efb5032ee8f7cd");
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
    // text = b'{"mutators":[{"folder":false,"id":"ridge-pace","options":{"boost":false,"pace":1.5,"temper":"hot"},"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","version":"1.2"},{"folder":true,"id":"ridge-kit","options":{},"sha256":"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210","version":"3"}],"oaloadout":1}'
    // layer = bytes.fromhex("0f193230fd2df02e7fee7ea8c2a5da796ecb40da04d817c4ae1dc177307d6d95")
    // print(hashlib.sha256(b"oa-mutators-1\n" + text + layer).hexdigest())
    // PY
    const sha::Digest mutator_pin =
        digest_of_hex("52c097378a2a9ab324a9e9820cd8e449dda3e197b7c3991abf57265a0c24c0a1");
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
    expect(too_many.size() <= mutators::loadout_text_most_bytes, "33 entries fit in 64 KiB");
    refuses(too_many, "32");
    many.entries.pop_back();
    std::string limit_error;
    const std::optional<mutators::Loadout> thirty_two =
        mutators::parse_loadout(mutators::canonical_loadout(many), &limit_error);
    expect(thirty_two.has_value() && thirty_two->entries.size() == 32, "32 entries are read");

    const std::string over(mutators::loadout_text_most_bytes + 1, 'x');
    refuses(over, "65536");
    std::string boundary_error;
    const std::string boundary(mutators::loadout_text_most_bytes, 'x');
    expect(
        !mutators::parse_loadout(boundary, &boundary_error).has_value(), "64 KiB of junk is refused"
    );
    expect(boundary_error.find("65536") == std::string::npos, "64 KiB is inside the size limit");
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

/// Checks options_text for each type, a fraction and no options.
void option_text() {
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
}

/// The directory this run owns.
struct Scratch {
    fs::path root;

    Scratch() : root(oa::test::make_scratch_directory("V10-loadout")) {}

    ~Scratch() {
        std::error_code error;
        fs::remove_all(root, error);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

/// Writes a file, creating its parent directories.
///
/// @param path the host path
/// @param text the bytes
void write_text(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream)
        throw std::runtime_error("cannot write " + path.string());
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

} // namespace

int main() {
    canonical_form();
    digests();
    refusals();
    defaults();
    option_text();
    {
        const Scratch scratch;
        folder(scratch);
    }
    const int status = oa::test::check_exit_status();
    if (status == 0)
        std::puts("loadout: ok");
    return status;
}
