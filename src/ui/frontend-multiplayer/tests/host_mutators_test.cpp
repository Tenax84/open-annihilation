// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Whether a guest can fetch one mutator of the host's list. Every state,
// over one hand-made mutator: the prefix is only the first 16 hex digits of
// a 64-digit SHA-256, another release of the id is never fetched, and a
// registry that is off or whose downloads are off is said so.
#include "oa/ui/frontend_multiplayer/host_mutators.hpp"

#include <array>
#include <cstdio>
#include <string>

namespace mp = oa::ui::frontend_multiplayer;
namespace ng = oa::netgame;
using mp::MutatorFetch;

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++failures;
    }
}

/// The host's prefix: the first eight bytes, sixteen digits of 2.
constexpr const char* kPrefix = "2222222222222222";
/// The host's release: that prefix, then 48 other digits.
constexpr const char* kHostSha = "2222222222222222333333333333333344444444444444445555555555555555";
/// Another release of the same id. The host's prefix sits in the middle, not at the start.
constexpr const char* kOtherSha =
    "1111111111111111222222222222222233333333333333334444444444444444";
/// The host's prefix with its last digit changed.
constexpr const char* kNearSha = "2222222222222223333333333333333344444444444444445555555555555555";

/// The host's mutator, made by hand.
mp::HostMutator host_mutator() {
    mp::HostMutator host;
    host.id = "brisk-build";
    host.version = "1.2.0";
    host.sha256_prefix = kPrefix;
    return host;
}

/// A registry that lists the host's release.
mp::MutatorOffer offering() {
    mp::MutatorOffer offer;
    offer.registry_known = true;
    offer.registry_usable = true;
    offer.downloads_on = true;
    offer.catalogue_loaded = true;
    offer.listed = true;
    offer.registry_id = "example-mods";
    offer.registry_name = "Example Mods";
    offer.sha256 = kHostSha;
    offer.name = "Brisk Build";
    offer.version = "1.2.0";
    offer.release = 4;
    offer.bytes = 18000;
    return offer;
}

/// Another release of the id, installed here.
mp::LocalMutator other_release() {
    mp::LocalMutator local;
    local.installed = true;
    local.sha256 = kOtherSha;
    local.version = "1.0.0";
    return local;
}

MutatorFetch state_of(
    const mp::LocalMutator& local,
    const mp::MutatorOffer& offer,
    bool downloading = false,
    bool failed = false
) {
    return mp::mutator_fetch_state(host_mutator(), local, offer, downloading, failed);
}

void the_prefix_from_the_record() {
    ng::PresenceMutator item;
    item.id = "brisk-build";
    item.version = "1.2.0";
    item.sha256_prefix.fill(0x22);
    item.sha256_prefix[0] = 0xab;
    item.sha256_prefix[1] = 0xcd;
    const mp::HostMutator host = mp::host_mutator_of(item);
    expect(
        host.id == "brisk-build" && host.version == "1.2.0" &&
            host.sha256_prefix == "abcd222222222222" &&
            host.sha256_prefix.size() == mp::host_mutator_prefix_digits,
        "the record's id, version and first eight bytes as 16 lower-case hex digits"
    );
}

void every_state() {
    const mp::LocalMutator missing{};
    const mp::MutatorOffer offer = offering();

    mp::LocalMutator same = other_release();
    same.sha256 = kHostSha;
    expect(state_of(same, offer) == MutatorFetch::have, "the host's prefix installed: have");
    expect(
        state_of(same, offer, true, true) == MutatorFetch::have,
        "the host's prefix installed: have, downloading or failed or not"
    );

    expect(
        state_of(missing, offer) == MutatorFetch::get,
        "not installed, the host's release offered: get"
    );
    expect(
        state_of(missing, offer, true) == MutatorFetch::downloading,
        "a download under way: downloading"
    );
    expect(
        state_of(missing, offer, true, true) == MutatorFetch::downloading,
        "downloading comes before failed"
    );
    expect(
        state_of(missing, offer, false, true) == MutatorFetch::failed, "the last try failed: failed"
    );

    mp::MutatorOffer unread = offer;
    unread.catalogue_loaded = false;
    unread.listed = false;
    expect(
        state_of(missing, unread) == MutatorFetch::waiting_for_catalogue,
        "a usable registry whose catalogue has not been read: waiting_for_catalogue"
    );

    expect(
        state_of(other_release(), offer) == MutatorFetch::other_release_get,
        "another release installed, the host's offered: other_release_get"
    );

    mp::MutatorOffer unlisted = offer;
    unlisted.listed = false;
    unlisted.sha256.clear();
    expect(state_of(missing, unlisted) == MutatorFetch::gone, "the catalogue lacks the id: gone");
    expect(
        state_of(other_release(), unlisted) == MutatorFetch::other_release_gone,
        "another release installed, the host's offered by nobody: other_release_gone"
    );

    mp::MutatorOffer unknown;
    expect(state_of(missing, unknown) == MutatorFetch::gone, "no registry: gone");
    expect(
        state_of(other_release(), unknown) == MutatorFetch::other_release_gone,
        "another release installed, no registry: other_release_gone"
    );
}

void the_prefix_is_only_the_first_sixteen_digits() {
    const mp::LocalMutator missing{};
    mp::MutatorOffer offer = offering();
    expect(
        state_of(missing, offer) == MutatorFetch::get,
        "the first 16 digits of a 64-digit SHA-256, the rest different: get"
    );

    mp::LocalMutator local;
    local.installed = true;
    local.sha256 = kHostSha;
    local.version = "9.9.9";
    expect(
        state_of(local, offer) == MutatorFetch::have,
        "an installed SHA-256 whose first 16 digits are the prefix: have"
    );

    offer.sha256 = kNearSha;
    expect(
        state_of(missing, offer) == MutatorFetch::gone,
        "a SHA-256 that differs in the 16th digit: gone, not get"
    );

    offer.sha256 = kOtherSha;
    expect(
        state_of(missing, offer) == MutatorFetch::gone,
        "the prefix later in a 64-digit SHA-256: gone, not get"
    );

    local.sha256 = kOtherSha;
    expect(
        state_of(local, offer) == MutatorFetch::other_release_gone,
        "an installed SHA-256 that carries the prefix later on: not have"
    );

    mp::HostMutator short_prefix = host_mutator();
    short_prefix.sha256_prefix = "222222222222222";
    mp::MutatorOffer full = offering();
    expect(
        mp::mutator_fetch_state(short_prefix, missing, full, false, false) == MutatorFetch::gone,
        "a prefix shorter than 16 digits matches nothing"
    );
}

void another_release_is_never_fetched() {
    const mp::LocalMutator missing{};
    mp::MutatorOffer offer = offering();
    offer.sha256 = kOtherSha;
    offer.version = "1.2.0";
    offer.release = 9;
    expect(
        state_of(missing, offer) == MutatorFetch::gone,
        "the id listed at another SHA-256: gone, never get"
    );
    expect(
        state_of(missing, offer, false, false) != MutatorFetch::get &&
            state_of(other_release(), offer) == MutatorFetch::other_release_gone,
        "another installed release, and an offer of a third: other_release_gone, never get"
    );
    expect(
        state_of(missing, offer) != MutatorFetch::other_release_get,
        "an offer of another SHA-256 is not other_release_get when nothing is installed"
    );
}

void a_registry_off_or_its_downloads() {
    const mp::LocalMutator missing{};
    mp::MutatorOffer off = offering();
    off.registry_usable = false;
    expect(
        state_of(missing, off) == MutatorFetch::downloads_off, "the registry is off: downloads_off"
    );

    mp::MutatorOffer downloads = offering();
    downloads.downloads_on = false;
    expect(
        state_of(missing, downloads) == MutatorFetch::downloads_off,
        "the registry's downloads are off: downloads_off"
    );

    off.catalogue_loaded = false;
    off.listed = false;
    expect(
        state_of(missing, off) == MutatorFetch::downloads_off,
        "a registry that is off comes before a catalogue that has not been read"
    );

    mp::MutatorOffer unknown;
    unknown.downloads_on = false;
    expect(
        state_of(missing, unknown) == MutatorFetch::gone,
        "no registry, downloads unset: gone, not downloads_off"
    );
}

} // namespace

int main() {
    the_prefix_from_the_record();
    every_state();
    the_prefix_is_only_the_first_sixteen_digits();
    another_release_is_never_fetched();
    a_registry_off_or_its_downloads();
    if (failures != 0)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
