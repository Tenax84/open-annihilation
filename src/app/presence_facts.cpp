// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What this machine is playing, read for the presence it tells other machines.
#include "oa/app/presence_facts.hpp"

#include "oa/app/runtime.hpp"

#include "engine_settings_state.hpp"

#include "oa/app/content/service.hpp"
#include "oa/app/package_install/origin.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/data/mod_profile/overrides.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace oa::app {

namespace {

namespace profiles = oa::data::mod_profile;

/// Returns the processor architecture this build is for, from the compiler's own names for it.
///
/// @return "x64", "x86", "arm64" or "armhf", else "unknown"
const char* build_architecture() noexcept {
#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__) || defined(__amd64__)
    return "x64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_ARM) || (defined(__arm__) && defined(__ARM_PCS_VFP))
    return "armhf";
#else
    return "unknown";
#endif
}

/// The mod's origin record as last read, and its registry's descriptor URL
/// as last looked up. The main thread reads presence facts; nothing else
/// touches it.
struct OriginReading {
    bool read = false;                               ///< a record has been looked for
    std::filesystem::path folder{};                  ///< the mod folder it was looked for in
    std::string mod_id{};                            ///< the mod's id then
    std::string mod_version{};                       ///< the mod's version then
    int64_t packaging_revision = 0;                  ///< the mod's packaging revision then
    std::optional<package_install::Origin> origin{}; ///< a catalogue origin with its SHA-256
    bool url_found = false;                          ///< the URL below was looked up
    const content::Service* service = nullptr;       ///< the service it was looked up in
    uint64_t generation = 0;                         ///< that service's snapshot generation then
    std::string url_registry{};                      ///< the registry id it was looked up for
    std::string url{}; ///< the descriptor URL; empty when there is none
};

/// Returns the reading kept between calls.
///
/// @return the reading
OriginReading& origin_reading() {
    static OriginReading reading;
    return reading;
}

/// Returns the descriptor URL of a registry in effect, looked up again only
/// when the content service's snapshot has moved on.
///
/// @param service the content service; null before it has started
/// @param[in,out] reading the reading kept between calls
/// @param registry the registry's id
/// @return the URL; empty for a built-in registry, one added from a file, one not in effect, or
///         without the service
std::string
registry_url(const content::Service* service, OriginReading& reading, const std::string& registry) {
    if (service == nullptr)
        return {};
    const auto generation = service->generation();
    if (reading.url_found && reading.service == service && reading.generation == generation &&
        reading.url_registry == registry)
        return reading.url;
    reading.url_found = true;
    reading.service = service;
    reading.generation = generation;
    reading.url_registry = registry;
    reading.url.clear();
    const auto snapshot = service->snapshot();
    for (const auto& view : snapshot->registries)
        if (view.registry.descriptor.id == registry) {
            reading.url = view.registry.url;
            break;
        }
    return reading.url;
}

/// Returns where the mod in play was published, reading its folder's origin
/// record again only when the folder or the mod has changed since.
///
/// @param service the content service; null before it has started
/// @param folder the mod's folder
/// @param mod the mod in play
/// @return the origin; none for a file origin, a folder without a record, or a record without its
///         SHA-256
std::optional<PresenceOrigin> mod_origin(
    const content::Service* service, const std::filesystem::path& folder, const PresenceMod& mod
) {
    auto& reading = origin_reading();
    if (!reading.read || reading.folder != folder || reading.mod_id != mod.id ||
        reading.mod_version != mod.version ||
        reading.packaging_revision != mod.packaging_revision) {
        reading.read = true;
        reading.folder = folder;
        reading.mod_id = mod.id;
        reading.mod_version = mod.version;
        reading.packaging_revision = mod.packaging_revision;
        reading.origin.reset();
        try {
            auto origin = package_install::read_origin(folder);
            if (origin && origin->kind == package_install::OriginKind::catalogue && origin->sha256)
                reading.origin = std::move(origin);
        } catch (const std::exception&) {
            // A record that cannot be read is no record.
        }
    }
    if (!reading.origin)
        return std::nullopt;
    PresenceOrigin origin{};
    origin.registry = reading.origin->registry;
    origin.catalogue_release = reading.origin->release;
    std::copy(
        reading.origin->sha256->begin(), reading.origin->sha256->end(), origin.sha256.begin()
    );
    origin.descriptor_url = registry_url(service, reading, origin.registry);
    return origin;
}

} // namespace

PresenceFacts presence_facts(const Runtime& runtime) {
    PresenceFacts facts{};
    const char* const platform = SDL_GetPlatform();
    facts.platform = platform != nullptr ? platform : "";
    facts.architecture = build_architecture();
    facts.developer_mode = runtime.developer_mode();
    const auto* profile = runtime.mod_profile();
    const bool mutators_folded = runtime.match_mutator_digest().has_value();
    facts.sim_hash = played_sim_hash(runtime);

    if (profile == nullptr) {
        facts.rules_differ_from_base = false;
    } else if (runtime.engine_settings_ == nullptr || !runtime.engine_settings_->layered) {
        facts.rules_differ_from_base = true;
    } else {
        facts.rules_differ_from_base = profile->sim_hash != runtime.engine_settings_->base_sim_hash;
    }
    if (mutators_folded)
        facts.rules_differ_from_base = true;

    if (profile != nullptr) {
        facts.mod =
            PresenceMod{profile->id, profile->name, profile->version, profile->packaging.revision};
        // The played mod's folder is the one this start layered. An expansion
        // bundle is not that folder, so it is not named as where the mod was
        // published. The content service has started with the runtime, so
        // asking for it starts nothing.
        const content::Service* service =
            runtime.content_ ? &const_cast<Runtime&>(runtime).content_service() : nullptr;
        if (!runtime.options_.layered_mod_folder.empty())
            facts.mod_origin = mod_origin(service, runtime.options_.layered_mod_folder, *facts.mod);
    }

    // Whether this machine can fetch content: a downloads folder, which the
    // content folder holds, and a registry it may download from. Asking for
    // the service starts nothing: it has started with the runtime when
    // content_ is set.
    if (runtime.content_) {
        const content::Service& service = const_cast<Runtime&>(runtime).content_service();
        const auto snapshot = service.snapshot();
        facts.fetches_content = !service.content_folder().empty() && snapshot != nullptr &&
                                std::any_of(
                                    snapshot->registries.begin(),
                                    snapshot->registries.end(),
                                    [&runtime](const content::RegistryView& view) {
                                        return runtime.registry_fetches(view);
                                    }
                                );
    }

    const auto* state = runtime.engine_settings_ ? runtime.engine_settings_.get() : nullptr;
    if (state != nullptr) {
        const auto hacks = profiles::standard_hacks();
        const auto count =
            hacks.size() < state->profile_hacks.size() ? hacks.size() : state->profile_hacks.size();
        const auto overrides =
            std::span<const profiles::HackOverride>{state->current.hack_overrides};
        for (std::size_t index = 0; index < count; ++index) {
            const auto* hack = hacks[index];
            bool on = state->profile_hacks[index].on;
            if (facts.developer_mode) {
                if (const auto* found = profiles::find_override(overrides, hack->id))
                    on = found->on;
            }
            if (!on)
                continue;
            if (hack->scope == profiles::registry::Scope::sim)
                facts.game_hacks.emplace_back(hack->id);
            else if (hack->scope == profiles::registry::Scope::view)
                facts.view_hacks.emplace_back(hack->id);
        }
        if (facts.developer_mode) {
            facts.overrides.reserve(state->current.hack_overrides.size());
            for (const auto& override : state->current.hack_overrides)
                facts.overrides.push_back(PresenceOverride{override.hack, override.on});
        }
    }
    return facts;
}

} // namespace oa::app
