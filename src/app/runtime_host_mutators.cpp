// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What a guest can fetch of the host's mutator list, and the one press that
// queues those releases. Nothing here is drawn: the battle room's screen
// reads host_mutators_fetch and calls fetch_host_mutators.

#include "oa/app/runtime.hpp"

#include "oa/base/sha256.hpp"
#include "oa/data/catalogue/catalogue.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/ui/frontend_multiplayer/host_mutators.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {

namespace mp = oa::ui::frontend_multiplayer;
namespace oamut = oa::formats::oamut;

namespace {

/// One download this machine started for a mutator of the host's list, and
/// why the last try failed.
struct TrackedFetch {
    std::string id;      ///< the host's id
    std::string prefix;  ///< the host's SHA-256 prefix
    uint64_t item = 0;   ///< the queue item; 0 when none is being followed
    std::string failure; ///< why the last try failed, in English; empty after a press
};

/// A catalogue entry the fetch may queue, beside the offer the state reads.
struct Look {
    mp::MutatorOffer offer{};
    std::string package_id;       ///< the catalogue key, when fetchable_exact is set
    bool fetchable_exact = false; ///< a usable registry with downloads on lists the prefix
};

/// Writes one line about fetching the host's mutators.
///
/// @param line what happened, without a line end
void log_line(std::string_view line) {
    std::cerr << "open-annihilation: host mutators: " << line << '\n';
}

/// Returns a digest as 64 lower-case hex digits.
///
/// @param digest the digest
/// @return the text
std::string hex_text(const oa::base::sha256::Digest& digest) {
    const auto hex = oa::base::sha256::to_hex(digest);
    return std::string(hex.data(), hex.size());
}

/// Returns hex digits in lower case.
///
/// @param text the SHA-256 as stored
/// @return the same digits in lower case
std::string lower_hex(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char letter : text) {
        if (letter >= 'A' && letter <= 'Z')
            out.push_back(static_cast<char>(letter - 'A' + 'a'));
        else
            out.push_back(static_cast<char>(letter));
    }
    return out;
}

/// Tells whether a package SHA-256 starts with the host's prefix.
///
/// The same rule as mutator_fetch_state: 16 lower-case hex digits, and only
/// at the start.
///
/// @param sha256 the package SHA-256, lower-case hex
/// @param prefix the host's prefix
/// @return true when the SHA-256 starts with that prefix
bool starts_with_prefix(std::string_view sha256, std::string_view prefix) noexcept {
    return prefix.size() == mp::host_mutator_prefix_digits && sha256.size() >= prefix.size() &&
           sha256.compare(0, prefix.size(), prefix) == 0;
}

/// Tells whether two ASCII texts are the same, case ignored, as package keys are.
///
/// @param first a text
/// @param second another
/// @return true when they are the same
bool same_key(std::string_view first, std::string_view second) noexcept {
    if (first.size() != second.size())
        return false;
    for (std::size_t at = 0; at < first.size(); ++at) {
        const auto lower = [](char letter) {
            return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
        };
        if (lower(first[at]) != lower(second[at]))
            return false;
    }
    return true;
}

/// Tells whether a queue item has ended: done, failed or cancelled.
///
/// @param stage where it stands
/// @return true when it has ended
bool ended(content::DownloadState stage) noexcept {
    return stage == content::DownloadState::done || stage == content::DownloadState::failed ||
           stage == content::DownloadState::cancelled;
}

/// Tells whether a registry may be used: on, not set aside for a built-in
/// registry of its id, and allowed by Developer Mode.
///
/// @param registry the registry
/// @return true when it may be used
bool registry_usable(const content::RegistryView& registry) noexcept {
    return registry.registry.enabled && !registry.registry.conflicting &&
           registry.status != content::RegistryStatus::disabled &&
           registry.status != content::RegistryStatus::needs_developer_mode &&
           registry.status != content::RegistryStatus::conflicting;
}

/// Tells whether downloads from a registry are on: its install ID is not
/// off where the registry needs one.
///
/// @param values the preference values
/// @param registry the registry
/// @return true when they are on
bool registry_downloads_on(
    const std::map<std::string, std::string>& values, const content::RegistryView& registry
) {
    if (registry.registry.descriptor.install_id != oa::data::registry::InstallIdUse::required)
        return true;
    return content::read_install_id(values, registry.registry.descriptor.id).state !=
           content::InstallIdState::off;
}

/// Tells whether a registry's catalogue has been read.
///
/// A refresh that found nothing has been read too: it lists nothing.
///
/// @param registry the registry
/// @return true when a decision can treat the catalogue as known
bool catalogue_loaded(const content::RegistryView& registry) noexcept {
    return registry.catalogue != nullptr ||
           (registry.status == content::RegistryStatus::failed && !registry.refreshing);
}

/// Fills the registry half of an offer.
///
/// @param registry the registry
/// @param usable the registry may be used
/// @param downloads downloads from it are on
/// @param loaded its catalogue has been read
/// @return the offer, with nothing listed
mp::MutatorOffer
registry_offer(const content::RegistryView& registry, bool usable, bool downloads, bool loaded) {
    mp::MutatorOffer offer;
    offer.registry_known = true;
    offer.registry_usable = usable;
    offer.downloads_on = downloads;
    offer.catalogue_loaded = loaded;
    offer.registry_id = registry.registry.descriptor.id;
    offer.registry_name = registry.registry.descriptor.name;
    return offer;
}

/// Fills an offer from one catalogue entry.
///
/// @param registry the registry that lists it
/// @param package the entry
/// @param usable the registry may be used
/// @param downloads downloads from it are on
/// @return the offer
mp::MutatorOffer package_offer(
    const content::RegistryView& registry,
    const oa::data::catalogue::Package& package,
    bool usable,
    bool downloads
) {
    mp::MutatorOffer offer = registry_offer(registry, usable, downloads, true);
    offer.listed = true;
    offer.sha256 = hex_text(package.sha256);
    offer.name = package.name;
    offer.version = package.version;
    offer.release = package.release;
    offer.bytes = package.size;
    return offer;
}

/// Looks through the catalogues for one mutator of the host's list.
///
/// Registries are taken in the snapshot's order. A release whose SHA-256
/// starts with the prefix, from a registry that is on and whose downloads
/// are on, is the one a press can queue. Until a usable registry's catalogue
/// has been read, the offer waits. A registry that is off, or whose
/// downloads are off, is reported only when no fetchable release and no
/// unread catalogue remain. Another release of the id is reported as listed
/// and is never the one a press queues.
///
/// @param snapshot the catalogues in effect
/// @param values the preference values
/// @param downloads_folder the game has a downloads folder
/// @param id the mutator's id
/// @param prefix the host's SHA-256 prefix
/// @return the offer, and the catalogue key when the host's release can be queued
Look look_up(
    const content::Snapshot& snapshot,
    const std::map<std::string, std::string>& values,
    bool downloads_folder,
    std::string_view id,
    std::string_view prefix
) {
    Look fetchable;
    Look blocked_exact;
    Look other;
    Look waiting;
    Look blocked;
    Look opened;
    for (const content::RegistryView& registry : snapshot.registries) {
        if (registry.registry.conflicting)
            continue;
        const bool usable = registry_usable(registry);
        const bool downloads = downloads_folder && registry_downloads_on(values, registry);
        const bool fetch = usable && downloads;
        const bool loaded = catalogue_loaded(registry);
        if (!fetch && !blocked.offer.registry_known)
            blocked.offer = registry_offer(registry, usable, downloads, loaded);
        if (fetch && !loaded) {
            if (!waiting.offer.registry_known)
                waiting.offer = registry_offer(registry, true, true, false);
            continue;
        }
        if (registry.catalogue == nullptr)
            continue;
        if (fetch && !opened.offer.registry_known)
            opened.offer = registry_offer(registry, true, true, true);
        for (const oa::data::catalogue::Package& package : registry.catalogue->packages) {
            if (package.kind != oa::data::catalogue::Kind::oamut || !same_key(package.id, id))
                continue;
            const mp::MutatorOffer offer = package_offer(registry, package, usable, downloads);
            const bool exact = starts_with_prefix(offer.sha256, prefix);
            if (exact && fetch && !fetchable.fetchable_exact) {
                fetchable.offer = offer;
                fetchable.package_id = package.id;
                fetchable.fetchable_exact = true;
            } else if (exact && !fetch && !blocked_exact.offer.listed) {
                blocked_exact.offer = offer;
            } else if (!exact && fetch && !other.offer.listed) {
                other.offer = offer;
            }
        }
    }
    if (fetchable.fetchable_exact)
        return fetchable;
    if (waiting.offer.registry_known)
        return waiting;
    if (blocked_exact.offer.listed)
        return blocked_exact;
    if (other.offer.listed)
        return other;
    if (opened.offer.registry_known)
        return opened;
    if (blocked.offer.registry_known)
        return blocked;
    return {};
}

/// The installed release of an id, when the Mutators folder has one.
///
/// @param listing the installed mutators
/// @param id the mutator's id
/// @return the release; not installed when the id is absent
mp::LocalMutator local_of(const oa::data::mutator_library::Listing& listing, std::string_view id) {
    mp::LocalMutator local;
    const oa::data::mutator_library::InstalledMutator* found =
        oa::data::mutator_library::find(listing, id);
    if (found == nullptr)
        return local;
    local.installed = true;
    local.sha256 = lower_hex(found->package_sha256);
    local.version = oamut::version_text(found->manifest.version);
    return local;
}

/// Finds the download this machine is following for one mutator.
///
/// @param tracks the downloads being followed
/// @param host the mutator
/// @return the download; null when this press has not queued it
TrackedFetch* tracked(std::vector<TrackedFetch>& tracks, const mp::HostMutator& host) {
    for (TrackedFetch& one : tracks) {
        if (one.id == host.id && one.prefix == host.sha256_prefix)
            return &one;
    }
    return nullptr;
}

/// Finds or starts the download followed for one mutator of the host's list.
///
/// @param tracks the downloads being followed
/// @param host the mutator
/// @return the download
TrackedFetch& remember(std::vector<TrackedFetch>& tracks, const mp::HostMutator& host) {
    if (TrackedFetch* found = tracked(tracks, host))
        return *found;
    TrackedFetch made;
    made.id = host.id;
    made.prefix = host.sha256_prefix;
    tracks.push_back(std::move(made));
    return tracks.back();
}

/// Keeps a failure, in English, and says so in the log.
///
/// @param track the download
/// @param id the mutator's id, as the host named it
/// @param reason why, in English; empty becomes the queue's refusal
void keep_failure(TrackedFetch& track, std::string_view id, std::string_view reason) {
    track.item = 0;
    track.failure = std::string(reason);
    if (track.failure.empty())
        track.failure = content::failure_text(content::DownloadFailure::refused);
    log_line("couldn't get " + std::string(id) + ": " + track.failure);
}

/// Follows this machine's own downloads. A failure is kept until a press.
/// A cancelled download is forgotten, so the offer shows again and nothing
/// is queued until the next press. An install is logged once.
///
/// @param tracks the downloads being followed
/// @param items the download queue
void follow_tracks(
    std::vector<TrackedFetch>& tracks, const std::vector<content::DownloadView>& items
) {
    for (TrackedFetch& track : tracks) {
        if (track.item == 0)
            continue;
        const auto found =
            std::find_if(items.begin(), items.end(), [&track](const content::DownloadView& item) {
                return item.item == track.item;
            });
        if (found != items.end() && !ended(found->state))
            continue;
        if (found != items.end() && found->state == content::DownloadState::failed) {
            track.failure = content::failure_text(found->failure);
            if (track.failure.empty())
                track.failure = content::failure_text(content::DownloadFailure::refused);
            log_line("couldn't get " + track.id + ": " + track.failure);
        } else if (found != items.end() && found->state == content::DownloadState::done) {
            log_line("installed " + track.id);
        }
        track.item = 0;
    }
}

/// Tells whether a queued package is the host's release of this mutator and
/// has not ended.
///
/// @param item the queue item
/// @param host the mutator
/// @return true when it is that release, still running
bool running_release(const content::DownloadView& item, const mp::HostMutator& host) {
    if (ended(item.state) || item.target.kind != oa::data::catalogue::Kind::oamut)
        return false;
    if (!same_key(item.target.key, host.id))
        return false;
    return starts_with_prefix(hex_text(item.target.sha256), host.sha256_prefix);
}

/// Drops downloads for mutators the host's list no longer names.
///
/// @param tracks the downloads being followed
/// @param hosts the host's list
void keep_listed(std::vector<TrackedFetch>& tracks, const std::vector<mp::HostMutator>& hosts) {
    std::vector<TrackedFetch> still;
    still.reserve(tracks.size());
    for (const TrackedFetch& track : tracks) {
        const bool named =
            std::any_of(hosts.begin(), hosts.end(), [&track](const mp::HostMutator& host) {
                return host.id == track.id && host.sha256_prefix == track.prefix;
            });
        if (named)
            still.push_back(track);
    }
    tracks = std::move(still);
}

} // namespace

struct Runtime::HostMutatorFetchState {
    std::vector<TrackedFetch> tracks{}; ///< one followed download per mutator of the list
    /// The Mutators folder's last write when the listing was last read again;
    /// nothing before that, or while the folder cannot be read.
    std::optional<std::filesystem::file_time_type> listing_stamp{};
    /// The rows of this tick. Empty means the read ran and found nothing.
    /// Nothing means this tick has not read yet.
    std::optional<std::vector<Runtime::HostMutatorFetch>> shown{};
    std::size_t tick_reads = 0;   ///< reads since this tick began
    bool tick_pressed = false;    ///< fetch_host_mutators ran this tick
    std::size_t window_reads = 0; ///< reads since the last count was logged
    std::size_t window_ticks = 0; ///< ticks since the last count was logged
    std::size_t window_most = 0;  ///< the most reads of one tick without a press
    std::chrono::steady_clock::time_point window_start{}; ///< when the count began
};

void Runtime::destroy_host_mutator_fetch_state(HostMutatorFetchState* state) noexcept {
    delete state;
}

Runtime::HostMutatorFetchState& Runtime::ensure_host_mutator_fetches() {
    if (host_mutator_fetch_state_ == nullptr) {
        std::unique_ptr<HostMutatorFetchState, void (*)(HostMutatorFetchState*) noexcept> made{
            new HostMutatorFetchState, destroy_host_mutator_fetch_state
        };
        host_mutator_fetch_state_ = std::move(made);
    }
    return *host_mutator_fetch_state_;
}

void Runtime::drop_host_mutator_fetch_cache() {
    if (host_mutator_fetch_state_ == nullptr)
        return;
    HostMutatorFetchState& held = *host_mutator_fetch_state_;
    held.shown.reset();
    // The tick that ended joins the count. A press reads again by design,
    // so its tick does not set the most.
    held.window_reads += held.tick_reads;
    if (!held.tick_pressed)
        held.window_most = std::max(held.window_most, held.tick_reads);
    ++held.window_ticks;
    held.tick_reads = 0;
    held.tick_pressed = false;
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (held.window_reads == 0) {
        held.window_ticks = 0;
        held.window_start = now;
        return;
    }
    // One line a second while the rows are read, not one a tick.
    if (now - held.window_start < std::chrono::seconds(1))
        return;
    log_line(
        "read " + std::to_string(held.window_reads) + " times in " +
        std::to_string(held.window_ticks) + " ticks, at most " + std::to_string(held.window_most) +
        " in a tick"
    );
    held.window_reads = 0;
    held.window_ticks = 0;
    held.window_most = 0;
    held.window_start = now;
}

std::vector<Runtime::HostMutatorFetch> Runtime::host_mutators_fetch() {
    HostMutatorFetchState& held = ensure_host_mutator_fetches();
    if (held.shown)
        return *held.shown;

    std::vector<HostMutatorFetch> rows;
    netgame::PresenceLoadoutAssembly assembly;
    ++held.tick_reads;
    if (!mp::multiplayer_room_mutators(&assembly) || !netgame::presence_assembly_whole(assembly)) {
        held.shown = rows;
        return *held.shown;
    }

    // The battle room asks every frame. The installer reads the listing
    // again after each mutator it installs; a folder changed by hand is read
    // again when its last write moves, not on every call.
    std::error_code stamp_error;
    const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(
        user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name), stamp_error
    );
    if (stamp_error || !held.listing_stamp || *held.listing_stamp != stamp) {
        reload_installed_mutators();
        if (stamp_error)
            held.listing_stamp.reset();
        else
            held.listing_stamp = stamp;
    }
    const oa::data::mutator_library::Listing& listing = installed_mutators();
    content::Service& service = content_service();
    const std::shared_ptr<const content::Snapshot> snapshot = service.snapshot();
    const bool downloads_folder = !service.content_folder().empty();
    const std::vector<content::DownloadView> items = content_downloads().view();
    follow_tracks(held.tracks, items);

    std::vector<mp::HostMutator> hosts;
    rows.reserve(assembly.mutators.size());
    hosts.reserve(assembly.mutators.size());
    for (const netgame::PresenceMutator& item : assembly.mutators) {
        HostMutatorFetch row;
        row.mutator = mp::host_mutator_of(item);
        hosts.push_back(row.mutator);
        row.offer = snapshot == nullptr ? mp::MutatorOffer{}
                                        : look_up(
                                              *snapshot,
                                              preference_values_,
                                              downloads_folder,
                                              row.mutator.id,
                                              row.mutator.sha256_prefix
                                          )
                                              .offer;
        const mp::LocalMutator local = local_of(listing, row.mutator.id);
        const TrackedFetch* follow = tracked(held.tracks, row.mutator);
        const bool failed = follow != nullptr && !follow->failure.empty();
        bool downloading = false;
        if (follow != nullptr && follow->item != 0) {
            for (const content::DownloadView& queued : items) {
                if (queued.item != follow->item || ended(queued.state))
                    continue;
                downloading = true;
                row.done = queued.done;
                if (row.offer.bytes == 0)
                    row.offer.bytes = queued.target.size;
                break;
            }
        }
        if (!downloading) {
            for (const content::DownloadView& queued : items) {
                if (!running_release(queued, row.mutator))
                    continue;
                downloading = true;
                row.done = queued.done;
                if (row.offer.bytes == 0)
                    row.offer.bytes = queued.target.size;
                break;
            }
        }
        row.state = mp::mutator_fetch_state(row.mutator, local, row.offer, downloading, failed);
        if (row.state == mp::MutatorFetch::failed && follow != nullptr)
            row.failure = follow->failure;
        rows.push_back(std::move(row));
    }
    keep_listed(held.tracks, hosts);
    held.shown = rows;
    return *held.shown;
}

void Runtime::fetch_host_mutators() {
    HostMutatorFetchState& held = ensure_host_mutator_fetches();
    held.shown.reset();
    held.tick_pressed = true;
    for (TrackedFetch& track : held.tracks) {
        if (track.item == 0)
            track.failure.clear();
    }

    const std::vector<HostMutatorFetch> rows = host_mutators_fetch();
    content::Service& service = content_service();
    const std::shared_ptr<const content::Snapshot> snapshot = service.snapshot();
    const bool downloads_folder = !service.content_folder().empty();
    for (const HostMutatorFetch& row : rows) {
        if (row.state != mp::MutatorFetch::get && row.state != mp::MutatorFetch::other_release_get)
            continue;
        const Look found = snapshot == nullptr ? Look{}
                                               : look_up(
                                                     *snapshot,
                                                     preference_values_,
                                                     downloads_folder,
                                                     row.mutator.id,
                                                     row.mutator.sha256_prefix
                                                 );
        TrackedFetch& track = remember(held.tracks, row.mutator);
        // The catalogue's SHA-256 must start with the host's prefix. The
        // queue then checks the file against that catalogue SHA-256.
        if (!found.fetchable_exact || found.package_id.empty() ||
            !starts_with_prefix(found.offer.sha256, row.mutator.sha256_prefix)) {
            keep_failure(
                track, row.mutator.id, content::failure_text(content::DownloadFailure::not_offered)
            );
            continue;
        }
        std::string why;
        const std::optional<uint64_t> queued = queue_download(
            found.offer.registry_id,
            found.package_id,
            content::DownloadReason::offered_in_lobby,
            &why
        );
        if (!queued) {
            keep_failure(track, row.mutator.id, why);
            continue;
        }
        track.item = *queued;
        track.failure.clear();
        log_line(
            "downloading " + row.mutator.id + " release " + std::to_string(found.offer.release) +
            " from " + found.offer.registry_id
        );
    }
    // The rows just read were taken before these downloads were queued.
    held.shown.reset();
}

mp::MutatorOffer Runtime::mutator_offer(const mp::HostMutator& mutator) {
    content::Service& service = content_service();
    const std::shared_ptr<const content::Snapshot> snapshot = service.snapshot();
    if (snapshot == nullptr)
        return {};
    const bool downloads_folder = !service.content_folder().empty();
    return look_up(
               *snapshot, preference_values_, downloads_folder, mutator.id, mutator.sha256_prefix
    )
        .offer;
}

} // namespace oa::app
