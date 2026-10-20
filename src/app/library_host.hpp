// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Library's host (Runtime::LibraryHost), which runtime_library.cpp
// drives: the Library's model and its screen's state, the feed its inputs
// come from (LibraryFeed), the pictures it has taken, the updates it keeps
// for the queue, and its worker (LibraryWork), which finds the installed
// packages and their updates and reads and fetches the badge pictures off
// the main thread. The Library itself is a screen of the OA layer
// (Runtime::LibraryScreen, named library).
#pragma once

#include "oa/app/content/downloads.hpp"
#include "oa/app/content/pictures.hpp"
#include "oa/app/content/service.hpp"
#include "oa/app/content/updates.hpp"
#include "oa/app/mod_summary.hpp"
#include "oa/app/runtime.hpp"
#include "oa/base/threads.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/screen.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

/// The name the Library has on the OA layer.
inline constexpr std::string_view library_screen_name = "library";

/// Minimaps the Library has decoded. Defined in runtime_library.cpp: this
/// header stays free of the preview cache, which the rest of the app does
/// not include. The host holds them because the Library's inputs are built
/// here and the screen asks for the rows it shows.
struct MapPreviewState;

/// Where the Library's inputs come from, and where its downloads go. The
/// runtime fills it from the content service, the Library's worker and the
/// download queue (queue_download); --check-library fills it with fixtures
/// and starts no download.
struct LibraryFeed {
    void* context{}; ///< passed back to each function
    /// Returns everything the Library is built from now. Null shows an empty
    /// Library.
    oa::ui::library::Inputs (*inputs)(void* context){};
    /// Returns a count that moves on whenever inputs would give something
    /// else. Null never moves: the Library is built once.
    uint64_t (*generation)(void* context){};
    /// Queues one package of a registry for download, GET's or UPDATE's.
    /// Null queues nothing.
    ///
    /// Returns the queue item, or nothing when it was refused, why saying
    /// why when it is not null.
    std::optional<uint64_t> (*enqueue)(
        void* context,
        std::string_view registry,
        std::string_view key,
        content::DownloadReason reason,
        std::string* why
    ){};
    /// Cancels the download of one package of a registry that has not ended.
    /// Null cancels nothing.
    void (*cancel)(void* context, std::string_view registry, std::string_view key){};
    /// Queues one package's failed download of a registry again. Null queues
    /// nothing.
    void (*retry)(void* context, std::string_view registry, std::string_view key){};
    /// Tells whether a registry is built in: the Registry::built_in inputs
    /// gives it. Null tells that none is.
    bool (*built_in)(void* context, std::string_view registry){};
};

/// The Library's worker: one thread of its own, started at its first job and
/// joined as the host goes, which finds the installed packages and the
/// updates the catalogues offer for them after a catalogue refresh or an
/// install, and reads and fetches the badge pictures asked for. What it
/// found is kept under its lock for the main thread to read.
struct LibraryWork {
    base::threads::Mutex mutex{};            ///< guards every member below but cancel and pictures
    base::threads::ConditionVariable wake{}; ///< signalled when a job is due or the worker stops
    base::threads::Thread thread{};          ///< the worker, once started
    bool started{};                          ///< the worker was started
    bool stop{};                             ///< the worker stops at its next wait
    /// Set as the worker stops: a picture's fetch ends within 50 milliseconds.
    std::atomic<bool> cancel{false};
    /// The next search for updates is due: list the installed packages
    /// again, match them to the catalogues and find their updates.
    bool updates_due{};
    std::shared_ptr<const content::Snapshot> snapshot{}; ///< the catalogues the next search reads
    std::filesystem::path user_folder{};                 ///< the player's folder it lists
    /// The packages the last search found installed, in list_installed's order.
    std::vector<content::InstalledPackage> installed{};
    std::vector<content::Match> matches{};  ///< the catalogue release each is matched to
    std::vector<content::Update> updates{}; ///< the updates the catalogues offer for them
    std::vector<ModBadge>
        installed_badges{}; ///< each installed package's own picture; empty for none
    /// Each installed package's mutator facts, in `installed`'s order. Unset
    /// for every other kind. The names are empty until the main thread
    /// resolves them from the other inputs.
    std::vector<std::optional<oa::ui::library::MutatorFacts>> installed_mutators{};
    uint64_t searches{};  ///< counts the searches done
    bool pictures_due{};  ///< pictures wait to be read or fetched
    bool match_running{}; ///< a match runs: nothing is fetched
    /// The pictures asked for, read and fetched; its own lock guards it.
    std::unique_ptr<content::PictureCache> pictures{};
};

/// An update queued from the Library, kept so that the queue line still says
/// what it changes once its update note is gone.
struct KeptUpdate {
    std::string registry{};       ///< the package's registry
    oa::ui::library::Kind kind{}; ///< its kind
    std::string key{};            ///< its key
    /// Whether the update changes the rules saved games are tied to, as its
    /// update note said when it was queued.
    oa::ui::library::UpdateNote::Rules rules{oa::ui::library::UpdateNote::Rules::same};
    std::string from{}; ///< what ROLL BACK restores, as change_texts gives it
    std::string to{};   ///< what the update installs
};

/// The Library's host: its model and screen state, its feed, the pictures
/// it holds, the updates it keeps for the queue and its worker.
struct Runtime::LibraryHost {
    /// Makes the host, its worker not started.
    LibraryHost() = default;

    /// Stops the worker and joins it, within a second even while it fetches.
    ~LibraryHost();

    LibraryHost(const LibraryHost&) = delete;
    LibraryHost& operator=(const LibraryHost&) = delete;

    oa::ui::library::Library model{};     ///< the Library's model
    oa::ui::library::ScreenState state{}; ///< its screen's hover, focus, scroll and search
    LibraryFeed feed{};                   ///< where its inputs come from
    bool own_feed{true};                  ///< the feed is the runtime's own
    bool refreshed{};                     ///< the model was built from the feed once
    uint64_t feed_generation{};           ///< the feed's generation the model was built at
    uint64_t service_generation{};        ///< the content service's generation last searched at
    uint64_t queue_generation{};          ///< the download queue's generation last read
    uint64_t model_revision{};            ///< counts the model's rebuilds
    std::size_t update_count{};           ///< the updates that can be installed (update_count)
    /// The pictures taken from the worker, by address: badges and registry
    /// icons. They stay while the host lives.
    std::map<std::string, oa::ui::library::Badge> badges{};
    /// The logo the game ships, decoded once for a built-in registry that
    /// names it. Never fetched.
    std::optional<oa::ui::library::Badge> shipped_logo{};
    bool shipped_logo_tried{}; ///< true once the logo was decoded, or the decode failed
    /// Minimaps decoded for the rows on screen. Null until the host is made.
    /// The type lives in runtime_library.cpp.
    std::unique_ptr<MapPreviewState> previews{};
    /// Each listing's picture's address, by registry and key, as the last
    /// inputs resolved it.
    std::map<std::pair<std::string, std::string>, std::string> picture_urls{};
    std::vector<KeptUpdate> kept_updates{};  ///< the updates queued from the Library
    std::vector<uint64_t> installed_items{}; ///< the queue's items seen installed
    /// The installed-data reading a finished queue item was first listed
    /// against, by the queue's item id. A later reading of the installed
    /// packages no longer lets that item hide its update.
    std::map<uint64_t, uint64_t> finished_scans{};
    LibraryWork work{}; ///< the worker and what it found

    /// Starts the worker when it has not started.
    ///
    /// @return false when the worker could not start
    bool ensure_worker();

    /// Asks the worker to search for updates again over the content
    /// service's snapshot now: after a catalogue refresh, an install or a
    /// roll back. The runtime's own feed only.
    ///
    /// @param runtime the runtime
    void look_again(Runtime& runtime);

    /// Builds the model from the feed's inputs, as they are now, and keeps
    /// the update count; the screen lays itself out again at its next tick.
    ///
    /// @param runtime the runtime
    void rebuild(Runtime& runtime);

    /// Asks for the pictures of the rows in view and a few rows beyond each
    /// edge, the selected row's first, and for a registry icon the Registries
    /// tab or a From line is showing. Wakes the worker while any waits.
    /// The runtime's own feed only; nothing while a match runs. A picture
    /// already asked for, held or refused is not asked for again. A map
    /// pack's catalogue preview is asked for the same rows. One picture
    /// cache serves badges, map previews and registry icons.
    ///
    /// @param list the display list the Library shows
    void request_pictures(const oa::ui::kit::DisplayList& list);

    /// Returns the logo the game ships, decoded once from the app icon.
    ///
    /// The visible part of the window icon, so the plates fill the card.
    /// Nothing is fetched. Empty when the icon cannot be decoded, and then
    /// the lettermark stays.
    ///
    /// @return the logo; null when it could not be decoded
    const oa::ui::library::Badge* shipped_logo_badge();

    /// Asks the picture cache for one picture and wakes the worker. Nothing
    /// while a match runs. A picture already asked for, held or refused is
    /// not asked for again.
    ///
    /// @param registry the registry the catalogue names it under
    /// @param url its address, resolved against the host that served the catalogue
    void ask_cached_picture(std::string_view registry, std::string_view url);

    /// Returns a picture the cache has handed over, taking it into `badges`
    /// the first time. The caller gets a copy.
    ///
    /// @param registry the registry it was asked for under
    /// @param url its address, as asked for
    /// @return the picture; nothing while it is not ready
    [[nodiscard]] std::optional<oa::ui::library::Badge>
    cached_picture(std::string_view registry, std::string_view url);

    /// The picture source's known: an entry whose badge and map previews
    /// are held, already asked for or refused.
    ///
    /// @param context the host
    /// @param entry the entry
    /// @return true when nothing of it remains to ask for
    static bool picture_known(void* context, const oa::ui::library::Entry& entry);

    /// The picture source's request: the entry's badge, and a map pack's
    /// catalogue previews. The lead, the selected row, is fetched first.
    ///
    /// @param context the host
    /// @param entry the entry
    /// @param lead the selected row
    static void picture_request(void* context, const oa::ui::library::Entry& entry, bool lead);

    /// The registry-icon source's known: a shipped logo, an empty address,
    /// a picture already held, or one already asked for or refused.
    ///
    /// @param context the host
    /// @param registry the registry
    /// @return true when nothing remains to ask for
    static bool registry_icon_known(void* context, const oa::ui::library::Registry& registry);

    /// The registry-icon source's request. A shipped logo is never asked for.
    ///
    /// @param context the host
    /// @param registry the registry
    static void registry_icon_request(void* context, const oa::ui::library::Registry& registry);

    /// Decodes a few minimaps for the rows this list shows, into `previews`.
    ///
    /// An installed map uses its preview picture, or its terrain file's
    /// minimap. A catalogue picture uses bytes the worker has already handed
    /// over. One terrain file is decoded per call, or a few pictures when
    /// none is waiting, so a frame does not decode every map.
    ///
    /// @param runtime the runtime, whose game palette colours a terrain minimap
    /// @param list the display list the Library shows
    void pump_map_previews(Runtime& runtime, const oa::ui::kit::DisplayList& list);

    /// The runtime's own feed: the Library's inputs (Runtime::library_inputs).
    ///
    /// @param context the runtime
    /// @return the inputs
    static oa::ui::library::Inputs own_inputs(void* context);

    /// The runtime's own feed: what moves whenever the inputs would change
    /// (Runtime::library_feed_generation).
    ///
    /// @param context the runtime
    /// @return the count
    static uint64_t own_generation(void* context);

    /// The runtime's own feed: queues a package (Runtime::queue_download).
    ///
    /// @param context the runtime
    /// @param registry the registry's id
    /// @param key the package's key
    /// @param reason why the player asked
    /// @param[out] why why it was not queued; may be null
    /// @return the queue item; nothing when it was refused
    static std::optional<uint64_t> own_enqueue(
        void* context,
        std::string_view registry,
        std::string_view key,
        content::DownloadReason reason,
        std::string* why
    );

    /// The runtime's own feed: cancels the latest download of a package that
    /// has not ended.
    ///
    /// @param context the runtime
    /// @param registry the registry's id
    /// @param key the package's key
    static void own_cancel(void* context, std::string_view registry, std::string_view key);

    /// The runtime's own feed: queues the latest failed or cancelled
    /// download of a package again.
    ///
    /// @param context the runtime
    /// @param registry the registry's id
    /// @param key the package's key
    static void own_retry(void* context, std::string_view registry, std::string_view key);

    /// The runtime's own feed: whether a registry is built in, from the
    /// content service's snapshot: its origin is built-in, as the inputs'
    /// Registry::built_in says it.
    ///
    /// @param context the runtime
    /// @param registry the registry's id
    /// @return true for a registry that ships with the game
    static bool own_built_in(void* context, std::string_view registry);
};

/// Returns the Library's tab a tab of the settings dialog names.
///
/// @param tab the dialog's tab (Dialog::library_tab)
/// @return the Library's
[[nodiscard]] oa::ui::library::Tab library_tab_of(oa::ui::engine_settings::LibraryTab tab) noexcept;

} // namespace oa::app
