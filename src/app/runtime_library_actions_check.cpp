// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-library-install: made-up mod packages posted to the installer as
// the download queue hands them over, each with a catalogue origin, while
// the Library shows over a feed of three made-up registries: check-builtin,
// built in, and check-added and check-added-2, added. No network is reached.
// Each run of the check is one turn. The first: an added registry's mod
// installs with no question and writes its origin record, and its next
// release replaces it keeping the first; the same key from the other added
// registry asks REPLACE MOD over the Library, CANCEL changing nothing and
// REPLACE replacing; the built-in registry's replaces it with no question
// and no prompt, and ROLL BACK's question over the Library brings the added
// registry's copy back; the built-in registry's replaces a file's other
// version with no question; a folder of the player's own is refused in the
// Library's queue, with MOD NOT INSTALLED once the Library is closed, and
// asks FOLDER IN USE from an added registry; a file still asks UPDATE MOD;
// a package that does not resolve is refused in the queue; then PLAY NOW.
// The second plays the mod: ROLL BACK's question says the game reloads, a
// built-in update to it asks nothing and waits, held, for the player to
// leave the mod, CANCEL on that line drops it and the update is taken again
// and held, and PLAY NOW switches to another mod; the third finds the
// update put in place as the mod was switched, with no notice, then asks for made-up updates:
// an added registry's whose rules change asks the update question first, NOT
// NOW leaving it and UPDATE queuing it, and one that keeps the rules is
// queued at once; the built-in registry's whose rules change, or cannot be
// known, are queued at once, their status lines and Rules facts saying so
// before, and again each in turn, the header's refresh icon offered once a registry can be
// asked; and UPDATE ALL queues the built-in one at once and asks about the
// added one only; then UNINSTALL: off for a map pack during a game, a notice
// for the language pack in use and for the mod being played, each keeping
// its folder, the question for a built-in registry's mod, CANCEL removing
// nothing and UNINSTALL its folder with the copy ROLL BACK keeps, its row
// staying not installed, and a mod of the player's own leaving the Library.
// The fourth plays No Mod and installs a made-up mutator, Fast Builders:
// GET queues it, the package installs under Mutators with its origin record,
// CHECK WITH THIS MOD runs the fixture's fake checker and shows its verdict,
// OPEN FOLDER and HOMEPAGE reach the recorders, and UNINSTALL asks and
// removes the folder. The fifth is a run nobody watches, on SDL's dummy
// video driver without the prompts the other runs show: a built-in registry's
// update waits in the inbox while the Library is closed, then installs with
// no prompt once the Library shows, the folder it drops deleted; an added
// registry's copy asks REPLACE MOD over the Library, which waits for its
// answer, and REPLACE installs it; and a file opened outside the game stays
// in the inbox.

#include "library_host.hpp"
#include "mod_install_state.hpp"
#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "user_folder_state.hpp"
#include "web_link_state.hpp"

#include "oa/app/content/downloads.hpp"
#include "oa/app/content/updates.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/package_install.hpp"
#include "oa/app/package_install/inbox.hpp"
#include "oa/app/package_install/oamod.hpp"
#include "oa/app/package_install/oamut.hpp"
#include "oa/app/package_install/origin.hpp"
#include "oa/data/mutators/verdicts.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/base/sha256.hpp"
#include "oa/formats/zip.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/engine_settings/prompt.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/library/uninstall.hpp"
#include "oa/ui/library/update_question.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace settings = oa::ui::engine_settings;
namespace layout = oa::ui::display_layout;
namespace library = oa::ui::library;
namespace install = oa::app::package_install;

/// The made-up built-in registry.
constexpr std::string_view kBuiltIn = "check-builtin";
/// The first made-up registry the player added.
constexpr std::string_view kAdded = "check-added";
/// The second made-up registry the player added.
constexpr std::string_view kAddedOther = "check-added-2";
/// The made-up mod the check installs, and its folder: Ridge.
constexpr std::string_view kRidge = "ridge";
/// A made-up mod whose folder name a folder of the player's own holds.
constexpr std::string_view kCinder = "cinder";
/// A made-up mod the check switches to.
constexpr std::string_view kLattice = "lattice";
/// A made-up mod whose profile does not resolve.
constexpr std::string_view kDunes = "dunes";
/// A made-up mod of an added registry whose update changes the rules.
constexpr std::string_view kEmber = "ember";
/// A made-up mod of an added registry whose update keeps the rules.
constexpr std::string_view kFlint = "flint";
/// A made-up mod of the built-in registry the check uninstalls.
constexpr std::string_view kBasalt = "basalt";
/// A made-up mod of the player's own, no registry listing it, the check uninstalls.
constexpr std::string_view kScree = "scree";
/// A made-up map pack of the built-in registry, installed while a game is played.
constexpr std::string_view kArchipelago = "archipelago";
/// A made-up language pack of the built-in registry, the one in use.
constexpr std::string_view kPseudo = "en-XA";
/// A made-up mutator the fourth run gets, checks and uninstalls.
constexpr std::string_view kFast = "fast-builders";
/// The most frames a step of the check runs before it gives up.
constexpr int kMostFrames = 400;
/// The frames a step runs to show that something waits: nothing comes of it.
constexpr int kWaitingFrames = 30;
/// The run of the check that nobody watches, the last: the fifth.
constexpr uint32_t kUnwatchedRun = 4;
/// What the download queue says of a package the installer refused, which
/// the installer's own reason replaces in the Library.
constexpr std::string_view kQueueRefused = "The package was refused";
/// Where the pointer rests: over no control of the main menu.
constexpr layout::Point kRestingPointer{8, 8};

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("library install check: " + std::string(what));
}

/// One package the fixture feed was asked to queue.
struct FeedQueued {
    std::string registry{};           ///< its registry
    std::string key{};                ///< its key
    content::DownloadReason reason{}; ///< why
};

/// The fixture feed the check gives the Library: its inputs, which the check
/// changes as the download queue and the player's folder would, and what it
/// was asked to queue, which it records and starts no download for.
struct InstallFeed {
    library::Inputs inputs{};         ///< what the Library is built from
    uint64_t generation{1};           ///< moves whenever the check changes inputs
    std::vector<FeedQueued> queued{}; ///< what it was asked to queue, in order
    /// Verdicts `keep_verdict` stored, keyed by the mutator's id.
    std::map<std::string, oa::data::mutators::VerdictRecord> kept{};
    int keep_count{0}; ///< how many times `keep_verdict` was called
    /// The verdict the running check returns once `release` is set.
    oa::data::mutators::VerdictRecord canned{};
    std::atomic<bool> release{false};      ///< the check may return `canned`
    std::atomic<bool> work_started{false}; ///< the worker has entered the check
};

/// The fixture's inputs.
///
/// @param context the fixture
/// @return its inputs
library::Inputs feed_inputs(void* context) {
    return static_cast<InstallFeed*>(context)->inputs;
}

/// The fixture's generation.
///
/// @param context the fixture
/// @return the generation
uint64_t feed_generation(void* context) {
    return static_cast<InstallFeed*>(context)->generation;
}

/// Records a package the fixture was asked to queue.
///
/// @param context the fixture
/// @param registry the registry's id
/// @param key the package's key
/// @param reason why
/// @return a queue item's number: one more for each package recorded
std::optional<uint64_t> feed_enqueue(
    void* context,
    std::string_view registry,
    std::string_view key,
    content::DownloadReason reason,
    std::string* /*why*/
) {
    auto& fixture = *static_cast<InstallFeed*>(context);
    fixture.queued.push_back({std::string(registry), std::string(key), reason});
    return static_cast<uint64_t>(fixture.queued.size());
}

/// Tells whether a registry of the fixture's inputs is built in: the
/// Registry::built_in its inputs give.
///
/// @param context the fixture
/// @param registry the registry's id
/// @return true for check-builtin
bool feed_built_in(void* context, std::string_view registry) {
    for (const library::Registry& listed : static_cast<InstallFeed*>(context)->inputs.registries)
        if (listed.id == registry)
            return listed.built_in;
    return false;
}

/// The verdict `keep_verdict` stored for a mutator. Called on the main thread.
///
/// @param context the fixture
/// @param id the mutator's id
/// @return the record, or nothing when none was kept
std::optional<oa::data::mutators::VerdictRecord>
feed_remembered(void* context, std::string_view id) {
    const InstallFeed& fixture = *static_cast<const InstallFeed*>(context);
    const auto found = fixture.kept.find(std::string(id));
    if (found == fixture.kept.end())
        return std::nullopt;
    return found->second;
}

/// Stores a verdict the worker found. Called on the main thread.
///
/// @param context the fixture
/// @param id the mutator's id
/// @param record the verdict
void feed_keep(
    void* context, std::string_view id, const oa::data::mutators::VerdictRecord& record
) {
    InstallFeed& fixture = *static_cast<InstallFeed*>(context);
    fixture.kept.insert_or_assign(std::string(id), record);
    ++fixture.keep_count;
}

/// Starts a fake check. The work runs on the Library's worker: it waits until
/// the fixture releases it, and returns nothing when the check is cancelled.
///
/// @param context the fixture
/// @param id the mutator's id; the canned verdict does not depend on it
/// @return the work, which reads `canned` once it is released
std::function<std::optional<oa::data::mutators::VerdictRecord>(const std::atomic<bool>&)>
feed_start(void* context, std::string_view /*id*/) {
    auto* fixture = static_cast<InstallFeed*>(context);
    return [fixture](
               const std::atomic<bool>& cancel
           ) -> std::optional<oa::data::mutators::VerdictRecord> {
        fixture->work_started.store(true);
        while (!fixture->release.load() && !cancel.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (cancel.load())
            return std::nullopt;
        return fixture->canned;
    };
}

/// Returns one of the fixture's registries.
///
/// @param id its id
/// @param name its name
/// @param built_in it ships with the game
/// @return the registry
library::Registry feed_registry(std::string_view id, std::string_view name, bool built_in) {
    library::Registry registry;
    registry.id = std::string(id);
    registry.name = std::string(name);
    registry.built_in = built_in;
    return registry;
}

/// Returns a made-up profile.
///
/// @param id its id
/// @param name its name
/// @param version its version
/// @param revision its packaging revision
/// @param more lines after it
/// @return its text
std::string profile_text(
    std::string_view id,
    std::string_view name,
    std::string_view version,
    int revision,
    std::string_view more = {}
) {
    return "oamod: 1\nid: " + std::string(id) + "\nname: " + std::string(name) + "\nversion: \"" +
           std::string(version) +
           "\"\ndescription: A made-up mod the Library install check installs.\n"
           "requires: {base: ta-3.1c, catalogue: 1}\n"
           "author: {name: unknown}\n"
           "packaging: {revision: " +
           std::to_string(revision) + ", date: 2026-10-11, packager: Open Annihilation}\n" +
           std::string(more);
}

/// Writes a package of stored entries, its profile at its top.
///
/// @param file the package
/// @param profile its oamod.yaml
/// @param revision written into its one other file
void write_package(const fs::path& file, const std::string& profile, int revision) {
    const std::string readme = "revision " + std::to_string(revision);
    const std::vector<uint8_t> manifest(profile.begin(), profile.end());
    const std::vector<uint8_t> text(readme.begin(), readme.end());
    const std::vector<oa::formats::zip::NewEntry> listed{
        {"oamod.yaml", manifest},
        {"units/readme.txt", text},
    };
    std::vector<uint8_t> archive;
    oa::formats::zip::ZipError error{};
    require(oa::formats::zip::write_archive(listed, archive, error), "a package could not be made");
    std::error_code made;
    fs::create_directories(file.parent_path(), made);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size())
    );
    require(static_cast<bool>(out), "a package could not be written");
}

/// Writes Fast Builders as a small .oamut: its manifest, its entry and a note.
///
/// @param file the package
void write_mutator(const fs::path& file) {
    const std::string manifest = "oamut: 1\n"
                                 "id: fast-builders\n"
                                 "name: \"Fast Builders\"\n"
                                 "version: \"1.0\"\n"
                                 "author:\n"
                                 "  name: \"Ada\"\n"
                                 "description: \"Builds faster.\"\n"
                                 "tags: [\"economy\"]\n"
                                 "works-with: [\"*\"]\n"
                                 "layers: [\"units\"]\n"
                                 "entry: \"mutator.lua\"\n";
    const std::string script = "return {}\n";
    const std::string note = "Fast Builders\n";
    const std::vector<uint8_t> manifest_bytes(manifest.begin(), manifest.end());
    const std::vector<uint8_t> script_bytes(script.begin(), script.end());
    const std::vector<uint8_t> note_bytes(note.begin(), note.end());
    const std::vector<oa::formats::zip::NewEntry> listed{
        {"oamut.yaml", manifest_bytes},
        {"mutator.lua", script_bytes},
        {"README.md", note_bytes},
    };
    std::vector<uint8_t> archive;
    oa::formats::zip::ZipError error{};
    require(oa::formats::zip::write_archive(listed, archive, error), "a mutator could not be made");
    std::error_code made;
    fs::create_directories(file.parent_path(), made);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size())
    );
    require(static_cast<bool>(out), "a mutator could not be written");
}

/// Returns the SHA-256 of a file.
///
/// @param file the file
/// @return its digest
oa::base::sha256::Digest file_hash(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>{in}, {}};
    return oa::base::sha256::digest_of(bytes);
}

/// Returns the revision a folder's profile gives; -1 when it holds no mod.
///
/// @param folder the folder
/// @return the revision
int64_t revision_in(const fs::path& folder) {
    const auto held = install::oamod::read_installed_mod(folder);
    return held.kind == install::FolderKind::package ? held.revision : -1;
}

/// Tells whether a folder's origin record names a catalogue's release.
///
/// @param folder the folder
/// @param registry the registry
/// @param release the release
/// @return true when it does
bool recorded_from(const fs::path& folder, std::string_view registry, int64_t release) {
    const std::optional<install::Origin> record = install::read_origin(folder);
    return record && record->kind == install::OriginKind::catalogue &&
           record->registry == registry && record->catalogue_id == kRidge &&
           record->release == release;
}

/// Tells whether the Mods folder holds a folder an install keeps while it works.
///
/// @param mods the Mods folder
/// @return true when one is there
bool reserved_left(const fs::path& mods) {
    const install::FolderNames names = install::folder_names(install::mod_kind());
    std::error_code error;
    for (fs::directory_iterator entry{mods, error}, end; !error && entry != end;
         entry.increment(error)) {
        const std::string name = path_to_utf8(entry->path().filename());
        if (name.starts_with(names.reserved) && name != names.lock)
            return true;
    }
    return false;
}

/// Returns a folder's path as the settings keep it: absolute, normal, in UTF-8.
///
/// @param folder the folder
/// @return its path
std::string kept_path(const fs::path& folder) {
    std::error_code error;
    const fs::path absolute = fs::absolute(folder, error);
    return path_to_utf8((error ? folder : absolute).lexically_normal());
}

} // namespace

void Runtime::check_library_install() {
    require(sdl_.window != nullptr, "the check needs the SDL presenter");
    require(!user_folder_.empty(), "the run has no folder of the player's own");
    const fs::path mods = user_folder_ / std::string(user_mods_folder_name);
    const fs::path packages = user_folder_.parent_path() / "files";
    const fs::path ridge = mods / std::string(kRidge);
    const fs::path kept = ridge / std::string(install::backup_folder_name);
    auto& state = mod_install_state();
    // Every run but the last shows the prompts although nobody watches it;
    // the last shows only what a run nobody watches shows.
    state.check_shows_prompts = options_.restarts < kUnwatchedRun;

    // The Library takes the fixture's feed from the first frame on, and the
    // runtime's own back as the check ends.
    InstallFeed fixture;
    fixture.inputs.registries.push_back(feed_registry(kBuiltIn, "Check Built-in", true));
    fixture.inputs.registries.push_back(feed_registry(kAdded, "Check Added", false));
    fixture.inputs.registries.push_back(feed_registry(kAddedOther, "Check Added Two", false));
    fixture.inputs.engine = content::this_engine_rules().engine;
    fixture.inputs.version_text = "v0.8.0";
    LibraryHost& host = library_host();
    const LibraryFeed own = host.feed;

    struct OwnFeedBack {
        LibraryHost& host;
        LibraryFeed feed;

        ~OwnFeedBack() {
            host.feed = feed;
            host.own_feed = true;
            host.refreshed = false;
        }
    } own_back{host, own};

    host.feed = LibraryFeed{
        &fixture, feed_inputs, feed_generation, feed_enqueue, nullptr, nullptr, feed_built_in
    };
    host.own_feed = false;
    host.refreshed = false;

    bool running = true;
    // Installed packages of the other kinds the fixture lists after the mods.
    std::vector<library::Installed> beside_mods;
    // The installed mods as the Library's worker would list them: each
    // folder of Mods that holds a mod, its registry and key from its origin
    // record, the version its .backup keeps and whether the game plays it;
    // then the packages beside them.
    const auto list_installed = [&] {
        fixture.inputs.installed.clear();
        std::error_code error;
        for (fs::directory_iterator entry{mods, error}, end; !error && entry != end;
             entry.increment(error)) {
            const fs::path folder = entry->path();
            // The folders an install keeps while it works are not mods.
            if (path_to_utf8(folder.filename()).starts_with('.'))
                continue;
            const install::InstalledPackage held = install::oamod::read_installed_mod(folder);
            if (held.kind != install::FolderKind::package)
                continue;
            const std::optional<install::Origin> record = install::read_origin(folder);
            library::Installed shown;
            shown.kind = library::Kind::mod;
            shown.key = held.id;
            shown.folder = folder;
            shown.name = held.name;
            shown.version = held.version;
            shown.revision = held.revision;
            if (record && record->kind == install::OriginKind::catalogue) {
                shown.key = record->catalogue_id;
                shown.registry = record->registry;
                shown.release = record->release;
            }
            shown.playing = package_target_in_use(install::mod_kind(), folder);
            if (const auto backup = install::oamod::read_backup(folder);
                backup && backup->kind == install::FolderKind::package)
                shown.kept = library::Kept{backup->version, backup->revision};
            fixture.inputs.installed.push_back(std::move(shown));
        }
        error.clear();
        const fs::path mutators_root =
            user_folder_ / std::string(install::oamut::mutators_folder_name);
        if (fs::is_directory(mutators_root, error)) {
            error.clear();
            for (fs::directory_iterator entry{mutators_root, error}, end; !error && entry != end;
                 entry.increment(error)) {
                const fs::path folder = entry->path();
                if (path_to_utf8(folder.filename()).starts_with('.'))
                    continue;
                const install::InstalledPackage held = install::oamut::read_installed(folder);
                if (held.kind != install::FolderKind::package)
                    continue;
                const std::optional<install::Origin> record = install::read_origin(folder);
                library::Installed shown;
                shown.kind = library::Kind::mutator;
                shown.key = held.id;
                shown.folder = folder;
                shown.name = held.name;
                shown.version = held.version;
                shown.revision = held.revision;
                shown.author = held.author;
                shown.description = held.description;
                shown.mutator = library::MutatorFacts{};
                if (record && record->kind == install::OriginKind::catalogue) {
                    shown.key = record->catalogue_id;
                    shown.registry = record->registry;
                    shown.release = record->release;
                }
                fixture.inputs.installed.push_back(std::move(shown));
            }
        }
        fixture.inputs.installed.insert(
            fixture.inputs.installed.end(), beside_mods.begin(), beside_mods.end()
        );
        ++fixture.generation;
    };
    const auto frame = [&] {
        run_frame(running);
        require(running, "a frame ended the run");
    };
    // Two frames: the Library's model follows its feed in the first, after
    // the OA layer's screens have ticked, and the Library lays itself out
    // again in the second.
    const auto frames = [&] {
        frame();
        frame();
    };
    const auto tap = [this](SDL_Keycode code) {
        bool going = true;
        for (const bool down : {true, false}) {
            SDL_Event event{};
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.windowID = SDL_GetWindowID(sdl_.window);
            event.key.key = code;
            event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
            event.key.down = down;
            dispatch_event(event, going);
        }
        require(going, "a key ended the run");
    };
    const auto named = [this](std::string_view name) -> std::optional<AutomationControl> {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        for (const AutomationControl& control : controls)
            if (control.name == name && control.visible && control.window_width > 0 &&
                control.window_height > 0)
                return control;
        return std::nullopt;
    };
    const auto click_named = [&](std::string_view name) {
        const std::optional<AutomationControl> control = named(name);
        require(control.has_value(), std::string(name) + " is not shown");
        const layout::Point pixel{
            control->window_x + control->window_width / 2,
            control->window_y + control->window_height / 2
        };
        for (const SDL_EventType type :
             {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event = window_pointer_event(
                sdl_.window,
                sdl_.renderer,
                type,
                pixel,
                type == SDL_EVENT_MOUSE_MOTION ? 0 : SDL_BUTTON_LEFT
            );
            bool going = true;
            dispatch_event(event, going);
            require(going, "a click ended the run");
        }
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, kRestingPointer, 0);
    };
    const auto top_is = [this](std::string_view name) {
        const LayerScreen* top = oa_layer().top();
        return top != nullptr && top->name() == name;
    };
    // A few frames on the main menu; a notice nobody asked for is closed.
    const auto settle = [&] {
        for (int count = 0; count < 12; ++count) {
            frame();
            const LayerScreen* top = oa_layer().top();
            if (top != nullptr && dynamic_cast<const NoticeScreen*>(top) != nullptr)
                tap(SDLK_RETURN);
            else if (top == nullptr && count >= 2)
                return;
        }
        require(oa_layer().top() == nullptr, "a screen stayed over the main menu");
    };
    const auto open_the_library = [&] {
        list_installed();
        open_library(library::Tab::mods);
        frames();
        require(top_is(library_screen_name), "the Library did not open over the main menu");
    };
    const auto close_the_library = [&] {
        close_library();
        frame();
        require(oa_layer().top() == nullptr, "the Library did not close");
    };
    // Shows an entry's details: its row selected, and at Compact its page.
    const auto show_entry = [&](std::string_view row) {
        if (named("oa.library.back"))
            click_named("oa.library.back");
        click_named(row);
    };
    // Presses one of the shown entry's actions: a button, or an item of
    // MORE ▾ at Compact.
    const auto press_action = [&](std::string_view word) {
        const std::string action = "oa.library.action." + std::string(word);
        if (!named(action))
            click_named("oa.library.action");
        click_named(action);
    };
    // The queue item the Library shows for a package of a registry.
    const auto queued = [&](std::string_view registry,
                            std::string_view key) -> std::optional<library::QueueItem> {
        for (const library::QueueItem& item : host.model.inputs.queue)
            if (item.registry == registry && item.key == key)
                return item;
        return std::nullopt;
    };
    // Posts a package as the download queue's hand-off does, with its
    // catalogue origin, and lists it in the fixture's queue as installing.
    const auto post_catalogue = [&](const fs::path& file,
                                    std::string_view registry,
                                    std::string_view key,
                                    std::string_view name,
                                    int64_t release,
                                    library::Kind kind = library::Kind::mod) {
        install::Origin origin{};
        origin.kind = install::OriginKind::catalogue;
        origin.registry = std::string(registry);
        origin.catalogue_id = std::string(key);
        origin.release = release;
        origin.sha256 = file_hash(file);
        install::post_package_file(file, origin);
        auto& queue = fixture.inputs.queue;
        queue.erase(
            std::remove_if(
                queue.begin(),
                queue.end(),
                [&](const library::QueueItem& item) {
                    return item.registry == registry && item.key == key;
                }
            ),
            queue.end()
        );
        library::QueueItem item;
        item.registry = std::string(registry);
        item.kind = kind;
        item.key = std::string(key);
        item.name = std::string(name);
        item.phase = library::QueueItem::Phase::installing;
        queue.push_back(std::move(item));
        ++fixture.generation;
    };
    // Writes Ridge at a version and revision and posts it from a registry.
    const auto post_ridge =
        [&](std::string_view registry, std::string_view version, int revision, int64_t release) {
            const fs::path file = packages / ("ridge-" + std::string(registry) + "-" +
                                              std::to_string(release) + ".oamod");
            write_package(file, profile_text(kRidge, "Ridge", version, revision), revision);
            post_catalogue(file, registry, kRidge, "Ridge", release);
        };
    // The installer's frames, as the main loop runs them, until the package
    // posted has an outcome, which is taken as the download queue takes it,
    // or a question shows.
    const auto until_outcome =
        [&](std::string_view what) -> std::optional<install::PackageOutcome> {
        for (int count = 0; count < kMostFrames; ++count) {
            tell_mod_installs();
            std::vector<install::PackageOutcome> outcomes = install::take_package_outcomes();
            if (!outcomes.empty())
                return outcomes.front();
            if (state.prompt != nullptr && state.stage == ModInstallState::Stage::asking)
                return std::nullopt;
        }
        require(false, std::string("nothing came of ") + std::string(what));
        return std::nullopt;
    };
    // What the download queue makes of an outcome: an installed item, a
    // failed one with its own words, or a cancelled one, which the Library
    // leaves out; the player's folder listed again and a frame drawn.
    const auto take_outcome = [&](const install::PackageOutcome& outcome,
                                  std::string_view registry,
                                  std::string_view key) {
        auto& queue = fixture.inputs.queue;
        for (auto item = queue.begin(); item != queue.end(); ++item) {
            if (item->registry != registry || item->key != key)
                continue;
            if (outcome.result == install::OutcomeResult::installed) {
                item->phase = library::QueueItem::Phase::installed;
            } else if (outcome.result == install::OutcomeResult::set_aside) {
                queue.erase(item);
            } else {
                item->phase = library::QueueItem::Phase::failed;
                item->problem = std::string(kQueueRefused);
                item->retryable = true;
            }
            break;
        }
        list_installed();
        frames();
    };
    // A catalogue package installed with no question and no prompt.
    const auto installs_quietly =
        [&](std::string_view what, std::string_view registry, std::string_view key) {
            const uint32_t shown = state.prompts_shown;
            const std::optional<install::PackageOutcome> outcome = until_outcome(what);
            require(outcome.has_value(), std::string(what) + " asked a question");
            require(
                outcome->result == install::OutcomeResult::installed,
                std::string(what) + " was not installed: " + outcome->reason
            );
            require(
                state.prompts_shown == shown && state.prompt == nullptr,
                std::string(what) + " showed a prompt"
            );
            take_outcome(*outcome, registry, key);
        };
    // A catalogue package's question, pushed over the Library at once.
    const auto asks_over_library = [&](std::string_view what, std::string_view title) {
        const std::optional<install::PackageOutcome> outcome = until_outcome(what);
        require(!outcome.has_value(), std::string(what) + " asked no question");
        require(
            state.prompt != nullptr && oa_layer().top() == state.prompt &&
                oa_layer().find(library_screen_name) != nullptr &&
                state.prompt->question().title == title,
            std::string(what) + " did not ask " + std::string(title) + " over the Library"
        );
    };
    // Frames over the main menu until a prompt that asks or tells shows.
    const auto until_prompt = [&](std::string_view what) {
        for (int count = 0; count < kMostFrames; ++count) {
            tell_mod_installs();
            if (state.prompt != nullptr && (state.stage == ModInstallState::Stage::asking ||
                                            state.stage == ModInstallState::Stage::telling))
                return;
        }
        require(false, std::string("no prompt showed: ") + std::string(what));
    };
    const auto says = [&](std::string_view part) {
        if (state.prompt == nullptr)
            return false;
        for (const auto& paragraph : state.prompt->question().paragraphs)
            if (paragraph.text.find(part) != std::string::npos)
                return true;
        return false;
    };
    const auto titled = [&](std::string_view title) {
        return state.prompt != nullptr && state.prompt->question().title == title;
    };
    const auto playing = [&](const fs::path& folder) {
        return !options_.layered_mod_folder.empty() &&
               kept_path(options_.layered_mod_folder) == kept_path(folder);
    };

    load(Screen::main_menu);
    require(screen_ == Screen::main_menu, "the main menu did not open");
    require(engine_settings_fonts() != nullptr, "the dialog's fonts did not load");
    send_check_pointer(SDL_EVENT_MOUSE_MOTION, kRestingPointer, 0);

    switch (options_.restarts) {
    case 0: {
        // The check starts afresh: an earlier run's packages and mods go.
        settle();
        require(
            options_.layered_mod_folder.empty(),
            "an earlier run of the check that stopped left its mod chosen; start it again"
        );
        std::error_code error;
        fs::remove_all(mods, error);
        fs::remove_all(packages, error);
        require(
            library_registry_built_in(kBuiltIn) && !library_registry_built_in(kAdded) &&
                !library_registry_built_in(kAddedOther),
            "library_registry_built_in does not answer as the feed's registries say"
        );
        open_the_library();

        // 1. An added registry's mod installs with no question, and its
        // folder records where it came from.
        post_ridge(kAdded, "1.0", 1, 1);
        installs_quietly("check-added's first release", kAdded, kRidge);
        require(
            revision_in(ridge) == 1 && recorded_from(ridge, kAdded, 1),
            "check-added's first release is not in Mods with its origin record"
        );
        require(top_is(library_screen_name), "the Library did not stay open over the install");

        // 2. Its next release replaces it with no question, keeping the first.
        post_ridge(kAdded, "1.0", 2, 2);
        installs_quietly("check-added's second release", kAdded, kRidge);
        require(
            revision_in(ridge) == 2 && revision_in(kept) == 1 && recorded_from(kept, kAdded, 1),
            "check-added's second release did not keep the first as the .backup"
        );
        require(
            queued(kAdded, kRidge) &&
                queued(kAdded, kRidge)->phase == library::QueueItem::Phase::installed,
            "the Library's queue does not say check-added's release is installed"
        );

        // 3. The same key from the other added registry asks REPLACE MOD
        // over the Library: CANCEL changes nothing, REPLACE replaces.
        post_ridge(kAddedOther, "1.0", 3, 1);
        asks_over_library("check-added-2's release", "REPLACE MOD");
        {
            const settings::Prompt& question = state.prompt->question();
            require(
                question.buttons.size() == 2 && question.buttons[0].id == "cancel" &&
                    question.buttons[1].id == "replace" && question.marked == 0 &&
                    says(
                        "Ridge from Check Added is installed. Replace it with Ridge from Check "
                        "Added Two? The one from Check Added is kept, and ROLL BACK brings it back."
                    ),
                "REPLACE MOD does not name both registries with CANCEL marked and REPLACE"
            );
            frame();
            require(
                named("oa.prompt.replace").has_value() && named("oa.prompt.cancel").has_value() &&
                    named("oa.prompt.body").has_value(),
                "automation does not list REPLACE MOD's oa.prompt.replace, cancel and body"
            );
        }
        tap(SDLK_ESCAPE);
        {
            const std::optional<install::PackageOutcome> outcome = until_outcome("CANCEL");
            require(
                outcome && outcome->result == install::OutcomeResult::set_aside &&
                    state.prompt == nullptr,
                "CANCEL on REPLACE MOD did not set the package aside"
            );
            take_outcome(*outcome, kAddedOther, kRidge);
        }
        require(
            revision_in(ridge) == 2 && recorded_from(ridge, kAdded, 2) && revision_in(kept) == 1,
            "CANCEL on REPLACE MOD changed the folder"
        );
        post_ridge(kAddedOther, "1.0", 3, 1);
        asks_over_library("check-added-2's release again", "REPLACE MOD");
        tap(SDLK_Y);
        {
            const std::optional<install::PackageOutcome> outcome = until_outcome("REPLACE");
            require(
                outcome && outcome->result == install::OutcomeResult::installed &&
                    state.prompt == nullptr,
                "REPLACE on REPLACE MOD did not install check-added-2's release"
            );
            take_outcome(*outcome, kAddedOther, kRidge);
        }
        require(
            revision_in(ridge) == 3 && recorded_from(ridge, kAddedOther, 1) &&
                recorded_from(kept, kAdded, 2),
            "REPLACE did not keep check-added's copy as the .backup"
        );

        // 4. The built-in registry's replaces it with no question and no
        // prompt, keeping the added registry's copy with its record, which
        // ROLL BACK's question over the Library brings back.
        post_ridge(kBuiltIn, "1.0", 4, 1);
        installs_quietly("check-builtin's release", kBuiltIn, kRidge);
        require(
            revision_in(ridge) == 4 && recorded_from(ridge, kBuiltIn, 1) &&
                revision_in(kept) == 3 && recorded_from(kept, kAddedOther, 1),
            "check-builtin's release did not keep check-added-2's copy with its record"
        );
        show_entry("oa.library.row.check-builtin.ridge");
        press_action("roll-back");
        {
            const auto* asked = dynamic_cast<const QuestionScreen*>(oa_layer().top());
            require(
                asked != nullptr && oa_layer().find(library_screen_name) != nullptr &&
                    engine_settings_dialog() == nullptr,
                "ROLL BACK did not ask its question over the Library"
            );
            const settings::Prompt& question = asked->question();
            require(
                question.title == "ROLL BACK MOD" && question.buttons.size() == 2 &&
                    question.buttons[0].id == "cancel" && question.buttons[1].id == "roll-back" &&
                    question.marked == 0 && question.paragraphs.size() == 1 &&
                    question.paragraphs[0].text ==
                        "Roll back Ridge to 1.0 revision 3? 1.0 revision 4 is kept in its place, "
                        "and ROLL BACK again brings it back.",
                "ROLL BACK's question is not the Roll Back Mod question for Ridge"
            );
        }
        tap(SDLK_ESCAPE);
        require(
            top_is(library_screen_name) && revision_in(ridge) == 4,
            "CANCEL on ROLL BACK's question changed the folder"
        );
        press_action("roll-back");
        tap(SDLK_Y);
        require(
            top_is(library_screen_name) && revision_in(ridge) == 3 &&
                recorded_from(ridge, kAddedOther, 1) && revision_in(kept) == 4 &&
                recorded_from(kept, kBuiltIn, 1) && !soft_restart_requested(),
            "ROLL BACK did not bring check-added-2's copy back with its record"
        );
        list_installed();
        frames();
        std::cout << "library install check: built-in package installed with no question\n";

        // 5. The built-in registry's replaces another version a file
        // installed, ANOTHER VERSION for a file, with no question.
        close_the_library();
        fs::remove_all(ridge, error);
        const fs::path other_version = packages / "ridge-2.0.oamod";
        write_package(other_version, profile_text(kRidge, "Ridge", "2.0", 1), 1);
        install::post_package_file(other_version);
        until_prompt("the file of another version");
        require(titled("MOD INSTALLED"), "the file of another version was not installed");
        tap(SDLK_RETURN);
        require(state.prompt == nullptr, "Enter did not close the file's notice");
        {
            const std::optional<install::Origin> record = install::read_origin(ridge);
            require(
                record && record->kind == install::OriginKind::file &&
                    install::oamod::read_installed_mod(ridge).version == "2.0",
                "the file of another version is not in Mods with a file origin"
            );
        }
        open_the_library();
        post_ridge(kBuiltIn, "1.0", 4, 1);
        installs_quietly("check-builtin's release over a file's other version", kBuiltIn, kRidge);
        require(
            revision_in(ridge) == 4 && recorded_from(ridge, kBuiltIn, 1) &&
                install::oamod::read_installed_mod(kept).version == "2.0",
            "check-builtin's release did not keep the file's other version as the .backup"
        );

        // 6. A folder of the player's own where a built-in registry's mod
        // goes is refused in the Library's queue with no question and no
        // prompt, and left as it is; an added registry's asks FOLDER IN USE;
        // with the Library closed, the refusal is told.
        const fs::path players_folder = mods / std::string(kCinder);
        fs::create_directories(players_folder, error);
        {
            std::ofstream(players_folder / "notes.txt") << "not a mod";
        }
        const fs::path cinder = packages / "cinder.oamod";
        write_package(cinder, profile_text(kCinder, "Cinder", "1.0", 1), 1);
        const std::string own_reason =
            "Mods/cinder is a folder of your own, not Cinder. Move or rename it, then RETRY.";
        {
            const uint32_t shown = state.prompts_shown;
            post_catalogue(cinder, kBuiltIn, kCinder, "Cinder", 1);
            const std::optional<install::PackageOutcome> outcome =
                until_outcome("check-builtin's Cinder");
            require(
                outcome && outcome->result == install::OutcomeResult::refused &&
                    state.prompts_shown == shown && state.prompt == nullptr,
                "check-builtin's Cinder over a folder of the player's own asked or showed a prompt"
            );
            take_outcome(*outcome, kBuiltIn, kCinder);
        }
        {
            const std::optional<library::QueueItem> item = queued(kBuiltIn, kCinder);
            require(
                item && item->phase == library::QueueItem::Phase::failed &&
                    item->problem == own_reason && item->retryable,
                "the Library's queue does not say the folder is the player's own"
            );
        }
        const auto own_folder_unchanged = [&] {
            return fs::exists(players_folder / "notes.txt") &&
                   !fs::exists(players_folder / "oamod.yaml") && !fs::exists(mods / "cinder-1.0");
        };
        require(own_folder_unchanged(), "the folder of the player's own was changed");
        post_catalogue(cinder, kAdded, kCinder, "Cinder", 1);
        asks_over_library("check-added's Cinder", "FOLDER IN USE");
        tap(SDLK_ESCAPE);
        {
            const std::optional<install::PackageOutcome> outcome = until_outcome("CANCEL");
            require(
                outcome && outcome->result == install::OutcomeResult::set_aside,
                "CANCEL on FOLDER IN USE did not set the package aside"
            );
            take_outcome(*outcome, kAdded, kCinder);
        }
        require(own_folder_unchanged(), "CANCEL on FOLDER IN USE changed the folder");
        close_the_library();
        post_catalogue(cinder, kBuiltIn, kCinder, "Cinder", 1);
        until_prompt("check-builtin's Cinder with the Library closed");
        require(
            titled("MOD NOT INSTALLED") && says(own_reason),
            "with the Library closed the folder's refusal was not told in MOD NOT INSTALLED"
        );
        tap(SDLK_RETURN);
        require(state.prompt == nullptr, "Enter did not close MOD NOT INSTALLED");
        std::ignore = install::take_package_outcomes();
        require(own_folder_unchanged(), "a refusal with the Library closed changed the folder");

        // 7. A file the player opens still asks UPDATE MOD.
        const fs::path file_update = packages / "ridge-1.0-r5.oamod";
        write_package(file_update, profile_text(kRidge, "Ridge", "1.0", 5), 5);
        install::post_package_file(file_update);
        until_prompt("a file of the next revision");
        require(titled("UPDATE MOD"), "a file of the next revision did not ask UPDATE MOD");
        tap(SDLK_ESCAPE);
        require(
            state.prompt == nullptr && revision_in(ridge) == 4,
            "CANCEL on UPDATE MOD changed the folder"
        );

        // 8. With the Library open, a refused package shows no prompt and
        // the Library's queue says why.
        open_the_library();
        const fs::path dunes = packages / "dunes.oamod";
        write_package(
            dunes,
            profile_text(kDunes, "Dunes", "0.9", 1, "hacks:\n  example.no-such-hack: true\n"),
            1
        );
        {
            const uint32_t shown = state.prompts_shown;
            post_catalogue(dunes, kAdded, kDunes, "Dunes", 1);
            const std::optional<install::PackageOutcome> outcome =
                until_outcome("check-added's Dunes");
            require(
                outcome && outcome->result == install::OutcomeResult::refused &&
                    state.prompts_shown == shown && state.prompt == nullptr,
                "a refused package showed a prompt over the Library"
            );
            take_outcome(*outcome, kAdded, kDunes);
        }
        {
            const std::optional<library::QueueItem> item = queued(kAdded, kDunes);
            require(
                item && item->phase == library::QueueItem::Phase::failed &&
                    item->problem.starts_with("Its oamod.yaml has errors: ") &&
                    item->problem.find("no-such-hack") != std::string::npos,
                "the Library's queue does not say why Dunes was refused"
            );
        }
        require(!fs::exists(mods / std::string(kDunes)), "a refused package was installed");

        // 11. PLAY NOW plays the mod: the run ends for it.
        show_entry("oa.library.row.check-builtin.ridge");
        press_action("play-now");
        require(soft_restart_requested(), "PLAY NOW did not end the run");
        const auto stored = preference_values_.find(std::string(settings::key::mod_directory));
        require(
            stored != preference_values_.end() && stored->second == kept_path(ridge),
            "PLAY NOW did not store Ridge"
        );
        std::cout << "library install check: run 0 installed from three registries and played "
                     "Ridge\n";
        return;
    }
    case 1: {
        // 10. ROLL BACK's question over the Library says the game reloads
        // for the mod played; CANCEL changes nothing.
        settle();
        require(
            playing(ridge) && mod_profile() != nullptr && mod_profile()->id == kRidge &&
                revision_in(ridge) == 4,
            "the run does not play Ridge's fourth revision"
        );
        open_the_library();
        show_entry("oa.library.row.check-builtin.ridge");
        press_action("roll-back");
        {
            const auto* asked = dynamic_cast<const QuestionScreen*>(oa_layer().top());
            require(
                asked != nullptr && !asked->question().paragraphs.empty() &&
                    asked->question().paragraphs[0].text.ends_with(
                        "The game reloads its data for the mod and returns to the main menu."
                    ),
                "ROLL BACK's question does not say the game reloads for the mod played"
            );
        }
        tap(SDLK_ESCAPE);
        require(
            top_is(library_screen_name) && !soft_restart_requested() &&
                !install::pending_change_waiting(),
            "CANCEL on ROLL BACK's question changed the mod played"
        );

        // 9. A built-in registry's update to the mod played asks nothing,
        // and waits for the player to leave the mod: no soft restart.
        post_ridge(kBuiltIn, "1.0", 5, 2);
        installs_quietly("check-builtin's update to the mod played", kBuiltIn, kRidge);
        require(
            !soft_restart_requested() && install::pending_change_waiting() &&
                install::pending_change_target() == std::string(kRidge),
            "the update to the mod played did not wait, named, without a soft restart"
        );
        require(
            revision_in(ridge) == 4 &&
                revision_in(
                    mods /
                    (install::folder_names(install::mod_kind()).staging + std::string(kRidge))
                ) == 5,
            "the update to the mod played is not staged whole, the mod left as it is"
        );
        {
            const std::optional<library::QueueItem> item = queued(kBuiltIn, kRidge);
            const library::QueueLines lines = library::queue_lines(host.model);
            require(
                item && item->phase == library::QueueItem::Phase::held &&
                    lines.first == "Waiting · finishes when you leave Ridge",
                "the Library's queue does not say the update waits until Ridge is left"
            );
        }

        // CANCEL on the held line drops it. The catalogue still offers the
        // update, so the Library shows UPDATE again; nothing fetches it until
        // UPDATE is pressed.
        {
            library::UpdateNote offered;
            offered.registry = std::string(kBuiltIn);
            offered.kind = library::Kind::mod;
            offered.key = std::string(kRidge);
            offered.to_release = 2;
            offered.to_version = "1.0";
            offered.to_revision = 5;
            fixture.inputs.updates.push_back(std::move(offered));
            ++fixture.generation;
            frames();
            show_entry("oa.library.row.check-builtin.ridge");
            require(
                named("oa.library.action.cancel").has_value(),
                "the held update's queue line offers no CANCEL"
            );
            press_action("cancel");
            for (int count = 0; count < kMostFrames && state.discarder.busy(); ++count)
                frame();
            frames();
            require(!install::pending_change_waiting(), "CANCEL left the held update waiting");
            require(revision_in(ridge) == 4, "CANCEL changed the mod played");
            require(
                !fs::exists(
                    mods /
                    (install::folder_names(install::mod_kind()).staging + std::string(kRidge))
                ),
                "CANCEL left the staged folder"
            );
            require(!reserved_left(mods), "CANCEL left a folder an install keeps");
            require(!queued(kBuiltIn, kRidge), "CANCEL left the held update's queue line");
            require(
                named("oa.library.action.update").has_value(),
                "the Library does not offer UPDATE again"
            );
            std::cout << "library install check: CANCEL dropped the held update\n";
        }

        // Taken again, and held, so the next turn still finds it made.
        post_ridge(kBuiltIn, "1.0", 5, 2);
        installs_quietly("check-builtin's update taken again", kBuiltIn, kRidge);
        require(
            !soft_restart_requested() && install::pending_change_waiting() &&
                install::pending_change_target() == std::string(kRidge) && revision_in(ridge) == 4,
            "the update taken again did not wait, the mod left as it is"
        );
        {
            const std::optional<library::QueueItem> item = queued(kBuiltIn, kRidge);
            require(
                item && item->phase == library::QueueItem::Phase::held,
                "the update taken again is not held"
            );
        }

        // A second mod, and PLAY NOW on it: switching the mod makes the change.
        const fs::path lattice = packages / "lattice.oamod";
        write_package(lattice, profile_text(kLattice, "Lattice", "3.0", 1), 1);
        post_catalogue(lattice, kBuiltIn, kLattice, "Lattice", 1);
        {
            const uint32_t shown = state.prompts_shown;
            const std::optional<install::PackageOutcome> outcome = until_outcome("Lattice");
            require(
                outcome && outcome->result == install::OutcomeResult::installed &&
                    state.prompts_shown == shown,
                "Lattice was not installed with no prompt"
            );
            take_outcome(*outcome, kBuiltIn, kLattice);
        }
        require(revision_in(ridge) == 4, "the mod played changed before it was left");
        show_entry("oa.library.row.check-builtin.lattice");
        press_action("play-now");
        require(soft_restart_requested(), "PLAY NOW on Lattice did not end the run");
        std::cout << "library install check: run 1 held the update to the mod played\n";
        return;
    }
    case 2: {
        // The update was put in place as the mod was switched, and is told.
        require(playing(mods / std::string(kLattice)), "the run does not play Lattice");
        require(
            revision_in(ridge) == 5 && recorded_from(ridge, kBuiltIn, 2) &&
                revision_in(kept) == 4 && recorded_from(kept, kBuiltIn, 1) &&
                !install::pending_change_waiting(),
            "the held update was not put in place as the mod was switched"
        );
        for (int tick = 0; tick < 30; ++tick)
            tell_mod_installs();
        require(
            state.prompt == nullptr && state.stage != ModInstallState::Stage::telling,
            "the held catalogue update showed a notice"
        );
        // The folders the change dropped go, a step a frame, and nothing an
        // install keeps while it works is left.
        for (int count = 0; count < kMostFrames && state.discarder.busy(); ++count)
            tell_mod_installs();
        require(
            !state.discarder.busy() && !reserved_left(mods),
            "the held update left a folder of its own in Mods"
        );

        // 12. The update question. Ember and Flint come from check-added;
        // Ridge, and Lattice, played, from check-builtin. Ember's update
        // changes the rules and Flint's keeps them; Ridge's changes them
        // and Lattice's cannot be known.
        open_the_library();
        const fs::path ember_file = packages / "ember.oamod";
        write_package(ember_file, profile_text(kEmber, "Ember", "2.0", 1), 1);
        post_catalogue(ember_file, kAdded, kEmber, "Ember", 1);
        installs_quietly("check-added's Ember", kAdded, kEmber);
        const fs::path flint_file = packages / "flint.oamod";
        write_package(flint_file, profile_text(kFlint, "Flint", "1.0", 1), 1);
        post_catalogue(flint_file, kAdded, kFlint, "Flint", 1);
        installs_quietly("check-added's Flint", kAdded, kFlint);
        oa::base::sha256::Digest old_rules{};
        old_rules[0] = 0x9c;
        oa::base::sha256::Digest new_rules{};
        new_rules[0] = 0x51;
        const auto fixture_update = [&](std::string_view registry,
                                        std::string_view key,
                                        std::string_view version,
                                        int64_t revision,
                                        library::UpdateNote::Rules rules) {
            library::UpdateNote update;
            update.registry = std::string(registry);
            update.kind = library::Kind::mod;
            update.key = std::string(key);
            update.to_release = 9;
            update.to_version = std::string(version);
            update.to_revision = revision;
            update.size = 3355443;
            update.rules = rules;
            update.from_rules = old_rules;
            if (rules == library::UpdateNote::Rules::changes)
                update.to_rules = new_rules;
            else if (rules == library::UpdateNote::Rules::same)
                update.to_rules = old_rules;
            fixture.inputs.updates.push_back(std::move(update));
        };
        fixture.queued.clear();
        fixture.inputs.queue.clear();
        fixture.inputs.updates.clear();
        fixture_update(kAdded, kEmber, "2.0", 2, library::UpdateNote::Rules::changes);
        fixture_update(kAdded, kFlint, "1.0", 2, library::UpdateNote::Rules::same);
        fixture_update(kBuiltIn, kRidge, "1.0", 6, library::UpdateNote::Rules::changes);
        fixture_update(kBuiltIn, kLattice, "3.0", 2, library::UpdateNote::Rules::unknown);
        list_installed();
        frames();
        const auto queued_now = [&](std::string_view registry, std::string_view key) {
            return !fixture.queued.empty() && fixture.queued.back().registry == registry &&
                   fixture.queued.back().key == key &&
                   fixture.queued.back().reason == content::DownloadReason::update;
        };
        const auto asked = [&]() -> const QuestionScreen* {
            return dynamic_cast<const QuestionScreen*>(oa_layer().top());
        };
        const auto asks_update = [&](std::string_view title, std::string_view over) {
            const QuestionScreen* question = asked();
            require(
                question != nullptr && question->question().title == title &&
                    oa_layer().find(over) != nullptr,
                std::string("the update question ") + std::string(title) + " was not asked over " +
                    std::string(over)
            );
            const settings::Prompt& shown = question->question();
            require(
                shown.buttons.size() == 2 && shown.buttons[0].id == library::not_now_button_id &&
                    shown.buttons[1].id == library::update_button_id && shown.buttons[1].accent &&
                    shown.marked == 0,
                "the update question is not NOT NOW, marked, then UPDATE, primary"
            );
            frame();
            require(
                named("oa.prompt.not-now").has_value() && named("oa.prompt.update").has_value() &&
                    named("oa.prompt.body").has_value(),
                "automation does not list the update question's oa.prompt.not-now, update and body"
            );
        };

        // From the Library: Ember asks first, NOT NOW leaving it and UPDATE
        // queuing it; Flint is queued at once.
        show_entry("oa.library.row.check-added.ember");
        press_action("update");
        asks_update("Update Ember", library_screen_name);
        {
            const settings::Prompt& shown = asked()->question();
            require(
                shown.paragraphs.size() == 4 &&
                    shown.paragraphs[0].text == "Ember 2.0 · revision 1 → 2" &&
                    shown.paragraphs[1].text.starts_with(
                        "Revision 2 changes the game's rules. Saved games from revision 1 won't "
                        "load under revision 2."
                    ) &&
                    shown.paragraphs[2].text == "Rules: 9c00 0000 → 5100 0000" &&
                    shown.paragraphs[3].text == "Download: 3.2 MB",
                "the update question does not say what Ember's update does"
            );
        }
        tap(SDLK_ESCAPE);
        require(
            top_is(library_screen_name) && fixture.queued.empty(),
            "NOT NOW on the update question queued Ember's update"
        );
        press_action("update");
        asks_update("Update Ember", library_screen_name);
        click_named("oa.prompt.update");
        require(
            top_is(library_screen_name) && fixture.queued.size() == 1 && queued_now(kAdded, kEmber),
            "UPDATE on the update question did not queue Ember's update"
        );
        show_entry("oa.library.row.check-added.flint");
        press_action("update");
        require(
            top_is(library_screen_name) && fixture.queued.size() == 2 && queued_now(kAdded, kFlint),
            "an added registry's update that keeps the rules was not queued at once"
        );

        // The built-in registry's: their Rules facts say what they do, and
        // UPDATE queues them at once with no question.
        const auto rules_say = [&](std::string_view part) {
            const std::optional<AutomationControl> rules = named("oa.library.rules");
            return rules.has_value() && rules->text.find(part) != std::string::npos;
        };
        show_entry("oa.library.row.check-builtin.ridge");
        require(
            rules_say("changes the game's rules"),
            "the Library's Rules fact does not say Ridge's update changes the game's rules"
        );
        press_action("update");
        require(
            top_is(library_screen_name) && fixture.queued.size() == 3 &&
                queued_now(kBuiltIn, kRidge),
            "the built-in registry's update that changes the rules asked or was not queued"
        );
        show_entry("oa.library.row.check-builtin.lattice");
        require(
            rules_say("unknown"),
            "the Library's Rules fact does not say Lattice's rules are unknown"
        );
        press_action("update");
        require(
            top_is(library_screen_name) && fixture.queued.size() == 4 &&
                queued_now(kBuiltIn, kLattice),
            "the built-in registry's update with unknown rules asked or was not queued"
        );

        // Again, each in turn: each
        // entry's status line and Rules fact say what its update does, CHECK
        // NOW can ask the registries, the built-in registry's updates are
        // queued at once, and Ember asks over the Library.
        const auto status_says = [&](std::string_view part) {
            const std::optional<AutomationControl> status = named("oa.library.status");
            return status.has_value() && status->text.find(part) != std::string::npos;
        };
        show_entry("oa.library.row.check-builtin.ridge");
        require(
            status_says("Revision 6 available · 3.2 MB") && rules_say("changes the game's rules"),
            "the Library does not say what Ridge's update does"
        );
        show_entry("oa.library.row.check-builtin.lattice");
        require(
            status_says("Revision 2 available · 3.2 MB") && rules_say("unknown"),
            "the Library does not say what Lattice's update does"
        );
        show_entry("oa.library.row.check-added.ember");
        require(
            status_says("Revision 2 available · 3.2 MB") && rules_say("changes the game's rules"),
            "the Library does not say what Ember's update does"
        );
        {
            // The refresh icon is in the header. Compact's details page goes
            // back to the list first. It stays disabled, and says so, while
            // the registries are being asked.
            if (named("oa.library.back"))
                click_named("oa.library.back");
            const std::optional<AutomationControl> off = named("oa.library.refresh");
            require(
                off.has_value() && !off->enabled && off->text == "Refresh",
                "the Library offers refresh with no registry to ask"
            );
            fixture.inputs.can_check = true;
            ++fixture.generation;
            frames();
            const std::optional<AutomationControl> on = named("oa.library.refresh");
            require(
                on.has_value() && on->enabled && on->text == "Refresh",
                "the Library offers no refresh icon to ask the registries"
            );
            fixture.inputs.checking = true;
            ++fixture.generation;
            frames();
            const std::optional<AutomationControl> spinning = named("oa.library.refresh");
            require(
                spinning.has_value() && !spinning->enabled && spinning->text == "Refreshing",
                "the refresh icon can be pressed, or does not say it is refreshing, while a "
                "registry is being asked"
            );
            fixture.inputs.checking = false;
            fixture.inputs.can_check = false;
            ++fixture.generation;
            frames();
        }
        show_entry("oa.library.row.check-builtin.ridge");
        press_action("update");
        require(
            top_is(library_screen_name) && fixture.queued.size() == 5 &&
                queued_now(kBuiltIn, kRidge),
            "UPDATE on Ridge's entry asked or did not queue its update"
        );
        show_entry("oa.library.row.check-builtin.lattice");
        press_action("update");
        require(
            top_is(library_screen_name) && fixture.queued.size() == 6 &&
                queued_now(kBuiltIn, kLattice),
            "UPDATE on Lattice's entry asked or did not queue its update"
        );
        show_entry("oa.library.row.check-added.ember");
        press_action("update");
        asks_update("Update Ember", library_screen_name);
        tap(SDLK_ESCAPE);
        require(
            top_is(library_screen_name) && fixture.queued.size() == 6,
            "NOT NOW on the update question over the Library queued Ember's update"
        );
        std::cout << "library install check: built-in update queued with no question\n";

        // UPDATE ALL: the built-in registry's updates, and the added one's
        // that keeps the rules, at once in list order; one question about
        // Ember alone, NOT NOW leaving it.
        close_the_library();
        fixture.queued.clear();
        list_installed();
        open_library(library::Tab::updates);
        frames();
        require(top_is(library_screen_name), "the Library did not open on Updates");
        click_named("oa.library.action.update-all");
        {
            const QuestionScreen* question = asked();
            require(
                question != nullptr && question->question().title == "Update all" &&
                    question->question().paragraphs.size() == 1 &&
                    question->question().paragraphs[0].text.starts_with(
                        "1 update changes the game's rules: Ember."
                    ),
                "UPDATE ALL did not ask about Ember alone"
            );
            bool ember = false;
            bool ridge_queued = false;
            bool lattice_queued = false;
            for (const FeedQueued& each : fixture.queued) {
                ember = ember || each.key == kEmber;
                ridge_queued = ridge_queued || (each.registry == kBuiltIn && each.key == kRidge);
                lattice_queued =
                    lattice_queued || (each.registry == kBuiltIn && each.key == kLattice);
            }
            require(
                !ember && ridge_queued && lattice_queued && fixture.queued.size() == 3,
                "UPDATE ALL did not queue the built-in registry's updates and Flint's at once"
            );
        }
        tap(SDLK_ESCAPE);
        require(
            top_is(library_screen_name) && fixture.queued.size() == 3,
            "NOT NOW on UPDATE ALL's question queued Ember's update"
        );
        click_named("oa.library.action.update-all");
        click_named("oa.prompt.update");
        require(
            fixture.queued.size() == 7 && queued_now(kAdded, kEmber),
            "UPDATE on UPDATE ALL's question did not queue Ember's update"
        );
        fixture.inputs.updates.clear();
        ++fixture.generation;
        close_the_library();

        // UNINSTALL. A map pack's is off while a game is played, and says
        // why; the language pack in use and the mod being played, Lattice,
        // show a notice and keep their folders; a built-in registry's mod
        // asks first, CANCEL removing nothing and UNINSTALL its folder and
        // the copy ROLL BACK keeps, and its row stays, not installed; and a
        // mod of the player's own leaves the Library with its folder.
        std::error_code uninstall_error;
        const fs::path maps_folder = user_folder_ / "Maps";
        const fs::path languages_folder = user_folder_ / "Languages";
        const fs::path pack_folder = maps_folder / std::string(kArchipelago);
        const fs::path language_folder = languages_folder / std::string(kPseudo);
        for (const fs::path& made : {pack_folder, language_folder}) {
            fs::create_directories(made, uninstall_error);
            std::ofstream(made / "kept.txt") << "kept";
        }
        const auto still_there = [](const fs::path& made) { return fs::exists(made / "kept.txt"); };
        const auto beside = [&](library::Kind kind,
                                std::string_view key,
                                std::string_view name,
                                const fs::path& folder) {
            library::Installed shown;
            shown.kind = kind;
            shown.key = std::string(key);
            shown.folder = folder;
            shown.name = std::string(name);
            shown.version = "1.0";
            shown.registry = std::string(kBuiltIn);
            shown.release = 1;
            return shown;
        };
        beside_mods.push_back(
            beside(library::Kind::map_pack, kArchipelago, "Archipelago", pack_folder)
        );
        beside_mods.push_back(beside(library::Kind::language, kPseudo, "Pseudo", language_folder));
        beside_mods.back().in_use = true;
        const auto notice_says = [&](std::string_view title, std::string_view text) {
            const QuestionScreen* told = asked();
            return told != nullptr && told->question().title == title &&
                   told->question().paragraphs.size() == 1 &&
                   told->question().paragraphs[0].text == text &&
                   told->question().buttons.size() == 1 &&
                   told->question().buttons[0].id == library::uninstall_ok_button_id;
        };
        open_the_library();

        // A map pack while a game is played: UNINSTALL is off and the note
        // says why; the runtime, asked directly, does nothing either.
        fixture.inputs.in_match = true;
        ++fixture.generation;
        frames();
        click_named("oa.library.tab.maps");
        frames();
        show_entry("oa.library.row.check-builtin.archipelago");
        {
            const std::optional<AutomationControl> button = named("oa.library.action.uninstall");
            const std::optional<AutomationControl> note = named("oa.library.note");
            require(
                button.has_value() && !button->enabled && note.has_value() &&
                    note->text == "Map packs can't be uninstalled during a game.",
                "a map pack's UNINSTALL is not off during a game, with its reason"
            );
        }
        library_entry_action(
            library::Action::uninstall,
            {std::string(kBuiltIn), library::Kind::map_pack, std::string(kArchipelago)}
        );
        frame();
        require(
            top_is(library_screen_name) && still_there(pack_folder),
            "UNINSTALL of a map pack during a game did something"
        );
        fixture.inputs.in_match = false;
        ++fixture.generation;
        frames();

        // The language pack in use: a notice, and its folder stays.
        click_named("oa.library.tab.languages");
        frames();
        show_entry("oa.library.row.check-builtin.en-xa");
        press_action("uninstall");
        require(
            notice_says(
                "Uninstall Pseudo",
                "The game is shown in Pseudo. Choose another language in Settings › Language "
                "first, then uninstall it."
            ),
            "UNINSTALL of the language pack in use did not say to choose another language first"
        );
        frame();
        require(
            named("oa.prompt.ok").has_value(), "automation does not list the notice's oa.prompt.ok"
        );
        tap(SDLK_RETURN);
        require(
            top_is(library_screen_name) && still_there(language_folder),
            "the notice for the language pack in use did not close, or its folder went"
        );
        click_named("oa.library.tab.mods");
        frames();

        // The mod being played: a notice, and its folder stays.
        show_entry("oa.library.row.check-builtin.lattice");
        press_action("uninstall");
        require(
            notice_says(
                "Uninstall Lattice",
                "You are playing Lattice. Switch it off first with SWITCH OFF, then uninstall it."
            ),
            "UNINSTALL of the mod being played did not say to switch it off first"
        );
        tap(SDLK_RETURN);
        require(
            top_is(library_screen_name) && revision_in(mods / std::string(kLattice)) == 1,
            "the notice for the mod being played did not close, or the mod changed"
        );

        // A built-in registry's mod with a copy kept: the question, CANCEL
        // removing nothing, and UNINSTALL removing both.
        const fs::path basalt = mods / std::string(kBasalt);
        for (const int release : {1, 2}) {
            const fs::path file = packages / ("basalt-" + std::to_string(release) + ".oamod");
            write_package(file, profile_text(kBasalt, "Basalt", "1.0", release), release);
            post_catalogue(file, kBuiltIn, kBasalt, "Basalt", release);
            installs_quietly("check-builtin's Basalt", kBuiltIn, kBasalt);
        }
        require(
            revision_in(basalt) == 2 &&
                revision_in(basalt / std::string(install::backup_folder_name)) == 1,
            "Basalt's second release did not keep the first"
        );
        {
            library::Listing listed;
            listed.registry = std::string(kBuiltIn);
            listed.kind = library::Kind::mod;
            listed.key = std::string(kBasalt);
            listed.name = "Basalt";
            listed.version = "1.0";
            listed.revision = 2;
            listed.release = 2;
            listed.size = 1024;
            fixture.inputs.listings.push_back(std::move(listed));
            ++fixture.generation;
            frames();
        }
        const auto asks_uninstall = [&](std::string_view title,
                                        std::string_view first,
                                        std::string_view second) {
            const QuestionScreen* question = asked();
            require(
                question != nullptr && question->question().title == title &&
                    oa_layer().find(library_screen_name) != nullptr,
                std::string("UNINSTALL did not ask ") + std::string(title) + " over the Library"
            );
            const settings::Prompt& shown = question->question();
            require(
                shown.paragraphs.size() == 2 && shown.paragraphs[0].text == first &&
                    shown.paragraphs[1].text == second,
                std::string(title) + " does not say what it removes"
            );
            require(
                shown.buttons.size() == 2 &&
                    shown.buttons[0].id == library::uninstall_cancel_button_id &&
                    shown.buttons[1].id == library::uninstall_button_id &&
                    shown.buttons[1].accent && shown.marked == 0,
                std::string(title) + " is not CANCEL, marked, then UNINSTALL, primary"
            );
            frame();
            require(
                named("oa.prompt.cancel").has_value() && named("oa.prompt.uninstall").has_value() &&
                    named("oa.prompt.body").has_value(),
                "automation does not list the question's oa.prompt.cancel, uninstall and body"
            );
        };
        const auto drain = [&] {
            for (int count = 0; count < kMostFrames && state.discarder.busy(); ++count)
                frame();
            require(
                !state.discarder.busy() && !reserved_left(mods),
                "an uninstall left a folder of its own in Mods"
            );
        };
        show_entry("oa.library.row.check-builtin.basalt");
        press_action("uninstall");
        asks_uninstall(
            "Uninstall Basalt",
            "Uninstall Basalt? Its folder, Mods/basalt, is removed, and with it the copy ROLL BACK "
            "keeps.",
            "GET installs it again."
        );
        tap(SDLK_ESCAPE);
        require(
            top_is(library_screen_name) && revision_in(basalt) == 2 &&
                revision_in(basalt / std::string(install::backup_folder_name)) == 1,
            "CANCEL on the Uninstall question changed Basalt"
        );
        press_action("uninstall");
        click_named("oa.prompt.uninstall");
        require(
            top_is(library_screen_name) && !fs::exists(fs::symlink_status(basalt)),
            "UNINSTALL did not remove Basalt's folder"
        );
        drain();
        list_installed();
        frames();
        {
            const library::Entry* entry = library::find_entry(
                host.model, {std::string(kBuiltIn), library::Kind::mod, std::string(kBasalt)}
            );
            require(
                entry != nullptr && entry->installed == nullptr &&
                    entry->state == library::State::get &&
                    named("oa.library.row.check-builtin.basalt").has_value(),
                "the Library does not show Basalt as not installed"
            );
        }
        std::cout << "library install check: UNINSTALL removed a built-in registry's mod\n";

        // A mod of the player's own, which no registry lists, leaves the Library.
        const fs::path scree = mods / std::string(kScree);
        fs::create_directories(scree, uninstall_error);
        std::ofstream(scree / "oamod.yaml") << profile_text(kScree, "Scree", "1.0", 1);
        list_installed();
        frames();
        show_entry("oa.library.row.scree");
        press_action("uninstall");
        asks_uninstall(
            "Uninstall Scree",
            "Uninstall Scree? Its folder, Mods/scree, is removed.",
            "No registry lists it, so the Library cannot get it again."
        );
        click_named("oa.prompt.uninstall");
        require(
            top_is(library_screen_name) && !fs::exists(fs::symlink_status(scree)),
            "UNINSTALL did not remove the player's own mod"
        );
        drain();
        list_installed();
        frames();
        require(
            library::find_entry(
                host.model, {std::string(), library::Kind::mod, std::string(kScree)}
            ) == nullptr &&
                !named("oa.library.row.scree").has_value(),
            "the player's own mod is still in the Library"
        );
        require(
            still_there(pack_folder) && still_there(language_folder) &&
                revision_in(mods / std::string(kLattice)) == 1 && revision_in(ridge) == 5,
            "an uninstall removed something it was not asked to"
        );
        beside_mods.clear();
        list_installed();
        close_the_library();
        fs::remove_all(maps_folder, uninstall_error);
        fs::remove_all(languages_folder, uninstall_error);
        std::cout
            << "library install check: UNINSTALL kept the mod played and the language in use\n";

        // The last run plays No Mod, as the next start of the check does.
        preference_values_.erase(std::string(settings::key::mod_directory));
        preferences_dirty_ = true;
        request_soft_restart();
        std::cout << "library install check: run 2 found the held update made and queued the "
                     "updates\n";
        return;
    }
    case 3: {
        // 12. No Mod is played. GET on Fast Builders queues it, the package
        // installs under Mutators with its origin record, CHECK WITH THIS MOD
        // runs the fixture's fake checker, OPEN FOLDER and HOMEPAGE reach the
        // recorders, and UNINSTALL asks and removes the folder. Ridge stays
        // as the previous run left it.
        settle();
        require(options_.layered_mod_folder.empty(), "the mutator run plays a mod");
        require(revision_in(ridge) == 5, "the mutator run does not find Ridge's fifth revision");
        const fs::path mutators_root =
            user_folder_ / std::string(install::oamut::mutators_folder_name);
        const fs::path fast_folder = mutators_root / std::string(kFast);
        std::error_code fast_error;
        fs::remove_all(fast_folder, fast_error);

        {
            library::Listing listed;
            listed.registry = std::string(kBuiltIn);
            listed.kind = library::Kind::mutator;
            listed.key = std::string(kFast);
            listed.name = "Fast Builders";
            listed.version = "2.0";
            listed.revision = 1;
            listed.release = 1;
            listed.author = "Ada";
            listed.summary = "Builds faster.";
            listed.homepage = "https://example.test/fast-builders";
            listed.size = 1024;
            library::MutatorFacts facts;
            facts.works_with.push_back({"*", {}, {}});
            facts.layers.push_back("units");
            facts.tags.push_back("economy");
            listed.mutator = std::move(facts);
            fixture.inputs.listings.push_back(std::move(listed));
            ++fixture.generation;
        }
        list_installed();
        open_library(library::Tab::mutators);
        frames();
        require(top_is(library_screen_name), "the Library did not open on Mutators");
        show_entry("oa.library.row.check-builtin.fast-builders");
        const std::size_t queued_before = fixture.queued.size();
        press_action("get");
        require(
            fixture.queued.size() == queued_before + 1 &&
                fixture.queued.back().registry == kBuiltIn && fixture.queued.back().key == kFast &&
                fixture.queued.back().reason == content::DownloadReason::install,
            "GET did not queue Fast Builders"
        );
        std::cout << "library install check: mutator queued\n";

        const fs::path fast_file = packages / "fast-builders.oamut";
        write_mutator(fast_file);
        post_catalogue(fast_file, kBuiltIn, kFast, "Fast Builders", 1, library::Kind::mutator);
        installs_quietly("Fast Builders", kBuiltIn, kFast);
        {
            const std::optional<install::Origin> record = install::read_origin(fast_folder);
            require(
                fs::exists(fast_folder) && record &&
                    record->kind == install::OriginKind::catalogue &&
                    record->registry == kBuiltIn && record->catalogue_id == kFast &&
                    record->release == 1,
                "Fast Builders is not under Mutators with its origin record"
            );
        }
        std::cout << "library install check: mutator installed\n";

        host.feed.remembered_verdict = feed_remembered;
        host.feed.start_check = feed_start;
        host.feed.keep_verdict = feed_keep;
        ++fixture.generation;
        frames();

        const auto fast_entry = [&]() -> const library::Entry* {
            return library::find_entry(
                host.model, {std::string(kBuiltIn), library::Kind::mutator, std::string(kFast)}
            );
        };
        const auto checked_value = [&]() -> std::string {
            const library::Entry* shown_entry = fast_entry();
            if (shown_entry == nullptr)
                return {};
            for (const library::Fact& fact : library::facts(host.model, *shown_entry))
                if (fact.long_label == library::words::fact_checked)
                    return fact.long_value;
            return {};
        };
        const auto offers_check = [&]() -> bool {
            const library::Entry* shown_entry = fast_entry();
            if (shown_entry == nullptr)
                return false;
            for (const library::ActionButton& button :
                 library::entry_actions(host.model, *shown_entry))
                if (button.action == library::Action::check_with_mod)
                    return true;
            return false;
        };
        {
            const library::Entry* shown_entry = fast_entry();
            require(
                shown_entry != nullptr && shown_entry->installed != nullptr && offers_check() &&
                    library::status_line(host.model, *shown_entry) ==
                        library::words::status_installed,
                "CHECK WITH THIS MOD is not offered for the installed mutator"
            );
        }

        const auto wait_until = [&](const auto& ready, std::string_view what) {
            for (int count = 0; count < kMostFrames && !ready(); ++count)
                frame();
            require(ready(), what);
        };

        fixture.release.store(false);
        fixture.work_started.store(false);
        fixture.canned = {};
        fixture.canned.verdict = oa::data::mutators::Verdict::compatible;
        fixture.canned.counts.units_changed = 2u;
        press_action("check-with-mod");
        wait_until(
            [&] { return fixture.work_started.load(); }, "the check did not start on the worker"
        );
        require(
            checked_value().find("Checking with No Mod") != std::string::npos,
            "the Checked fact did not say the check was running"
        );
        fixture.release.store(true);
        wait_until([&] { return fixture.keep_count >= 1; }, "keep_verdict was not called");
        require(
            checked_value().find("changes 2 units") != std::string::npos,
            "the Checked fact does not say changes 2 units"
        );
        std::cout << "library install check: mutator checked (fake verdict)\n";

        fixture.release.store(false);
        fixture.work_started.store(false);
        fixture.canned = {};
        fixture.canned.verdict = oa::data::mutators::Verdict::incompatible;
        fixture.canned.reason = "needs a unit";
        press_action("check-with-mod");
        wait_until([&] { return fixture.work_started.load(); }, "the second check did not start");
        fixture.release.store(true);
        wait_until([&] { return fixture.keep_count >= 2; }, "the second verdict was not kept");
        {
            const library::Entry* shown_entry = fast_entry();
            require(
                shown_entry != nullptr && shown_entry->not_compatible &&
                    checked_value().find("needs a unit") != std::string::npos,
                "an incompatible verdict did not mark the row"
            );
        }

        host.feed.remembered_verdict = nullptr;
        host.feed.start_check = nullptr;
        host.feed.keep_verdict = nullptr;
        ++fixture.generation;
        frames();
        require(
            checked_value().empty() && !offers_check(),
            "null verdict entries still show Checked or CHECK WITH THIS MOD"
        );

        require(web_links_ != nullptr, "the run keeps no record of web links");
        const std::size_t opened = user_folder_state().opened.size();
        press_action("open-folder");
        require(
            user_folder_state().opened.size() == opened + 1 &&
                kept_path(user_folder_state().opened.back()) == kept_path(fast_folder),
            "OPEN FOLDER did not show Fast Builders' folder"
        );
        const std::size_t links = web_links_->requests.size();
        press_action("homepage");
        require(
            web_links_->requests.size() == links + 1 &&
                web_links_->requests.back() == "https://example.test/fast-builders",
            "HOMEPAGE did not record Fast Builders' homepage"
        );

        press_action("uninstall");
        {
            const auto* asked = dynamic_cast<const QuestionScreen*>(oa_layer().top());
            require(
                asked != nullptr && oa_layer().find(library_screen_name) != nullptr,
                "UNINSTALL did not ask over the Library"
            );
            const settings::Prompt& shown = asked->question();
            require(shown.title == "Uninstall Fast Builders", "the question's title is wrong");
            require(
                shown.paragraphs.size() == 2 &&
                    shown.paragraphs[0].text == "Uninstall Fast Builders? Its folder, "
                                                "Mutators/fast-builders, is removed." &&
                    shown.paragraphs[1].text == "GET installs it again.",
                "the question does not say what it removes"
            );
            require(
                shown.buttons.size() == 2 &&
                    shown.buttons[0].id == library::uninstall_cancel_button_id &&
                    shown.buttons[1].id == library::uninstall_button_id &&
                    shown.buttons[1].accent && shown.marked == 0,
                "the question is not CANCEL, marked, then UNINSTALL, primary"
            );
        }
        frame();
        require(
            named("oa.prompt.uninstall").has_value(), "automation does not list oa.prompt.uninstall"
        );
        click_named("oa.prompt.uninstall");
        for (int count = 0; count < kMostFrames &&
                            (state.discarder.busy() || fs::exists(fs::symlink_status(fast_folder)));
             ++count)
            frame();
        require(
            !fs::exists(fs::symlink_status(fast_folder)),
            "UNINSTALL did not remove Fast Builders' folder"
        );
        list_installed();
        frames();
        {
            const library::Entry* shown_entry = fast_entry();
            require(
                shown_entry != nullptr && shown_entry->installed == nullptr &&
                    shown_entry->state == library::State::get,
                "Fast Builders is not shown as not installed"
            );
        }
        std::cout << "library install check: mutator uninstalled\n";
        require(revision_in(ridge) == 5, "the mutator run changed Ridge");
        // The worker has finished: both checks returned before this restart.
        request_soft_restart();
        return;
    }
    default: {
        // 13. A run nobody watches: SDL's dummy video driver, and none of
        // the prompts the other runs show. A built-in registry's update to
        // Ridge waits in the inbox while the Library is closed, and installs
        // with no prompt once the Library is the top screen, the folder it
        // drops deleted as on a menu someone watches.
        require(
            UserFolderState::unwatched(options_.unattended) && !state.check_shows_prompts,
            "the last run is watched: run the check on SDL's dummy video driver"
        );
        settle();
        require(options_.layered_mod_folder.empty(), "the last run plays a mod");
        require(
            revision_in(ridge) == 5 && recorded_from(ridge, kBuiltIn, 2) &&
                revision_in(kept) == 4 && !install::package_files_waiting(),
            "the last run does not find Ridge's fifth revision with nothing waiting"
        );
        const uint32_t prompts_before = state.prompts_shown;
        post_ridge(kBuiltIn, "1.0", 6, 3);
        for (int count = 0; count < kWaitingFrames; ++count) {
            tell_mod_installs();
            frame();
        }
        {
            const std::optional<install::OpenedPackage> chosen_waiting =
                install::next_package_file();
            require(
                chosen_waiting && chosen_waiting->origin.kind == install::OriginKind::catalogue &&
                    chosen_waiting->origin.registry == kBuiltIn && revision_in(ridge) == 5 &&
                    state.stage == ModInstallState::Stage::idle && state.prompt == nullptr &&
                    state.prompts_shown == prompts_before,
                "a chosen mod did not wait in the inbox with the Library closed and nobody "
                "watching"
            );
        }
        list_installed();
        open_library(library::Tab::mods);
        require(top_is(library_screen_name), "the Library did not open over the main menu");
        installs_quietly("check-builtin's update with nobody watching", kBuiltIn, kRidge);
        require(
            revision_in(ridge) == 6 && recorded_from(ridge, kBuiltIn, 3) &&
                revision_in(kept) == 5 && recorded_from(kept, kBuiltIn, 2) &&
                state.prompts_shown == prompts_before,
            "check-builtin's update with nobody watching did not keep the fifth revision"
        );
        for (int count = 0; count < kMostFrames && state.discarder.busy(); ++count)
            tell_mod_installs();
        require(
            !state.discarder.busy() && !reserved_left(mods),
            "the update with nobody watching left a folder of its own in Mods"
        );
        std::cout << "library install check: nobody watching, a chosen mod waited for the "
                     "Library, then installed with no prompt\n";

        // 14. An added registry's copy over the built-in registry's asks
        // REPLACE MOD over the Library with nobody watching. It waits for its
        // answer, and REPLACE, pressed where automation lists it, installs.
        post_ridge(kAdded, "1.0", 7, 3);
        asks_over_library("check-added's release with nobody watching", "REPLACE MOD");
        for (int count = 0; count < kWaitingFrames; ++count)
            frame();
        require(
            state.prompt != nullptr && oa_layer().top() == state.prompt && titled("REPLACE MOD") &&
                state.prompts_shown == prompts_before + 1 && revision_in(ridge) == 6,
            "REPLACE MOD did not wait for its answer with nobody watching"
        );
        click_named("oa.prompt.replace");
        {
            const std::optional<install::PackageOutcome> outcome =
                until_outcome("REPLACE with nobody watching");
            require(
                outcome && outcome->result == install::OutcomeResult::installed &&
                    state.prompt == nullptr,
                "REPLACE on REPLACE MOD with nobody watching did not install check-added's release"
            );
            take_outcome(*outcome, kAdded, kRidge);
        }
        require(
            top_is(library_screen_name) && revision_in(ridge) == 7 &&
                recorded_from(ridge, kAdded, 3) && revision_in(kept) == 6 &&
                recorded_from(kept, kBuiltIn, 3),
            "REPLACE with nobody watching did not keep check-builtin's copy as the .backup"
        );
        for (int count = 0; count < kMostFrames && state.discarder.busy(); ++count)
            tell_mod_installs();
        require(
            !state.discarder.busy() && !reserved_left(mods),
            "REPLACE with nobody watching left a folder of its own in Mods"
        );
        std::cout << "library install check: nobody watching, REPLACE MOD waited for its "
                     "answer, and REPLACE installed\n";

        // 15. A file opened outside the game waits in the inbox for a run
        // someone watches, with the Library open and closed: nothing is read,
        // asked or installed.
        const fs::path opened_update = packages / "ridge-1.0-r8.oamod";
        write_package(opened_update, profile_text(kRidge, "Ridge", "1.0", 8), 8);
        install::post_package_file(opened_update);
        const uint32_t prompts_with_file = state.prompts_shown;
        const auto file_waits = [&](std::string_view where) {
            for (int count = 0; count < kWaitingFrames; ++count) {
                tell_mod_installs();
                frame();
            }
            const std::optional<install::OpenedPackage> file_waiting = install::next_package_file();
            require(
                file_waiting && file_waiting->origin.kind == install::OriginKind::file &&
                    file_waiting->file.filename() == opened_update.filename() &&
                    state.stage == ModInstallState::Stage::idle && state.prompt == nullptr &&
                    state.prompts_shown == prompts_with_file && revision_in(ridge) == 7,
                "a file opened outside the game did not wait in the inbox " + std::string(where)
            );
        };
        file_waits("with the Library open");
        close_the_library();
        file_waits("on the main menu");
        {
            // Taken back out, so the check leaves nothing waiting.
            const std::optional<install::OpenedPackage> taken = install::take_package_file();
            install::finish_package_file();
            require(
                taken && !install::package_files_waiting(),
                "the file opened outside the game was not the only package waiting"
            );
        }
        std::cout << "library install check: nobody watching, a file opened outside the game "
                     "stayed in the inbox\n";
        std::cout << "library install check: passed\n";
        return;
    }
    }
}

} // namespace oa::app
