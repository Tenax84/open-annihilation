// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The last mutator set of each setup, and the crash record beside the
// preferences file. Sets are the canonical loadout text. The files are
// exercised in scratch folders.

#include "oa/app/mutators_memory.hpp"

#include "oa/base/sha256.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ostream>
#include <stdexcept>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

namespace fs = std::filesystem;
namespace memory = oa::app::mutators_memory;
namespace mutators = oa::data::mutators;
namespace oamod = oa::formats::oamod;
namespace oamut = oa::formats::oamut;
namespace preferences = oa::platform::preferences;
namespace sha = oa::base::sha256;

/// Swaps standard error for a string and puts it back, even when a check fails.
struct CapturedError {
    std::ostringstream buffer;
    std::streambuf* previous = std::cerr.rdbuf(buffer.rdbuf());

    CapturedError() = default;
    CapturedError(const CapturedError&) = delete;
    CapturedError& operator=(const CapturedError&) = delete;

    ~CapturedError() { std::cerr.rdbuf(previous); }

    /// The text written so far.
    ///
    /// @return the text
    [[nodiscard]] std::string text() const { return buffer.str(); }
};

/// A scratch folder this test removes when it is done.
struct Scratch {
    fs::path folder = oa::test::make_scratch_directory("mutators-memory");

    Scratch() = default;
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    ~Scratch() {
        std::error_code error;
        fs::remove_all(folder, error);
    }
};

/// Reads a digest from lower-case hex.
///
/// @param hex 64 hex digits
/// @return the digest
sha::Digest digest_of(std::string_view hex) {
    const std::optional<sha::Digest> digest = sha::parse_hex(hex);
    if (!digest)
        throw std::runtime_error("not a digest");
    return *digest;
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

/// A number option.
///
/// @param id the option id
/// @param text the canonical number
/// @return the option
mutators::LoadoutOption number_option(std::string_view id, std::string_view text) {
    mutators::LoadoutOption option;
    option.id = std::string{id};
    option.type = oamut::OptionType::number;
    oamod::Number number{};
    if (oamod::parse_number(text, number) != oamod::NumberStatus::ok)
        throw std::runtime_error("not a number");
    option.value.number = true;
    option.value.number_value = number;
    option.value.text = oamod::canonical_number_text(number);
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

/// Two made-up Ridge mutators, a toggle, a number and a string choice.
///
/// @return the loadout
mutators::Loadout sample_loadout() {
    mutators::LoadoutEntry pace;
    pace.id = "ridge-pace";
    pace.version = "1.2";
    pace.package = digest_of("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    pace.options.push_back(toggle_option("boost", false));
    pace.options.push_back(number_option("pace", "1.5"));
    pace.options.push_back(choice_option("temper", "hot"));

    mutators::LoadoutEntry kit;
    kit.id = "ridge-kit";
    kit.version = "3";
    kit.folder = true;
    kit.package = digest_of("fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210");

    mutators::Loadout loadout;
    loadout.entries.push_back(std::move(pace));
    loadout.entries.push_back(std::move(kit));
    return loadout;
}

/// The option of an entry with an id.
///
/// @param entry the entry
/// @param id the option id
/// @return the option, or null
const mutators::LoadoutOption*
option_named(const mutators::LoadoutEntry& entry, std::string_view id) {
    for (const mutators::LoadoutOption& option : entry.options)
        if (option.id == id)
            return &option;
    return nullptr;
}

/// Reads a set the way a setup's key is read: absent or unreadable is none.
///
/// @param values the preferences
/// @param setup the setup
/// @return the set, or none
std::optional<mutators::Loadout> read_set(const preferences::Values& values, memory::Setup setup) {
    const auto found = values.find(std::string(memory::preference_key(setup)));
    if (found == values.end())
        return std::nullopt;
    return mutators::parse_loadout(found->second, nullptr);
}

/// A stored set reads back with its order, versions, hashes and option kinds.
void a_set_reads_back() {
    const mutators::Loadout loadout = sample_loadout();
    const std::string canonical = mutators::canonical_loadout(loadout);
    Scratch scratch;
    const fs::path file = scratch.folder / "preferences.conf";
    preferences::save(
        file, {{std::string(memory::preference_key(memory::Setup::skirmish)), canonical}}
    );
    const preferences::Values stored = preferences::load(file);
    const auto found = stored.find(std::string(memory::preference_key(memory::Setup::skirmish)));
    OA_CHECK(found != stored.end());
    OA_CHECK(found->second == canonical);
    const std::optional<mutators::Loadout> read = read_set(stored, memory::Setup::skirmish);
    OA_CHECK(read.has_value());
    OA_CHECK(*read == loadout);
    OA_CHECK(read->entries.size() == 2);
    OA_CHECK(read->entries[0].id == "ridge-pace" && read->entries[0].version == "1.2");
    OA_CHECK(!read->entries[0].folder);
    OA_CHECK(read->entries[1].id == "ridge-kit" && read->entries[1].version == "3");
    OA_CHECK(read->entries[1].folder);
    const mutators::LoadoutOption* boost = option_named(read->entries[0], "boost");
    const mutators::LoadoutOption* pace = option_named(read->entries[0], "pace");
    const mutators::LoadoutOption* temper = option_named(read->entries[0], "temper");
    OA_CHECK(boost != nullptr && boost->type == oamut::OptionType::toggle && !boost->toggle);
    OA_CHECK(pace != nullptr && pace->type == oamut::OptionType::number && pace->value.number);
    OA_CHECK(temper != nullptr && temper->type == oamut::OptionType::choice);
    OA_CHECK(temper->value.text == "hot");
}

/// Text parse_loadout refuses is no set.
void text_that_does_not_parse_is_no_set() {
    mutators::Loadout many;
    for (int index = 0; index < 33; ++index) {
        mutators::LoadoutEntry entry;
        entry.id = "m";
        entry.version = "1";
        many.entries.push_back(std::move(entry));
    }
    const std::string key{memory::preference_key(memory::Setup::skirmish)};
    const preferences::Values not_json{{key, "not json"}};
    const preferences::Values array_form{{key, R"([{"id":"ridge-pace","version":"1"}])"}};
    const preferences::Values too_many{{key, mutators::canonical_loadout(many)}};
    OA_CHECK(!read_set(not_json, memory::Setup::skirmish).has_value());
    OA_CHECK(!read_set(array_form, memory::Setup::skirmish).has_value());
    OA_CHECK(!read_set(too_many, memory::Setup::skirmish).has_value());
    OA_CHECK(!read_set({}, memory::Setup::campaign).has_value());
}

/// The three keys differ and start with the engine prefix.
void the_three_keys_differ() {
    const std::string_view skirmish = memory::preference_key(memory::Setup::skirmish);
    const std::string_view multiplayer = memory::preference_key(memory::Setup::multiplayer);
    const std::string_view campaign = memory::preference_key(memory::Setup::campaign);
    OA_CHECK(skirmish != multiplayer && multiplayer != campaign && campaign != skirmish);
    for (const std::string_view key : {skirmish, multiplayer, campaign}) {
        OA_CHECK(key.starts_with("open-annihilation.mutators."));
        OA_CHECK(key.find('|') == std::string_view::npos);
    }
}

/// crash_values and read_crash round-trip each setup and none.
void crash_values_round_trip() {
    const std::optional<memory::Setup> setups[] = {
        memory::Setup::skirmish,
        memory::Setup::multiplayer,
        memory::Setup::campaign,
        std::nullopt,
    };
    for (const std::optional<memory::Setup>& setup : setups) {
        memory::Crash crash;
        crash.setup = setup;
        crash.loadout = sample_loadout();
        const std::optional<memory::Crash> read = memory::read_crash(memory::crash_values(crash));
        OA_CHECK(read.has_value());
        OA_CHECK(read->setup == setup);
        OA_CHECK(read->loadout == crash.loadout);
    }
    memory::Crash none;
    const preferences::Values values = memory::crash_values(none);
    OA_CHECK(values.at("setup") == "none");
    const std::optional<memory::Crash> read = memory::read_crash(values);
    OA_CHECK(read.has_value() && !read->setup.has_value() && read->loadout.entries.empty());
}

/// The sentinel file: missing, written, cleared, garbled and unwritable.
void the_sentinel_file() {
    {
        Scratch scratch;
        const memory::Sentinel opened = memory::Sentinel::open_folder(scratch.folder);
        OA_CHECK(!opened.leftover().has_value());
        OA_CHECK(!fs::exists(scratch.folder / memory::sentinel_file_name));
    }
    {
        Scratch scratch;
        memory::Crash crash;
        crash.setup = memory::Setup::multiplayer;
        crash.loadout = sample_loadout();
        {
            memory::Sentinel writing = memory::Sentinel::open_folder(scratch.folder);
            writing.write(crash);
        }
        const memory::Sentinel opened = memory::Sentinel::open_folder(scratch.folder);
        OA_CHECK(opened.leftover().has_value());
        OA_CHECK(opened.leftover()->setup == memory::Setup::multiplayer);
        OA_CHECK(opened.leftover()->loadout == crash.loadout);
        memory::Sentinel clearing = memory::Sentinel::open_folder(scratch.folder);
        clearing.clear();
        OA_CHECK(!fs::exists(scratch.folder / memory::sentinel_file_name));
        // clear deletes the file and leaves the leftover that was read.
        OA_CHECK(clearing.leftover().has_value());
    }
    {
        Scratch scratch;
        const fs::path file = scratch.folder / memory::sentinel_file_name;
        {
            std::ofstream output(file, std::ios::binary);
            output << "this is not a crash record\n";
        }
        CapturedError captured;
        const memory::Sentinel opened = memory::Sentinel::open_folder(scratch.folder);
        OA_CHECK(opened.leftover().has_value());
        OA_CHECK(opened.leftover()->loadout.entries.empty());
        OA_CHECK(!opened.leftover()->setup.has_value());
        const std::string logged = captured.text();
        OA_CHECK(logged.find("open-annihilation: mutators:") != std::string::npos);
        OA_CHECK(logged.find('\n') != std::string::npos);
        OA_CHECK(logged.find('\n') == logged.size() - 1);
    }
    {
        Scratch scratch;
        const fs::path blocker = scratch.folder / "blocker";
        {
            std::ofstream output(blocker, std::ios::binary);
            output << "a file, not a folder\n";
        }
        CapturedError captured;
        memory::Sentinel sentinel = memory::Sentinel::open_folder(blocker / "profile");
        memory::Crash crash;
        crash.loadout = sample_loadout();
        sentinel.write(crash);
        sentinel.write(crash);
        const std::string logged = captured.text();
        OA_CHECK(logged.find("cannot write") != std::string::npos);
        OA_CHECK(logged.find('\n') == logged.size() - 1);
    }
    {
        Scratch scratch;
        CapturedError captured;
        memory::Sentinel sentinel = memory::Sentinel::disabled();
        memory::Crash crash;
        crash.loadout = sample_loadout();
        sentinel.write(crash);
        sentinel.clear();
        OA_CHECK(!sentinel.leftover().has_value());
        OA_CHECK(captured.text().empty());
        OA_CHECK(!fs::exists(scratch.folder / memory::sentinel_file_name));
    }
}

} // namespace

int main() {
    try {
        a_set_reads_back();
        text_that_does_not_parse_is_no_set();
        the_three_keys_differ();
        crash_values_round_trip();
        the_sentinel_file();
    } catch (const std::exception& error) {
        std::cerr << "mutators memory: " << error.what() << '\n';
        return 1;
    }
    return oa::test::check_exit_status();
}
