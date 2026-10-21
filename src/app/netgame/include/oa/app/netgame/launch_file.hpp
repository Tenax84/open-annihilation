// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A launch: Open Annihilation's .oalaunch file, TA Forever's INI, the
// options that name each launch setting, or a launch piped in on standard
// input. All of them fill one launch plan, and one table names every setting
// with its field, its option and what it sets, so that the file, the options,
// --help and the manual say the same. The header lives under this module's
// include folder; the ticket's file list names it beside the sources.
#pragma once

#include "oa/ui/frontend_multiplayer/launch_block.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app::netgame {

/// A launch the game refuses before a window opens.
///
/// The message names the file and the field, or the option, and why. The
/// process exits with status 2.
class LaunchRefusal : public std::runtime_error {
  public:

    /// Reports a launch the game will not start from.
    ///
    /// @param message the line, naming the file and the field, or the option, and why
    explicit LaunchRefusal(const std::string& message);
};

/// The most characters of a player's name: what the multiplayer screens'
/// name entry takes, what the battle room keeps and what the session sends
/// the other players. A launch's longer name is refused; TA Forever's file
/// keeps the first ones.
inline constexpr std::size_t kPlayerNameLimit = 16;
/// The most characters of a game's name: what the multiplayer screens'
/// entry takes, what the battle room keeps and what the game list shows of
/// it. A launch's longer name is refused; TA Forever's file keeps the first
/// ones.
inline constexpr std::size_t kGameNameLimit = 16;
/// The most characters of a game's password: what the multiplayer screens'
/// entry takes and what the battle room keeps and sends every player. A
/// launch's longer password is refused; TA Forever's file keeps the first
/// ones.
inline constexpr std::size_t kPasswordLimit = 10;

/// Whether the launch hosts a game or joins one. Unset until a file or the command line says.
enum class LaunchRole : uint8_t {
    unset, ///< neither host nor join
    host,  ///< create the game
    join,  ///< join a game at an address
};

/// When a launched game leaves after the match.
enum class QuitAfter : uint8_t {
    unset,      ///< the launch did not say
    off,        ///< stay after the game
    game,       ///< leave when the game ends
    after_score ///< leave after the score
};

/// One mutator a launch names, in the order the file lists them.
///
/// The version is empty when the file names none. Each option is its id and
/// its value as written. `on` and `true` are stored as `true`; `off` and
/// `false` are stored as `false`.
struct LaunchMutator {
    std::string id{};
    std::string version{};
    std::vector<std::pair<std::string, std::string>> options{};
};

/// The most mutators one launch names. The same cap as one loadout.
inline constexpr std::size_t kLaunchMutatorLimit = 32;

/// The base game, expansions and mod a launch names for this run.
struct LaunchContentChoice {
    std::string base_game{};               ///< content.base-game; empty when unnamed
    std::vector<std::string> expansions{}; ///< content.expansions
    std::string mod{};                     ///< content.mod, an id or a path
    bool named = false;                    ///< the launch has a content group with a choice
    /// The mutators this launch names, top first. It does not set `named`.
    std::vector<LaunchMutator> mutators{};
    /// The file has `content.mutators`, including an empty list. It does not set `named`.
    bool mutators_named = false;
};

/// What a launch asks to be told about the game: the status stream.
struct LaunchStatusChoice {
    bool standard_output_named = false;      ///< status.standard-output was written
    bool standard_output = false;            ///< status.standard-output asks for standard output
    std::string file{};                      ///< status.file; empty when unnamed
    QuitAfter quit_after = QuitAfter::unset; ///< status.quit-after-game
};

/// One launch, from any of its forms: the battle room's block, the direct game, DirectPlay and content.
struct LaunchPlan {
    oa::ui::frontend_multiplayer::launch::LaunchBlock block{};
    LaunchRole role = LaunchRole::unset;
    std::string address{};     ///< the host's address, for a join
    std::string game_name{};   ///< the game's name
    std::string player_name{}; ///< the player's name
    std::string password{};    ///< the game's password; never written to the log
    std::optional<std::array<uint8_t, 16>> dplay_guid{}; ///< wire byte order
    std::optional<uint16_t> dplay_port{};
    LaunchContentChoice content{};
    LaunchStatusChoice status{};
    bool ini = false; ///< read from TA Forever's INI
    /// The INI names connect, creator or address, which then decide the
    /// transport, hosting or joining and the address, as in 3.1c.
    bool connection_named = false;
    std::string source{}; ///< the file's name as it was given, "standard input", or empty
    /// The fields an option replaced, each with the option that did, so a
    /// later refusal of that field names the option.
    std::vector<std::pair<std::string, std::string>> replaced{};
    /// The fields keys of the INI filled, each with the key as the file
    /// wrote it, so a later refusal of that field names the key.
    std::vector<std::pair<std::string, std::string>> ini_keys{};
    /// Lines for the game's log: keys the INI holds that are not read, and
    /// where a file named without a folder was found.
    std::vector<std::string> notes{};
};

/// Which part of the game reads a launch setting's option from the command line.
enum class LaunchOptionReader : uint8_t {
    network_play, ///< network play's launch options, which replace the field in the plan
    /// network play's options for a game to host or join (--host, --join, the
    /// names, the password and DirectPlay's), which work without a launch too
    direct_game,
    engine,        ///< the engine's own options, which choose the content
    status_stream, ///< the status stream's options
};

/// Where a value comes from: the file's YAML value, or an option's text.
struct LaunchValue;

/// Where a refusal points: the file and the field, or the option.
struct LaunchPlace {
    std::string_view source{}; ///< the file's name; empty for an option
    std::string_view name{};   ///< the field, such as rules.metal, or the option, such as --metal
};

/// One launch setting: its field in the file, its option and what it sets.
///
/// launch_fields() lists every setting. The file's reader, the options,
/// --help and the manual's table are all made from that list.
struct LaunchField {
    std::string_view field{};   ///< the dotted name in the file, such as rules.metal
    std::string_view option{};  ///< the option with its value, such as --metal N
    std::string_view meaning{}; ///< what it sets, as the manual's table says it
    LaunchOptionReader reader = LaunchOptionReader::network_play;
    /// Stores the file's value, or a network_play option's, into a plan; throws
    /// LaunchRefusal naming the place.
    void (*store)(LaunchPlan& plan, const LaunchValue& value, const LaunchPlace& place){};
};

/// Returns every launch setting, in the order the manual lists them.
///
/// @return the table
[[nodiscard]] std::span<const LaunchField> launch_fields() noexcept;

/// Finds the setting a field names.
///
/// @param field the dotted name, such as rules.metal
/// @return the setting, or null when no setting has that field
[[nodiscard]] const LaunchField* find_launch_field(std::string_view field) noexcept;

/// Finds the setting network play reads from an option.
///
/// @param option the option as written, such as --metal or --lock-options=off
/// @return the setting, or null when the option names none of network play's
///         launch settings; --host, --join, the names, the password and the
///         DirectPlay options keep their own readers and are not found here
[[nodiscard]] const LaunchField* find_launch_option(std::string_view option) noexcept;

/// Tells whether an option of network play's launch settings takes a value
/// from the next argument.
///
/// --lock-options and --tournament take none: they turn the setting on, and
/// --lock-options=off and --tournament=off turn it off.
///
/// @param option the option as written
/// @return true when the next argument is its value
[[nodiscard]] bool launch_option_takes_value(std::string_view option) noexcept;

/// One key of the INI 3.1c's -c reads, which TA Forever writes, and the launch field it fills.
struct LaunchIniKey {
    std::string_view key{};     ///< the key, as 3.1c names it; read in any case
    std::string_view field{};   ///< the launch field it fills; empty when none
    std::string_view meaning{}; ///< what the game does with the value
    bool read = true;           ///< false for a key 3.1c never reads, which is logged
};

/// Returns every key of the INI and the launch field each fills.
///
/// The 22 keys 3.1c reads come first, in the order it reads them, then the
/// two TA Forever writes that 3.1c never reads.
///
/// @return the table, in the order the manual lists it
[[nodiscard]] std::span<const LaunchIniKey> launch_ini_keys() noexcept;

/// Decides hosting or joining from a launch block, as 3.1c's screens do.
///
/// With the TCP/IP transport and an address, the host flag hosts and its
/// absence joins the game at the address. Anything else leaves the choice
/// to the multiplayer screens.
///
/// @param block the block
/// @return host, join, or unset for the screens
[[nodiscard]] LaunchRole
role_from_record(const oa::ui::frontend_multiplayer::launch::LaunchBlock& block) noexcept;

/// The two kinds of launch file, told apart by what they hold.
enum class LaunchFileKind : uint8_t {
    neither,           ///< not a launch file
    open_annihilation, ///< begins with an oalaunch: line
    ta_forever,        ///< has a [totala] section
};

/// Tells which kind of launch file a text is, whatever the file's name.
///
/// A byte order mark is skipped. A text whose first line, after blank lines
/// and lines that start with #, begins with `oalaunch:` is Open
/// Annihilation's. Otherwise a text with a `[totala]` section is TA
/// Forever's.
///
/// @param text the file's bytes
/// @return the kind
[[nodiscard]] LaunchFileKind launch_file_kind(std::string_view text) noexcept;

/// Reads a launch file's text into one plan, by what it holds.
///
/// An INI is read as 3.1c reads it: the first `[TotalA]` section, in any
/// case, its keys in any case, the first copy of each key, a value's quotes
/// dropped, a text cut to its field, and a number the whole number its value
/// begins with. A key it does not read is noted, never refused, and no INI
/// value is refused. An INI that names none of connect, creator and address
/// hosts over TCP/IP, where 3.1c leaves the choice to its menus, so that TA
/// Forever's file starts a game. An .oalaunch file is YAML that begins with
/// `oalaunch: 1`, and a value the battle room cannot use throws
/// LaunchRefusal. The password is not copied into that message.
///
/// @param source the file's name, used in a refusal and a note
/// @param text the file's bytes
/// @return the plan
/// @throws LaunchRefusal the text is not a launch file, or an .oalaunch field cannot be used
[[nodiscard]] LaunchPlan read_launch_text(std::string_view source, std::string_view text);

/// Where a launch file named without a folder is looked for after the current folder.
struct LaunchFolders {
    /// Finds the game folder; null, or a call that finds none, leaves the
    /// current folder alone.
    std::optional<std::filesystem::path> (*game_folder)(const void* context){};
    const void* context{}; ///< handed back to game_folder
};

/// Tells whether a launch file's name names no folder: it holds no `\`, `/` or `:`.
///
/// @param path the name
/// @return true for a bare name
[[nodiscard]] bool names_bare_file(std::string_view path) noexcept;

/// Reads a launch file from a path, or from standard input when `path` is "-".
///
/// A bare name (names_bare_file) not in the current folder is looked for in
/// the game folder `folders` finds.
///
/// @param path the file, or "-" for standard input
/// @param folders where a bare name is looked for after the current folder
/// @return the plan; its source is the path, or "standard input"
/// @throws LaunchRefusal the file cannot be read, or read_launch_text refuses it
[[nodiscard]] LaunchPlan read_launch_file(std::string_view path, const LaunchFolders& folders = {});

/// Replaces one field of a plan with an option's value, checked as the file's value is.
///
/// @param[in,out] plan the plan
/// @param field the setting
/// @param option the option as written, which a refusal names
/// @param text the option's value; empty for an option that takes none
/// @throws LaunchRefusal the value cannot be used
void replace_launch_field(
    LaunchPlan& plan, const LaunchField& field, std::string_view option, std::string_view text
);

/// Checks a plan as a whole once every field is known.
///
/// A deathmatch or a tournament game seats no computer players, the computer
/// players and the player fit the player limit, and a host who watches
/// allows watchers.
///
/// @param plan the plan
/// @throws LaunchRefusal naming the field, or the option that replaced it
void check_launch_plan(const LaunchPlan& plan);

/// Returns where a refusal of a field points: the option that replaced it, or the file and the field.
///
/// A field a key of the INI filled is named by that key.
///
/// @param plan the plan
/// @param field the dotted field
/// @return "--map", "launch.oalaunch: map" or "taforever.ini: mission"
[[nodiscard]] std::string launch_place(const LaunchPlan& plan, std::string_view field);

/// The text standard input is read from on this system: a file descriptor, or a Windows handle.
#if defined(_WIN32)
using InputHandle = void*;
#else
using InputHandle = int;
#endif

/// How long a start waits for the first byte of standard input before it starts as usual.
inline constexpr std::chrono::milliseconds kPipedLaunchWait{250};

/// How long the rest of a launch piped in may take once it began.
inline constexpr std::chrono::milliseconds kPipedLaunchRest{2000};

/// The most bytes a launch file may hold, as for every YAML file the game reads.
inline constexpr std::size_t kLaunchFileLimit = std::size_t{256} * 1024;

/// What a look at standard input found.
struct PipedLaunch {
    bool found = false; ///< the input began with `oalaunch:` and was read to its end
    std::string text{}; ///< the whole launch, when found
};

/// Returns this process's standard input.
///
/// @return the descriptor or handle
[[nodiscard]] InputHandle standard_input() noexcept;

/// Reads a launch piped in, when the input is a pipe or a file and begins one.
///
/// A terminal, a device, an empty file and an input that has no first byte
/// within `first_byte_wait` are left alone. The first bytes are read one at
/// a time and the read stops at the first that does not begin `oalaunch:`
/// (after a byte order mark), so another program's input loses at most those
/// bytes. A launch is then read to the input's end.
///
/// @param input the input
/// @param first_byte_wait how long to wait for the first byte of a pipe
/// @param rest_wait how long the rest of the launch may take
/// @return the launch, or found false when the input holds none
/// @throws LaunchRefusal the launch does not end within `rest_wait`, or is
///         longer than kLaunchFileLimit
[[nodiscard]] PipedLaunch read_piped_launch(
    InputHandle input,
    std::chrono::milliseconds first_byte_wait,
    std::chrono::milliseconds rest_wait
);

/// Keeps the status block of the launch this run starts from, for the status stream.
///
/// Network play keeps it as it checks the command line. The status stream,
/// whose own check comes after, turns on what the block asks for where its
/// own options did not.
///
/// @param status the block; an empty one when the run has no launch
void keep_launch_status(const LaunchStatusChoice& status);

/// Returns the status block network play kept.
///
/// @return the block; empty until a launch is read
[[nodiscard]] const LaunchStatusChoice& kept_launch_status() noexcept;

} // namespace oa::app::netgame
