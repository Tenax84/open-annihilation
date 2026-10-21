// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A launch file the system opens in the game: told by its name or by its first bytes, read
// as it arrives and kept until the game takes it; at start it becomes the launch exactly as
// -c FILE makes it, a refusal puts nothing in place, its content is compared with what the
// game plays, and the running game goes to its battle room from the main menu, goes back to
// the main menu from the single-player menu, and refuses it in a match, while a game is set
// up, anywhere else, and while a launch is already under way.
#include "net_options.hpp"
#include "oa/app/app.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/netgame/opened_launch.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace net = oa::app::netgame;
namespace mp = oa::ui::frontend_multiplayer;
namespace nl = oa::ui::frontend_multiplayer::launch;

// A host's launch, with two computer players.
constexpr std::string_view kLaunch = R"(oalaunch: 1
game:
  role: host
  name: Example game
  player-limit: 4
  launcher-name: Ladder
player:
  name: Commander
  side: core
  team: 1
  colour: 2
map: Acid Foursome
rules:
  commander-death: ends
  metal: 1200
  energy: 1500
computer-players:
  - side: arm
    team: 2
    difficulty: hard
  - side: core
    team: 1
)";

// The same launch with a comment first and a byte order mark, as an editor may save it.
constexpr std::string_view kMarkedLaunch = "\xef\xbb\xbf# for tonight's game\n\noalaunch: 1\n"
                                           "game:\n  role: join\n  address: 192.0.2.10\n";

constexpr std::string_view kIni = "[totala]\nsession=Example game\nmission=Acid Foursome\n";

/// A scratch folder that is removed when the checks end.
struct Scratch {
    fs::path dir = oa::test::make_scratch_directory("G106-opened-launch");

    ~Scratch() {
        std::error_code failure;
        fs::remove_all(dir, failure);
    }

    /// Writes one file and returns its path.
    ///
    /// @param name the file's name
    /// @param text the bytes
    /// @return the path
    fs::path write(const char* name, std::string_view text) const {
        const fs::path file = dir / name;
        std::ofstream output(file, std::ios::binary);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        OA_CHECK(static_cast<bool>(output));
        return file;
    }
};

/// Tells whether a fixed text field holds `text`.
///
/// @param field the field
/// @param text the text
/// @return true when it does
template <std::size_t Size>
bool same_text(const char (&field)[Size], std::string_view text) {
    return std::string_view(field, ::strnlen(field, Size)) == text;
}

/// What the launch puts in place, compared field by field.
///
/// @param left one context
/// @param right the other
/// @return true when the launch block, the game to host or join and the launch's record agree
bool same_launch(const oa::app::NetgameContext& left, const oa::app::NetgameContext& right) {
    const nl::LaunchBlock& one = left.launch.block;
    const nl::LaunchBlock& other = right.launch.block;
    const bool texts = std::strcmp(one.session_name, other.session_name) == 0 &&
                       std::strcmp(one.mission, other.mission) == 0 &&
                       std::strcmp(one.provider_label, other.provider_label) == 0 &&
                       std::strcmp(one.user_name, other.user_name) == 0 &&
                       std::strcmp(one.address, other.address) == 0;
    const bool values = one.connection_type == other.connection_type &&
                        one.hosting == other.hosting && one.player_limit == other.player_limit &&
                        one.metal == other.metal && one.energy == other.energy &&
                        one.death_option == other.death_option && one.faction == other.faction &&
                        one.team == other.team && one.color == other.color &&
                        one.computer_count == other.computer_count;
    bool computers = true;
    for (int32_t index = 0; index < one.computer_count && index < nl::kLaunchComputerLimit; ++index)
        computers = computers && one.computers[index].faction == other.computers[index].faction &&
                    one.computers[index].team == other.computers[index].team &&
                    one.computers[index].difficulty == other.computers[index].difficulty;
    const oa::app::NetOptions& a = left.options;
    const oa::app::NetOptions& b = right.options;
    return texts && values && computers && a.launch_planned && b.launch_planned &&
           a.direct_game.kind == b.direct_game.kind &&
           a.direct_game.game_name == b.direct_game.game_name &&
           a.direct_game.player_name == b.direct_game.player_name &&
           a.direct_game.address == b.direct_game.address && a.launch_source == b.launch_source;
}

/// A file is a launch by its extension, in any case, or by its first bytes.
void test_told_by_name_or_bytes() {
    OA_CHECK(net::is_launch_file("oalaunch", ""));
    OA_CHECK(net::is_launch_file("OALaunch", "hello\n"));
    OA_CHECK(net::is_launch_file("txt", kLaunch));
    OA_CHECK(net::is_launch_file("", kMarkedLaunch));
    OA_CHECK(!net::is_launch_file("txt", "hello\n"));
    OA_CHECK(!net::is_launch_file("yaml", "game:\n  role: host\noalaunch: 1\n"));
    // TA Forever's INI starts a game only after -c, or named .oalaunch.
    OA_CHECK(!net::is_launch_file("ini", kIni));
    OA_CHECK(!net::is_launch_file("oamod", ""));
}

/// An opened file is read as it arrives: a launch whole, another file not at all.
void test_read_as_it_arrives(const Scratch& scratch) {
    const fs::path named = scratch.write("host-now.oalaunch", kLaunch);
    const fs::path piped = scratch.write("4B1C0E2A.txt", kLaunch);
    const fs::path notes = scratch.write("notes.txt", "Bring snacks.\n");
    const std::optional<net::OpenedLaunch> by_name = net::read_opened_launch(named);
    OA_CHECK(by_name && by_name->problem.empty() && by_name->text == kLaunch);
    OA_CHECK(by_name && by_name->source == oa::app::path_to_utf8(named));
    // `open -f` hands over a temporary .txt file; it is read by what it holds.
    const std::optional<net::OpenedLaunch> by_content = net::read_opened_launch(piped);
    OA_CHECK(by_content && by_content->problem.empty() && by_content->text == kLaunch);
    OA_CHECK(!net::read_opened_launch(notes));
    // A launch file that cannot be read is still taken, so the player is told why.
    const std::optional<net::OpenedLaunch> missing =
        net::read_opened_launch(scratch.dir / "gone.oalaunch");
    OA_CHECK(missing && missing->text.empty());
    OA_CHECK(
        missing &&
        missing->problem.find("gone.oalaunch: the file: cannot be read") != std::string::npos
    );
    OA_CHECK(!net::read_opened_launch(scratch.dir / "gone.txt"));
    const fs::path long_file =
        scratch.write("long.oalaunch", std::string(net::kLaunchFileLimit + 1, '#'));
    const std::optional<net::OpenedLaunch> too_long = net::read_opened_launch(long_file);
    OA_CHECK(
        too_long && too_long->problem.find("is longer than 262144 bytes") != std::string::npos
    );
    // A refusal of an unreadable file names it.
    bool refused = false;
    try {
        static_cast<void>(net::read_opened_launch_plan(*missing));
    } catch (const net::LaunchRefusal& refusal) {
        refused = std::string(refusal.what()) == missing->problem;
    }
    OA_CHECK(refused);
}

/// The waiting launches are taken oldest first, and the same file twice is kept once.
void test_waiting() {
    while (net::take_opened_launch()) {
    }
    OA_CHECK(!net::opened_launch_waiting());
    net::post_opened_launch({"first.oalaunch", std::string(kLaunch), {}});
    net::post_opened_launch({"second.txt", std::string(kMarkedLaunch), {}});
    net::post_opened_launch({"first.oalaunch", std::string(kLaunch), {}});
    OA_CHECK(net::opened_launch_waiting());
    const auto first = net::take_opened_launch();
    const auto second = net::take_opened_launch();
    OA_CHECK(first && first->source == "first.oalaunch");
    OA_CHECK(second && second->source == "second.txt");
    OA_CHECK(!net::take_opened_launch() && !net::opened_launch_waiting());
}

/// Parses one command line through the game's option hooks into `context`.
///
/// @param[in,out] context the context the hooks fill
/// @param arguments the arguments after the program name
/// @return the run's options
oa::app::Options
parse(oa::app::NetgameContext& context, const std::vector<std::string>& arguments) {
    std::vector<std::string> storage{"open-annihilation"};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& argument : storage)
        argv.push_back(argument.data());
    oa::app::Extension table{};
    oa::app::fill_option_hooks(table, context);
    return oa::app::parse_options(static_cast<int>(argv.size()), argv.data(), table);
}

/// A launch file opened with the game becomes the start's launch exactly as -c FILE makes it.
void test_start_is_minus_c(const Scratch& scratch) {
    const fs::path file = scratch.write("tonight.oalaunch", kLaunch);
    oa::app::NetgameContext with_c{};
    const oa::app::Options c_run = parse(with_c, {"-c", oa::app::path_to_utf8(file)});
    // A .oalaunch file after -c is -c's file, as the registrations write it: it is not
    // taken out of the switch line as a file to open.
    OA_CHECK(c_run.open_files.empty());
    OA_CHECK(with_c.options.launch_planned);
    OA_CHECK(with_c.options.launch_source == oa::app::path_to_utf8(file));
    OA_CHECK(with_c.launch.block.computer_count == 2);
    // So is one after 3.1c's /C, in either case.
    oa::app::NetgameContext with_slash{};
    const oa::app::Options slash_run = parse(with_slash, {"/C", oa::app::path_to_utf8(file)});
    OA_CHECK(slash_run.open_files.empty());
    OA_CHECK(with_slash.options.launch_planned);
    OA_CHECK(with_slash.options.launch_source == oa::app::path_to_utf8(file));
    oa::app::NetgameContext opened{};
    static_cast<void>(parse(opened, {}));
    OA_CHECK(!opened.options.launch_planned);
    const std::optional<net::OpenedLaunch> taken = net::read_opened_launch(file);
    OA_CHECK(taken.has_value());
    if (!taken)
        return;
    oa::app::put_launch_in_place(opened, net::read_opened_launch_plan(*taken));
    OA_CHECK(same_launch(with_c, opened));
    OA_CHECK(opened.options.direct_game.kind == mp::DirectGame::Kind::host);
    OA_CHECK(opened.launch.block.connection_type == nl::connection_type::tcpip);
    OA_CHECK(opened.launch.block.computer_count == 2);
    OA_CHECK(same_text(opened.launch.block.mission, "Acid Foursome"));
    OA_CHECK(same_text(opened.launch.block.provider_label, "Ladder"));

    // The temporary .txt file `open -f` hands over gives the same launch.
    const fs::path piped = scratch.write("0F3A9C.txt", kLaunch);
    oa::app::NetgameContext from_text{};
    static_cast<void>(parse(from_text, {}));
    const std::optional<net::OpenedLaunch> text = net::read_opened_launch(piped);
    OA_CHECK(text.has_value());
    if (text) {
        oa::app::put_launch_in_place(from_text, net::read_opened_launch_plan(*text));
        OA_CHECK(from_text.launch.block.computer_count == 2);
        OA_CHECK(from_text.options.launch_source == oa::app::path_to_utf8(piped));
    }

    // A file the system opens is handed over whole, so a space in its path is part of it.
    fs::create_directories(scratch.dir / "Downloads folder");
    const fs::path spaced = scratch.write("Downloads folder/host now.oalaunch", kLaunch);
    oa::app::NetgameContext from_spaced{};
    static_cast<void>(parse(from_spaced, {}));
    const std::optional<net::OpenedLaunch> spaced_launch = net::read_opened_launch(spaced);
    OA_CHECK(spaced_launch && spaced_launch->problem.empty());
    if (spaced_launch) {
        oa::app::put_launch_in_place(from_spaced, net::read_opened_launch_plan(*spaced_launch));
        OA_CHECK(from_spaced.options.launch_planned);
        OA_CHECK(from_spaced.options.launch_source == oa::app::path_to_utf8(spaced));
    }

    // A join keeps its address.
    oa::app::NetgameContext join{};
    static_cast<void>(parse(join, {}));
    oa::app::put_launch_in_place(join, net::read_launch_text("join.oalaunch", kMarkedLaunch));
    OA_CHECK(join.options.direct_game.kind == mp::DirectGame::Kind::join);
    OA_CHECK(join.options.direct_game.address == "192.0.2.10");
    OA_CHECK(join.launch.block.hosting == 0 && same_text(join.launch.block.address, "192.0.2.10"));
}

/// A launch the battle room cannot use puts nothing in place.
void test_refusal_puts_nothing(const Scratch& scratch) {
    // A deathmatch seats no computer players: the whole plan is refused.
    std::string deathmatch(kLaunch);
    const std::string_view ends = "commander-death: ends";
    deathmatch.replace(deathmatch.find(ends), ends.size(), "commander-death: deathmatch");
    const fs::path file = scratch.write("deathmatch.oalaunch", deathmatch);
    oa::app::NetgameContext context{};
    static_cast<void>(parse(context, {}));
    const std::optional<net::OpenedLaunch> taken = net::read_opened_launch(file);
    OA_CHECK(taken.has_value());
    std::string refusal;
    try {
        oa::app::put_launch_in_place(context, net::read_opened_launch_plan(*taken));
    } catch (const net::LaunchRefusal& error) {
        refusal = error.what();
    }
    OA_CHECK(refusal.find("deathmatch.oalaunch") != std::string::npos);
    OA_CHECK(!context.options.launch_planned);
    OA_CHECK(context.options.direct_game.kind == mp::DirectGame::Kind::none);
    OA_CHECK(context.launch.block.connection_type == 0);
    OA_CHECK(context.launch.block.computer_count == 0);
    OA_CHECK(context.launch.block.mission[0] == '\0');
}

/// A launch's content is compared with what the game plays.
void test_content(const Scratch&) {
    net::RunningContent running;
    running.bundles_known = true;
    running.base_game = "totala";
    running.expansions = {"core-contingency"};
    running.mod_id = "ridge";
    running.mod_folder = "Ridge";
    net::LaunchContentChoice none;
    OA_CHECK(!net::launch_content_differs(none, running));
    net::LaunchContentChoice only_mutators;
    only_mutators.mutators_named = true;
    only_mutators.mutators.push_back(net::LaunchMutator{"wide-sight", "", {}});
    OA_CHECK(!only_mutators.named);
    OA_CHECK(!net::launch_content_differs(only_mutators, running));
    net::LaunchContentChoice same;
    same.named = true;
    same.base_game = "totala";
    same.expansions = {"music", "Core-Contingency"};
    same.mod = "ridge";
    OA_CHECK(!net::launch_content_differs(same, running));
    net::LaunchContentChoice by_folder = same;
    by_folder.mod = "/Users/player/Documents/Open Annihilation/Mods/Ridge";
    OA_CHECK(!net::launch_content_differs(by_folder, running));
    net::LaunchContentChoice other_mod = same;
    other_mod.mod = "ridge-classic";
    OA_CHECK(net::launch_content_differs(other_mod, running));
    net::LaunchContentChoice no_expansion = same;
    no_expansion.expansions.clear();
    OA_CHECK(net::launch_content_differs(no_expansion, running));
    net::LaunchContentChoice other_base = same;
    other_base.base_game = "kingdoms";
    OA_CHECK(net::launch_content_differs(other_base, running));
    // A mod named while none plays differs; a game folder named on the command line is not
    // compared by its bundles.
    net::RunningContent folder;
    OA_CHECK(net::launch_content_differs(same, folder));
    net::LaunchContentChoice bundles_only = same;
    bundles_only.mod.clear();
    OA_CHECK(!net::launch_content_differs(bundles_only, folder));
}

/// Where the game stands chooses what it does with an opened launch file.
void test_steps() {
    using net::GameStanding;
    using net::OpenedLaunchStep;
    OA_CHECK(
        net::opened_launch_step(GameStanding::main_menu, false) ==
        OpenedLaunchStep::go_to_battle_room
    );
    OA_CHECK(
        net::opened_launch_step(GameStanding::single_player, false) ==
        OpenedLaunchStep::back_to_main_menu
    );
    OA_CHECK(net::opened_launch_step(GameStanding::passing, false) == OpenedLaunchStep::wait);
    OA_CHECK(net::opened_launch_step(GameStanding::passing, true) == OpenedLaunchStep::wait);
    OA_CHECK(
        net::opened_launch_step(GameStanding::match, false) == OpenedLaunchStep::refuse_in_match
    );
    OA_CHECK(
        net::opened_launch_step(GameStanding::match, true) == OpenedLaunchStep::refuse_in_match
    );
    OA_CHECK(net::opened_launch_step(GameStanding::elsewhere, false) == OpenedLaunchStep::refuse);
    // A launch already under way is not replaced, not even from the main menu.
    OA_CHECK(net::opened_launch_step(GameStanding::main_menu, true) == OpenedLaunchStep::refuse);
    OA_CHECK(
        net::opened_launch_step(GameStanding::single_player, true) == OpenedLaunchStep::refuse
    );
}

/// --check-opened-launch takes when and the file.
void test_check_option() {
    oa::app::NetgameContext start{};
    static_cast<void>(parse(start, {"--check-opened-launch", "start", "host.oalaunch"}));
    OA_CHECK(start.options.opened_launch_check == oa::app::OpenedLaunchCheck::start);
    OA_CHECK(start.options.opened_launch_check_file == "host.oalaunch");
    oa::app::NetgameContext menu{};
    static_cast<void>(parse(menu, {"--check-opened-launch", "menu", "launch.txt"}));
    OA_CHECK(menu.options.opened_launch_check == oa::app::OpenedLaunchCheck::menu);
    std::string refused;
    try {
        oa::app::NetgameContext later{};
        static_cast<void>(parse(later, {"--check-opened-launch", "later", "launch.txt"}));
    } catch (const std::runtime_error& error) {
        refused = error.what();
    }
    OA_CHECK(refused == "--check-opened-launch expects start or menu, then a file");
}

} // namespace

int main() {
    const Scratch scratch;
    test_told_by_name_or_bytes();
    test_read_as_it_arrives(scratch);
    test_waiting();
    test_start_is_minus_c(scratch);
    test_refusal_puts_nothing(scratch);
    test_content(scratch);
    test_steps();
    test_check_option();
    return oa::test::check_exit_status();
}
