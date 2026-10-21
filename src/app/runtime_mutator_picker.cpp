// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutator picker on the OA layer. open_mutator_picker pushes it, modal
// over the screen that asked, and hands that screen's set back on USE THESE
// once every mutator in it is installed. The list comes from the installed
// mutators, the catalogues' mutator entries, the download queue and the
// verdicts the feed gives for the mod being played. The runtime's own feed
// is installed until a check replaces it. Switching on an uninstalled
// mutator queues it and the prerequisites it still needs. A prerequisite
// that other On mutators need asks before it goes off.

#include "oa_layer.hpp"
#include "library_host.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/url.hpp"
#include "oa/platform/files.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/library/prerequisites.hpp"
#include "oa/ui/mutator_picker/screen.hpp"
#include "oa/ui/mutator_picker/text.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace kit = oa::ui::kit;
namespace picker = oa::ui::mutator_picker;
namespace library = oa::ui::library;

/// The modifier that holds Find: Cmd on macOS, Ctrl elsewhere.
#ifdef SDL_PLATFORM_MACOS
constexpr uint16_t kCommandModifier = SDL_KMOD_GUI;
#else
constexpr uint16_t kCommandModifier = SDL_KMOD_CTRL;
#endif

/// The setting a mutator uses when its manifest names no clash mode.
constexpr std::string_view kClashPreference = "mutators.clash-default";

/// Said when a download was refused and the queue gave no reason.
constexpr std::string_view kCouldNotQueue = "It could not be queued.";

/// A toggle's off value, written as the manifest writes one.
constexpr std::string_view kOffValue = "false";
/// A toggle's on value.
constexpr std::string_view kOnValue = "true";

/// Returns one of the interface's words in the language shown.
///
/// @param context unused
/// @param english the word, in English
/// @return its translation, or the English
std::string_view shown_word(void* /*context*/, std::string_view english) {
    return oa::data::languages::interface_text(english);
}

/// The hooks the picker and the catalogue look words up through.
///
/// @return the hooks
library::TextHooks text_hooks() {
    return library::TextHooks{nullptr, shown_word};
}

/// Tells whether two language tags name one language.
///
/// ASCII case is ignored, and '_' is read as '-'.
///
/// @param left a tag
/// @param right a tag
/// @return true when they name one language
bool same_tag(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        unsigned char first = static_cast<unsigned char>(left[index]);
        unsigned char second = static_cast<unsigned char>(right[index]);
        if (first == '_')
            first = '-';
        if (second == '_')
            second = '-';
        if (std::tolower(first) != std::tolower(second))
            return false;
    }
    return true;
}

/// Drops an id from a list.
///
/// @param[in,out] ids the list
/// @param id the id
void erase_id(std::vector<std::string>& ids, std::string_view id) {
    std::erase_if(ids, [&](const std::string& held) { return held == id; });
}

/// Tells whether a list holds an id.
///
/// @param ids the list
/// @param id the id
/// @return true when it does
bool holds_id(const std::vector<std::string>& ids, std::string_view id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

/// Returns a badge the picker draws from a decoded picture.
///
/// @param picture the decoded picture
/// @return the badge; nothing for an empty picture
std::optional<library::Badge> badge_of(const ModBadge& picture) {
    if (picture.width == 0 || picture.height == 0 || picture.pixels.empty())
        return std::nullopt;
    return library::Badge{picture.pixels, picture.width, picture.height};
}

/// Reads a mutator's icon and decodes it as a mod badge.
///
/// A file larger than max_mod_badge_bytes, an empty file, or one that cannot
/// be read gives no badge. Called only while the own feed builds its inputs.
///
/// @param path the icon file
/// @return the badge; empty when there is none
ModBadge read_limited_badge(const std::filesystem::path& path) {
    std::FILE* file = oa::platform::open_file(path, "rb");
    if (file == nullptr)
        return {};
    std::vector<uint8_t> bytes;
    std::array<uint8_t, 4096> chunk{};
    bool failed = false;
    while (!failed) {
        const std::size_t read = std::fread(chunk.data(), 1, chunk.size(), file);
        if (read > 0) {
            if (bytes.size() + read > max_mod_badge_bytes) {
                failed = true;
                break;
            }
            bytes.insert(
                bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(read)
            );
        }
        if (read < chunk.size()) {
            if (std::ferror(file) != 0)
                failed = true;
            break;
        }
    }
    std::fclose(file);
    if (failed || bytes.empty())
        return {};
    return read_mod_badge(bytes);
}

/// Returns where a queued download stands, as the Library's queue line says it.
///
/// @param state the queue's state
/// @return the phase; nothing for a cancelled item, which the picker leaves out
std::optional<library::QueueItem::Phase> phase_of(content::DownloadState state) noexcept {
    using Phase = library::QueueItem::Phase;
    switch (state) {
    case content::DownloadState::waiting:
    case content::DownloadState::asking:
    case content::DownloadState::challenge:
    case content::DownloadState::paused_for_match:
        return Phase::waiting;
    case content::DownloadState::downloading:
        return Phase::downloading;
    case content::DownloadState::verifying:
        return Phase::verifying;
    case content::DownloadState::handing_over:
    case content::DownloadState::installing:
        return Phase::installing;
    case content::DownloadState::failed:
        return Phase::failed;
    case content::DownloadState::done:
        return Phase::installed;
    case content::DownloadState::cancelled:
        return std::nullopt;
    }
    return std::nullopt;
}

/// Copies a mutator manifest's facts.
///
/// @param manifest the installed manifest
/// @return the facts
library::MutatorFacts facts_of(const formats::oamut::Manifest& manifest) {
    library::MutatorFacts facts;
    facts.tags = manifest.tags;
    for (const formats::oamut::Condition& condition : manifest.works_with) {
        library::WorksWith item;
        item.id = condition.id;
        item.range = condition.range_text;
        facts.works_with.push_back(std::move(item));
    }
    for (const formats::oamut::Condition& condition : manifest.requires_mutators) {
        library::Prerequisite need;
        need.id = condition.id;
        need.range = condition.range_text;
        facts.needs.push_back(std::move(need));
    }
    for (const formats::oamut::Condition& condition : manifest.conflicts_with)
        facts.conflicts.push_back(condition.id);
    for (const formats::oamut::Layer layer : manifest.layers)
        facts.layers.emplace_back(formats::oamut::layer_name(layer));
    for (const formats::oamut::Option& option : manifest.options) {
        library::OptionLine line;
        line.label = option.label;
        if (!option.value_labels.empty())
            line.values = option.value_labels;
        else
            for (const formats::oamut::ChoiceValue& value : option.values)
                line.values.push_back(value.text);
        facts.options.push_back(std::move(line));
    }
    return facts;
}

/// Copies a catalogue mutator's facts.
///
/// @param package the catalogue entry
/// @return the facts
library::MutatorFacts facts_of(const data::catalogue::Package& package) {
    library::MutatorFacts facts;
    facts.tags = package.tags;
    for (const std::string& written : package.works_with) {
        const std::optional<formats::oamut::Condition> condition =
            formats::oamut::parse_condition(written);
        if (!condition)
            continue;
        library::WorksWith item;
        item.id = condition->id;
        item.range = condition->range_text;
        facts.works_with.push_back(std::move(item));
    }
    for (const data::catalogue::MutatorRequirement& requirement : package.requires_mutators) {
        library::Prerequisite need;
        need.id = requirement.id;
        need.range = requirement.range_text;
        facts.needs.push_back(std::move(need));
    }
    for (const data::catalogue::MutatorRequirement& requirement : package.conflicts_with)
        facts.conflicts.push_back(requirement.id);
    for (const formats::oamut::Layer layer : package.layers)
        facts.layers.emplace_back(formats::oamut::layer_name(layer));
    for (const data::catalogue::OptionSummary& option : package.options) {
        library::OptionLine line;
        line.label = option.label;
        if (!option.value_labels.empty())
            line.values = option.value_labels;
        else
            for (const data::catalogue::ChoiceValueText& value : option.values)
                line.values.push_back(value.text);
        facts.options.push_back(std::move(line));
    }
    return facts;
}

/// The names a manifest's translations give, filled along the fallback chain.
struct ResolvedStrings {
    std::string name{};
    std::string description{};
    std::map<std::string, std::string> option_labels{};
    std::map<std::string, std::vector<std::string>> value_labels{};
};

/// Fills empty fields from one translation.
///
/// @param[in,out] into the fields filled so far
/// @param translation the translation
void absorb(ResolvedStrings& into, const formats::oamut::Translation& translation) {
    if (into.name.empty())
        into.name = translation.name;
    if (into.description.empty())
        into.description = translation.description;
    for (const formats::oamut::OptionLabel& option : translation.options) {
        if (!option.label.empty() && !into.option_labels.contains(option.id))
            into.option_labels.emplace(option.id, option.label);
    }
    for (const formats::oamut::ValueLabels& labels : translation.labels) {
        if (!labels.labels.empty() && !into.value_labels.contains(labels.id))
            into.value_labels.emplace(labels.id, labels.labels);
    }
}

/// Resolves a manifest's strings for the language the interface shows.
///
/// The fallback chain is walked preferred first. Empty fields stay empty
/// here; the caller fills them from the manifest's own strings.
///
/// @param translations the manifest's translations
/// @return the fields a translation gave
ResolvedStrings resolve_strings(const std::vector<formats::oamut::Translation>& translations) {
    ResolvedStrings resolved;
    const oa::data::languages::FallbackChain chain =
        oa::data::languages::fallback_chain(oa::data::languages::interface_language());
    for (const oa::data::languages::Language* language : chain.view()) {
        if (language == nullptr)
            continue;
        for (const formats::oamut::Translation& translation : translations)
            if (same_tag(translation.language, language->tag))
                absorb(resolved, translation);
    }
    return resolved;
}

/// Tells whether a listing's version meets every range.
///
/// @param listing the listing
/// @param ranges the ranges as written
/// @return true when it does; an empty range list is any version
bool listing_meets(const library::Listing& listing, const std::vector<std::string>& ranges) {
    for (const std::string& range_text : ranges) {
        if (range_text.empty())
            continue;
        const std::optional<formats::oamut::VersionRange> range =
            formats::oamut::parse_version_range(range_text);
        if (!range)
            return false;
        const std::optional<formats::oamut::Version> version =
            formats::oamut::parse_version(listing.version);
        if (!version || !formats::oamut::version_in_range(*range, *version))
            return false;
    }
    return true;
}

/// Tells whether `candidate` is a newer offer than `chosen`.
///
/// A higher release wins, then a higher version, then a lower registry id.
///
/// @param candidate the listing being considered
/// @param chosen the listing kept so far
/// @return true when `candidate` should replace `chosen`
bool newer_listing(const library::Listing& candidate, const library::Listing& chosen) {
    if (candidate.release != chosen.release)
        return candidate.release > chosen.release;
    const std::optional<formats::oamut::Version> candidate_version =
        formats::oamut::parse_version(candidate.version);
    const std::optional<formats::oamut::Version> chosen_version =
        formats::oamut::parse_version(chosen.version);
    if (candidate_version && chosen_version && *candidate_version != *chosen_version)
        return *candidate_version > *chosen_version;
    if (candidate_version && !chosen_version)
        return true;
    if (!candidate_version && chosen_version)
        return false;
    return candidate.registry < chosen.registry;
}

/// The listing a mutator row is taken from.
///
/// No parent registry is preferred: a registry that ships with the game
/// wins, else the lowest registry id. Within that set the newest release
/// wins. This is the rule get_plan uses for a prerequisite, with an empty
/// parent registry so the "own registry" pool is empty.
///
/// @param listings the catalogue listings
/// @param registries the registries, for which of them ship with the game
/// @param key the mutator's id
/// @return the listing, or null when none lists it
const library::Listing* choose_root_listing(
    const std::vector<library::Listing>& listings,
    const std::vector<library::Registry>& registries,
    std::string_view key
) {
    std::vector<const library::Listing*> meeting;
    for (const library::Listing& listing : listings)
        if (listing.kind == library::Kind::mutator && listing.key == key &&
            listing_meets(listing, {}))
            meeting.push_back(&listing);
    if (meeting.empty())
        return nullptr;
    std::vector<const library::Listing*> built_in;
    for (const library::Listing* listing : meeting) {
        for (const library::Registry& registry : registries)
            if (registry.id == listing->registry && registry.built_in)
                built_in.push_back(listing);
    }
    std::vector<const library::Listing*> pool;
    if (!built_in.empty()) {
        pool = std::move(built_in);
    } else {
        std::string lowest = meeting.front()->registry;
        for (const library::Listing* listing : meeting)
            if (listing->registry < lowest)
                lowest = listing->registry;
        for (const library::Listing* listing : meeting)
            if (listing->registry == lowest)
                pool.push_back(listing);
    }
    const library::Listing* chosen = pool.front();
    for (const library::Listing* listing : pool)
        if (listing != chosen && newer_listing(*listing, *chosen))
            chosen = listing;
    return chosen;
}

/// Joins names as a sentence lists them.
///
/// @param text how texts are looked up
/// @param names the names
/// @return the list; empty when there are none
std::string join_names(const library::TextHooks& text, const std::vector<std::string>& names) {
    if (names.empty())
        return {};
    if (names.size() == 1)
        return names.front();
    std::string joined = names.front();
    for (std::size_t index = 1; index + 1 < names.size(); ++index)
        joined = library::filled(
            text, library::words::names_comma, {{"left", joined}, {"right", names[index]}}
        );
    return library::filled(
        text, library::words::names_and, {{"left", joined}, {"right", names.back()}}
    );
}

/// The mods a mutator works with, in words.
///
/// `*` means any mod, and then the text stays empty so the details say so.
/// `ta-3.1c` uses the base game's name.
///
/// @param works the works-with list
/// @param played the mod being played; its name is used when the id matches
/// @param text how texts are looked up
/// @return the list; empty when it works with any mod
std::string works_with_names(
    const std::vector<library::WorksWith>& works,
    const library::PlayedMod& played,
    const library::TextHooks& text
) {
    std::vector<std::string> names;
    for (const library::WorksWith& item : works) {
        if (item.id == "*")
            return {};
        if (item.id == library::supported_base)
            names.emplace_back(library::shown(text, library::words::base_supported));
        else if (item.id == played.id && !played.name.empty())
            names.push_back(played.name);
        else if (!item.name.empty())
            names.push_back(item.name);
        else
            names.push_back(item.id);
    }
    return join_names(text, names);
}

/// Why a mutator cannot be switched on: the engine, else works-with.
///
/// The works-with sentence is the inner clause. The row wraps it.
///
/// @param requirement the engine requirement as written; empty for none
/// @param facts the mutator's facts
/// @param played the mod being played
/// @param text how texts are looked up
/// @return the reason; empty when it can be switched on
std::string unavailable_reason(
    std::string_view requirement,
    const library::MutatorFacts& facts,
    const library::PlayedMod& played,
    const library::TextHooks& text
) {
    if (!requirement.empty()) {
        const std::optional<formats::oamod::EngineRange> range =
            formats::oamod::parse_engine_range(requirement);
        const formats::oamod::EngineVersion engine = content::this_engine_rules().engine;
        if (!range || !formats::oamod::engine_range_met(*range, engine)) {
            const std::string shown_range =
                range ? formats::oamod::describe_engine_range(*range) : std::string(requirement);
            return library::filled(text, library::words::blocked_engine, {{"range", shown_range}});
        }
    }
    library::Inputs probe;
    probe.played = played;
    if (library::works_with_played(probe, facts))
        return {};
    const std::string names = works_with_names(facts.works_with, played, text);
    return library::filled(text, picker::words::works_with_mods, {{"names", names}});
}

/// Tells whether a mutator brings units or weapons.
///
/// @param layers the layer names
/// @return true when it does
bool brings_units(const std::vector<std::string>& layers) {
    const std::string_view units = formats::oamut::layer_name(formats::oamut::Layer::units);
    const std::string_view weapons = formats::oamut::layer_name(formats::oamut::Layer::weapons);
    for (const std::string& layer : layers)
        if (layer == units || layer == weapons)
            return true;
    return false;
}

/// The picker's clash mode for a manifest's.
///
/// @param mode the manifest's mode; empty when it names none
/// @return the picker's mode; empty when the manifest names none
std::optional<picker::Clash> clash_of(std::optional<formats::oamut::ClashMode> mode) {
    if (!mode)
        return std::nullopt;
    if (*mode == formats::oamut::ClashMode::add)
        return picker::Clash::add;
    return picker::Clash::replace;
}

/// A toggle's two values, off first, captioned in the language shown.
///
/// @param[out] spec the option
/// @param fallback `true` or `false`
void fill_toggle(picker::OptionSpec& spec, std::string fallback) {
    spec.type = picker::OptionSpec::Type::toggle;
    spec.values.emplace_back(kOffValue);
    spec.values.emplace_back(kOnValue);
    spec.captions.emplace_back(oa::data::languages::interface_text("Off"));
    spec.captions.emplace_back(oa::data::languages::interface_text("On"));
    spec.fallback = std::move(fallback);
}

/// Copies an installed manifest's options.
///
/// @param manifest the manifest
/// @param resolved the translated labels
/// @return the options, in the manifest's order
std::vector<picker::OptionSpec>
options_of(const formats::oamut::Manifest& manifest, const ResolvedStrings& resolved) {
    std::vector<picker::OptionSpec> options;
    for (const formats::oamut::Option& option : manifest.options) {
        picker::OptionSpec spec;
        spec.id = option.id;
        if (const auto label = resolved.option_labels.find(option.id);
            label != resolved.option_labels.end())
            spec.label = label->second;
        else
            spec.label = option.label;
        if (option.type == formats::oamut::OptionType::toggle) {
            fill_toggle(
                spec, option.toggle_default ? std::string(kOnValue) : std::string(kOffValue)
            );
        } else if (option.type == formats::oamut::OptionType::number) {
            spec.type = picker::OptionSpec::Type::number;
            spec.least = formats::oamod::canonical_number_text(option.minimum);
            spec.most = formats::oamod::canonical_number_text(option.maximum);
            spec.step = formats::oamod::canonical_number_text(option.step);
            spec.fallback = formats::oamod::canonical_number_text(option.number_default);
        } else {
            spec.type = picker::OptionSpec::Type::choice;
            for (const formats::oamut::ChoiceValue& value : option.values)
                spec.values.push_back(value.text);
            const auto translated = resolved.value_labels.find(option.id);
            if (translated != resolved.value_labels.end() &&
                translated->second.size() == spec.values.size())
                spec.captions = translated->second;
            else if (option.value_labels.size() == spec.values.size())
                spec.captions = option.value_labels;
            else
                spec.captions = spec.values;
            if (option.default_index < spec.values.size())
                spec.fallback = spec.values[option.default_index];
            else if (!spec.values.empty())
                spec.fallback = spec.values.front();
        }
        options.push_back(std::move(spec));
    }
    return options;
}

/// Copies a catalogue entry's options.
///
/// @param package the entry
/// @return the options, in the entry's order
std::vector<picker::OptionSpec> options_of(const data::catalogue::Package& package) {
    std::vector<picker::OptionSpec> options;
    for (const data::catalogue::OptionSummary& option : package.options) {
        picker::OptionSpec spec;
        spec.id = option.id;
        spec.label = option.label;
        if (option.type == formats::oamut::OptionType::toggle) {
            const bool on = option.default_text == kOnValue;
            fill_toggle(spec, on ? std::string(kOnValue) : std::string(kOffValue));
        } else if (option.type == formats::oamut::OptionType::number) {
            spec.type = picker::OptionSpec::Type::number;
            spec.least = option.minimum;
            spec.most = option.maximum;
            spec.step = option.step;
            spec.fallback = option.default_text;
        } else {
            spec.type = picker::OptionSpec::Type::choice;
            for (const data::catalogue::ChoiceValueText& value : option.values)
                spec.values.push_back(value.text);
            if (option.value_labels.size() == spec.values.size())
                spec.captions = option.value_labels;
            else
                spec.captions = spec.values;
            spec.fallback = option.default_text;
            if (spec.fallback.empty() && !spec.values.empty())
                spec.fallback = spec.values.front();
        }
        options.push_back(std::move(spec));
    }
    return options;
}

/// One package a GET queues, copied off the plan before anything else runs.
struct StepCopy {
    std::string registry{};
    std::string key{};
    std::string name{};
};

/// The mod being played, read once for a build of the list.
struct PlayedReading {
    bool playing{false};
    std::string id{};
    std::string name{};
    std::string version{};
    std::string folder_name{};
};

/// The name the header shows for the mod being played.
///
/// @param played the reading
/// @return the name, or No Mod when none plays
std::string mod_display(const PlayedReading& played) {
    const std::string none(oa::data::languages::interface_text(picker::words::no_mod));
    if (!played.playing)
        return none;
    if (!played.name.empty())
        return played.name;
    if (!played.folder_name.empty())
        return played.folder_name;
    return none;
}

/// The Library's played mod, from the reading.
///
/// @param played the reading
/// @return the mod; an empty id when none plays
library::PlayedMod played_mod(const PlayedReading& played) {
    library::PlayedMod mod;
    if (!played.playing)
        return mod;
    mod.id = played.id;
    mod.version = played.version;
    mod.name = played.name.empty() ? played.folder_name : played.name;
    return mod;
}

/// Finds a mutator in a list.
///
/// @param mutators the list
/// @param id the mutator's id
/// @return the mutator, or null
picker::Mutator* find_mutator(std::vector<picker::Mutator>& mutators, std::string_view id) {
    for (picker::Mutator& mutator : mutators)
        if (mutator.id == id)
            return &mutator;
    return nullptr;
}

/// Finds a mutator in a list.
///
/// @param mutators the list
/// @param id the mutator's id
/// @return the mutator, or null
const picker::Mutator*
find_mutator(const std::vector<picker::Mutator>& mutators, std::string_view id) {
    for (const picker::Mutator& mutator : mutators)
        if (mutator.id == id)
            return &mutator;
    return nullptr;
}

/// The registry of a snapshot that has an id and is not conflicting.
///
/// @param snapshot the snapshot
/// @param registry the registry's id
/// @return the registry; null when the snapshot has none of that id
const content::RegistryView*
registry_of(const content::Snapshot& snapshot, std::string_view registry) {
    for (const content::RegistryView& candidate : snapshot.registries)
        if (!candidate.registry.conflicting && candidate.registry.descriptor.id == registry)
            return &candidate;
    return nullptr;
}

/// The latest queued download of a mutator, when one has not finished.
///
/// @param downloads the queue, copied off view()
/// @param key the package key
/// @param name the name a player sees
/// @return the item; nothing when none is queued, or it installed or was cancelled
std::optional<library::QueueItem> queued_item(
    const std::vector<content::DownloadView>& downloads, std::string_view key, std::string name
) {
    const content::DownloadView* latest = nullptr;
    for (const content::DownloadView& item : downloads)
        if (item.target.key == key && item.target.kind == data::catalogue::Kind::oamut)
            latest = &item;
    if (latest == nullptr)
        return std::nullopt;
    const std::optional<library::QueueItem::Phase> phase = phase_of(latest->state);
    if (!phase || *phase == library::QueueItem::Phase::installed)
        return std::nullopt;
    library::QueueItem made;
    made.registry = latest->target.registry;
    made.kind = library::Kind::mutator;
    made.key = latest->target.key;
    made.name = std::move(name);
    made.phase = *phase;
    made.done_bytes = latest->done;
    made.total_bytes = latest->target.size;
    if (*phase == library::QueueItem::Phase::failed) {
        if (!latest->detail.empty())
            made.problem = latest->detail;
        else
            made.problem = content::failure_text(latest->failure);
    }
    return made;
}

} // namespace

/// The catalogue, pictures and downloads the own feed keeps while the picker is open.
struct Runtime::MutatorPickerState {
    /// The catalogue the own feed last built, so a GET can ask get_plan.
    /// Empty while a check's feed is installed.
    std::optional<library::Library> library{};
    /// Badge pictures. Pointers into this stay valid until it is cleared.
    std::deque<library::Badge> badges{};
    /// Which badge a mutator id already has, so an icon is not read twice.
    std::map<std::string, std::size_t> badge_index{};

    /// One download the picker queued.
    struct Tracked {
        std::string id{};
        std::string registry{};
        std::string key{};
        uint64_t queue_id{};
        bool synthetic{false};
        library::QueueItem::Phase phase{library::QueueItem::Phase::waiting};
    };

    std::vector<Tracked> tracked{};
    /// Why a mutator was refused, shown until a real download or an install replaces it.
    std::map<std::string, std::string> notes{};
};

void Runtime::destroy_mutator_picker_state(MutatorPickerState* state) noexcept {
    delete state;
}

namespace {

/// Keeps a badge for a mutator id. A second call returns the first picture.
///
/// @param[in,out] state the picker's state
/// @param id the mutator's id
/// @param badge the picture
/// @return the stored picture
template <class State>
const library::Badge* keep_badge(State& state, std::string_view id, library::Badge badge) {
    const auto found = state.badge_index.find(std::string(id));
    if (found != state.badge_index.end() && found->second < state.badges.size())
        return &state.badges[found->second];
    state.badges.push_back(std::move(badge));
    state.badge_index.emplace(std::string(id), state.badges.size() - 1);
    return &state.badges.back();
}

/// A badge already kept for an id.
///
/// @param state the picker's state
/// @param id the mutator's id
/// @return the picture; null when this opening has none yet
template <class State>
const library::Badge* cached_badge(const State& state, std::string_view id) {
    const auto found = state.badge_index.find(std::string(id));
    if (found == state.badge_index.end() || found->second >= state.badges.size())
        return nullptr;
    return &state.badges[found->second];
}

/// Builds one catalogue listing from a mutator entry.
///
/// A base other than the one the game plays, and every hack, is cleared so
/// a GET is not blocked for a mod's reason.
///
/// @param entry the snapshot entry
/// @param package the entry's package
/// @return the listing
library::Listing listing_of(const content::Entry& entry, const data::catalogue::Package& package) {
    library::Listing listing;
    listing.registry = entry.registry;
    listing.kind = library::Kind::mutator;
    listing.key = package.id;
    listing.name = package.name;
    listing.version = package.version;
    listing.revision = package.revision;
    listing.release = package.release;
    listing.publisher = package.publisher;
    listing.author = package.author;
    listing.summary = package.summary;
    listing.homepage = package.homepage;
    listing.tags = package.tags;
    listing.requires_engine = package.requires_engine_text;
    if (package.requires_base == library::supported_base)
        listing.base = package.requires_base;
    listing.size = package.size;
    listing.published = package.published;
    listing.mutator = facts_of(package);
    return listing;
}

/// The own feed's mutators, from the installed folders and the catalogues.
///
/// `read_mod_badge` and the content snapshot are read here and nowhere else
/// in this file. The screen fills the setup, the set, the mod's name and
/// the clash default itself.
///
/// @param[in,out] state the picker's state
/// @param pictures the Library's host, for catalogue pictures
/// @param snapshot the catalogues; null lists none
/// @param downloads the queue, copied off view()
/// @param mutators_folder the player's Mutators folder
/// @param played the mod being played
/// @return the mutators
template <class State, class Pictures>
picker::Inputs build_own_inputs(
    State& state,
    Pictures& pictures,
    const content::Snapshot* snapshot,
    const std::vector<content::DownloadView>& downloads,
    const std::filesystem::path& mutators_folder,
    const PlayedReading& played
) {
    const library::TextHooks text = text_hooks();
    const library::PlayedMod playing = played_mod(played);
    const data::mutator_library::Listing installed =
        data::mutator_library::list_installed(mutators_folder);

    library::Inputs library_inputs;
    library_inputs.engine = content::this_engine_rules().engine;
    library_inputs.played = playing;
    library_inputs.in_match = false;
    if (snapshot != nullptr) {
        for (const content::RegistryView& candidate : snapshot->registries) {
            if (!candidate.registry.enabled || candidate.registry.conflicting)
                continue;
            library::Registry registry;
            registry.id = candidate.registry.descriptor.id;
            registry.name = candidate.registry.descriptor.name;
            registry.built_in = candidate.registry.origin == data::registry::Origin::built_in;
            registry.downloads_on = true;
            registry.on = true;
            library_inputs.registries.push_back(std::move(registry));
        }
        for (const content::Entry& entry : snapshot->entries) {
            if (entry.package == nullptr || entry.package->kind != data::catalogue::Kind::oamut)
                continue;
            library_inputs.listings.push_back(listing_of(entry, *entry.package));
        }
    }
    for (const data::mutator_library::InstalledMutator& one : installed.mutators) {
        library::Installed shown;
        shown.kind = library::Kind::mutator;
        shown.key = one.manifest.id;
        shown.folder = one.folder;
        shown.name = one.manifest.name.empty() ? one.manifest.id : one.manifest.name;
        shown.version = formats::oamut::version_text(one.manifest.version);
        shown.registry = one.registry;
        shown.release = one.release;
        shown.author = one.manifest.author.name;
        shown.description = one.manifest.description;
        shown.mutator = facts_of(one.manifest);
        library_inputs.installed.push_back(std::move(shown));
    }

    if (!state.library)
        state.library.emplace();
    state.library->text = text;
    library::refresh(*state.library, std::move(library_inputs));
    state.library->text = text;

    for (auto& track : state.tracked) {
        const content::DownloadView* found = nullptr;
        for (const content::DownloadView& item : downloads)
            if (item.item == track.queue_id)
                found = &item;
        if (found == nullptr)
            continue;
        if (const std::optional<library::QueueItem::Phase> phase = phase_of(found->state))
            track.phase = *phase;
        track.synthetic = false;
    }

    std::vector<std::string> ids;
    auto add_id = [&](std::string id) {
        if (id.empty() || holds_id(ids, id))
            return;
        ids.push_back(std::move(id));
    };
    for (const data::mutator_library::InstalledMutator& one : installed.mutators)
        add_id(one.manifest.id);
    for (const library::Listing& listing : state.library->inputs.listings)
        add_id(listing.key);

    picker::Inputs inputs;
    inputs.text = text;
    for (const std::string& id : ids) {
        const data::mutator_library::InstalledMutator* installed_one = nullptr;
        for (const data::mutator_library::InstalledMutator& one : installed.mutators)
            if (one.manifest.id == id)
                installed_one = &one;
        const library::Listing* listing = choose_root_listing(
            state.library->inputs.listings, state.library->inputs.registries, id
        );
        const data::catalogue::Package* package = nullptr;
        std::string picture_url;
        if (snapshot != nullptr && listing != nullptr) {
            for (const content::Entry& entry : snapshot->entries) {
                if (entry.package == nullptr || entry.registry != listing->registry ||
                    entry.package->id != listing->key ||
                    entry.package->kind != data::catalogue::Kind::oamut)
                    continue;
                package = entry.package;
                if (!package->badge.empty()) {
                    if (const content::RegistryView* registry =
                            registry_of(*snapshot, entry.registry);
                        registry != nullptr && registry->served_from)
                        if (const std::optional<formats::url::Url> resolved =
                                formats::url::resolve_reference(
                                    *registry->served_from, package->badge
                                ))
                            picture_url = formats::url::url_text(*resolved);
                }
                break;
            }
        }

        const library::MutatorFacts facts = installed_one != nullptr
                                                ? facts_of(installed_one->manifest)
                                            : package != nullptr ? facts_of(*package)
                                                                 : library::MutatorFacts{};
        const ResolvedStrings resolved = installed_one != nullptr
                                             ? resolve_strings(installed_one->manifest.strings)
                                             : ResolvedStrings{};
        picker::Mutator mutator;
        mutator.id = id;
        if (installed_one != nullptr) {
            mutator.name = !resolved.name.empty() ? resolved.name : installed_one->manifest.name;
            mutator.description = !resolved.description.empty()
                                      ? resolved.description
                                      : installed_one->manifest.description;
            mutator.version = formats::oamut::version_text(installed_one->manifest.version);
            mutator.author = installed_one->manifest.author.name;
            mutator.options = options_of(installed_one->manifest, resolved);
            mutator.clashes = clash_of(installed_one->manifest.clashes);
            mutator.installed = true;
        } else if (package != nullptr) {
            mutator.name = package->name;
            mutator.description = package->summary;
            mutator.summary = package->summary;
            mutator.version = package->version;
            mutator.author = package->author;
            mutator.options = options_of(*package);
            mutator.clashes = clash_of(package->clashes);
        }
        if (mutator.name.empty())
            mutator.name = id;
        if (installed_one != nullptr && mutator.summary.empty())
            mutator.summary = mutator.description;
        mutator.tags = facts.tags;
        mutator.needs = facts.needs;
        mutator.conflicts = facts.conflicts;
        mutator.layers = facts.layers;
        mutator.brings = brings_units(mutator.layers);
        mutator.works_with_text = works_with_names(facts.works_with, playing, text);
        std::string_view requirement;
        if (installed_one != nullptr)
            requirement = installed_one->manifest.requires_engine;
        else if (package != nullptr)
            requirement = package->requires_engine_text;
        mutator.unavailable = unavailable_reason(requirement, facts, playing, text);
        if (listing != nullptr)
            mutator.listing =
                library::EntryId{listing->registry, library::Kind::mutator, listing->key};
        else if (
            installed_one != nullptr && !installed_one->registry.empty() &&
            !installed_one->catalogue_id.empty()
        )
            mutator.listing = library::EntryId{
                installed_one->registry, library::Kind::mutator, installed_one->catalogue_id
            };
        mutator.queued = queued_item(downloads, id, mutator.name);

        if (const library::Badge* badge = cached_badge(state, id)) {
            mutator.badge = badge;
        } else if (installed_one != nullptr && !installed_one->manifest.icon.empty()) {
            if (std::optional<library::Badge> decoded = badge_of(
                    read_limited_badge(installed_one->folder / installed_one->manifest.icon)
                ))
                mutator.badge = keep_badge(state, id, std::move(*decoded));
        }
        if (mutator.badge == nullptr && !picture_url.empty() && listing != nullptr) {
            pictures.ask_cached_picture(listing->registry, picture_url);
            if (std::optional<library::Badge> decoded =
                    pictures.cached_picture(listing->registry, picture_url))
                mutator.badge = keep_badge(state, id, std::move(*decoded));
        }
        inputs.mutators.push_back(std::move(mutator));
    }

    std::map<std::string, std::string> names;
    for (const picker::Mutator& mutator : inputs.mutators)
        names.emplace(mutator.id, mutator.name);
    for (picker::Mutator& mutator : inputs.mutators) {
        for (library::Prerequisite& need : mutator.needs) {
            if (!need.name.empty())
                continue;
            if (const auto found = names.find(need.id); found != names.end())
                need.name = found->second;
        }
    }
    return inputs;
}

} // namespace

picker::Inputs Runtime::own_picker_inputs(void* context, picker::Setup /*setup*/) {
    Runtime& runtime = *static_cast<Runtime*>(context);
    if (runtime.mutator_picker_ == nullptr)
        runtime.mutator_picker_.reset(new MutatorPickerState{});
    PlayedReading played;
    played.playing = runtime.plays_mod();
    if (played.playing) {
        if (const oa::data::mod_profile::ModProfile* profile = runtime.mod_profile()) {
            played.id = profile->id;
            played.name = profile->name;
            played.version = profile->version;
        }
        played.folder_name = path_to_utf8(runtime.played_mod_folder().filename());
    }
    const std::shared_ptr<const content::Snapshot> snapshot = runtime.content_service().snapshot();
    const std::vector<content::DownloadView> downloads = runtime.content_downloads().view();
    const std::filesystem::path folder =
        runtime.user_folder() / std::string(data::mutator_library::mutators_folder_name);
    return build_own_inputs(
        *runtime.mutator_picker_, runtime.library_host(), snapshot.get(), downloads, folder, played
    );
}

uint64_t Runtime::own_picker_generation(void* context) {
    Runtime& runtime = *static_cast<Runtime*>(context);
    uint64_t generation = runtime.content_service().generation();
    generation += runtime.content_downloads().generation();
    if (runtime.library_ != nullptr && runtime.library_->work.pictures != nullptr)
        generation += runtime.library_->work.pictures->generation();
    return generation;
}

std::optional<uint64_t> Runtime::own_picker_enqueue(
    void* context, std::string_view registry, std::string_view key, std::string* why
) {
    return static_cast<Runtime*>(context)->queue_download(
        registry, key, content::DownloadReason::install, why
    );
}

void Runtime::own_picker_cancel(void* context, std::string_view registry, std::string_view key) {
    Runtime& runtime = *static_cast<Runtime*>(context);
    const std::vector<content::DownloadView> downloads = runtime.content_downloads().view();
    std::optional<uint64_t> waiting;
    for (const content::DownloadView& item : downloads)
        if (item.target.registry == registry && item.target.key == key &&
            item.state == content::DownloadState::waiting)
            waiting = item.item;
    if (waiting)
        runtime.content_downloads().cancel(*waiting);
}

/// The picker screen. Defined here so the question's host can find it by name.
struct Runtime::MutatorPickerScreen final : LayerScreen {
    /// Makes the screen. start fills it and is called before the layer shows it.
    ///
    /// @param runtime the runtime
    /// @param request the setup, the set and what it hands back
    MutatorPickerScreen(Runtime& runtime, MutatorPickerRequest request)
        : runtime_(runtime), request_(std::move(request)) {}

    /// Fills the picker from the feed and asks for a check of each On mutator
    /// that has not been checked. False when the feed gives no list.
    ///
    /// @return false when the picker cannot be filled
    bool start();

    /// Switches off the prerequisite the question names, and what needs it.
    void confirm_asked();

    /// Leaves the On list as it was.
    void cancel_asked();

    /// Drops badge pointers the rows hold, before the pictures are freed.
    void drop_badges();

    [[nodiscard]] std::string_view name() const override { return picker::screen_name; }

    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        if (placed_on.match || runtime_.match_ != nullptr || runtime_.screen_ == Screen::match ||
            runtime_.screen_ != opened_over_)
            return {};
        const kit::Point size = picker::window_size(placed_on.frame);
        return centred_placement(placed_on, size.x, size.y);
    }

    [[nodiscard]] bool modal() const override { return true; }

    [[nodiscard]] bool backdrop() const override { return true; }

    void lay_out(const LayerView& placed_on) override {
        if (laid_out_ && placed_on.frame.area.width == frame_.area.width &&
            placed_on.frame.area.height == frame_.area.height &&
            placed_on.frame.size_class == frame_.size_class &&
            placed_on.frame.scale_percent == frame_.scale_percent)
            return;
        frame_ = placed_on.frame;
        laid_out_ = true;
        relayout();
    }

    void draw(const kit::Canvas& canvas) const override {
        picker::draw_picker(canvas, list_, picker::window_size(frame_));
    }

    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        picker::PointerKind moved = picker::PointerKind::move;
        if (kind == ScreenInputKind::pointer_down)
            moved = picker::PointerKind::down;
        else if (kind == ScreenInputKind::pointer_up)
            moved = picker::PointerKind::up;
        else if (kind != ScreenInputKind::pointer_move)
            return LayerAnswer::none;
        return take(
            picker::picker_pointer(picker_, state_, list_, moved, button, at, finger_reach)
        );
    }

    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        return take(picker::picker_key(picker_, state_, list_, pressed));
    }

    LayerAnswer shortcut(uint32_t sdl_key, uint16_t modifiers) override {
        std::optional<picker::ScreenCommand> command;
        if (sdl_key == SDLK_F && (modifiers & kCommandModifier) != 0)
            command = picker::ScreenCommand::find;
        else if (
            sdl_key == SDLK_SLASH && (modifiers & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) == 0 &&
            !picker::focused_field(state_, list_)
        )
            command = picker::ScreenCommand::find;
        if (!command)
            return LayerAnswer::pass;
        return take(picker::picker_command(picker_, state_, list_, *command));
    }

    LayerAnswer pad_action(oa::ui::pad_controls::Action action) override {
        using oa::ui::pad_controls::Action;
        std::optional<picker::ScreenCommand> command;
        if (action == Action::library_search)
            command = picker::ScreenCommand::find;
        else if (action == Action::back)
            command = picker::ScreenCommand::back;
        else if (action == Action::library_action)
            command = picker::ScreenCommand::flip;
        else if (action == Action::default_button)
            command = picker::ScreenCommand::use;
        else
            return LayerAnswer::pass;
        return take(picker::picker_command(picker_, state_, list_, *command));
    }

    LayerAnswer pad_stick(float notches) override {
        return take(picker::picker_stick(picker_, state_, list_, notches));
    }

    LayerAnswer wheel(kit::Point at, float notches) override {
        return take(picker::picker_wheel(picker_, state_, list_, at, notches));
    }

    LayerAnswer text(std::string_view utf8) override {
        return take(picker::picker_text(picker_, state_, list_, utf8));
    }

    [[nodiscard]] std::optional<kit::Rect> text_field() const override {
        return picker::focused_field(state_, list_);
    }

    [[nodiscard]] kit::DisplayList display_list() const override { return list_; }

    [[nodiscard]] kit::Interaction interaction() const override { return state_.interaction; }

    LayerAnswer tick() override {
        if (runtime_.screen_ != opened_over_ || runtime_.match_ != nullptr ||
            runtime_.screen_ == Screen::match)
            return LayerAnswer::close;
        const MutatorPickerFeed& feed = runtime_.mutator_picker_feed_;
        uint64_t generation = seen_generation_;
        if (feed.generation != nullptr)
            generation = feed.generation(feed.context);
        bool changed = false;
        if (generation != seen_generation_) {
            seen_generation_ = generation;
            reload();
            changed = true;
        }
        if (update_waiting())
            changed = true;
        if (!changed)
            return LayerAnswer::none;
        relayout();
        return LayerAnswer::redraw;
    }

    [[nodiscard]] uint64_t revision() const override { return revision_; }

    void close(bool /*by_key*/) override;

  private:

    /// How the picker is leaving. A close that is not a hand cancels.
    enum class Ending : uint8_t { cancel, hand };

    /// Reads the feed and opens or refreshes the model.
    ///
    /// @param opening true the first time, which opens rather than refreshes
    /// @return false when the feed gives no list
    bool load_inputs(bool opening);

    /// Reads the feed again, keeping the On list.
    void reload();

    /// Asks for a check of each On mutator whose verdict has not arrived.
    void ask_open_checks();

    /// Asks for a check after an option or a clash mode changed.
    ///
    /// @param id the mutator's id
    void force_check(std::string id);

    /// Puts a checking verdict back while the feed's has not arrived.
    ///
    /// @param[in,out] inputs the inputs about to be opened or refreshed
    void reapply_checking(picker::Inputs& inputs);

    /// Shows a refused mutator's reason as a failed download.
    ///
    /// @param[in,out] inputs the inputs about to be opened or refreshed
    void apply_notes(picker::Inputs& inputs);

    /// Shows a download queued before the feed's generation moved.
    ///
    /// @param[in,out] inputs the inputs about to be opened or refreshed
    void apply_synthetics(picker::Inputs& inputs);

    /// Drops notes and tracked downloads for mutators that are now installed.
    void forget_installed();

    /// Forgets refusal notes. A new action can refuse again.
    void clear_notes();

    /// Queues the mutators a switch-on asked for.
    ///
    /// @param ids the mutators, prerequisites first
    void enqueue_ids(const std::vector<std::string>& ids);

    /// Queues one mutator and the prerequisites it still needs.
    ///
    /// @param id the mutator's id
    /// @param[in,out] seen registry and key pairs already queued for this action
    /// @return false when it was refused
    bool enqueue_one(std::string_view id, std::set<std::pair<std::string, std::string>>& seen);

    /// Switches a mutator off and shows why it was not queued.
    ///
    /// @param id the mutator's id
    /// @param reason the reason
    void refuse(std::string_view id, std::string reason);

    /// Remembers a queued package and shows it on the row at once.
    ///
    /// @param step the package
    /// @param queue_id the queue item
    void remember_queue(const StepCopy& step, uint64_t queue_id);

    /// Cancels waiting downloads for mutators that left the On list.
    void settle_left();

    /// Writes the footer's waiting line from the On mutators' downloads.
    ///
    /// @return true when the line changed
    bool update_waiting();

    /// The waiting line, or empty when nothing On is still downloading or failed.
    ///
    /// @return the line
    [[nodiscard]] std::string waiting_text() const;

    /// USE THESE: hands the set when it can start, else queues what is missing.
    ///
    /// @return close when the set was handed; redraw when it is still waiting
    LayerAnswer handle_use();

    /// Asks, over the picker, before a prerequisite is switched off.
    void ask_question();

    /// Carries out what one input asked, then lays the picker out again.
    ///
    /// @param result what the input asked
    /// @return what the layer should do
    LayerAnswer take(picker::ScreenResult result);

    /// Lays the picker out and counts the change.
    void relayout();

    /// Tells whether an id is on.
    ///
    /// @param id the mutator's id
    /// @return true when it is
    [[nodiscard]] bool is_on(std::string_view id) const;

    /// The On mutator's chosen values.
    ///
    /// @param id the mutator's id
    /// @return the values; null when it is off
    [[nodiscard]] const picker::Chosen* chosen_of(std::string_view id) const;

    /// The clash mode a mutator uses when its manifest names none.
    ///
    /// @return the mode
    [[nodiscard]] picker::Clash clash_default() const;

    Runtime& runtime_;
    MutatorPickerRequest request_;
    Screen opened_over_{};
    picker::Picker picker_{};
    picker::ScreenState state_{};
    kit::DisplayList list_{};
    kit::Frame frame_{};
    bool laid_out_{false};
    uint64_t revision_{};
    uint64_t seen_generation_{};
    bool answered_{false};
    Ending ending_{Ending::cancel};
    std::vector<picker::Chosen> handed_set_{};
    /// On mutators asked as the picker opened, while the verdict is still unchecked.
    std::vector<std::string> pending_{};
    /// An option change asked again, until a later list brings a resolved verdict.
    std::vector<std::string> forced_{};
};

bool Runtime::MutatorPickerScreen::start() {
    opened_over_ = runtime_.screen_;
    if (runtime_.mutator_picker_ == nullptr)
        runtime_.mutator_picker_.reset(new MutatorPickerState{});
    if (!load_inputs(true))
        return false;
    ask_open_checks();
    const MutatorPickerFeed& feed = runtime_.mutator_picker_feed_;
    if (feed.generation != nullptr)
        seen_generation_ = feed.generation(feed.context);
    frame_ = runtime_.oa_layer().view().frame;
    laid_out_ = true;
    relayout();
    return true;
}

void Runtime::MutatorPickerScreen::confirm_asked() {
    picker::confirm_turn_off(picker_);
    settle_left();
    update_waiting();
    relayout();
}

void Runtime::MutatorPickerScreen::cancel_asked() {
    picker::cancel_turn_off(picker_);
    relayout();
}

void Runtime::MutatorPickerScreen::drop_badges() {
    for (picker::Mutator& mutator : picker_.inputs.mutators)
        mutator.badge = nullptr;
    if (runtime_.mutator_picker_ != nullptr) {
        runtime_.mutator_picker_->badges.clear();
        runtime_.mutator_picker_->badge_index.clear();
    }
}

void Runtime::MutatorPickerScreen::close(bool /*by_key*/) {
    if (answered_)
        return;
    answered_ = true;
    drop_badges();
    std::function<void(std::vector<picker::Chosen>)> used = std::move(request_.used);
    std::function<void()> cancelled = std::move(request_.cancelled);
    std::vector<picker::Chosen> handed = std::move(handed_set_);
    const bool hand = ending_ == Ending::hand;
    runtime_.mutator_picker_.reset();
    if (hand) {
        if (used)
            used(std::move(handed));
    } else if (cancelled) {
        cancelled();
    }
}

bool Runtime::MutatorPickerScreen::load_inputs(bool opening) {
    const MutatorPickerFeed& feed = runtime_.mutator_picker_feed_;
    if (feed.inputs == nullptr)
        return false;
    picker::Inputs inputs = feed.inputs(feed.context, request_.setup);
    inputs.setup = request_.setup;
    inputs.read_only = request_.read_only;
    inputs.last_set = request_.set;
    inputs.remembered = false;
    inputs.text = text_hooks();
    PlayedReading played;
    played.playing = runtime_.plays_mod();
    if (played.playing) {
        if (const oa::data::mod_profile::ModProfile* profile = runtime_.mod_profile()) {
            played.id = profile->id;
            played.name = profile->name;
            played.version = profile->version;
        }
        played.folder_name = path_to_utf8(runtime_.played_mod_folder().filename());
    }
    inputs.mod_name = mod_display(played);
    inputs.clash_default = clash_default();
    apply_notes(inputs);
    apply_synthetics(inputs);
    reapply_checking(inputs);
    if (opening)
        picker::open(picker_, std::move(inputs));
    else
        picker::refresh(picker_, std::move(inputs));
    forget_installed();
    return true;
}

void Runtime::MutatorPickerScreen::reload() {
    load_inputs(false);
}

void Runtime::MutatorPickerScreen::ask_open_checks() {
    const MutatorPickerFeed& feed = runtime_.mutator_picker_feed_;
    if (feed.check == nullptr)
        return;
    const std::vector<picker::Chosen> on = picker_.on;
    for (const picker::Chosen& chosen : on) {
        picker::Mutator* mutator = find_mutator(picker_.inputs.mutators, chosen.id);
        if (mutator == nullptr || mutator->verdict.state != library::Verdict::State::unchecked)
            continue;
        mutator->verdict.state = library::Verdict::State::checking;
        if (!holds_id(pending_, chosen.id))
            pending_.push_back(chosen.id);
        feed.check(feed.context, chosen.id, chosen);
    }
}

void Runtime::MutatorPickerScreen::force_check(std::string id) {
    const MutatorPickerFeed& feed = runtime_.mutator_picker_feed_;
    if (feed.check == nullptr)
        return;
    const picker::Chosen* chosen = chosen_of(id);
    if (chosen == nullptr)
        return;
    const picker::Chosen copy = *chosen;
    if (!holds_id(forced_, id))
        forced_.push_back(std::move(id));
    if (picker::Mutator* mutator = find_mutator(picker_.inputs.mutators, copy.id))
        mutator->verdict.state = library::Verdict::State::checking;
    feed.check(feed.context, copy.id, copy);
}

void Runtime::MutatorPickerScreen::reapply_checking(picker::Inputs& inputs) {
    if (runtime_.mutator_picker_feed_.check == nullptr) {
        pending_.clear();
        forced_.clear();
        return;
    }
    for (picker::Mutator& mutator : inputs.mutators) {
        const bool forced = holds_id(forced_, mutator.id);
        const bool pending = holds_id(pending_, mutator.id);
        if (mutator.verdict.state != library::Verdict::State::unchecked) {
            if (forced)
                erase_id(forced_, mutator.id);
            if (pending)
                erase_id(pending_, mutator.id);
            continue;
        }
        if (forced || pending)
            mutator.verdict.state = library::Verdict::State::checking;
    }
}

void Runtime::MutatorPickerScreen::apply_notes(picker::Inputs& inputs) {
    MutatorPickerState* state = runtime_.mutator_picker_.get();
    if (state == nullptr)
        return;
    for (picker::Mutator& mutator : inputs.mutators) {
        const auto found = state->notes.find(mutator.id);
        if (found == state->notes.end())
            continue;
        if (mutator.installed || mutator.queued) {
            state->notes.erase(found);
            continue;
        }
        library::QueueItem failed;
        failed.registry = mutator.listing.registry;
        failed.kind = library::Kind::mutator;
        failed.key = mutator.id;
        failed.name = mutator.name.empty() ? mutator.id : mutator.name;
        failed.phase = library::QueueItem::Phase::failed;
        failed.problem = found->second;
        mutator.queued = std::move(failed);
    }
}

void Runtime::MutatorPickerScreen::apply_synthetics(picker::Inputs& inputs) {
    MutatorPickerState* state = runtime_.mutator_picker_.get();
    if (state == nullptr)
        return;
    for (MutatorPickerState::Tracked& track : state->tracked) {
        if (!track.synthetic)
            continue;
        picker::Mutator* mutator = find_mutator(inputs.mutators, track.id);
        if (mutator == nullptr)
            continue;
        if (mutator->installed || mutator->queued) {
            track.synthetic = false;
            continue;
        }
        library::QueueItem waiting;
        waiting.registry = track.registry;
        waiting.kind = library::Kind::mutator;
        waiting.key = track.key;
        waiting.name = mutator->name.empty() ? mutator->id : mutator->name;
        waiting.phase = library::QueueItem::Phase::waiting;
        mutator->queued = std::move(waiting);
    }
}

void Runtime::MutatorPickerScreen::forget_installed() {
    MutatorPickerState* state = runtime_.mutator_picker_.get();
    if (state == nullptr)
        return;
    std::erase_if(state->notes, [&](const std::pair<const std::string, std::string>& note) {
        const picker::Mutator* mutator = find_mutator(picker_.inputs.mutators, note.first);
        return mutator != nullptr && mutator->installed;
    });
    std::erase_if(state->tracked, [&](const MutatorPickerState::Tracked& track) {
        const picker::Mutator* mutator = find_mutator(picker_.inputs.mutators, track.id);
        return mutator != nullptr && mutator->installed;
    });
}

void Runtime::MutatorPickerScreen::clear_notes() {
    MutatorPickerState* state = runtime_.mutator_picker_.get();
    if (state == nullptr)
        return;
    for (picker::Mutator& mutator : picker_.inputs.mutators) {
        if (!state->notes.contains(mutator.id))
            continue;
        if (mutator.queued && mutator.queued->phase == library::QueueItem::Phase::failed)
            mutator.queued.reset();
    }
    state->notes.clear();
}

void Runtime::MutatorPickerScreen::enqueue_ids(const std::vector<std::string>& ids) {
    std::set<std::pair<std::string, std::string>> seen;
    if (runtime_.mutator_picker_ != nullptr) {
        for (const MutatorPickerState::Tracked& track : runtime_.mutator_picker_->tracked)
            if (track.phase != library::QueueItem::Phase::failed &&
                track.phase != library::QueueItem::Phase::installed)
                seen.emplace(track.registry, track.key);
    }
    for (const std::string& id : ids) {
        if (!is_on(id))
            continue;
        enqueue_one(id, seen);
    }
    settle_left();
}

bool Runtime::MutatorPickerScreen::enqueue_one(
    std::string_view id, std::set<std::pair<std::string, std::string>>& seen
) {
    const picker::Mutator* mutator = find_mutator(picker_.inputs.mutators, id);
    if (mutator == nullptr || mutator->installed)
        return true;
    const std::string listing_registry = mutator->listing.registry;
    const std::string listing_key = mutator->listing.key;
    const std::string listing_name = mutator->name.empty() ? std::string(id) : mutator->name;
    const library::EntryId listing = mutator->listing;

    std::vector<StepCopy> steps;
    std::string blocked;
    if (runtime_.mutator_picker_ != nullptr && runtime_.mutator_picker_->library) {
        const library::Library& catalogue = *runtime_.mutator_picker_->library;
        const library::Entry* entry = nullptr;
        if (!listing.key.empty())
            entry = library::find_entry(catalogue, listing);
        if (entry == nullptr) {
            for (const library::Entry& candidate : catalogue.entries)
                if (candidate.id.kind == library::Kind::mutator && candidate.id.key == id &&
                    candidate.listing != nullptr) {
                    entry = &candidate;
                    break;
                }
        }
        if (entry != nullptr) {
            const library::GetPlan plan = library::get_plan(catalogue, *entry);
            blocked = plan.blocked;
            if (blocked.empty()) {
                steps.reserve(plan.steps.size());
                for (const library::GetStep& step : plan.steps)
                    steps.push_back(StepCopy{step.id.registry, step.id.key, step.name});
            }
        }
    }
    if (!blocked.empty()) {
        refuse(id, blocked);
        return false;
    }
    if (steps.empty() && !listing_registry.empty() && !listing_key.empty())
        steps.push_back(StepCopy{listing_registry, listing_key, listing_name});
    if (steps.empty()) {
        refuse(id, std::string(kCouldNotQueue));
        return false;
    }
    for (const StepCopy& step : steps) {
        if (!is_on(id))
            return false;
        if (seen.contains(std::pair<std::string, std::string>{step.registry, step.key}))
            continue;
        if (const picker::Mutator* row = find_mutator(picker_.inputs.mutators, step.key);
            row != nullptr && row->installed)
            continue;
        std::string why;
        const MutatorPickerFeed& feed = runtime_.mutator_picker_feed_;
        std::optional<uint64_t> queued;
        if (feed.enqueue != nullptr)
            queued = feed.enqueue(feed.context, step.registry, step.key, &why);
        if (!queued) {
            if (why.empty())
                why = std::string(kCouldNotQueue);
            refuse(id, std::move(why));
            return false;
        }
        seen.emplace(step.registry, step.key);
        remember_queue(step, *queued);
    }
    return true;
}

void Runtime::MutatorPickerScreen::refuse(std::string_view id, std::string reason) {
    if (runtime_.mutator_picker_ != nullptr)
        runtime_.mutator_picker_->notes[std::string(id)] = reason;
    const std::optional<picker::TurnOff> asked = picker::switch_off(picker_, id);
    if (asked)
        picker::confirm_turn_off(picker_);
    if (picker::Mutator* mutator = find_mutator(picker_.inputs.mutators, id)) {
        if (!mutator->installed) {
            library::QueueItem failed;
            failed.kind = library::Kind::mutator;
            failed.key = std::string(id);
            failed.name = mutator->name.empty() ? std::string(id) : mutator->name;
            failed.phase = library::QueueItem::Phase::failed;
            failed.problem = std::move(reason);
            mutator->queued = std::move(failed);
        }
    }
}

void Runtime::MutatorPickerScreen::remember_queue(const StepCopy& step, uint64_t queue_id) {
    if (runtime_.mutator_picker_ == nullptr)
        return;
    MutatorPickerState::Tracked track;
    track.id = step.key;
    track.registry = step.registry;
    track.key = step.key;
    track.queue_id = queue_id;
    track.synthetic = true;
    track.phase = library::QueueItem::Phase::waiting;
    runtime_.mutator_picker_->tracked.push_back(std::move(track));
    if (picker::Mutator* mutator = find_mutator(picker_.inputs.mutators, step.key)) {
        if (mutator->installed || mutator->queued)
            return;
        library::QueueItem waiting;
        waiting.registry = step.registry;
        waiting.kind = library::Kind::mutator;
        waiting.key = step.key;
        waiting.name = !step.name.empty() ? step.name : mutator->name;
        waiting.phase = library::QueueItem::Phase::waiting;
        mutator->queued = std::move(waiting);
    }
}

void Runtime::MutatorPickerScreen::settle_left() {
    MutatorPickerState* state = runtime_.mutator_picker_.get();
    if (state == nullptr)
        return;
    const MutatorPickerFeed& feed = runtime_.mutator_picker_feed_;
    std::vector<MutatorPickerState::Tracked> kept;
    kept.reserve(state->tracked.size());
    for (MutatorPickerState::Tracked& track : state->tracked) {
        if (is_on(track.id)) {
            kept.push_back(std::move(track));
            continue;
        }
        const bool waiting = track.phase == library::QueueItem::Phase::waiting || track.synthetic;
        if (waiting && feed.cancel != nullptr)
            feed.cancel(feed.context, track.registry, track.key);
        if (waiting) {
            if (picker::Mutator* mutator = find_mutator(picker_.inputs.mutators, track.id);
                mutator != nullptr && track.synthetic)
                mutator->queued.reset();
            continue;
        }
        kept.push_back(std::move(track));
    }
    state->tracked = std::move(kept);
}

bool Runtime::MutatorPickerScreen::update_waiting() {
    std::string next = waiting_text();
    if (next == state_.waiting)
        return false;
    state_.waiting = std::move(next);
    return true;
}

std::string Runtime::MutatorPickerScreen::waiting_text() const {
    const library::TextHooks text = text_hooks();
    const picker::Mutator* failed = nullptr;
    for (const picker::Chosen& chosen : picker_.on) {
        const picker::Mutator* mutator = find_mutator(picker_.inputs.mutators, chosen.id);
        if (mutator == nullptr || mutator->installed || !mutator->queued)
            continue;
        const library::QueueItem& item = *mutator->queued;
        if (item.phase == library::QueueItem::Phase::failed) {
            if (failed == nullptr)
                failed = mutator;
            continue;
        }
        if (item.phase == library::QueueItem::Phase::installed)
            continue;
        const std::string name = mutator->name.empty() ? mutator->id : mutator->name;
        std::uint64_t percent = 0;
        if (item.total_bytes != 0) {
            percent = (item.done_bytes / item.total_bytes) * 100;
            percent += (item.done_bytes % item.total_bytes) * 100 / item.total_bytes;
        }
        if (percent > 100)
            percent = 100;
        const std::string number = std::to_string(percent);
        return library::filled(
            text, picker::words::waiting_getting, {{"name", name}, {"percent", number}}
        );
    }
    if (failed == nullptr)
        return {};
    const std::string name = failed->name.empty() ? failed->id : failed->name;
    const std::string problem = failed->queued ? failed->queued->problem : std::string{};
    return library::filled(
        text, picker::words::waiting_failed, {{"name", name}, {"problem", problem}}
    );
}

LayerAnswer Runtime::MutatorPickerScreen::handle_use() {
    bool installed = true;
    for (const picker::Chosen& chosen : picker_.on) {
        const picker::Mutator* mutator = find_mutator(picker_.inputs.mutators, chosen.id);
        if (mutator == nullptr || !mutator->installed)
            installed = false;
    }
    if (!picker::ready(picker_) || !installed) {
        std::vector<std::string> ids;
        for (const picker::Chosen& chosen : picker_.on) {
            const picker::Mutator* mutator = find_mutator(picker_.inputs.mutators, chosen.id);
            if (mutator == nullptr || mutator->installed || mutator->queued)
                continue;
            ids.push_back(chosen.id);
        }
        if (!ids.empty())
            enqueue_ids(ids);
        update_waiting();
        relayout();
        return LayerAnswer::redraw;
    }
    handed_set_ = picker::result(picker_);
    ending_ = Ending::hand;
    return LayerAnswer::close;
}

void Runtime::MutatorPickerScreen::ask_question() {
    const kit::Question question = picker::turn_off_question(picker_);
    if (question.buttons.empty())
        return;
    const Screen over = opened_over_;
    Runtime* runtime = &runtime_;
    QuestionScreen::Host host;
    host.answer = [runtime](std::int32_t button) {
        LayerScreen* found = runtime->oa_layer().find(picker::screen_name);
        if (found == nullptr)
            return true;
        auto* screen = static_cast<MutatorPickerScreen*>(found);
        if (button == 1)
            screen->confirm_asked();
        else
            screen->cancel_asked();
        return true;
    };
    host.tick = [runtime, over]() {
        if (runtime->oa_layer().find(picker::screen_name) == nullptr)
            return true;
        return runtime->screen_ != over || runtime->match_ != nullptr ||
               runtime->screen_ == Screen::match;
    };
    runtime_.oa_layer().push(
        std::make_unique<QuestionScreen>(runtime_.oa_layer(), over, question, std::move(host))
    );
}

LayerAnswer Runtime::MutatorPickerScreen::take(picker::ScreenResult result) {
    if (result.action != picker::PickerAction::none)
        clear_notes();
    switch (result.action) {
    case picker::PickerAction::cancel:
        ending_ = Ending::cancel;
        return LayerAnswer::close;
    case picker::PickerAction::use:
        return handle_use();
    case picker::PickerAction::ask_turn_off:
        ask_question();
        break;
    case picker::PickerAction::get:
        enqueue_ids(result.ids);
        break;
    case picker::PickerAction::check:
        if (!result.ids.empty())
            force_check(result.ids.front());
        break;
    case picker::PickerAction::none:
        break;
    }
    if (result.action != picker::PickerAction::none || result.redraw) {
        update_waiting();
        relayout();
        return LayerAnswer::redraw;
    }
    return LayerAnswer::none;
}

void Runtime::MutatorPickerScreen::relayout() {
    ++revision_;
    const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
    if (fonts == nullptr) {
        list_ = {};
        return;
    }
    list_ = picker::picker_layout(picker_, state_, frame_, *fonts);
}

bool Runtime::MutatorPickerScreen::is_on(std::string_view id) const {
    return chosen_of(id) != nullptr;
}

const picker::Chosen* Runtime::MutatorPickerScreen::chosen_of(std::string_view id) const {
    for (const picker::Chosen& chosen : picker_.on)
        if (chosen.id == id)
            return &chosen;
    return nullptr;
}

picker::Clash Runtime::MutatorPickerScreen::clash_default() const {
    const auto found = runtime_.preference_values_.find(std::string(kClashPreference));
    if (found == runtime_.preference_values_.end())
        return picker::Clash::replace;
    const std::optional<formats::oamut::ClashMode> mode =
        formats::oamut::parse_clash_mode(found->second);
    if (mode == formats::oamut::ClashMode::add)
        return picker::Clash::add;
    return picker::Clash::replace;
}

bool Runtime::open_mutator_picker(MutatorPickerRequest request) {
    if (match_ != nullptr || screen_ == Screen::match)
        return false;
    if (oa::ui::frontend_dialogs::dialog_count() != 0)
        return false;
    if (oa_layer().modal_shown(false))
        return false;
    if (oa_layer().screen_fonts() == nullptr)
        return false;
    if (oa_layer().find(picker::screen_name) != nullptr)
        return false;
    if (!mutator_picker_feed_set_)
        restore_mutator_picker_feed();
    if (mutator_picker_feed_.inputs == nullptr)
        return false;
    auto screen = std::make_unique<MutatorPickerScreen>(*this, std::move(request));
    if (!screen->start()) {
        mutator_picker_.reset();
        return false;
    }
    oa_layer().push(std::move(screen));
    return true;
}

void Runtime::set_mutator_picker_feed(MutatorPickerFeed feed) {
    mutator_picker_feed_ = feed;
    mutator_picker_feed_set_ = true;
    if (LayerScreen* screen = oa_layer().find(picker::screen_name))
        static_cast<MutatorPickerScreen*>(screen)->drop_badges();
    if (mutator_picker_ != nullptr) {
        mutator_picker_->library.reset();
        mutator_picker_->badges.clear();
        mutator_picker_->badge_index.clear();
    }
}

void Runtime::restore_mutator_picker_feed() {
    MutatorPickerFeed feed;
    feed.context = this;
    feed.inputs = own_picker_inputs;
    feed.generation = own_picker_generation;
    feed.enqueue = own_picker_enqueue;
    feed.cancel = own_picker_cancel;
    feed.check = nullptr;
    set_mutator_picker_feed(feed);
}

} // namespace oa::app
