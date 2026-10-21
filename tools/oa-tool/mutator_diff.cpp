// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// `mutator diff` mounts a game the way the game does, runs package folders
// through the mutation pipeline, and prints what changed. `--versus` compares
// a second loadout after folding two ways of saying the same removal.

#include "command.hpp"

#include "arguments.hpp"
#include "mutator_report.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/mod_profile_loader.hpp"
#include "oa/data/content_bundles/content_choice.hpp"
#include "oa/data/mod_profile/overrides.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/data/mutators/pipeline.hpp"
#include "oa/formats/oamut/manifest.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef OA_ENGINE_VERSION
#error OA_ENGINE_VERSION is set from the project version
#endif

namespace oa::tool {
namespace {

namespace fs = std::filesystem;
namespace app = oa::app;
namespace bundles = oa::data::content_bundles;
namespace mutators = oa::data::mutators;
namespace oamut = oa::formats::oamut;
namespace report = mutator_report;

constexpr std::string_view command_name = "oa-tool mutator diff";

const OptionSpec diff_options[] = {
    {"game-dir", true, false},
    {"mod-dir", true, false},
    {"map", true, false},
    {"seed", true, false},
    {"mutator", true, true},
    {"versus", true, true},
    {"allow", true, false},
    {"dump", true, false},
};

/// Prints one usage line and returns exit_usage.
int usage(Output& output, std::string_view problem) {
    output.err << command_name << ": " << problem << '\n';
    return exit_usage;
}

/// The file name of a path, as UTF-8.
std::string file_name_of(const fs::path& path) {
    const auto text = path.filename().u8string();
    return {text.begin(), text.end()};
}

/// A path as UTF-8, for a refusal that names a file the player passed.
std::string path_text(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

/// Folds ASCII letters and turns backslashes into slashes.
std::string normalize_path(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char piece : text) {
        char lowered = static_cast<char>(piece);
        if (lowered == '\\')
            lowered = '/';
        else if (lowered >= 'A' && lowered <= 'Z')
            lowered = static_cast<char>(lowered - 'A' + 'a');
        out.push_back(lowered);
    }
    return out;
}

/// Every value of a repeating option, or none.
std::vector<std::string> repeated(const Arguments& arguments, std::string_view name) {
    const auto found = arguments.values.find(name);
    if (found == arguments.values.end())
        return {};
    return found->second;
}

/// Reads a match seed. Empty, a non-digit and an overflow are refused.
bool parse_seed(std::string_view text, uint64_t& seed) {
    if (text.empty())
        return false;
    uint64_t value = 0;
    for (const char piece : text) {
        if (piece < '0' || piece > '9')
            return false;
        const auto digit = static_cast<uint64_t>(piece - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    seed = value;
    return true;
}

/// Reads an allow file. A missing file is a usage error.
std::optional<report::AllowList> read_allow(const fs::path& path, std::string& problem) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) {
        problem = path_text(path) + ": cannot be read";
        return std::nullopt;
    }
    std::string text;
    char piece = 0;
    while (in.get(piece))
        text.push_back(piece);
    if (!in.eof()) {
        problem = path_text(path) + ": cannot be read";
        return std::nullopt;
    }
    std::string error;
    std::optional<report::AllowList> list = report::parse_allowed(text, &error);
    if (!list)
        problem = path_text(path) + ":" + error;
    return list;
}

/// Archive file names in mount order, separated by one space.
std::string game_label(const std::vector<fs::path>& archives) {
    std::string line;
    for (const fs::path& archive : archives) {
        if (!line.empty())
            line.push_back(' ');
        line.append(file_name_of(archive));
    }
    return line;
}

/// One mutator's place in the loadout line.
std::string mutator_label(const mutators::ResolvedMutator& resolved) {
    std::string line = resolved.manifest.id;
    line.push_back(' ');
    line.append(oamut::version_text(resolved.manifest.version));
    for (const mutators::LoadoutOption& option : resolved.options) {
        line.push_back(' ');
        line.append(option.id);
        line.push_back('=');
        line.append(mutators::option_text(option));
    }
    return line;
}

/// The loadout header, or `none`.
std::string loadout_label(const std::vector<mutators::ResolvedMutator>& listed) {
    if (listed.empty())
        return "none";
    std::string line;
    for (std::size_t index = 0; index < listed.size(); ++index) {
        if (index != 0)
            line.append("; ");
        line.append(mutator_label(listed[index]));
    }
    return line;
}

/// The report header. `# map` is written only when a map was named.
std::string header_text(
    const app::GameInstall& install,
    const std::vector<mutators::ResolvedMutator>& listed,
    uint64_t seed,
    const std::string* map_name
) {
    std::string text = "# oa-tool mutator diff 1\n# game: ";
    text.append(game_label(install.archives));
    text.append("\n# mod: ");
    if (install.profile == nullptr) {
        text.append("none");
    } else {
        text.append(install.profile->id);
        text.push_back(' ');
        text.append(install.profile->version);
    }
    text.append("\n# loadout: ");
    text.append(loadout_label(listed));
    text.append("\n# seed: ");
    text.append(std::to_string(seed));
    text.push_back('\n');
    if (map_name != nullptr) {
        text.append("# map: ");
        text.append(*map_name);
        text.push_back('\n');
    }
    return text;
}

/// Resolves package folders. An id, a bad choice and a bad folder are usage errors.
std::optional<std::vector<mutators::ResolvedMutator>>
resolve_specs(const std::vector<std::string>& specs, std::string& problem) {
    std::vector<mutators::ResolvedMutator> listed;
    listed.reserve(specs.size());
    for (const std::string& spec : specs) {
        std::string error;
        const std::optional<mutators::MutatorChoice> choice = mutators::parse_choice(spec, &error);
        if (!choice) {
            problem = error.empty() ? spec + " is not a mutator choice" : error;
            return std::nullopt;
        }
        if (!choice->by_path) {
            problem = choice->target + " names an id, and this command reads a package folder";
            return std::nullopt;
        }
        const std::optional<mutators::ResolvedMutator> resolved =
            mutators::resolve_choice(*choice, {}, &error);
        if (!resolved) {
            problem = error.empty() ? spec + " is not a mutator package" : error;
            return std::nullopt;
        }
        listed.push_back(*resolved);
    }
    return listed;
}

/// The map the maps phase reads. A missing OTA leaves the bytes empty.
mutators::ChosenMap chosen_map(const oa::AssetStore& store, std::string_view name) {
    mutators::ChosenMap chosen;
    chosen.key = std::string(name);
    chosen.ota_path = "maps/" + chosen.key + ".ota";
    const std::size_t mark = chosen.key.rfind('@');
    if (mark != std::string::npos && mark + 1 < chosen.key.size())
        chosen.pack = chosen.key.substr(mark + 1);
    if (const std::optional<std::vector<uint8_t>> bytes = store.load_file_contents(chosen.ota_path))
        chosen.ota = std::make_shared<const std::vector<uint8_t>>(std::move(*bytes));
    return chosen;
}

/// Records journal writers in loadout order, each id once per path and field.
void attribute(
    report::Attribution& attribution,
    const mutators::Journal& journal,
    const std::vector<std::string>& ids
) {
    std::map<std::string, std::set<uint16_t>> buckets;
    for (const mutators::JournalEntry& entry : journal.entries) {
        const bool file_level = entry.field.empty() && (entry.change == mutators::Change::added ||
                                                        entry.change == mutators::Change::hidden ||
                                                        entry.change == mutators::Change::removed);
        if (entry.field.empty() && !file_level)
            continue;
        std::string key = normalize_path(entry.path);
        key.push_back('\n');
        key.append(entry.field);
        std::set<uint16_t>& indices = buckets[key];
        for (const uint16_t index : entry.mutators) {
            if (static_cast<std::size_t>(index) < ids.size())
                indices.insert(index);
        }
    }
    for (const auto& [key, indices] : buckets) {
        const auto split = key.find('\n');
        const std::string path = key.substr(0, split);
        const std::string field = key.substr(split + 1);
        for (const uint16_t index : indices)
            attribution.add(path, field, ids[index]);
    }
}

/// Why the first mutator that did not apply stopped.
std::string failure_reason(const mutators::PipelineResult& result) {
    for (const mutators::MutatorResult& one : result.mutators) {
        if (one.outcome == mutators::Outcome::applied)
            continue;
        if (!one.message.empty())
            return one.id + ": " + one.message;
        if (!result.refusal.empty())
            return one.id + ": " + result.refusal;
        return one.id + ": the mutator did not apply";
    }
    if (!result.refusal.empty())
        return result.refusal;
    return "the mutator did not apply";
}

/// One loadout's change lines and the store after it ran.
struct Side {
    std::vector<std::string> lines;
    report::Dump after;
};

/// Runs one loadout. An empty list leaves the store as it was.
Side apply_loadout(
    oa::AssetStore& store,
    const report::Dump& before,
    const std::vector<mutators::ResolvedMutator>& listed,
    const app::GameInstall& install,
    const oa::data::defs::DataLayout& layout,
    uint64_t seed,
    const std::optional<mutators::ChosenMap>& map
) {
    Side side;
    if (listed.empty()) {
        side.after = before;
        return side;
    }
    mutators::PipelineInput input;
    input.mutators.reserve(listed.size());
    std::vector<std::string> ids;
    ids.reserve(listed.size());
    for (const mutators::ResolvedMutator& resolved : listed) {
        mutators::PipelineMutator item;
        item.package = resolved.package_folder;
        item.manifest = resolved.manifest;
        item.options = resolved.options;
        ids.push_back(resolved.manifest.id);
        input.mutators.push_back(std::move(item));
    }
    if (install.profile != nullptr) {
        input.mod.id = install.profile->id;
        input.mod.version = install.profile->version;
        input.mod.game = install.profile->supported_games.empty()
                             ? std::string(bundles::default_base_game_id)
                             : install.profile->supported_games.front();
    } else {
        input.mod.id = std::string(oa::data::mod_profile::base_game_id);
        input.mod.game = std::string(bundles::default_base_game_id);
    }
    input.engine_version = OA_ENGINE_VERSION;
    input.layout = &layout;
    input.seed = seed;
    input.map = map;
    const mutators::PipelineResult result = mutators::run_pipeline(store, input);
    if (!result.playable())
        throw Failure(failure_reason(result));
    report::Attribution attribution;
    attribute(attribution, result.journal, ids);
    side.after = report::dump_store(store);
    side.lines = report::report_lines(before, side.after, attribution);
    return side;
}

/// Prints the comparison and returns 0 only when nothing remains unmatched.
int write_comparison(Output& output, const report::Comparison& compared) {
    for (const std::string& line : compared.left_only)
        output.out << line << '\n';
    for (const std::string& line : compared.right_only)
        output.out << line << '\n';
    for (const std::string& pattern : compared.unused_patterns)
        output.out << "allowed difference matched nothing: " << pattern << '\n';
    const std::size_t differences = compared.left_only.size() + compared.right_only.size();
    output.out << "mutator diff: " << differences << " differences, " << compared.allowed.size()
               << " allowed, " << compared.unused_patterns.size() << " unused allowances\n";
    if (differences == 0 && compared.unused_patterns.empty())
        return exit_done;
    return exit_failed;
}

} // namespace

int run_mutator_diff(std::span<const std::string> arguments, Output& output) {
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, diff_options, problem);
    if (!parsed)
        return usage(output, problem);
    if (!parsed->positional.empty())
        return usage(output, "unexpected argument '" + parsed->positional.front() + "'");
    const std::string* const game_dir = parsed->value("game-dir");
    if (game_dir == nullptr)
        return usage(output, "--game-dir is required");
    const std::string* const allow_path = parsed->value("allow");
    const std::vector<std::string> versus = repeated(*parsed, "versus");
    if (allow_path != nullptr && versus.empty())
        return usage(output, "--allow needs --versus");

    uint64_t seed = 0;
    if (const std::string* const seed_text = parsed->value("seed")) {
        if (!parse_seed(*seed_text, seed))
            return usage(output, "seed is not a whole number");
    }
    const std::string* const map_name = parsed->value("map");
    if (map_name != nullptr && (map_name->empty() || map_name->find('/') != std::string::npos ||
                                map_name->find('\\') != std::string::npos)) {
        return usage(output, "map name is empty or holds a slash");
    }

    report::AllowList allowed;
    if (allow_path != nullptr) {
        std::optional<report::AllowList> read = read_allow(fs::path(*allow_path), problem);
        if (!read)
            return usage(output, problem);
        allowed = std::move(*read);
    }

    const std::vector<std::string> specs = repeated(*parsed, "mutator");
    const std::optional<std::vector<mutators::ResolvedMutator>> listed =
        resolve_specs(specs, problem);
    if (!listed)
        return usage(output, problem);
    const std::optional<std::vector<mutators::ResolvedMutator>> other =
        resolve_specs(versus, problem);
    if (!other)
        return usage(output, problem);

    const fs::path game(*game_dir);
    const std::string* const mod_text = parsed->value("mod-dir");
    app::ModChoice choice;
    if (mod_text != nullptr)
        choice.folder = fs::path(*mod_text);
    const app::GameInstall install = app::inspect_game_install(game, {}, app::demo_1997, choice);
    if (!install.profile_errors.empty() || !install.problem.empty() || !install.folder ||
        install.archives.empty() || install.folders.empty()) {
        throw Failure(app::describe_install_problem(install));
    }

    oa::AssetStore store(install.folders);
    for (const fs::path& candidate : install.archives) {
        const fs::path archive = candidate.is_absolute() ? candidate : game / candidate;
        store.mount(archive);
    }
    const oa::data::defs::DataLayout layout = app::data_layout_of(install.profile.get());
    std::optional<mutators::ChosenMap> map;
    if (map_name != nullptr)
        map = chosen_map(store, *map_name);

    const report::Dump before = report::dump_store(store);
    const Side left = apply_loadout(store, before, *listed, install, layout, seed, map);
    const std::string* const dump_path = parsed->value("dump");
    if (dump_path != nullptr) {
        std::ofstream dumped(*dump_path, std::ios::out | std::ios::binary);
        if (!dumped)
            throw Failure("cannot write " + *dump_path);
        report::write_dump(dumped, left.after);
        if (!dumped)
            throw Failure("cannot write " + *dump_path);
    }

    if (!versus.empty()) {
        store.unmount_mutator_layer();
        const Side right = apply_loadout(store, before, *other, install, layout, seed, map);
        const report::Comparison compared =
            report::compare(left.lines, right.lines, left.after, right.after, allowed);
        return write_comparison(output, compared);
    }

    output.out << header_text(install, *listed, seed, map_name);
    for (const std::string& line : left.lines)
        output.out << line << '\n';
    return exit_done;
}

} // namespace oa::tool
