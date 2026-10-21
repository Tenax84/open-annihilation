// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One ordered run of mutators. The list is checked, each plan is read in a
// sandbox, the files layer is mounted, documents are parsed from that layer,
// rules run once, and each hook is a transaction. A failure drops that
// mutator and anyone who requires it, and the run starts again.
#include "oa/data/mutators/pipeline.hpp"

#include "oa/base/sha256.hpp"
#include "oa/data/defs/asset_files.hpp"
#include "oa/data/defs/files.hpp"
#include "oa/data/field_schema.hpp"
#include "oa/data/mutators/allow_list.hpp"
#include "oa/data/mutators/rules_lua.hpp"
#include "oa/data/mutation_rules/documents.hpp"
#include "oa/formats/soft_mutator.hpp"
#include "oa/formats/tdf/edit.hpp"
#include "oa/formats/tdf/write.hpp"

#include "lua.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::mutators {
namespace rules = oa::data::mutation_rules;
namespace host = oa::data::script_host;
namespace tdf = oa::formats::tdf;
namespace defs = oa::data::defs;
namespace fields = oa::data::field_schema;
namespace oamut = oa::formats::oamut;
namespace oamod = oa::formats::oamod;
namespace sm = oa::formats::soft_mutator;

namespace {

/// Base directory names, in DataDirectory order. A rule folder whose first
/// segment is one of these means the layout's folder of that kind.
/// Later tickets extend the plan-key and target-layer tables below, not this.
constexpr const char* k_base_directories[] = {
    "units",
    "weapons",
    "gamedata",
    "ai",
    "guis",
    "unitpics",
    "download",
};

static_assert(
    sizeof(k_base_directories) / sizeof(k_base_directories[0]) == defs::data_directory_count
);

/// How a supported plan key is read. D04, D06, D07 and D08 add rows here.
enum class KeyJob : uint8_t { files, rules, units_hook, weapons_hook };

struct PlanKeyRow {
    const char* name{};
    KeyJob job{};
    oamut::Layer layer{};
};

constexpr PlanKeyRow k_plan_keys[] = {
    {"files", KeyJob::files, oamut::Layer::files},
    {"rules", KeyJob::rules, oamut::Layer::tables},
    {"units", KeyJob::units_hook, oamut::Layer::units},
    {"weapons", KeyJob::weapons_hook, oamut::Layer::weapons},
};

/// Which manifest layer a rule target uses. The same tickets add rows here.
struct TargetLayerRow {
    rules::TargetKind kind{};
    oamut::Layer layer{};
};

constexpr TargetLayerRow k_target_layers[] = {
    {rules::TargetKind::units, oamut::Layer::units},
    {rules::TargetKind::weapons, oamut::Layer::weapons},
    {rules::TargetKind::features, oamut::Layer::features},
    {rules::TargetKind::sidedata, oamut::Layer::sides},
    {rules::TargetKind::other, oamut::Layer::tables},
};

/// Feature documents live under this folder. It is not one of the seven
/// directories a mod may rename.
constexpr const char* k_features_folder = "features";

/// One mutator's plan, kept after its sandbox has returned.
struct Plan {
    FilesPlan files;
    bool has_files{false};
    rules::MutatorRules rules;
    bool has_rules{false};
    int units_hook{LUA_NOREF};
    int weapons_hook{LUA_NOREF};
};

/// Why reading a plan stopped, when it did.
struct PlanFault {
    bool failed{false};
    Cause cause{Cause::none};
    std::string message;
};

/// Pointers a Lua callback may use. The callback holds none of its own.
struct ContextView {
    const ModIdentity* mod{};
    const std::string* engine{};
    const std::vector<OptionValue>* options{};
};

/// What `mutate` left in the registry.
struct MutateBox {
    const ContextView* context{};
    int ref{LUA_NOREF};
    int results{0};
    int missing{0};
};

/// What one hook call needs. The callback holds none of its own objects.
struct HookBox {
    Collections* collections{};
    int function_ref{LUA_NOREF};
    int units{0};
    const ContextView* context{};
};

/// The package files one sandbox may read, sorted byte by byte.
struct Listing {
    std::filesystem::path root;
    std::vector<std::string> paths;
    std::string error;
};

/// A document as it stood before one hook, so a failure can put it back.
struct DocSnap {
    std::string text;
    bool removed{false};
    bool added{false};
    bool mark_added{false};
};

/// Unmounts a layer this run mounted, unless the run published it.
struct MountGuard {
    oa::AssetStore& store;
    bool armed{false};

    ~MountGuard() {
        if (armed)
            store.unmount_mutator_layer();
    }

    void arm() { armed = true; }

    void disarm() { armed = false; }

    void release() {
        if (armed)
            store.unmount_mutator_layer();
        armed = false;
    }
};

/// Folds one byte to lower case.
char fold_ascii(char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return byte;
}

/// Copies a text folded to lower case.
std::string fold_copy(std::string_view text) {
    std::string out;
    out.resize(text.size());
    for (std::size_t offset = 0; offset < text.size(); ++offset)
        out[offset] = fold_ascii(text[offset]);
    return out;
}

/// Reports whether two texts match without regard to ASCII case.
bool same_text(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t offset = 0; offset < left.size(); ++offset) {
        if (fold_ascii(left[offset]) != fold_ascii(right[offset]))
            return false;
    }
    return true;
}

/// Folds a path byte: ASCII capitals become lower case, and '\\' becomes '/'.
char fold_path_byte(char byte) {
    if (byte == '\\')
        return '/';
    return fold_ascii(byte);
}

/// Reports whether two paths match, ignoring ASCII case and slash direction.
bool same_path(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t offset = 0; offset < left.size(); ++offset) {
        if (fold_path_byte(left[offset]) != fold_path_byte(right[offset]))
            return false;
    }
    return true;
}

/// Replaces a rule folder's first segment when it names a base directory.
std::string resolve_folder(std::string_view folder, const defs::DataLayout& layout) {
    const auto slash = folder.find('/');
    const std::string_view first =
        slash == std::string_view::npos ? folder : folder.substr(0, slash);
    const std::string_view rest =
        slash == std::string_view::npos ? std::string_view{} : folder.substr(slash);
    for (std::size_t directory_index = 0; directory_index < defs::data_directory_count;
         ++directory_index) {
        if (first == k_base_directories[directory_index]) {
            std::string resolved = layout.directories[directory_index];
            resolved.append(rest.data(), rest.size());
            return resolved;
        }
    }
    return std::string{folder};
}

/// Replaces the suffix `fbi` with the layout's unit extension.
std::string resolve_extension(std::string_view extension, const defs::DataLayout& layout) {
    if (extension == "fbi")
        return layout.unit_extension;
    return std::string{extension};
}

/// Classifies a resolved target the way the rules pass does.
rules::TargetKind kind_of(
    std::string_view rule_folder,
    std::string_view resolved_folder,
    std::string_view resolved_extension,
    const defs::DataLayout& layout
) {
    const std::size_t units = static_cast<std::size_t>(defs::DataDirectory::units);
    const std::size_t weapons = static_cast<std::size_t>(defs::DataDirectory::weapons);
    const std::size_t gamedata = static_cast<std::size_t>(defs::DataDirectory::gamedata);
    if (same_text(resolved_folder, layout.directories[units]) &&
        same_text(resolved_extension, layout.unit_extension))
        return rules::TargetKind::units;
    if (same_text(resolved_folder, layout.directories[weapons]))
        return rules::TargetKind::weapons;
    if (rule_folder == k_features_folder ||
        (rule_folder.size() > 9 && rule_folder.starts_with("features/")))
        return rules::TargetKind::features;
    if (same_text(resolved_folder, layout.directories[gamedata]) &&
        same_text(resolved_extension, "sidedata.tdf"))
        return rules::TargetKind::sidedata;
    return rules::TargetKind::other;
}

/// The manifest layer a target kind writes.
oamut::Layer layer_for_kind(rules::TargetKind kind) {
    for (const TargetLayerRow& row : k_target_layers) {
        if (row.kind == kind)
            return row.layer;
    }
    return oamut::Layer::tables;
}

/// Reports whether the manifest lists `layer`.
bool declares(const oamut::Manifest& manifest, oamut::Layer layer) {
    for (const oamut::Layer have : manifest.layers) {
        if (have == layer)
            return true;
    }
    return false;
}

/// Finds a plan key, or null when this engine does not support it.
const PlanKeyRow* plan_key(std::string_view name) {
    for (const PlanKeyRow& row : k_plan_keys) {
        if (name == row.name)
            return &row;
    }
    return nullptr;
}

/// The layout the input asked for, or the layout in force.
const defs::DataLayout& layout_of(const PipelineInput& input) {
    if (input.layout != nullptr)
        return *input.layout;
    return defs::data_layout();
}

/// Marks a mutator that was still in the run as failed.
void fail_mutator(MutatorResult& result, Phase phase, Cause cause, std::string message) {
    if (result.outcome != Outcome::applied)
        return;
    result.outcome = Outcome::failed;
    result.phase = phase;
    result.cause = cause;
    result.message = std::move(message);
}

/// Marks a mutator excluded because something it requires did not apply.
void exclude_mutator(MutatorResult& result, Phase phase, std::string message) {
    if (result.outcome != Outcome::applied)
        return;
    result.outcome = Outcome::excluded;
    result.phase = phase;
    result.cause = Cause::prerequisite_failed;
    result.message = std::move(message);
}

/// Turns a script-host error into a cause. The message stays the host's text.
Cause cause_of(host::ErrorKind kind) {
    if (kind == host::ErrorKind::instruction_budget)
        return Cause::instruction_budget;
    if (kind == host::ErrorKind::memory_budget)
        return Cause::memory_budget;
    return Cause::script_error;
}

/// The seed one mutator draws from: the first eight bytes of the SHA-256 of
/// the seed's eight little-endian bytes followed by the id, read little-endian.
uint64_t mutator_seed(uint64_t seed, std::string_view id) {
    std::vector<uint8_t> bytes(8 + id.size());
    for (int shift = 0; shift < 8; ++shift) {
        bytes[static_cast<std::size_t>(shift)] =
            static_cast<uint8_t>((seed >> (8 * shift)) & 0xffu);
    }
    for (std::size_t offset = 0; offset < id.size(); ++offset)
        bytes[8 + offset] = static_cast<uint8_t>(id[offset]);
    const oa::base::sha256::Digest digest = oa::base::sha256::digest_of(bytes);
    uint64_t mixed = 0;
    for (int shift = 0; shift < 8; ++shift) {
        mixed |= static_cast<uint64_t>(digest[static_cast<std::size_t>(shift)]) << (8 * shift);
    }
    return mixed;
}

/// A default for one manifest option.
OptionValue default_option(const oamut::Option& option) {
    OptionValue value;
    value.id = option.id;
    value.type = option.type;
    if (option.type == oamut::OptionType::toggle) {
        value.toggle = option.toggle_default;
        return value;
    }
    if (option.type == oamut::OptionType::number) {
        value.number = true;
        value.number_value = option.number_default;
        return value;
    }
    if (option.default_index < option.values.size()) {
        const oamut::ChoiceValue& chosen = option.values[option.default_index];
        value.number = chosen.number;
        value.number_value = chosen.number_value;
        value.text = chosen.text;
    }
    return value;
}

/// Manifest defaults, with the player's values written over the ids they set.
std::vector<OptionValue> resolve_options(const PipelineMutator& mutator) {
    std::vector<OptionValue> values;
    values.reserve(mutator.manifest.options.size());
    for (const oamut::Option& option : mutator.manifest.options) {
        const OptionValue* given = nullptr;
        for (const OptionValue& candidate : mutator.options) {
            if (candidate.id == option.id) {
                given = &candidate;
                break;
            }
        }
        if (given != nullptr)
            values.push_back(*given);
        else
            values.push_back(default_option(option));
    }
    return values;
}

/// Lists every regular file under `root`, as '/' separated paths sorted
/// byte by byte. A link, or a file that is not regular, refuses the listing.
Listing list_package(const std::filesystem::path& root) {
    Listing listing;
    listing.root = root;
    try {
        std::error_code exists;
        if (!std::filesystem::is_directory(root, exists)) {
            listing.error = "the package folder is missing";
            return listing;
        }
        const std::filesystem::recursive_directory_iterator end;
        std::filesystem::recursive_directory_iterator cursor{root};
        for (; cursor != end; ++cursor) {
            const std::filesystem::directory_entry& entry = *cursor;
            if (entry.is_symlink()) {
                listing.error = "the package holds a link";
                listing.paths.clear();
                return listing;
            }
            if (entry.is_directory())
                continue;
            if (!entry.is_regular_file()) {
                listing.error = "the package holds a file that is not a regular file";
                listing.paths.clear();
                return listing;
            }
            const std::filesystem::path relative = std::filesystem::relative(entry.path(), root);
            const std::string generic = relative.generic_string();
            if (generic.empty() || generic == ".." || generic.starts_with("../") ||
                generic.find(':') != std::string::npos) {
                listing.error = "the package holds a path outside its folder";
                listing.paths.clear();
                return listing;
            }
            listing.paths.push_back(generic);
        }
    } catch (const std::exception& ex) {
        listing.paths.clear();
        listing.error = ex.what() != nullptr ? ex.what() : "the package folder could not be read";
        return listing;
    }
    std::sort(listing.paths.begin(), listing.paths.end());
    return listing;
}

/// Reads `path` only when it is exactly one listed path. Never throws.
bool read_listed(const Listing& listing, std::string_view path, std::vector<uint8_t>& bytes) {
    bytes.clear();
    try {
        const auto found = std::lower_bound(listing.paths.begin(), listing.paths.end(), path);
        if (found == listing.paths.end() || *found != path)
            return false;
        const std::filesystem::path on_disk = listing.root / std::filesystem::path{*found};
        std::ifstream in(on_disk, std::ios::binary);
        if (!in)
            return false;
        in.seekg(0, std::ios::end);
        const std::streamoff length = in.tellg();
        if (length < 0)
            return false;
        in.seekg(0, std::ios::beg);
        bytes.resize(static_cast<std::size_t>(length));
        if (length > 0) {
            in.read(reinterpret_cast<char*>(bytes.data()), length);
            if (!in)
                return false;
        }
        return true;
    } catch (...) {
        bytes.clear();
        return false;
    }
}

/// SourceHooks reader. The context is a Listing.
bool source_read(void* context, std::string_view path, std::vector<uint8_t>& bytes) {
    if (context == nullptr)
        return false;
    return read_listed(*static_cast<const Listing*>(context), path, bytes);
}

/// Pushes one option value. Called only from a Lua callback, so it keeps no
/// C++ object that would need destroying if the allocator raises.
void push_option(lua_State* state, int table, const OptionValue* option) {
    if (option->type == oamut::OptionType::toggle)
        lua_pushboolean(state, option->toggle ? 1 : 0);
    else if (option->type == oamut::OptionType::number || option->number)
        lua_pushnumber(state, oamod::number_to_double(option->number_value));
    else
        lua_pushlstring(state, option->text.data(), option->text.size());
    lua_setfield(state, table, option->id.c_str());
}

/// Pushes `ctx` with mod, engine and options. Safe to long-jump out of.
void push_context(lua_State* state, const ContextView* shown) {
    lua_createtable(state, 0, 3);
    const int ctx = lua_gettop(state);
    lua_createtable(state, 0, 3);
    const int mod_table = lua_gettop(state);
    lua_pushlstring(state, shown->mod->id.data(), shown->mod->id.size());
    lua_setfield(state, mod_table, "id");
    lua_pushlstring(state, shown->mod->version.data(), shown->mod->version.size());
    lua_setfield(state, mod_table, "version");
    lua_pushlstring(state, shown->mod->game.data(), shown->mod->game.size());
    lua_setfield(state, mod_table, "game");
    lua_setfield(state, ctx, "mod");
    lua_createtable(state, 0, 1);
    const int engine_table = lua_gettop(state);
    lua_pushlstring(state, shown->engine->data(), shown->engine->size());
    lua_setfield(state, engine_table, "version");
    lua_setfield(state, ctx, "engine");
    const std::size_t count = shown->options->size();
    const int sized = count > 1000000 ? 1000000 : static_cast<int>(count);
    lua_createtable(state, 0, sized);
    const int options_table = lua_gettop(state);
    for (std::size_t option_index = 0; option_index < count; ++option_index)
        push_option(state, options_table, &(*shown->options)[option_index]);
    lua_setfield(state, ctx, "options");
}

int push_mutate(void* context, lua_State* state) {
    auto* box = static_cast<MutateBox*>(context);
    lua_getglobal(state, "mutate");
    if (!lua_isfunction(state, -1)) {
        box->missing = 1;
        lua_pop(state, 1);
        lua_pushnil(state);
        return 0;
    }
    push_context(state, box->context);
    return 1;
}

void read_mutate(void* context, lua_State* state, int results) {
    auto* box = static_cast<MutateBox*>(context);
    box->results = results;
    box->ref = LUA_NOREF;
    if (results != 1 || !lua_istable(state, -1))
        return;
    lua_pushvalue(state, -1);
    box->ref = luaL_ref(state, LUA_REGISTRYINDEX);
}

void look_mutate(void* context, lua_State* state) {
    auto* found = static_cast<int*>(context);
    lua_getglobal(state, "mutate");
    *found = lua_isfunction(state, -1) ? 1 : 0;
    lua_pop(state, 1);
}

void install_forms(void*, lua_State* state) {
    install_rule_forms(state);
}

int push_hook(void* context, lua_State* state) {
    auto* box = static_cast<HookBox*>(context);
    lua_rawgeti(state, LUA_REGISTRYINDEX, box->function_ref);
    if (box->units != 0)
        box->collections->push_units(state);
    else
        box->collections->push_weapons(state);
    push_context(state, box->context);
    const int ctx = lua_gettop(state);
    box->collections->set_context_collections(state, ctx);
    return 2;
}

/// Reads a sequence of strings. `what` names the field in the refusal.
bool read_string_list(
    lua_State* state, int table, std::vector<std::string>& out, std::string& error, const char* what
) {
    if (lua_isnil(state, table))
        return true;
    if (!lua_istable(state, table)) {
        error = std::string{what} + " must be a list of text";
        return false;
    }
    const lua_Unsigned count = lua_rawlen(state, table);
    for (lua_Unsigned slot = 1; slot <= count; ++slot) {
        lua_rawgeti(state, table, static_cast<lua_Integer>(slot));
        if (lua_type(state, -1) != LUA_TSTRING) {
            lua_pop(state, 1);
            error = std::string{what} + " must be a list of text";
            return false;
        }
        std::size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        out.emplace_back(text, length);
        lua_pop(state, 1);
    }
    return true;
}

/// Collects the plan table's keys and sorts them byte by byte.
bool collect_keys(lua_State* state, int table, std::vector<std::string>& keys, std::string& error) {
    bool non_string = false;
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING)
            non_string = true;
        else {
            std::size_t length = 0;
            const char* text = lua_tolstring(state, -2, &length);
            keys.emplace_back(text, length);
        }
        lua_pop(state, 1);
    }
    if (non_string) {
        error = "a plan key must be text";
        return false;
    }
    std::sort(keys.begin(), keys.end());
    return true;
}

/// Reads the files table into `plan`.
PlanFault
read_files_table(lua_State* state, int table, const oamut::Manifest& manifest, Plan& plan) {
    PlanFault fault;
    if (!lua_istable(state, table)) {
        fault.failed = true;
        fault.cause = Cause::invalid_plan;
        fault.message = "files must be a table";
        return fault;
    }
    lua_getfield(state, table, "mask");
    std::string shape;
    if (!read_string_list(state, lua_gettop(state), plan.files.mask, shape, "files.mask")) {
        lua_pop(state, 1);
        fault.failed = true;
        fault.cause = Cause::invalid_plan;
        fault.message = std::move(shape);
        return fault;
    }
    lua_pop(state, 1);
    lua_getfield(state, table, "add");
    if (!read_string_list(state, lua_gettop(state), plan.files.add, shape, "files.add")) {
        lua_pop(state, 1);
        fault.failed = true;
        fault.cause = Cause::invalid_plan;
        fault.message = std::move(shape);
        return fault;
    }
    lua_pop(state, 1);
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        std::string key;
        if (lua_type(state, -2) == LUA_TSTRING) {
            std::size_t length = 0;
            const char* text = lua_tolstring(state, -2, &length);
            key.assign(text, length);
        }
        lua_pop(state, 1);
        if (key != "mask" && key != "add") {
            fault.failed = true;
            fault.cause = Cause::invalid_plan;
            fault.message = "the plan key files." + key + " is not supported by this engine";
            return fault;
        }
    }
    if (!declares(manifest, oamut::Layer::files)) {
        fault.failed = true;
        fault.cause = Cause::undeclared_layer;
        fault.message = "returns a files plan but its manifest does not declare the files layer";
        return fault;
    }
    plan.has_files = !plan.files.mask.empty() || !plan.files.add.empty();
    return fault;
}

/// Reads the rules list and checks each target's layer.
PlanFault read_rules_table(
    lua_State* state,
    int table,
    const Listing& listing,
    std::string_view entry,
    const oamut::Manifest& manifest,
    const defs::DataLayout& layout,
    Plan& plan
) {
    PlanFault fault;
    if (!lua_istable(state, table)) {
        fault.failed = true;
        fault.cause = Cause::invalid_plan;
        fault.message = "rules must be a list";
        return fault;
    }
    const PackageReader reader = [&listing](std::string_view path, std::string* error) {
        std::vector<uint8_t> bytes;
        if (!read_listed(listing, path, bytes)) {
            if (error != nullptr)
                *error = "the package holds no such file";
            return std::optional<std::vector<uint8_t>>{};
        }
        return std::optional<std::vector<uint8_t>>{std::move(bytes)};
    };
    PlanRules parsed = read_plan_rules(state, table, reader, entry);
    if (!parsed.ok()) {
        fault.failed = true;
        fault.cause = Cause::unreadable_rules;
        fault.message = std::move(parsed.error);
        return fault;
    }
    plan.rules.sources = std::move(parsed.sources);
    plan.has_rules = !plan.rules.sources.empty();
    std::vector<rules::MutatorRules> one;
    one.push_back(plan.rules);
    const std::vector<sm::Target> targets = rules::targets_of(one);
    for (const sm::Target& target : targets) {
        const std::string folder = resolve_folder(target.folder, layout);
        const std::string extension = resolve_extension(target.extension, layout);
        const rules::TargetKind kind = kind_of(target.folder, folder, extension, layout);
        const oamut::Layer layer = layer_for_kind(kind);
        if (!declares(manifest, layer)) {
            fault.failed = true;
            fault.cause = Cause::undeclared_layer;
            const char* name = oamut::layer_name(layer);
            fault.message = std::string{"returns a rule for the "} + name +
                            " layer but its manifest does not declare the " + name + " layer";
            return fault;
        }
    }
    return fault;
}

/// Reads one hook function and checks its layer.
PlanFault read_hook(
    lua_State* state,
    int table,
    const char* name,
    oamut::Layer layer,
    const oamut::Manifest& manifest,
    int& ref
) {
    PlanFault fault;
    if (lua_isnil(state, table))
        return fault;
    if (!lua_isfunction(state, table)) {
        fault.failed = true;
        fault.cause = Cause::invalid_plan;
        fault.message = std::string{name} + " must be a function";
        return fault;
    }
    if (!declares(manifest, layer)) {
        fault.failed = true;
        fault.cause = Cause::undeclared_layer;
        fault.message = std::string{"returns a "} + name +
                        " hook but its manifest does not declare the " + name + " layer";
        return fault;
    }
    lua_pushvalue(state, table);
    ref = luaL_ref(state, LUA_REGISTRYINDEX);
    return fault;
}

/// Reads the table mutate returned. The state is alive and not inside a callback.
PlanFault read_returned_plan(
    lua_State* state,
    int ref,
    const Listing& listing,
    std::string_view entry,
    const oamut::Manifest& manifest,
    const defs::DataLayout& layout,
    Plan& plan
) {
    PlanFault fault;
    if (ref == LUA_NOREF) {
        fault.failed = true;
        fault.cause = Cause::invalid_plan;
        fault.message = "mutate must return a table";
        return fault;
    }
    lua_rawgeti(state, LUA_REGISTRYINDEX, ref);
    const int table = lua_gettop(state);
    std::vector<std::string> keys;
    std::string keys_error;
    if (!collect_keys(state, table, keys, keys_error)) {
        lua_pop(state, 1);
        fault.failed = true;
        fault.cause = Cause::invalid_plan;
        fault.message = std::move(keys_error);
        return fault;
    }
    for (const std::string& key : keys) {
        if (plan_key(key) == nullptr) {
            lua_pop(state, 1);
            fault.failed = true;
            fault.cause = Cause::invalid_plan;
            fault.message = "the plan key " + key + " is not supported by this engine";
            return fault;
        }
    }
    for (const std::string& key : keys) {
        const PlanKeyRow* row = plan_key(key);
        lua_getfield(state, table, key.c_str());
        const int value = lua_gettop(state);
        if (row->job == KeyJob::files)
            fault = read_files_table(state, value, manifest, plan);
        else if (row->job == KeyJob::rules)
            fault = read_rules_table(state, value, listing, entry, manifest, layout, plan);
        else if (row->job == KeyJob::units_hook)
            fault =
                read_hook(state, value, "units", oamut::Layer::units, manifest, plan.units_hook);
        else
            fault = read_hook(
                state, value, "weapons", oamut::Layer::weapons, manifest, plan.weapons_hook
            );
        lua_pop(state, 1);
        if (fault.failed) {
            lua_pop(state, 1);
            return fault;
        }
    }
    lua_pop(state, 1);
    return fault;
}

/// The index of `id` in the input list, or nullopt when it is not there.
std::optional<uint16_t> index_of(const std::vector<std::string>& ids, std::string_view id) {
    for (std::size_t slot = 0; slot < ids.size(); ++slot) {
        if (ids[slot] == id)
            return static_cast<uint16_t>(slot);
    }
    return std::nullopt;
}

/// Appends a journal field name in lower case.
void set_field(JournalEntry& entry, std::string_view key) {
    entry.field = fold_copy(key);
}

/// Which collection a path belongs to, from the layout's directory names.
Collection collection_for_path(std::string_view path, const defs::DataLayout& layout) {
    std::string folded;
    folded.resize(path.size());
    for (std::size_t offset = 0; offset < path.size(); ++offset)
        folded[offset] = fold_path_byte(path[offset]);
    const auto slash = folded.find('/');
    const std::string_view first = slash == std::string::npos
                                       ? std::string_view{folded}
                                       : std::string_view{folded}.substr(0, slash);
    const std::string_view rest = slash == std::string::npos
                                      ? std::string_view{}
                                      : std::string_view{folded}.substr(slash + 1);
    if (first == k_features_folder)
        return Collection::features;
    const auto directory = [&](defs::DataDirectory which) {
        return same_text(first, layout.directories[static_cast<std::size_t>(which)]);
    };
    if (directory(defs::DataDirectory::units))
        return Collection::units;
    if (directory(defs::DataDirectory::weapons))
        return Collection::weapons;
    if (directory(defs::DataDirectory::guis))
        return Collection::menus;
    if (directory(defs::DataDirectory::download))
        return Collection::menus;
    if (directory(defs::DataDirectory::gamedata)) {
        if (rest == "sidedata.tdf")
            return Collection::sides;
        return Collection::tables;
    }
    return Collection::other;
}

/// Finds a document by path. Returns size() when there is none.
std::size_t find_document(const rules::DocumentSet& documents, std::string_view path) {
    for (std::size_t slot = 0; slot < documents.size(); ++slot) {
        if (same_path(documents.at(slot).path, path))
            return slot;
    }
    return documents.size();
}

/// Records that `mutator` changed the document at `path`.
void note_writer(
    const rules::DocumentSet& documents,
    std::vector<std::vector<uint16_t>>& writers,
    std::vector<uint8_t>& menus,
    std::string_view path,
    uint16_t mutator,
    Collection collection
) {
    const std::size_t slot = find_document(documents, path);
    if (slot >= writers.size())
        return;
    writers[slot].push_back(mutator);
    if (collection == Collection::menus)
        menus[slot] = 1;
}

/// The last mutator in list order that changed a document.
uint16_t owner_of(const std::vector<uint16_t>& slots) {
    uint16_t best = slots.front();
    for (const uint16_t slot : slots) {
        if (slot > best)
            best = slot;
    }
    return best;
}

/// One journal entry for a field write.
JournalEntry entry_from_write(
    const rules::FieldWrite& write,
    const std::vector<std::string>& ids,
    Phase phase,
    const defs::DataLayout& layout
) {
    JournalEntry entry;
    entry.phase = phase;
    entry.collection = collection_for_path(write.path, layout);
    entry.change = write.new_text.has_value() ? Change::set : Change::removed_field;
    entry.record = write.record;
    entry.block = write.block;
    set_field(entry, write.key);
    entry.old_text = write.old_text;
    entry.new_text = write.new_text;
    entry.path = write.path;
    for (const std::string& name : write.mutators) {
        const std::optional<uint16_t> slot = index_of(ids, name);
        if (slot.has_value())
            entry.mutators.push_back(*slot);
    }
    return entry;
}

/// A file added or hidden in the files layer.
JournalEntry file_entry(uint16_t mutator, std::string path, Change change) {
    JournalEntry entry;
    entry.mutators.push_back(mutator);
    entry.phase = Phase::files;
    entry.collection = Collection::files;
    entry.change = change;
    entry.path = std::move(path);
    return entry;
}

/// The record kind a field check should use, or nullopt when the schema has none.
std::optional<fields::RecordKind>
record_kind(const rules::FieldRef& field, const defs::DataLayout& layout) {
    if (field.kind == rules::TargetKind::units)
        return fields::RecordKind::unit;
    if (field.kind == rules::TargetKind::weapons) {
        if (same_text(field.block, "damage"))
            return fields::RecordKind::weapon_damage;
        return fields::RecordKind::weapon;
    }
    const std::string folded = fold_copy(field.path);
    const auto slash = folded.rfind('/');
    const std::string_view file = slash == std::string::npos
                                      ? std::string_view{folded}
                                      : std::string_view{folded}.substr(slash + 1);
    if (file == "moveinfo.tdf") {
        const std::size_t gamedata = static_cast<std::size_t>(defs::DataDirectory::gamedata);
        const std::string folder = fold_copy(layout.directories[gamedata]);
        if (folded.starts_with(folder))
            return fields::RecordKind::move_class;
    }
    return std::nullopt;
}

/// A removal is accepted without a check. Known records go through the schema.
rules::CheckResult check_field(
    const rules::FieldRef& field,
    const std::optional<std::string>& text,
    const defs::DataLayout& layout
) {
    rules::CheckResult result;
    if (!text.has_value()) {
        result.outcome = rules::CheckOutcome::ok;
        return result;
    }
    const std::optional<fields::RecordKind> kind = record_kind(field, layout);
    if (!kind.has_value()) {
        result.outcome = rules::CheckOutcome::ok;
        return result;
    }
    const fields::Check verdict = fields::check_text(*kind, field.key, *text);
    if (verdict.verdict == fields::Verdict::warn) {
        result.outcome = rules::CheckOutcome::warn;
        result.reason = verdict.reason;
        return result;
    }
    if (verdict.verdict == fields::Verdict::refuse) {
        result.outcome = rules::CheckOutcome::refuse;
        result.reason = verdict.reason;
        return result;
    }
    result.outcome = rules::CheckOutcome::ok;
    return result;
}

/// True when any sample of the target is a path a mutator may change.
bool target_allowed(
    const sm::Target& target,
    const defs::DataLayout& layout,
    const PathContext& paths,
    std::string& message
) {
    const std::string folder = resolve_folder(target.folder, layout);
    const std::string extension = resolve_extension(target.extension, layout);
    std::vector<std::string> candidates;
    if (extension.find('.') != std::string::npos)
        candidates.push_back(folder + "/" + extension);
    candidates.push_back(folder + "/item." + extension);
    message.clear();
    for (const std::string& candidate : candidates) {
        const PathVerdict verdict = check_mutable_path(candidate, paths);
        if (verdict.allowed())
            return true;
        if (message.empty())
            message = verdict.message;
    }
    return false;
}

/// Unit-file stems the store lists, folded, so a build picture can name them.
std::vector<std::string> unit_stems(const oa::AssetStore& store, const defs::DataLayout& layout) {
    const std::size_t units = static_cast<std::size_t>(defs::DataDirectory::units);
    std::string suffix = ".";
    suffix += fold_copy(layout.unit_extension);
    const std::vector<std::string> names =
        store.list_effective_in_mount_order(layout.directories[units], suffix, true);
    std::vector<std::string> stems;
    stems.reserve(names.size());
    for (const std::string& name : names) {
        const auto slash = name.rfind('/');
        const std::string_view file = slash == std::string::npos
                                          ? std::string_view{name}
                                          : std::string_view{name}.substr(slash + 1);
        if (file.size() <= suffix.size())
            continue;
        if (!same_text(file.substr(file.size() - suffix.size()), suffix))
            continue;
        stems.emplace_back(file.substr(0, file.size() - suffix.size()));
    }
    std::sort(stems.begin(), stems.end());
    stems.erase(std::unique(stems.begin(), stems.end()), stems.end());
    return stems;
}

/// Parses one document. A text that does not parse is a warning, not a failure.
void consider_document(
    rules::DocumentSet& documents,
    std::vector<std::string>& originals,
    ConflictReport& report,
    const oa::AssetStore& store,
    std::string path
) {
    if (documents.find(path) != nullptr)
        return;
    try {
        const oa::AssetData data = store.read(path);
        std::string text(data.bytes.begin(), data.bytes.end());
        std::string error;
        if (!documents.add(path, text, &error)) {
            report.unread.push_back(std::move(path));
            return;
        }
        originals.push_back(std::move(text));
    } catch (const std::exception&) {
        report.unread.push_back(std::move(path));
    }
}

/// Lists `directory` and parses each file whose name ends with `suffix`.
void consider_listed(
    rules::DocumentSet& documents,
    std::vector<std::string>& originals,
    ConflictReport& report,
    const oa::AssetStore& store,
    std::string_view directory,
    std::string_view suffix,
    bool recursive
) {
    const std::vector<std::string> names =
        recursive ? store.list_effective_recursive(directory, suffix, true)
                  : store.list_effective_in_mount_order(directory, suffix, true);
    for (const std::string& name : names)
        consider_document(documents, originals, report, store, name);
}

/// Reads sidedata and moveinfo through the variant path the game uses.
void consider_variant_file(
    rules::DocumentSet& documents,
    std::vector<std::string>& originals,
    ConflictReport& report,
    oa::AssetStore& store,
    const defs::DataLayout& layout,
    const std::string& variant,
    const char* name
) {
    const defs::Files boundary = defs::asset_store_files(&store);
    std::array<char, defs::path_capacity> buffer{};
    const std::size_t gamedata = static_cast<std::size_t>(defs::DataDirectory::gamedata);
    const char* variant_word = variant.empty() ? nullptr : variant.c_str();
    defs::build_variant_path(
        &boundary,
        buffer.data(),
        buffer.size(),
        layout.directories[gamedata].c_str(),
        name,
        "tdf",
        variant_word
    );
    if (buffer[0] == '\0')
        return;
    consider_document(documents, originals, report, store, std::string{buffer.data()});
}

/// Fills the document set from the mounted store, in listing order.
void parse_documents(
    rules::DocumentSet& documents,
    std::vector<std::string>& originals,
    ConflictReport& report,
    oa::AssetStore& store,
    const defs::DataLayout& layout,
    const std::string& variant,
    const std::vector<rules::MutatorRules>& rule_sets
) {
    const std::size_t units = static_cast<std::size_t>(defs::DataDirectory::units);
    const std::size_t weapons = static_cast<std::size_t>(defs::DataDirectory::weapons);
    const std::size_t download = static_cast<std::size_t>(defs::DataDirectory::download);
    std::string unit_suffix = ".";
    unit_suffix += fold_copy(layout.unit_extension);
    consider_listed(
        documents, originals, report, store, layout.directories[units], unit_suffix, false
    );
    consider_listed(
        documents, originals, report, store, layout.directories[weapons], ".tdf", false
    );
    consider_listed(documents, originals, report, store, k_features_folder, ".tdf", true);
    consider_listed(
        documents, originals, report, store, layout.directories[download], ".tdf", false
    );
    consider_variant_file(documents, originals, report, store, layout, variant, "sidedata");
    consider_variant_file(documents, originals, report, store, layout, variant, "moveinfo");
    const std::vector<sm::Target> targets = rules::targets_of(rule_sets);
    for (const sm::Target& target : targets) {
        const std::string folder = resolve_folder(target.folder, layout);
        const std::string extension = resolve_extension(target.extension, layout);
        std::string suffix = ".";
        suffix += fold_copy(extension);
        consider_listed(documents, originals, report, store, folder, suffix, false);
    }
}

/// The text a document had, so a hook can be rolled back onto it.
std::vector<DocSnap>
snapshot_documents(rules::DocumentSet& documents, const std::vector<std::string>& originals) {
    std::vector<DocSnap> snaps(documents.size());
    for (std::size_t slot = 0; slot < documents.size(); ++slot) {
        rules::LoadedDocument& loaded = documents.at(slot);
        const bool edited = tdf::document_edited(loaded.document.get());
        snaps[slot].removed = loaded.removed;
        snaps[slot].added = loaded.added;
        snaps[slot].mark_added = (loaded.added || edited) && !loaded.removed;
        if (!edited && !loaded.added) {
            snaps[slot].text = originals[slot];
            continue;
        }
        const std::optional<std::string> written = tdf::write_text(loaded.document.get());
        snaps[slot].text = written.has_value() ? *written : originals[slot];
    }
    return snaps;
}

/// Puts documents back to a snapshot. A document that had been edited is
/// marked added, because parsing clears the edited flag and the file must
/// still be written. False means a snapshot did not parse, so the set is
/// not safe for a later hook.
bool restore_documents(rules::DocumentSet& documents, const std::vector<DocSnap>& snaps) {
    const std::size_t count = snaps.size() < documents.size() ? snaps.size() : documents.size();
    for (std::size_t slot = 0; slot < count; ++slot) {
        rules::LoadedDocument& loaded = documents.at(slot);
        if (!loaded.document.parse(snaps[slot].text))
            return false;
        loaded.removed = snaps[slot].removed;
        loaded.added = snaps[slot].added || snaps[slot].mark_added;
    }
    return true;
}

/// Calls one hook. A script error, a spent allowance or a C++ exception fails it.
bool call_hook(
    host::Sandbox& sandbox,
    Collections& collections,
    int function_ref,
    bool units,
    const ContextView& context,
    MutatorResult& result
) {
    HookBox box;
    box.collections = &collections;
    box.function_ref = function_ref;
    box.units = units ? 1 : 0;
    box.context = &context;
    host::CallHooks hooks;
    hooks.context = &box;
    hooks.push = push_hook;
    host::ScriptError error;
    bool called = false;
    try {
        called = sandbox.call("hooks", hooks, error);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& ex) {
        fail_mutator(
            result,
            Phase::hooks,
            Cause::engine_error,
            ex.what() != nullptr ? ex.what() : "the hook stopped"
        );
        return false;
    }
    if (called)
        return true;
    fail_mutator(result, Phase::hooks, cause_of(error.kind), error.message);
    return false;
}

/// Joins works-with ids for the incompatibility line.
std::string works_with_list(const oamut::Manifest& manifest) {
    std::string line;
    for (const oamut::Condition& condition : manifest.works_with) {
        if (!line.empty())
            line += ", ";
        line += condition.id;
    }
    if (line.empty())
        line = "nothing";
    return line;
}

/// True when "*" or a condition names this mod.
bool fits_mod(const oamut::Manifest& manifest, const ModIdentity& mod) {
    if (manifest.works_with.empty())
        return false;
    for (const oamut::Condition& condition : manifest.works_with) {
        if (condition.id == "*")
            return true;
        if (oamut::condition_met(condition, mod.id, mod.version))
            return true;
    }
    return false;
}

/// True when the engine requirement is empty or the engine meets it.
bool fits_engine(const oamut::Manifest& manifest, const std::string& engine_version) {
    if (manifest.requires_engine.empty())
        return true;
    const std::optional<oamut::VersionRange> range =
        oamut::parse_version_range(manifest.requires_engine, nullptr);
    const std::optional<oamut::Version> version = oamut::parse_version(engine_version);
    if (!range.has_value() || !version.has_value())
        return false;
    return oamut::version_in_range(*range, *version);
}

/// Drops mutators whose requirements are failed, incompatible or excluded.
void exclude_blocked(
    std::vector<MutatorResult>& results,
    const std::vector<PipelineMutator>& listed,
    const std::vector<std::string>& versions
) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t slot = 0; slot < listed.size(); ++slot) {
            if (results[slot].outcome != Outcome::applied)
                continue;
            for (const oamut::Condition& condition : listed[slot].manifest.requires_mutators) {
                for (std::size_t other = 0; other < listed.size(); ++other) {
                    if (other == slot)
                        continue;
                    if (!oamut::condition_met(condition, results[other].id, versions[other]))
                        continue;
                    if (results[other].outcome == Outcome::applied)
                        continue;
                    std::string message =
                        results[slot].id + " needs " + results[other].id + ", which ";
                    if (results[other].outcome == Outcome::incompatible)
                        message += "is incompatible";
                    else
                        message += "failed";
                    exclude_mutator(results[slot], results[other].phase, std::move(message));
                    changed = true;
                    break;
                }
                if (results[slot].outcome != Outcome::applied)
                    break;
            }
        }
    }
}

/// The first list problem, or an empty string when the list may run.
std::string
list_refusal(const std::vector<PipelineMutator>& listed, const std::vector<std::string>& versions) {
    if (listed.size() > max_mutators) {
        return "a loadout lists " + std::to_string(listed.size()) + " mutators, and " +
               std::to_string(max_mutators) + " is the most";
    }
    for (std::size_t slot = 0; slot < listed.size(); ++slot) {
        for (std::size_t earlier = 0; earlier < slot; ++earlier) {
            if (listed[slot].manifest.id == listed[earlier].manifest.id)
                return listed[slot].manifest.id + " is listed twice";
        }
    }
    for (std::size_t slot = 0; slot < listed.size(); ++slot) {
        for (const oamut::Condition& condition : listed[slot].manifest.requires_mutators) {
            bool met = false;
            for (std::size_t earlier = 0; earlier < slot; ++earlier) {
                if (oamut::condition_met(condition, listed[earlier].manifest.id, versions[earlier]))
                    met = true;
            }
            if (!met) {
                std::string message = listed[slot].manifest.id + " needs " + condition.id;
                if (!condition.range_text.empty()) {
                    message += " ";
                    message += condition.range_text;
                }
                message += " above it";
                return message;
            }
        }
    }
    for (std::size_t slot = 0; slot < listed.size(); ++slot) {
        for (const oamut::Condition& condition : listed[slot].manifest.conflicts_with) {
            for (std::size_t other = 0; other < listed.size(); ++other) {
                if (other == slot)
                    continue;
                if (oamut::condition_met(condition, listed[other].manifest.id, versions[other]))
                    return listed[slot].manifest.id + " conflicts with " +
                           listed[other].manifest.id;
            }
        }
    }
    return {};
}

/// Refuses the whole list. Nothing is mounted and no script runs.
void refuse_list(PipelineResult& result, std::string refusal) {
    result.refusal = std::move(refusal);
    for (MutatorResult& mutator : result.mutators) {
        mutator.outcome = Outcome::excluded;
        mutator.phase = Phase::loadout;
        mutator.cause = Cause::none;
        mutator.message.clear();
    }
}

/// Plans one mutator. A failure is recorded on `result` and the sandbox is dropped.
void plan_mutator(
    const PipelineInput& input,
    std::size_t slot,
    const defs::DataLayout& layout,
    Listing& listing,
    const std::vector<OptionValue>& options,
    std::optional<host::Sandbox>& sandbox,
    Plan& plan,
    MutatorResult& result
) {
    listing = list_package(input.mutators[slot].package);
    if (!listing.error.empty()) {
        const Cause cause = listing.error.find("link") != std::string::npos ? Cause::invalid_plan
                                                                            : Cause::engine_error;
        fail_mutator(result, Phase::plan, cause, listing.error);
        return;
    }
    host::Profile profile = host::mutator_profile();
    profile.budget = input.budget;
    host::OpenOptions options_for_open;
    options_for_open.package_id = result.id;
    options_for_open.profile = profile;
    options_for_open.sources.context = &listing;
    options_for_open.sources.read = source_read;
    options_for_open.random_seed = mutator_seed(input.seed, result.id);
    host::ScriptError error;
    std::optional<host::Sandbox> opened;
    try {
        opened = host::Sandbox::open(options_for_open, error);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& ex) {
        fail_mutator(
            result,
            Phase::plan,
            Cause::engine_error,
            ex.what() != nullptr ? ex.what() : "open failed"
        );
        return;
    }
    if (!opened.has_value()) {
        fail_mutator(result, Phase::plan, cause_of(error.kind), error.message);
        return;
    }
    sandbox = std::move(*opened);
    if (!sandbox->run_host("plan", nullptr, install_forms, error)) {
        fail_mutator(result, Phase::plan, cause_of(error.kind), error.message);
        sandbox.reset();
        return;
    }
    const std::string& entry = input.mutators[slot].manifest.entry;
    if (!sandbox->run_file(entry, "plan", nullptr, error)) {
        fail_mutator(result, Phase::plan, cause_of(error.kind), error.message);
        sandbox.reset();
        return;
    }
    int found = 0;
    if (!sandbox->run_host("plan", &found, look_mutate, error)) {
        fail_mutator(result, Phase::plan, cause_of(error.kind), error.message);
        sandbox.reset();
        return;
    }
    if (found == 0) {
        fail_mutator(result, Phase::plan, Cause::invalid_plan, "the entry does not define mutate");
        sandbox.reset();
        return;
    }
    ContextView context;
    context.mod = &input.mod;
    context.engine = &input.engine_version;
    context.options = &options;
    MutateBox box;
    box.context = &context;
    host::CallHooks hooks;
    hooks.context = &box;
    hooks.push = push_mutate;
    hooks.read = read_mutate;
    if (!sandbox->call("plan", hooks, error)) {
        fail_mutator(result, Phase::plan, cause_of(error.kind), error.message);
        sandbox.reset();
        return;
    }
    if (box.missing != 0 || box.ref == LUA_NOREF) {
        fail_mutator(result, Phase::plan, Cause::invalid_plan, "mutate must return a table");
        sandbox.reset();
        return;
    }
    plan.rules.mutator = result.id;
    PlanFault fault;
    try {
        fault = read_returned_plan(
            sandbox->state(), box.ref, listing, entry, input.mutators[slot].manifest, layout, plan
        );
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& ex) {
        fail_mutator(
            result,
            Phase::plan,
            Cause::engine_error,
            ex.what() != nullptr ? ex.what() : "the plan could not be read"
        );
        sandbox.reset();
        return;
    }
    if (fault.failed) {
        fail_mutator(result, Phase::plan, fault.cause, std::move(fault.message));
        sandbox.reset();
    }
}

/// True when a mutator that started this pass applied has since failed.
///
/// Mutators that failed on an earlier pass stay failed. They are not a new
/// failure, and the mutators still applied must be allowed to publish.
bool pass_failed(
    const std::vector<MutatorResult>& results, const std::vector<uint8_t>& started_applied
) {
    const std::size_t count =
        results.size() < started_applied.size() ? results.size() : started_applied.size();
    for (std::size_t slot = 0; slot < count; ++slot) {
        if (started_applied[slot] != 0 && results[slot].outcome == Outcome::failed)
            return true;
    }
    return false;
}

/// One pass over the mutators that are still applied. A clean pass publishes
/// its layer and returns true. A failure leaves the store unmounted.
bool run_pass(
    oa::AssetStore& store,
    const PipelineInput& input,
    const defs::DataLayout& layout,
    const std::vector<std::string>& ids,
    std::vector<MutatorResult>& results,
    Journal& journal,
    ConflictReport& report,
    oa::MutatorLayerSpec& layer_spec,
    MountGuard& guard
) {
    const std::size_t count = input.mutators.size();
    std::vector<uint8_t> started_applied(count, 0);
    for (std::size_t slot = 0; slot < count; ++slot) {
        if (results[slot].outcome == Outcome::applied)
            started_applied[slot] = 1;
    }
    std::vector<Listing> listings(count);
    std::vector<std::vector<OptionValue>> options(count);
    std::vector<std::optional<host::Sandbox>> sandboxes(count);
    std::vector<Plan> plans(count);
    for (std::size_t slot = 0; slot < count; ++slot) {
        if (results[slot].outcome != Outcome::applied)
            continue;
        options[slot] = resolve_options(input.mutators[slot]);
        plan_mutator(
            input,
            slot,
            layout,
            listings[slot],
            options[slot],
            sandboxes[slot],
            plans[slot],
            results[slot]
        );
    }
    if (pass_failed(results, started_applied))
        return false;

    std::vector<std::string> stems = unit_stems(store, layout);
    PathContext paths;
    paths.layout = &layout;
    paths.is_unit_name = [&stems](std::string_view name) {
        const std::string folded = fold_copy(name);
        return std::binary_search(stems.begin(), stems.end(), folded);
    };
    std::vector<MutatorFiles> files;
    for (std::size_t slot = 0; slot < count; ++slot) {
        if (results[slot].outcome != Outcome::applied || !plans[slot].has_files)
            continue;
        MutatorFiles one;
        one.id = ids[slot];
        one.package_root = input.mutators[slot].package;
        one.plan = plans[slot].files;
        files.push_back(std::move(one));
    }
    const FilesLayer built = build_files_layer(files, paths);
    for (const MutatorFailure& failure : built.failures) {
        const std::optional<uint16_t> slot = index_of(ids, failure.mutator);
        if (!slot.has_value())
            continue;
        fail_mutator(results[*slot], Phase::files, Cause::files_refused, failure.message);
    }
    if (pass_failed(results, started_applied)) {
        guard.release();
        return false;
    }

    oa::MutatorLayerSpec spec = built.spec;
    if (!spec.entries.empty() || !spec.masks.empty() || !spec.archives.empty()) {
        std::string mount_error;
        if (!store.mount_mutator_layer(spec, &mount_error)) {
            for (std::size_t slot = 0; slot < count; ++slot) {
                fail_mutator(
                    results[slot],
                    Phase::files,
                    Cause::engine_error,
                    mount_error.empty() ? "the mutator layer could not be mounted" : mount_error
                );
            }
            return false;
        }
        guard.arm();
    }

    rules::DocumentSet documents;
    std::vector<std::string> originals;
    std::vector<rules::MutatorRules> rule_sets;
    for (std::size_t slot = 0; slot < count; ++slot) {
        if (results[slot].outcome == Outcome::applied && plans[slot].has_rules)
            rule_sets.push_back(plans[slot].rules);
    }
    parse_documents(documents, originals, report, store, layout, input.variant, rule_sets);

    for (std::size_t slot = 0; slot < count; ++slot) {
        if (results[slot].outcome != Outcome::applied || !plans[slot].has_rules)
            continue;
        std::vector<rules::MutatorRules> one;
        one.push_back(plans[slot].rules);
        const std::vector<sm::Target> targets = rules::targets_of(one);
        for (const sm::Target& target : targets) {
            std::string message;
            if (!target_allowed(target, layout, paths, message)) {
                fail_mutator(
                    results[slot], Phase::rules, Cause::protected_path, std::move(message)
                );
                break;
            }
        }
    }
    if (pass_failed(results, started_applied)) {
        guard.release();
        return false;
    }

    rule_sets.clear();
    for (std::size_t slot = 0; slot < count; ++slot) {
        if (results[slot].outcome == Outcome::applied && plans[slot].has_rules)
            rule_sets.push_back(plans[slot].rules);
    }
    rules::ApplyOptions apply_options;
    apply_options.layout = &layout;
    apply_options.check = [&layout](
                              const rules::FieldRef& field, const std::optional<std::string>& text
                          ) { return check_field(field, text, layout); };
    const rules::ApplyResult applied = rules::apply_rules(rule_sets, documents, apply_options);

    std::optional<Collections> collections;
    collections.emplace(documents, &layout);
    for (std::size_t slot = 0; slot < count; ++slot) {
        if (results[slot].outcome != Outcome::applied)
            continue;
        if (plans[slot].units_hook == LUA_NOREF && plans[slot].weapons_hook == LUA_NOREF)
            continue;
        if (!sandboxes[slot].has_value() || !collections.has_value())
            continue;
        const std::vector<DocSnap> snaps = snapshot_documents(documents, originals);
        WriteGrant grant;
        grant.units = declares(input.mutators[slot].manifest, oamut::Layer::units);
        grant.weapons = declares(input.mutators[slot].manifest, oamut::Layer::weapons);
        grant.features = declares(input.mutators[slot].manifest, oamut::Layer::features);
        collections->begin_mutator(ids[slot], grant);
        ContextView context;
        context.mod = &input.mod;
        context.engine = &input.engine_version;
        context.options = &options[slot];
        bool hook_ok = true;
        if (plans[slot].units_hook != LUA_NOREF) {
            hook_ok = call_hook(
                *sandboxes[slot], *collections, plans[slot].units_hook, true, context, results[slot]
            );
        }
        if (hook_ok && plans[slot].weapons_hook != LUA_NOREF) {
            hook_ok = call_hook(
                *sandboxes[slot],
                *collections,
                plans[slot].weapons_hook,
                false,
                context,
                results[slot]
            );
        }
        if (!hook_ok) {
            if (!restore_documents(documents, snaps)) {
                fail_mutator(
                    results[slot],
                    Phase::hooks,
                    Cause::engine_error,
                    "the pipeline could not restore a document"
                );
                sandboxes.clear();
                collections.reset();
                guard.release();
                return false;
            }
            collections.reset();
            collections.emplace(documents, &layout);
            continue;
        }
        collections->end_mutator();
    }
    if (pass_failed(results, started_applied)) {
        sandboxes.clear();
        collections.reset();
        guard.release();
        return false;
    }

    std::vector<std::vector<uint16_t>> writers(documents.size());
    std::vector<uint8_t> menus(documents.size(), 0);
    for (const rules::FieldWrite& write : applied.writes) {
        journal.entries.push_back(entry_from_write(write, ids, Phase::rules, layout));
        for (const uint16_t mutator : journal.entries.back().mutators)
            note_writer(
                documents, writers, menus, write.path, mutator, journal.entries.back().collection
            );
    }
    for (const rules::RuleNote& note : applied.notes)
        report.conflicts.push_back(conflict_from_rule(note, ids));
    if (collections.has_value()) {
        for (const rules::FieldWrite& write : collections->writes()) {
            journal.entries.push_back(entry_from_write(write, ids, Phase::hooks, layout));
            for (const uint16_t mutator : journal.entries.back().mutators) {
                note_writer(
                    documents,
                    writers,
                    menus,
                    write.path,
                    mutator,
                    journal.entries.back().collection
                );
            }
        }
        for (const RecordRemoval& removal : collections->removals()) {
            JournalEntry entry;
            const std::optional<uint16_t> mutator = index_of(ids, removal.mutator);
            if (mutator.has_value())
                entry.mutators.push_back(*mutator);
            entry.phase = Phase::hooks;
            entry.change = Change::removed;
            entry.record = removal.name;
            entry.path = removal.path;
            if (removal.kind == RemovalKind::unit)
                entry.collection = Collection::units;
            else if (removal.kind == RemovalKind::weapon)
                entry.collection = Collection::weapons;
            else if (removal.kind == RemovalKind::feature)
                entry.collection = Collection::features;
            else if (removal.kind == RemovalKind::download_entry)
                entry.collection = Collection::menus;
            else
                entry.collection = collection_for_path(removal.path, layout);
            if (mutator.has_value())
                note_writer(documents, writers, menus, removal.path, *mutator, entry.collection);
            journal.entries.push_back(std::move(entry));
        }
        for (const HookNote& note : collections->notes())
            report.conflicts.push_back(conflict_from_hook(note, ids));
    }

    for (const oa::MutatorLayerEntry& entry : spec.entries) {
        const std::optional<uint16_t> mutator = index_of(ids, entry.owner);
        if (!mutator.has_value())
            continue;
        const Change change =
            entry.kind == oa::MutatorSourceKind::hidden ? Change::hidden : Change::added;
        journal.entries.push_back(file_entry(*mutator, entry.path, change));
    }
    for (const FileNote& note : built.notes) {
        report.conflicts.push_back(conflict_from_file(note, ids));
        if (note.kind != FileNoteKind::add_masked)
            continue;
        const std::optional<uint16_t> masker = index_of(ids, note.mutator);
        if (!masker.has_value())
            continue;
        JournalEntry hidden = file_entry(*masker, note.path, Change::hidden);
        const std::optional<uint16_t> adder = index_of(ids, note.other);
        if (adder.has_value())
            hidden.mutators.insert(hidden.mutators.begin(), *adder);
        journal.entries.push_back(std::move(hidden));
    }

    bool protected_write = false;
    for (std::size_t slot = 0; slot < documents.size(); ++slot) {
        if (!documents.changed(slot))
            continue;
        const rules::LoadedDocument& loaded = documents.at(slot);
        if (writers[slot].empty()) {
            for (MutatorResult& mutator : results) {
                if (mutator.outcome != Outcome::applied)
                    continue;
                fail_mutator(
                    mutator, Phase::hooks, Cause::engine_error, "a changed document has no writer"
                );
                break;
            }
            protected_write = true;
            continue;
        }
        const PathVerdict verdict = check_mutable_path(loaded.path, paths);
        const bool menus_pass = verdict.rule == PathRule::menus_only && menus[slot] != 0;
        if (!verdict.allowed() && !menus_pass) {
            for (const uint16_t writer : writers[slot])
                fail_mutator(results[writer], Phase::hooks, Cause::protected_path, verdict.message);
            protected_write = true;
            continue;
        }
        oa::MutatorLayerEntry entry;
        entry.path = loaded.path;
        entry.owner = ids[owner_of(writers[slot])];
        if (loaded.removed) {
            entry.kind = oa::MutatorSourceKind::hidden;
            journal.entries.push_back(
                file_entry(owner_of(writers[slot]), loaded.path, Change::hidden)
            );
        } else {
            const std::optional<std::string> text = tdf::write_text(loaded.document.get());
            if (!text.has_value()) {
                fail_mutator(
                    results[owner_of(writers[slot])],
                    Phase::hooks,
                    Cause::engine_error,
                    "a changed document could not be written"
                );
                protected_write = true;
                continue;
            }
            entry.kind = oa::MutatorSourceKind::contents;
            entry.contents =
                std::make_shared<const std::vector<uint8_t>>(text->begin(), text->end());
        }
        bool replaced = false;
        for (std::size_t entry_index = 0; entry_index < spec.entries.size(); ++entry_index) {
            if (!same_path(spec.entries[entry_index].path, loaded.path))
                continue;
            spec.entries[entry_index] = entry;
            replaced = true;
            break;
        }
        if (!replaced)
            spec.entries.push_back(std::move(entry));
    }
    sandboxes.clear();
    collections.reset();
    if (protected_write) {
        guard.release();
        return false;
    }

    spec.label = "mutators";
    for (std::size_t slot = 0; slot < count; ++slot) {
        if (results[slot].outcome != Outcome::applied)
            continue;
        spec.label.push_back(' ');
        spec.label += ids[slot];
        results[slot].phase = Phase::hooks;
        results[slot].cause = Cause::none;
        results[slot].message.clear();
    }
    journal.order();
    if (spec.entries.empty() && spec.masks.empty()) {
        guard.release();
        layer_spec = std::move(spec);
        return true;
    }
    std::string publish_error;
    const bool published = store.mutator_layer_mounted()
                               ? store.replace_mutator_layer(spec, &publish_error)
                               : store.mount_mutator_layer(spec, &publish_error);
    if (!published) {
        for (std::size_t slot = 0; slot < count; ++slot) {
            fail_mutator(
                results[slot],
                Phase::files,
                Cause::engine_error,
                publish_error.empty() ? "the mutator layer could not be mounted" : publish_error
            );
        }
        guard.release();
        return false;
    }
    guard.disarm();
    layer_spec = std::move(spec);
    return true;
}

PipelineResult run_checked(oa::AssetStore& store, const PipelineInput& input) {
    PipelineResult result;
    const std::size_t count = input.mutators.size();
    result.mutators.resize(count);
    std::vector<std::string> ids(count);
    std::vector<std::string> versions(count);
    for (std::size_t slot = 0; slot < count; ++slot) {
        ids[slot] = input.mutators[slot].manifest.id;
        versions[slot] = oamut::version_text(input.mutators[slot].manifest.version);
        result.mutators[slot].id = ids[slot];
        result.mutators[slot].version = versions[slot];
    }
    if (store.mutator_layer_mounted()) {
        refuse_list(result, "a mutator layer is already mounted");
        return result;
    }
    const std::string refusal = list_refusal(input.mutators, versions);
    if (!refusal.empty()) {
        refuse_list(result, refusal);
        return result;
    }
    for (std::size_t slot = 0; slot < count; ++slot) {
        result.mutators[slot].outcome = Outcome::applied;
        result.mutators[slot].phase = Phase::manifest;
        result.mutators[slot].cause = Cause::none;
        const oamut::Manifest& manifest = input.mutators[slot].manifest;
        if (!fits_mod(manifest, input.mod)) {
            result.mutators[slot].outcome = Outcome::incompatible;
            result.mutators[slot].cause = Cause::works_with;
            result.mutators[slot].message =
                ids[slot] + " works with " + works_with_list(manifest) + ", not " + input.mod.id;
            continue;
        }
        if (!fits_engine(manifest, input.engine_version)) {
            result.mutators[slot].outcome = Outcome::incompatible;
            result.mutators[slot].cause = Cause::engine_version;
            result.mutators[slot].message = ids[slot] + " needs engine " +
                                            manifest.requires_engine + ", not " +
                                            input.engine_version;
        }
    }
    exclude_blocked(result.mutators, input.mutators, versions);
    const defs::DataLayout& layout = layout_of(input);
    MountGuard guard{store, false};
    const std::size_t limit = count + 1;
    for (std::size_t pass = 0; pass < limit; ++pass) {
        bool any_applied = false;
        for (const MutatorResult& mutator : result.mutators) {
            if (mutator.outcome == Outcome::applied)
                any_applied = true;
        }
        if (!any_applied) {
            guard.release();
            result.layer = {};
            result.journal = {};
            result.report = {};
            break;
        }
        const std::size_t before = [&]() {
            std::size_t dropped = 0;
            for (const MutatorResult& mutator : result.mutators) {
                if (mutator.outcome != Outcome::applied)
                    ++dropped;
            }
            return dropped;
        }();
        Journal journal;
        ConflictReport report;
        oa::MutatorLayerSpec spec;
        const bool clean =
            run_pass(store, input, layout, ids, result.mutators, journal, report, spec, guard);
        if (clean) {
            result.journal = std::move(journal);
            result.report = std::move(report);
            result.layer = std::move(spec);
            break;
        }
        result.journal = {};
        result.report = {};
        result.layer = {};
        exclude_blocked(result.mutators, input.mutators, versions);
        std::size_t after = 0;
        for (const MutatorResult& mutator : result.mutators) {
            if (mutator.outcome != Outcome::applied)
                ++after;
        }
        if (after == before) {
            for (MutatorResult& mutator : result.mutators) {
                fail_mutator(
                    mutator,
                    Phase::files,
                    Cause::engine_error,
                    "the pipeline could not drop a failed mutator"
                );
            }
        }
        guard.release();
    }
    return result;
}

} // namespace

bool PipelineResult::playable() const noexcept {
    if (!refusal.empty())
        return false;
    for (const MutatorResult& mutator : mutators) {
        if (mutator.outcome != Outcome::applied)
            return false;
    }
    return true;
}

PipelineResult run_pipeline(oa::AssetStore& store, const PipelineInput& input) {
    try {
        return run_checked(store, input);
    } catch (const std::bad_alloc&) {
        PipelineResult result;
        try {
            result.refusal = "the pipeline ran out of memory";
        } catch (const std::bad_alloc&) {
        }
        return result;
    } catch (const std::exception& ex) {
        PipelineResult result;
        try {
            result.refusal = ex.what() != nullptr ? ex.what() : "the pipeline stopped";
        } catch (const std::bad_alloc&) {
        }
        return result;
    } catch (...) {
        PipelineResult result;
        try {
            result.refusal = "the pipeline stopped";
        } catch (const std::bad_alloc&) {
        }
        return result;
    }
}

} // namespace oa::data::mutators
