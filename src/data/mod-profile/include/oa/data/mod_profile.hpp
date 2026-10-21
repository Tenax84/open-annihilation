// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A mod profile (oamod.yaml) resolved against the registry: how a mod
// differs from 3.1c, as the plain records the rest of the engine reads, with
// the canonical form and the two hashes that name the ruleset.
//
// Resolution follows the registry's order for each parameter, later steps
// winning: the 3.1c baseline while the entry is absent or false; the
// registry default once it is on; a named preset; the profile's own value;
// the player's settings, for install and match parameters the profile binds;
// the player's own overrides of standard hacks (Developer Mode), each checked
// as a profile's hack is and left out with a warning when it does not fit;
// then the host's choice for one game, for match parameters, which therefore
// wins over an override. (A per-unit or per-weapon data key, the last step,
// applies to that unit or weapon only, when its files are read.)
//
// The profile is validated as it is read. A missing author or packaging
// block, an unknown key, hack, script
// extension or data key, a value of the wrong type or out of range, an
// index mounted twice, a binding of a fixed parameter, a constraint broken,
// a data key whose hack is off, or a hack this engine does not implement yet
// each refuses the profile, naming the offending path and where it is
// written; nothing falls back to 3.1c silently. A development run may accept
// hacks that are not implemented yet, which then only warn.
//
// The canonical form is the effective profile as RFC 8785 canonical JSON;
// the full hash is its SHA-256, and the sim hash the SHA-256 of its sim-scope
// part, which every machine of a network game must share.
//
// This layer reads bytes and plain values only and never opens a file.
#pragma once

#include "oa/base/sha256.hpp"
#include "oa/data/limits.hpp"
#include "oa/data/match_rules.hpp"
#include "oa/data/mod_profile/overrides.hpp"
#include "oa/data/mod_profile/value.hpp"
#include "oa/formats/oamod.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::mod_profile {

using match_rules::EnumSet;
using match_rules::FixedList;
using match_rules::FixedText;

#include "oa/data/mod_profile/records.inc"

/// The most characters (Unicode code points) a profile's description holds:
/// one line of plain text, with no line break or other control character.
inline constexpr size_t max_description_characters = 120;

/// A registry value a profile seeds for a fresh install, as the game would
/// find it in its registry.
struct RegistrySeed {
    std::string name{};  ///< the registry value's name
    std::string value{}; ///< its value as text
    bool integer{};      ///< the profile wrote an integer, not a string
};

/// Who made a mod, as its profile names them.
struct Author {
    std::string name{};  ///< "unknown" when nobody is known
    std::string email{}; ///< empty when the profile gives no address
};

/// Who packaged a mod's profile and files, when, and how often since.
struct Packaging {
    int64_t revision{};     ///< counts the package's updates from 1; 0 for base 3.1c
    std::string date{};     ///< the day the package was made, as YYYY-MM-DD
    std::string packager{}; ///< who made the package
};

/// Everything a resolved profile says about a mod.
///
/// A default-constructed ModProfile is base 3.1c.
struct ModProfile {
    std::string id{};      ///< the mod's stable id, kebab-case
    std::string name{};    ///< the mod's display name
    std::string version{}; ///< the mod's version
    /// One line about the mod for the Library; empty when the profile has none.
    std::string description{};
    /// An http or https address for the mod; empty when the profile has none.
    std::string homepage{};
    /// How the mod is filed, in the order written; empty when the profile has none.
    std::vector<std::string> tags{};
    /// The engine requirement as written; empty when the profile has none.
    std::string requires_engine{};
    /// Base games the mod runs with. totala when the profile omits supportedGames.
    std::vector<std::string> supported_games{"totala"};
    /// Expansions that must be on. Empty when the profile names none.
    std::vector<std::string> required_expansions{};
    Author author{};
    Packaging packaging{};
    Identity identity{};
    Layout layout{};
    limits::Limits limits{}; ///< the profile's limits block; 3.1c's when it has none
    match_rules::MatchRules rules{};
    NetworkRules network{};
    RecorderRules recorder{};
    UiRules ui{};
    DataKeyBindings data_keys{};
    Strings strings{};
    Media media{};
    std::vector<RegistrySeed> registry_seeds{}; ///< values the profile seeds, in its order
    base::sha256::Digest sim_hash{};            ///< SHA-256 of the sim-scope canonical form
    base::sha256::Digest full_hash{};           ///< SHA-256 of the whole canonical form
};

/// One error or warning about a profile.
struct Diagnostic {
    std::string source{};                    ///< the profile's name, such as its file
    formats::oamod::TextPosition position{}; ///< where in it; 0:0 when not in the file
    std::string path{};                      ///< the offending path, such as hacks.repair.rate.mode
    std::string message{};
};

/// Writes a diagnostic as one line: source:line:column: path: message.
///
/// @param diagnostic the diagnostic
/// @return the line, without a line break
[[nodiscard]] std::string format_diagnostic(const Diagnostic& diagnostic);

/// One setting the player has: an INI Section/Key, or a registry value's name.
struct SettingValue {
    std::string name{};  ///< matched without case
    std::string value{}; ///< as the file or registry holds it
};

/// The player's settings a profile may bind parameters to.
struct Settings {
    std::vector<SettingValue> ini{}; ///< the mod's INI file, by Section/Key
    std::vector<SettingValue>
        registry{}; ///< the mod's registry values; profile seeds fill the gaps
};

/// The host's choice of a match parameter for one game.
struct MatchOption {
    std::string path{};  ///< ENTRY.PARAM, or an entry with a shorthand parameter
    std::string value{}; ///< the value, written as in a profile
    /// Names who chose the value. Provenance reads "match <source>" when this
    /// is set, and "match" when it is empty.
    std::string source{};
    /// Turn the entry on when the profile or an override leaves it off; only
    /// for entries the registry marks mutator: true.
    bool turn_on{};
};

/// Reads "ENTRY.PARAM=VALUE".
///
/// @param text the option
/// @return the option, or nullopt without an '='
[[nodiscard]] std::optional<MatchOption> parse_match_option(std::string_view text);

/// The registry-only problem with a match option, without a profile.
///
/// A known ENTRY.PARAM or shorthand, adjustable per match, and a value of
/// the right type inside its bounds. It does not ask whether the entry is
/// in a profile. The message is the one resolving the option would report.
///
/// @param option the choice
/// @return that message, or none when the registry accepts the choice
[[nodiscard]] std::optional<std::string> match_option_problem(const MatchOption& option);

/// The registry's reading of one match option.
struct MatchOptionReading {
    /// The refusal, or none when the registry accepts the value.
    std::optional<std::string> problem{};
    /// The value's normal form when it is accepted. Empty when it is refused.
    std::string normal{};
};

/// The registry-only reading of a match option, including its normal form.
///
/// The same checks as `match_option_problem`. When the value is accepted,
/// `normal` is the text the registry stores, so a comment after a number
/// is not part of the value.
///
/// @param option the choice
/// @return the refusal, or the value's normal form
[[nodiscard]] MatchOptionReading match_option_reading(const MatchOption& option);

/// What resolution takes besides the profile.
struct ResolveOptions {
    Settings settings{};
    std::vector<MatchOption> match{};
    /// Accept hacks this engine does not implement yet, with a warning each,
    /// for development; a player's run refuses them.
    bool accept_unimplemented_hacks{};
    /// The player's overrides of standard hacks, laid over the profile after
    /// its own steps and the settings, and before the host's match choices:
    /// each turns its hack on or off and sets the parameters it names. Each
    /// is checked as a
    /// profile's hack is (a known hack and parameters, every value fitting
    /// its parameter, the hack's constraints kept, the hack implemented to be
    /// turned on, and off only while no data key needs it); one that fails is
    /// left out whole with a warning naming why. They change the hashes as
    /// the same values written in the profile would.
    std::vector<HackOverride> overrides{};
};

/// Which step set one value of the effective profile.
struct Provenance {
    std::string path{}; ///< such as hacks.units.id-reuse-delay.ticks
    std::string
        source{}; ///< baseline, default, profile, the profile's id, a preset, a setting or match
};

/// A resolved profile.
struct Resolution {
    ModProfile profile{};
    Value effective{};       ///< the effective profile
    Value sim{};             ///< its sim-scope part
    std::string canonical{}; ///< the effective profile's canonical JSON, the full hash's input
    std::vector<Provenance> provenance{}; ///< sorted by path
};

/// The outcome of resolving a profile.
struct ResolveResult {
    std::optional<Resolution> resolution{}; ///< set when there are no errors
    std::vector<Diagnostic> errors{};
    std::vector<Diagnostic> warnings{};
};

/// Reads, validates and resolves a profile.
///
/// @param text the profile's bytes
/// @param source the profile's name for diagnostics, such as its path
/// @param options the player's settings, the host's match options and
///        whether unimplemented hacks are accepted
/// @return the resolution, or the errors; warnings either way
[[nodiscard]] ResolveResult resolve_profile(
    std::span<const uint8_t> text, std::string_view source, const ResolveOptions& options = {}
);

/// Writes a resolved profile for people: the effective profile as indented
/// JSON, then "sha256-sim  " and "sha256-full " lines with the hashes.
///
/// @param resolution the resolved profile
/// @return the text, ending with a line feed
[[nodiscard]] std::string describe_resolution(const Resolution& resolution);

/// Writes a profile that turns on every limit and hack that can play as 3.1c
/// while on, each at its baseline preset, and nothing else.
///
/// Played, it must give exactly what a game without a profile gives; the
/// determinism checks hold every rule to that. Its hacks resolve only while
/// unimplemented hacks are accepted, until every one is implemented.
///
/// @return the profile's text, id "baseline-rules"
[[nodiscard]] std::string baseline_profile_text();

/// Writes a digest as lower-case hexadecimal.
///
/// @param digest the digest
/// @return 64 hexadecimal digits
[[nodiscard]] std::string digest_text(const base::sha256::Digest& digest);

} // namespace oa::data::mod_profile
