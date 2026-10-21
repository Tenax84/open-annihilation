// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/app/mutators_memory.hpp"

#include <exception>
#include <iostream>
#include <ostream>
#include <system_error>
#include <utility>

namespace oa::app::mutators_memory {

namespace preferences = oa::platform::preferences;
namespace mutators = oa::data::mutators;

namespace {

/// The crash file's key for the setup that started the match.
constexpr std::string_view setup_key = "setup";
/// The crash file's key for the canonical loadout text.
constexpr std::string_view mutators_key = "mutators";

/// The word `crash_values` writes for a setup, or `none` when no setup
/// started the match.
///
/// @param setup the setup, or nothing
/// @return `skirmish`, `multiplayer`, `campaign` or `none`
std::string_view setup_word(std::optional<Setup> setup) noexcept {
    if (!setup)
        return "none";
    switch (*setup) {
    case Setup::skirmish:
        return "skirmish";
    case Setup::multiplayer:
        return "multiplayer";
    case Setup::campaign:
        return "campaign";
    }
    return "none";
}

/// Reads `setup`'s word.
///
/// @param word `skirmish`, `multiplayer`, `campaign` or `none`
/// @param[out] setup the setup, empty for `none`
/// @return false when the word is not one of those
bool read_setup_word(std::string_view word, std::optional<Setup>& setup) noexcept {
    if (word == "none") {
        setup.reset();
        return true;
    }
    if (word == "skirmish")
        setup = Setup::skirmish;
    else if (word == "multiplayer")
        setup = Setup::multiplayer;
    else if (word == "campaign")
        setup = Setup::campaign;
    else
        return false;
    return true;
}

} // namespace

preferences::Values crash_values(const Crash& crash) {
    return preferences::Values{
        {std::string(setup_key), std::string(setup_word(crash.setup))},
        {std::string(mutators_key), mutators::canonical_loadout(crash.loadout)},
    };
}

std::optional<Crash> read_crash(const preferences::Values& values) {
    if (values.size() != 2)
        return std::nullopt;
    const auto setup_at = values.find(std::string(setup_key));
    const auto mutators_at = values.find(std::string(mutators_key));
    if (setup_at == values.end() || mutators_at == values.end())
        return std::nullopt;
    Crash crash;
    if (!read_setup_word(setup_at->second, crash.setup))
        return std::nullopt;
    std::optional<mutators::Loadout> loadout =
        mutators::parse_loadout(mutators_at->second, nullptr);
    if (!loadout)
        return std::nullopt;
    crash.loadout = std::move(*loadout);
    return crash;
}

Sentinel Sentinel::open_folder(const std::filesystem::path& folder) {
    Sentinel sentinel;
    sentinel.enabled_ = true;
    sentinel.folder_ = folder;
    const std::filesystem::path file = folder / sentinel_file_name;
    std::error_code present_error;
    const bool present = std::filesystem::exists(file, present_error);
    if (present_error || !present)
        return sentinel;
    try {
        const preferences::Values values = preferences::load(file);
        if (std::optional<Crash> crash = read_crash(values))
            sentinel.leftover_ = std::move(*crash);
        else {
            sentinel.leftover_ = Crash{};
            sentinel.log_failure(
                std::string(sentinel_file_name) + " cannot be read and is ignored"
            );
        }
    } catch (const std::exception& error) {
        sentinel.leftover_ = Crash{};
        sentinel.log_failure(
            std::string(sentinel_file_name) + " cannot be read and is ignored: " + error.what()
        );
    }
    return sentinel;
}

Sentinel Sentinel::disabled() {
    return Sentinel{};
}

void Sentinel::write(const Crash& crash) {
    if (!enabled_)
        return;
    try {
        preferences::save(folder_ / sentinel_file_name, crash_values(crash));
    } catch (const std::exception& error) {
        log_failure("cannot write " + std::string(sentinel_file_name) + ": " + error.what());
    }
}

void Sentinel::clear() {
    if (!enabled_)
        return;
    std::error_code error;
    std::filesystem::remove(folder_ / sentinel_file_name, error);
    if (error)
        log_failure("cannot delete " + std::string(sentinel_file_name) + ": " + error.message());
}

void Sentinel::log_failure(std::string_view text) {
    if (failure_logged_)
        return;
    failure_logged_ = true;
    std::cerr << "open-annihilation: mutators: " << text << '\n';
}

} // namespace oa::app::mutators_memory
