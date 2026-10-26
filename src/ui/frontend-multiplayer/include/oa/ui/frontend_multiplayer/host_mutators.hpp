// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Whether a guest can fetch one mutator of the host's list. Everything here
// is pure: the application reads the catalogues and the download queue, and
// the battle room's own screen draws the button, its line and the bar.
#pragma once

#include "oa/netgame/presence.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace oa::ui::frontend_multiplayer {

/// Lower-case hex digits of a host mutator's package prefix: its first eight bytes.
inline constexpr std::size_t host_mutator_prefix_digits = 16;

/// One mutator of the host's list, as its presence record names it.
struct HostMutator {
    std::string id;            ///< the mutator's id
    std::string version;       ///< the mutator's version; may be empty
    std::string sha256_prefix; ///< the package's first eight bytes, 16 lower-case hex digits
};

/// Returns one mutator of a presence record as the fetch decision reads it.
///
/// @param item the mutator, decoded
/// @return its id, version and SHA-256 prefix as 16 lower-case hex digits
[[nodiscard]] HostMutator host_mutator_of(const netgame::PresenceMutator& item);

/// The release of a mutator this machine has installed, by its id.
struct LocalMutator {
    bool installed{};    ///< a mutator of the id is installed
    std::string sha256;  ///< its package SHA-256, 64 lower-case hex digits; empty when unknown
    std::string version; ///< its version; empty when the install names none
};

/// What a registry this machine uses offers for one mutator id.
///
/// The host's list names no registry. The offer is the one the catalogues
/// chose: the host's release where a registry lists it, otherwise another
/// release of the id, otherwise the registry that is still being read or
/// that will not download.
struct MutatorOffer {
    bool registry_known{};     ///< a registry is in effect
    bool registry_usable{};    ///< that registry is on and may be used
    bool downloads_on{};       ///< downloads from it are on, and the game has a downloads folder
    bool catalogue_loaded{};   ///< that registry's catalogue has been read
    bool listed{};             ///< the catalogue lists a mutator of the id
    std::string registry_id;   ///< the registry's id
    std::string registry_name; ///< the registry's name, as its descriptor gives it
    std::string sha256;        ///< the listed release's SHA-256, 64 lower-case hex digits
    std::string name;          ///< the listed mutator's name
    std::string version;       ///< the listed mutator's version; may be empty
    int64_t release{};         ///< the listed release
    uint64_t bytes{};          ///< the listed release's size, in bytes
};

/// Whether a guest can fetch one mutator of the host's list.
enum class MutatorFetch : uint8_t {
    have,                  ///< this machine has a package whose SHA-256 starts with the prefix
    get,                   ///< the host's release can be fetched
    other_release_get,     ///< another release is installed; the host's can be fetched
    other_release_gone,    ///< another release is installed; no registry offers the host's
    gone,                  ///< no registry this machine uses offers the host's release
    downloads_off,         ///< the registry is off, or downloads from it are
    waiting_for_catalogue, ///< a usable registry's catalogue has not been read yet
    downloading,           ///< the host's release is on its way
    failed,                ///< the last try failed
};

/// Decides whether a guest can fetch one mutator of the host's list.
///
/// In order: have when the installed package's SHA-256 starts with the
/// prefix; then downloading and failed as given; downloads_off when a
/// registry is known and it or its downloads are off; waiting_for_catalogue
/// while a usable registry's catalogue has not been read; get when the
/// offer's SHA-256 starts with the prefix, or other_release_get when another
/// release is installed; other_release_gone when another release is
/// installed and no offer starts with the prefix, otherwise gone. The prefix
/// is the first 16 lower-case hex digits and nothing after them.
///
/// @param host the mutator the host plays
/// @param local this machine's mutator of the id
/// @param offer what a registry offers
/// @param downloading a download of the host's release has not ended
/// @param failed the last download of it failed
/// @return the state
[[nodiscard]] MutatorFetch mutator_fetch_state(
    const HostMutator& host,
    const LocalMutator& local,
    const MutatorOffer& offer,
    bool downloading,
    bool failed
) noexcept;

} // namespace oa::ui::frontend_multiplayer
