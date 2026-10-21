// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Whether a guest can fetch one mutator of the host's list, from the prefix
// the host sent, the release installed here and what a registry offers.
#include "oa/ui/frontend_multiplayer/host_mutators.hpp"

#include <string>
#include <string_view>

namespace oa::ui::frontend_multiplayer {

namespace {

/// Tells whether a package SHA-256 starts with the host's prefix.
///
/// The prefix is 16 lower-case hex digits, the first eight bytes and no
/// more. A shorter prefix, or the same digits later in the SHA-256, does
/// not match.
///
/// @param sha256 the package SHA-256, lower-case hex
/// @param prefix the host's prefix
/// @return true when the SHA-256 starts with that prefix
bool starts_with_prefix(std::string_view sha256, std::string_view prefix) noexcept {
    return prefix.size() == host_mutator_prefix_digits && sha256.size() >= prefix.size() &&
           sha256.compare(0, prefix.size(), prefix) == 0;
}

/// Writes eight bytes as 16 lower-case hex digits.
///
/// @param bytes the prefix
/// @return the digits
std::string prefix_hex(const netgame::PresenceMutator& item) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text(host_mutator_prefix_digits, '0');
    for (std::size_t index = 0; index < item.sha256_prefix.size(); ++index) {
        const auto byte = item.sha256_prefix[index];
        text[index * 2] = digits[byte >> 4];
        text[index * 2 + 1] = digits[byte & 0x0f];
    }
    return text;
}

} // namespace

HostMutator host_mutator_of(const netgame::PresenceMutator& item) {
    HostMutator host;
    host.id = item.id;
    host.version = item.version;
    host.sha256_prefix = prefix_hex(item);
    return host;
}

MutatorFetch mutator_fetch_state(
    const HostMutator& host,
    const LocalMutator& local,
    const MutatorOffer& offer,
    bool downloading,
    bool failed
) noexcept {
    if (local.installed && starts_with_prefix(local.sha256, host.sha256_prefix))
        return MutatorFetch::have;
    if (downloading)
        return MutatorFetch::downloading;
    if (failed)
        return MutatorFetch::failed;
    if (offer.registry_known && (!offer.registry_usable || !offer.downloads_on))
        return MutatorFetch::downloads_off;
    if (offer.registry_known && offer.registry_usable && offer.downloads_on &&
        !offer.catalogue_loaded)
        return MutatorFetch::waiting_for_catalogue;
    if (offer.listed && starts_with_prefix(offer.sha256, host.sha256_prefix)) {
        return local.installed ? MutatorFetch::other_release_get : MutatorFetch::get;
    }
    return local.installed ? MutatorFetch::other_release_gone : MutatorFetch::gone;
}

} // namespace oa::ui::frontend_multiplayer
