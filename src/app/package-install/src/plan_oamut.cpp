// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The oamut kind's own rules: its manifest, the files a mutator may hold,
// its scripts and icon, the engine it needs, what a folder of Mutators
// holds, and where a mutator installs. No script is parsed or run.

#include "files.hpp"

#include "oa/app/package_install/oamut.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/png.hpp"

#include <cstdint>
#include <exception>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef OA_ENGINE_VERSION
#error                                                                                             \
    "OA_ENGINE_VERSION names this build's version, which a mutator's engine requirement is checked against"
#endif

namespace oa::app::package_install::oamut {

namespace fs = std::filesystem;
namespace mut = oa::formats::oamut;
namespace png = oa::formats::png;
namespace keys = oa::formats::oamod;

static_assert(mutators_folder_name == oa::data::mutator_library::mutators_folder_name);

namespace {

/// The release this build is, parsed once from OA_ENGINE_VERSION.
///
/// @return that release; 0.0.0 when the macro is not a version
keys::EngineVersion this_build_version() {
    static const keys::EngineVersion version = [] {
        if (const std::optional<keys::EngineVersion> parsed =
                keys::parse_engine_version(OA_ENGINE_VERSION))
            return *parsed;
        return keys::EngineVersion{};
    }();
    return version;
}

/// Returns a problem.
///
/// @param refusal why
/// @param subject what it names
/// @param detail English, for the log and, where it is the whole sentence, the player
/// @return the problem
Problem problem_of(Refusal refusal, std::string subject = {}, std::string detail = {}) {
    Problem problem{};
    problem.refusal = refusal;
    problem.subject = std::move(subject);
    problem.detail = std::move(detail);
    return problem;
}

/// Tells whether a folder holds the incoming mutator, at any version.
///
/// @param held what the folder holds
/// @param incoming the mutator
/// @return true when it holds that id
bool holds(const InstalledPackage& held, const Incoming& incoming) noexcept {
    return held.kind == FolderKind::package && held.id == incoming.id;
}

/// Returns what a folder holds, through the hooks.
///
/// @param hooks the hooks
/// @param folder the folder's name
/// @return what it holds; missing without a hook
InstalledPackage look(const FolderHooks& hooks, std::string_view folder) {
    return hooks.look != nullptr ? hooks.look(hooks.context, folder) : InstalledPackage{};
}

/// Returns what a folder's .backup holds, through the hooks.
///
/// @param hooks the hooks
/// @param folder the folder's name
/// @return what it holds; nothing without a hook
std::optional<InstalledPackage> backup_of(const FolderHooks& hooks, std::string_view folder) {
    return hooks.backup_of != nullptr ? hooks.backup_of(hooks.context, folder) : std::nullopt;
}

/// Tells whether a packaged name is a folder.
///
/// @param name the name
/// @return true when it ends in '/'
bool directory_name(std::string_view name) noexcept {
    return !name.empty() && name.back() == '/';
}

/// Tells whether a file is one a mutator may hold.
///
/// @param name the file's name, below the package's top, ASCII-folded
/// @param entry the entry script, ASCII-folded
/// @param icon the icon, ASCII-folded; empty when the manifest names none
/// @return true for oamut.yaml, the entry, the icon, a .lua file under lua/,
///         a .sm file under rules/, any file under files/, or a top-level
///         .md or .txt note
bool allowed_file(std::string_view name, std::string_view entry, std::string_view icon) {
    if (name == mut::manifest_file)
        return true;
    if (!entry.empty() && name == entry)
        return true;
    if (!icon.empty() && name == icon)
        return true;
    if (name.starts_with("lua/") && name.ends_with(".lua"))
        return true;
    if (name.starts_with("rules/") && name.ends_with(".sm"))
        return true;
    if (name.starts_with("files/") && name.size() > std::string_view("files/").size())
        return true;
    if (name.find('/') != std::string_view::npos)
        return false;
    return name.ends_with(".md") || name.ends_with(".txt");
}

/// Reads a folder's oamut.yaml.
///
/// @param folder the folder
/// @param[out] manifest the manifest, when it was read
/// @return true when the folder holds a readable manifest
bool read_folder_manifest(const fs::path& folder, mut::Manifest& manifest) {
    const auto found = entry_without_case(folder, mut::manifest_file);
    if (!found)
        return false;
    std::ifstream in(*found, std::ios::binary);
    if (!in)
        return false;
    std::vector<uint8_t> bytes;
    for (std::istreambuf_iterator<char> at{in}, end;
         at != end && bytes.size() <= oa::formats::oamod::max_input_bytes;
         ++at)
        bytes.push_back(static_cast<uint8_t>(*at));
    if (bytes.size() > oa::formats::oamod::max_input_bytes)
        return false;
    std::vector<mut::Problem> problems;
    return mut::read_manifest(bytes, manifest, problems);
}

/// Fills what a folder holds from a manifest it gave.
///
/// @param manifest the manifest
/// @return the folder, of kind package; revision is 0
InstalledPackage installed_of(const mut::Manifest& manifest) {
    InstalledPackage installed{};
    installed.kind = FolderKind::package;
    installed.id = manifest.id;
    installed.name = manifest.name;
    installed.version = mut::version_text(manifest.version);
    installed.revision = 0;
    installed.author = manifest.author.name;
    installed.description = manifest.description;
    return installed;
}

/// The sentence for an engine requirement this build does not meet.
///
/// @param range the requirement
/// @return the wording a map pack uses for the same check
std::string unmet_engine(const keys::EngineRange& range) {
    return "needs Open Annihilation " + keys::describe_engine_range(range) +
           "; this is Open Annihilation " + keys::engine_version_text(this_build_version());
}

/// Refuses a script that is missing or larger than 1 MiB.
///
/// A missing script leaves limit_bytes 0. A script the reader would not
/// return whole sets it to most_script_bytes, which the sentence uses.
///
/// @param name the script
/// @param too_large true when the script is there and larger than the bound
/// @return the problem
Problem script_problem(std::string name, bool too_large) {
    Problem problem = problem_of(Refusal::script_missing, std::move(name));
    if (too_large)
        problem.limit_bytes = most_script_bytes;
    return problem;
}

} // namespace

bool read_manifest(
    std::span<const uint8_t> manifest_bytes,
    const ManifestContext& context,
    Package& package,
    Problem& problem
) {
    mut::Manifest manifest;
    std::vector<mut::Problem> problems;
    if (!mut::read_manifest(manifest_bytes, manifest, problems)) {
        problem = problem_of(Refusal::manifest_errors);
        for (const mut::Problem& one : problems)
            problem.lines.push_back(mut::describe(one));
        return false;
    }
    if (windows_device_name(manifest.id)) {
        problem = problem_of(Refusal::reserved_id, manifest.id);
        return false;
    }
    package.incoming.id = manifest.id;
    package.incoming.name = manifest.name;
    package.incoming.version = mut::version_text(manifest.version);
    package.incoming.revision = 0;

    const std::string entry = detail::folded(manifest.entry);
    const std::string icon = detail::folded(manifest.icon);
    std::set<std::string> held;
    for (const PackagedFile& file : package.files) {
        if (directory_name(file.name))
            continue;
        const std::string name = detail::folded(file.name);
        held.insert(name);
        if (!allowed_file(name, entry, icon)) {
            problem = problem_of(Refusal::outside_layout, file.name);
            return false;
        }
    }
    if (!held.contains(entry)) {
        problem = script_problem(manifest.entry, false);
        return false;
    }

    for (const PackagedFile& file : package.files) {
        if (directory_name(file.name))
            continue;
        const std::string name = detail::folded(file.name);
        if (!name.ends_with(".lua") && name != entry)
            continue;
        if (context.read_entry == nullptr) {
            problem = script_problem(file.name, false);
            return false;
        }
        std::vector<uint8_t> bytes;
        const EntryRead read =
            context.read_entry(context.reader, file.name, most_script_bytes, bytes);
        if (read == EntryRead::missing) {
            problem = script_problem(file.name, false);
            return false;
        }
        if (read != EntryRead::read) {
            problem = script_problem(file.name, true);
            return false;
        }
        // An empty file is source. Only the bytecode mark is precompiled.
        if (!bytes.empty() && bytes.front() == 0x1B) {
            problem = problem_of(Refusal::precompiled, file.name);
            return false;
        }
    }

    if (!manifest.icon.empty()) {
        if (!held.contains(icon) || context.read_entry == nullptr) {
            problem = problem_of(Refusal::icon_invalid, manifest.icon);
            return false;
        }
        std::vector<uint8_t> bytes;
        const EntryRead read =
            context.read_entry(context.reader, manifest.icon, most_icon_bytes, bytes);
        png::Info info{};
        const bool png_ok = read == EntryRead::read && png::has_signature(bytes) &&
                            png::read_info(bytes, png::Messages{}, &info);
        const bool square = png_ok && info.header.width == info.header.height &&
                            info.header.width >= 1 && info.header.width <= most_icon_side;
        if (!square) {
            problem = problem_of(Refusal::icon_invalid, manifest.icon);
            return false;
        }
    }

    if (!manifest.requires_engine.empty()) {
        const std::optional<keys::EngineRange> range =
            keys::parse_engine_range(manifest.requires_engine);
        if (!range || !keys::engine_range_met(*range, this_build_version())) {
            problem = problem_of(
                Refusal::engine_unmet,
                manifest.requires_engine,
                range ? unmet_engine(*range) : std::string("Its engine requirement cannot be read.")
            );
            return false;
        }
    }
    return true;
}

InstalledPackage read_installed(const fs::path& folder) {
    InstalledPackage installed{};
    try {
        std::error_code error;
        const fs::file_status status = fs::symlink_status(folder, error);
        if (error || !fs::exists(status))
            return installed;
        installed.kind = FolderKind::other;
        if (is_link_or_junction(folder) || !fs::is_directory(status))
            return installed;
        mut::Manifest manifest;
        if (read_folder_manifest(folder, manifest) && !manifest.id.empty())
            return installed_of(manifest);
        // Empty folders alone, as a file sync can bring back after an
        // uninstall, hold nothing of the player's: the folder is free, and
        // an install removes them first.
        if (detail::holds_only_empty_folders(folder))
            return InstalledPackage{};
        return installed;
    } catch (const std::exception&) {
        return installed;
    }
}

std::optional<InstalledPackage> read_backup(const fs::path& folder) {
    try {
        const auto kept = entry_without_case(folder, backup_folder_name);
        if (!kept)
            return std::nullopt;
        InstalledPackage backup = read_installed(*kept);
        if (backup.kind == FolderKind::missing)
            return std::nullopt;
        const InstalledPackage own = read_installed(folder);
        if (backup.kind == FolderKind::package &&
            (own.kind != FolderKind::package || own.id != backup.id))
            backup.kind = FolderKind::other;
        return backup;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

InstallPlan plan_install(const Incoming& incoming, const FolderHooks& hooks) {
    const std::string own = incoming.id;
    const InstalledPackage at_own = look(hooks, own);
    if (at_own.kind == FolderKind::missing) {
        InstallPlan plan{};
        plan.target = own;
        // A file the player opened asks before a new folder is made. The
        // Install button answers alongside, which the runtime installs into
        // `alongside`; that folder is this id, and it is missing.
        if (incoming.origin.kind == OriginKind::catalogue) {
            plan.kind = PlanKind::install;
            return plan;
        }
        plan.kind = PlanKind::ask_install;
        plan.alongside = own;
        return plan;
    }
    if (!holds(at_own, incoming)) {
        InstallPlan plan{};
        plan.kind = PlanKind::refuse;
        plan.refusal = Refusal::not_a_pack;
        plan.target = own;
        plan.installed = at_own;
        return plan;
    }
    const bool incoming_catalogue = incoming.origin.kind == OriginKind::catalogue;
    const bool held_catalogue =
        at_own.origin.has_value() && at_own.origin->kind == OriginKind::catalogue;
    if (incoming_catalogue && held_catalogue &&
        at_own.origin->registry == incoming.origin.registry) {
        InstallPlan plan{};
        plan.kind = PlanKind::install;
        plan.replacing = true;
        plan.target = own;
        plan.installed = at_own;
        plan.backup = backup_of(hooks, own);
        return plan;
    }
    if (incoming_catalogue && held_catalogue) {
        InstallPlan plan{};
        plan.kind = PlanKind::ask_version;
        plan.target = own;
        plan.installed = at_own;
        plan.backup = backup_of(hooks, own);
        return plan;
    }
    if (at_own.version == incoming.version) {
        InstallPlan plan{};
        plan.kind = PlanKind::ask_reinstall;
        plan.target = own;
        plan.installed = at_own;
        plan.backup = backup_of(hooks, own);
        return plan;
    }
    InstallPlan plan{};
    plan.kind = PlanKind::ask_update;
    plan.target = own;
    plan.installed = at_own;
    plan.backup = backup_of(hooks, own);
    return plan;
}

} // namespace oa::app::package_install::oamut
