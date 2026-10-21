// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One ordered run of mutators. The list is checked, each plan is read in a
// sandbox, the files layer is mounted and documents are parsed from that
// layer. Each mutator then applies its rules and then its hooks, in list
// order. A pass is a transaction: a failure ends it, drops that mutator and
// anyone who requires it, and the run starts again from the store.
#include "oa/data/mutators/pipeline.hpp"

#include "oa/base/sha256.hpp"
#include "oa/data/defs/asset_files.hpp"
#include "oa/data/defs/files.hpp"
#include "oa/data/field_schema.hpp"
#include "oa/data/mutators/allow_list.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/data/mutators/rules_lua.hpp"
#include "oa/data/mutation_rules/documents.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/oamod.hpp"
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
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <span>
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

/// What the plan phase found wrong with one part of the plan.
enum class PartFault : uint8_t {
    none,
    wrong_type,     ///< files or rules is not a table, or a hook is not a function
    metatable,      ///< files or rules has a metatable
    mask_shape,     ///< files.mask is not a list of text
    mask_metatable, ///< files.mask has a metatable
    add_shape,      ///< files.add is not a list of text
    add_metatable,  ///< files.add has a metatable
    key_not_text,   ///< files holds a key that is not text
    unknown_key,    ///< files holds a key other than mask and add
};

/// The plan as the plan phase read it, by raw access only.
///
/// It holds registry references, flags and faults, and nothing that needs
/// destroying, so the read can raise. The text of a refusal is made after
/// the phase, from these. Every table a reference names was checked to be
/// plain, and every list to hold only text.
struct PlanShape {
    bool returned_table{false};
    bool metatable{false};
    bool key_not_text{false};
    /// The first unsupported plan key, byte by byte.
    int unknown_key{LUA_NOREF};
    bool has_files{false};
    PartFault files_fault{PartFault::none};
    /// The unsupported key in files, when it is text.
    int files_key{LUA_NOREF};
    int mask_list{LUA_NOREF};
    int add_list{LUA_NOREF};
    bool has_rules{false};
    PartFault rules_fault{PartFault::none};
    int rules_list{LUA_NOREF};
    bool has_units{false};
    PartFault units_fault{PartFault::none};
    int units_hook{LUA_NOREF};
    bool has_weapons{false};
    PartFault weapons_fault{PartFault::none};
    int weapons_hook{LUA_NOREF};
};

/// Pointers a Lua callback may use. The callback holds none of its own.
struct ContextView {
    const ModIdentity* mod{};
    const std::string* engine{};
    const std::vector<LoadoutOption>* options{};
};

/// What `mutate` returned, as the plan phase read it.
struct MutateBox {
    const ContextView* context{};
    PlanShape shape;
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
/// The list decided it, so its phase is `loadout`.
void exclude_mutator(MutatorResult& result, std::string message) {
    if (result.outcome != Outcome::applied)
        return;
    result.outcome = Outcome::excluded;
    result.phase = Phase::loadout;
    result.cause = Cause::prerequisite_failed;
    result.message = std::move(message);
}

/// Turns a script-host error into a cause. The message stays the host's text.
///
/// The allowances, the real-time limit and the string cap each have their
/// own cause. They stop the script whatever data it works on, so they are
/// never read as the script's own error.
Cause cause_of(host::ErrorKind kind) {
    if (kind == host::ErrorKind::instruction_budget)
        return Cause::instruction_budget;
    if (kind == host::ErrorKind::memory_budget)
        return Cause::memory_budget;
    if (kind == host::ErrorKind::time_limit)
        return Cause::time_limit;
    if (kind == host::ErrorKind::string_length)
        return Cause::string_length;
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

/// Manifest defaults, with the player's values written over the ids they set.
std::vector<LoadoutOption> resolve_options(const PipelineMutator& mutator) {
    std::vector<LoadoutOption> values;
    values.reserve(mutator.manifest.options.size());
    for (const oamut::Option& option : mutator.manifest.options) {
        const LoadoutOption* given = nullptr;
        for (const LoadoutOption& candidate : mutator.options) {
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
void push_option(lua_State* state, int table, const LoadoutOption* option) {
    if (option->type == oamut::OptionType::toggle)
        lua_pushboolean(state, option->toggle ? 1 : 0);
    else if (option->type == oamut::OptionType::number || option->value.number)
        lua_pushnumber(state, oamod::number_to_double(option->value.number_value));
    else
        lua_pushlstring(state, option->value.text.data(), option->value.text.size());
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

void read_plan_shape(lua_State* state, int index, PlanShape* shape);

void read_mutate(void* context, lua_State* state, int results) {
    auto* box = static_cast<MutateBox*>(context);
    box->results = results;
    if (results != 1 || !lua_istable(state, -1))
        return;
    read_plan_shape(state, lua_gettop(state), &box->shape);
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

/// What the plan phase found in one list of text.
enum class ListFault : uint8_t {
    none,      ///< absent, or plain and holding only text
    shape,     ///< not a list of text
    metatable, ///< a table with a metatable
};

/// Reads one list of text inside the plan phase, by raw access only.
///
/// Nothing here needs destroying, so a refused allocation can raise. A list
/// it accepts is kept by a registry reference.
///
/// @param state the sandbox state
/// @param index the list's stack index
/// @param[out] ref the list's reference, when it is accepted and present
/// @return what was found
ListFault read_text_list(lua_State* state, int index, int* ref) {
    const int list = lua_absindex(state, index);
    if (lua_isnil(state, list))
        return ListFault::none;
    if (!lua_istable(state, list))
        return ListFault::shape;
    if (lua_getmetatable(state, list) != 0) {
        lua_pop(state, 1);
        return ListFault::metatable;
    }
    const lua_Unsigned count = lua_rawlen(state, list);
    for (lua_Unsigned slot = 1; slot <= count; ++slot) {
        const int kind = lua_rawgeti(state, list, static_cast<lua_Integer>(slot));
        lua_pop(state, 1);
        if (kind != LUA_TSTRING)
            return ListFault::shape;
    }
    lua_pushvalue(state, list);
    *ref = luaL_ref(state, LUA_REGISTRYINDEX);
    return ListFault::none;
}

/// Reads the files table inside the plan phase, by raw access only.
///
/// @param state the sandbox state
/// @param index the table's stack index
/// @param[out] shape where the lists and the fault go
void read_files_shape(lua_State* state, int index, PlanShape* shape) {
    const int files = lua_absindex(state, index);
    shape->has_files = true;
    if (!lua_istable(state, files)) {
        shape->files_fault = PartFault::wrong_type;
        return;
    }
    if (lua_getmetatable(state, files) != 0) {
        lua_pop(state, 1);
        shape->files_fault = PartFault::metatable;
        return;
    }
    lua_pushliteral(state, "mask");
    lua_rawget(state, files);
    const ListFault mask = read_text_list(state, -1, &shape->mask_list);
    lua_pop(state, 1);
    if (mask != ListFault::none) {
        shape->files_fault =
            mask == ListFault::metatable ? PartFault::mask_metatable : PartFault::mask_shape;
        return;
    }
    lua_pushliteral(state, "add");
    lua_rawget(state, files);
    const ListFault add = read_text_list(state, -1, &shape->add_list);
    lua_pop(state, 1);
    if (add != ListFault::none) {
        shape->files_fault =
            add == ListFault::metatable ? PartFault::add_metatable : PartFault::add_shape;
        return;
    }
    lua_pushnil(state);
    while (lua_next(state, files) != 0) {
        lua_pop(state, 1);
        if (lua_type(state, -1) != LUA_TSTRING) {
            lua_pop(state, 1);
            shape->files_fault = PartFault::key_not_text;
            return;
        }
        std::size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        const std::string_view key{text, length};
        if (key == "mask" || key == "add")
            continue;
        shape->files_fault = PartFault::unknown_key;
        shape->files_key = luaL_ref(state, LUA_REGISTRYINDEX);
        return;
    }
}

/// Reads the rules list inside the plan phase. D09 reads its entries later.
///
/// @param state the sandbox state
/// @param index the list's stack index
/// @param[out] shape where the list and the fault go
void read_rules_shape(lua_State* state, int index, PlanShape* shape) {
    const int list = lua_absindex(state, index);
    shape->has_rules = true;
    if (!lua_istable(state, list)) {
        shape->rules_fault = PartFault::wrong_type;
        return;
    }
    if (lua_getmetatable(state, list) != 0) {
        lua_pop(state, 1);
        shape->rules_fault = PartFault::metatable;
        return;
    }
    lua_pushvalue(state, list);
    shape->rules_list = luaL_ref(state, LUA_REGISTRYINDEX);
}

/// Reads one hook inside the plan phase and takes its reference there.
///
/// @param state the sandbox state
/// @param index the value's stack index
/// @param[out] present set: the plan names the hook
/// @param[out] fault set when the value is not a function
/// @param[out] ref the function's reference
void read_hook_shape(lua_State* state, int index, bool* present, PartFault* fault, int* ref) {
    const int value = lua_absindex(state, index);
    *present = true;
    if (!lua_isfunction(state, value)) {
        *fault = PartFault::wrong_type;
        return;
    }
    lua_pushvalue(state, value);
    *ref = luaL_ref(state, LUA_REGISTRYINDEX);
}

/// Reads what `mutate` returned, inside the plan phase.
///
/// Raw access only, so no metamethod of the script runs. Nothing here needs
/// destroying, so a refused allocation can raise; the phase reports it with
/// the mutator's allowances and real-time limit in force. A table with a
/// metatable is refused rather than read around: its raw contents need not
/// be what the script meant.
///
/// @param state the sandbox state
/// @param index the plan's stack index
/// @param[out] shape what was found
void read_plan_shape(lua_State* state, int index, PlanShape* shape) {
    const int plan = lua_absindex(state, index);
    shape->returned_table = true;
    if (lua_getmetatable(state, plan) != 0) {
        lua_pop(state, 1);
        shape->metatable = true;
        return;
    }
    lua_pushnil(state);
    const int first_unknown = lua_gettop(state);
    lua_pushnil(state);
    while (lua_next(state, plan) != 0) {
        const int value = lua_gettop(state);
        const int key = value - 1;
        if (lua_type(state, key) != LUA_TSTRING) {
            shape->key_not_text = true;
            lua_settop(state, key);
            continue;
        }
        std::size_t length = 0;
        const char* text = lua_tolstring(state, key, &length);
        const std::string_view name{text, length};
        const PlanKeyRow* row = plan_key(name);
        if (row == nullptr) {
            bool earlier = lua_isnil(state, first_unknown);
            if (!earlier) {
                std::size_t kept_length = 0;
                const char* kept = lua_tolstring(state, first_unknown, &kept_length);
                earlier = name < std::string_view{kept, kept_length};
            }
            if (earlier) {
                lua_pushvalue(state, key);
                lua_replace(state, first_unknown);
            }
        } else if (row->job == KeyJob::files) {
            read_files_shape(state, value, shape);
        } else if (row->job == KeyJob::rules) {
            read_rules_shape(state, value, shape);
        } else if (row->job == KeyJob::units_hook) {
            read_hook_shape(
                state, value, &shape->has_units, &shape->units_fault, &shape->units_hook
            );
        } else {
            read_hook_shape(
                state, value, &shape->has_weapons, &shape->weapons_fault, &shape->weapons_hook
            );
        }
        lua_settop(state, key);
    }
    if (lua_isnil(state, first_unknown))
        lua_pop(state, 1);
    else
        shape->unknown_key = luaL_ref(state, LUA_REGISTRYINDEX);
}

/// A plan refusal.
PlanFault refuse_plan(Cause cause, std::string message) {
    PlanFault fault;
    fault.failed = true;
    fault.cause = cause;
    fault.message = std::move(message);
    return fault;
}

/// Copies the text a registry reference names.
///
/// The plan phase made the reference to a string, and no script runs after
/// it, so nothing here calls a script or allocates in the state.
std::string text_at(lua_State* state, int ref) {
    lua_rawgeti(state, LUA_REGISTRYINDEX, ref);
    std::size_t length = 0;
    const char* text = lua_tolstring(state, -1, &length);
    std::string copied = text != nullptr ? std::string(text, length) : std::string{};
    lua_pop(state, 1);
    return copied;
}

/// Copies a list the plan phase accepted. It is plain and holds only text,
/// so nothing here calls a script or allocates in the state.
void copy_text_list(lua_State* state, int ref, std::vector<std::string>& out) {
    if (ref == LUA_NOREF)
        return;
    lua_rawgeti(state, LUA_REGISTRYINDEX, ref);
    const int list = lua_gettop(state);
    const lua_Unsigned count = lua_rawlen(state, list);
    for (lua_Unsigned slot = 1; slot <= count; ++slot) {
        lua_rawgeti(state, list, static_cast<lua_Integer>(slot));
        std::size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        out.emplace_back(text, length);
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
}

/// Turns the files part of a plan into F03's plan, or refuses it.
PlanFault read_files_part(
    lua_State* state, const PlanShape& shape, const oamut::Manifest& manifest, Plan& plan
) {
    const PartFault seen = shape.files_fault;
    if (seen == PartFault::wrong_type)
        return refuse_plan(Cause::invalid_plan, "files must be a table");
    if (seen == PartFault::metatable)
        return refuse_plan(
            Cause::invalid_plan, "files must be a plain table, not one with a metatable"
        );
    if (seen == PartFault::mask_shape)
        return refuse_plan(Cause::invalid_plan, "files.mask must be a list of text");
    if (seen == PartFault::mask_metatable)
        return refuse_plan(
            Cause::invalid_plan, "files.mask must be a plain list, not one with a metatable"
        );
    if (seen == PartFault::add_shape)
        return refuse_plan(Cause::invalid_plan, "files.add must be a list of text");
    if (seen == PartFault::add_metatable)
        return refuse_plan(
            Cause::invalid_plan, "files.add must be a plain list, not one with a metatable"
        );
    if (seen == PartFault::key_not_text)
        return refuse_plan(Cause::invalid_plan, "a key of files must be text");
    if (seen == PartFault::unknown_key)
        return refuse_plan(
            Cause::invalid_plan,
            "the plan key files." + text_at(state, shape.files_key) +
                " is not supported by this engine"
        );
    if (!declares(manifest, oamut::Layer::files))
        return refuse_plan(
            Cause::undeclared_layer,
            "returns a files plan but its manifest does not declare the files layer"
        );
    copy_text_list(state, shape.mask_list, plan.files.mask);
    copy_text_list(state, shape.add_list, plan.files.add);
    plan.has_files = !plan.files.mask.empty() || !plan.files.add.empty();
    return {};
}

/// Reads the rules list with D09 and checks each target's layer.
///
/// D09 reads the list by raw access and keeps C++ objects while it does,
/// so it runs after the plan phase, on a list that phase found plain.
PlanFault read_rules_part(
    lua_State* state,
    const PlanShape& shape,
    const Listing& listing,
    std::string_view entry,
    const oamut::Manifest& manifest,
    const defs::DataLayout& layout,
    Plan& plan
) {
    if (shape.rules_fault == PartFault::wrong_type)
        return refuse_plan(Cause::invalid_plan, "rules must be a list");
    if (shape.rules_fault == PartFault::metatable)
        return refuse_plan(
            Cause::invalid_plan, "rules must be a plain list, not one with a metatable"
        );
    const PackageReader reader = [&listing](std::string_view path, std::string* error) {
        std::vector<uint8_t> bytes;
        if (!read_listed(listing, path, bytes)) {
            if (error != nullptr)
                *error = "the package holds no such file";
            return std::optional<std::vector<uint8_t>>{};
        }
        return std::optional<std::vector<uint8_t>>{std::move(bytes)};
    };
    lua_rawgeti(state, LUA_REGISTRYINDEX, shape.rules_list);
    PlanRules parsed = read_plan_rules(state, lua_gettop(state), reader, entry);
    lua_pop(state, 1);
    if (!parsed.ok())
        return refuse_plan(Cause::unreadable_rules, std::move(parsed.error));
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
            const char* name = oamut::layer_name(layer);
            return refuse_plan(
                Cause::undeclared_layer,
                std::string{"returns a rule for the "} + name +
                    " layer but its manifest does not declare the " + name + " layer"
            );
        }
    }
    return {};
}

/// Checks one hook the plan phase found and keeps its reference.
PlanFault read_hook_part(
    PartFault seen,
    int found,
    const char* name,
    oamut::Layer layer,
    const oamut::Manifest& manifest,
    int& ref
) {
    if (seen == PartFault::wrong_type)
        return refuse_plan(Cause::invalid_plan, std::string{name} + " must be a function");
    if (!declares(manifest, layer))
        return refuse_plan(
            Cause::undeclared_layer,
            std::string{"returns a "} + name + " hook but its manifest does not declare the " +
                name + " layer"
        );
    ref = found;
    return {};
}

/// Turns the plan the phase read into a `Plan`, or refuses it.
///
/// The checks run in the order the keys sort in: files, rules, units, then
/// weapons, and the first refusal is the one reported.
PlanFault read_returned_plan(
    lua_State* state,
    const PlanShape& shape,
    const Listing& listing,
    std::string_view entry,
    const oamut::Manifest& manifest,
    const defs::DataLayout& layout,
    Plan& plan
) {
    if (!shape.returned_table)
        return refuse_plan(Cause::invalid_plan, "mutate must return a table");
    if (shape.metatable)
        return refuse_plan(
            Cause::invalid_plan, "the plan must be a plain table, not one with a metatable"
        );
    if (shape.key_not_text)
        return refuse_plan(Cause::invalid_plan, "a plan key must be text");
    if (shape.unknown_key != LUA_NOREF)
        return refuse_plan(
            Cause::invalid_plan,
            "the plan key " + text_at(state, shape.unknown_key) + " is not supported by this engine"
        );
    PlanFault fault;
    if (shape.has_files) {
        fault = read_files_part(state, shape, manifest, plan);
        if (fault.failed)
            return fault;
    }
    if (shape.has_rules) {
        fault = read_rules_part(state, shape, listing, entry, manifest, layout, plan);
        if (fault.failed)
            return fault;
    }
    if (shape.has_units) {
        fault = read_hook_part(
            shape.units_fault,
            shape.units_hook,
            "units",
            oamut::Layer::units,
            manifest,
            plan.units_hook
        );
        if (fault.failed)
            return fault;
    }
    if (shape.has_weapons) {
        fault = read_hook_part(
            shape.weapons_fault,
            shape.weapons_hook,
            "weapons",
            oamut::Layer::weapons,
            manifest,
            plan.weapons_hook
        );
    }
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

/// One change to a document: the mutator, and the phase it made it in.
struct Writer {
    uint16_t mutator{};
    Phase phase{};
};

/// Records that `mutator` changed the document at `path` in `phase`.
void note_writer(
    const rules::DocumentSet& documents,
    std::vector<std::vector<Writer>>& writers,
    std::vector<uint8_t>& menus,
    std::string_view path,
    uint16_t mutator,
    Phase phase,
    Collection collection
) {
    const std::size_t slot = find_document(documents, path);
    if (slot >= writers.size())
        return;
    writers[slot].push_back(Writer{mutator, phase});
    if (collection == Collection::menus)
        menus[slot] = 1;
}

/// The last mutator in list order that changed a document.
uint16_t owner_of(const std::vector<Writer>& changes) {
    uint16_t best = changes.front().mutator;
    for (const Writer& change : changes) {
        if (change.mutator > best)
            best = change.mutator;
    }
    return best;
}

/// The last phase in which `mutator` changed a document. Hooks come after rules.
Phase phase_of(const std::vector<Writer>& changes, uint16_t mutator) {
    Phase last = Phase::rules;
    for (const Writer& change : changes) {
        if (change.mutator == mutator &&
            static_cast<uint8_t>(change.phase) > static_cast<uint8_t>(last))
            last = change.phase;
    }
    return last;
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

/// A file added or hidden, in the phase that added or hid it.
JournalEntry file_entry(uint16_t mutator, std::string path, Change change, Phase phase) {
    JournalEntry entry;
    entry.mutators.push_back(mutator);
    entry.phase = phase;
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

/// Parses one document. A text that does not parse is a warning, not a
/// failure, and the warning names its path once however often it is listed.
void consider_document(
    rules::DocumentSet& documents,
    ConflictReport& report,
    const oa::AssetStore& store,
    std::string path
) {
    if (documents.find(path) != nullptr)
        return;
    for (const std::string& unread : report.unread) {
        if (same_path(unread, path))
            return;
    }
    try {
        const oa::AssetData data = store.read(path);
        const std::string text(data.bytes.begin(), data.bytes.end());
        std::string error;
        if (!documents.add(path, text, &error))
            report.unread.push_back(std::move(path));
    } catch (const std::exception&) {
        report.unread.push_back(std::move(path));
    }
}

/// Lists `directory` and parses each file whose name ends with `suffix`.
void consider_listed(
    rules::DocumentSet& documents,
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
        consider_document(documents, report, store, name);
}

/// Reads sidedata and moveinfo through the variant path the game uses.
void consider_variant_file(
    rules::DocumentSet& documents,
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
    consider_document(documents, report, store, std::string{buffer.data()});
}

/// Fills the document set from the mounted store, in listing order.
void parse_documents(
    rules::DocumentSet& documents,
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
    consider_listed(documents, report, store, layout.directories[units], unit_suffix, false);
    consider_listed(documents, report, store, layout.directories[weapons], ".tdf", false);
    consider_listed(documents, report, store, k_features_folder, ".tdf", true);
    consider_listed(documents, report, store, layout.directories[download], ".tdf", false);
    consider_variant_file(documents, report, store, layout, variant, "sidedata");
    consider_variant_file(documents, report, store, layout, variant, "moveinfo");
    const std::vector<sm::Target> targets = rules::targets_of(rule_sets);
    for (const sm::Target& target : targets) {
        const std::string folder = resolve_folder(target.folder, layout);
        const std::string extension = resolve_extension(target.extension, layout);
        std::string suffix = ".";
        suffix += fold_copy(extension);
        consider_listed(documents, report, store, folder, suffix, false);
    }
}

/// The folder a mask's files are listed from: its segments before the first
/// one that holds a wildcard, never including the last segment.
std::string mask_folder(std::string_view pattern) {
    std::string folder;
    std::size_t begin = 0;
    while (begin < pattern.size()) {
        const std::size_t slash = pattern.find_first_of("/\\", begin);
        if (slash == std::string_view::npos)
            break;
        const std::string_view segment = pattern.substr(begin, slash - begin);
        if (segment.find_first_of("*?") != std::string_view::npos)
            break;
        if (!folder.empty())
            folder.push_back('/');
        folder.append(segment.data(), segment.size());
        begin = slash + 1;
    }
    return folder;
}

/// One hidden entry for each file of the mod that a mask hides.
///
/// The store is read before the layer is mounted, so these are the mod's
/// own files. A file a mutator adds is not counted here: the layer serves
/// it over every mask, and a mask that hides another mutator's file is
/// journaled from the files layer's note. Each mutator names a path once.
std::vector<JournalEntry> mask_entries(
    const oa::AssetStore& store,
    const oa::MutatorLayerSpec& spec,
    const std::vector<std::string>& ids
) {
    std::vector<JournalEntry> entries;
    std::vector<std::string> folders;
    std::vector<std::vector<std::string>> listed;
    for (const oa::MutatorMask& mask : spec.masks) {
        const std::optional<uint16_t> owner = index_of(ids, mask.owner);
        if (!owner.has_value())
            continue;
        const std::string folder = mask_folder(mask.pattern);
        std::size_t known = 0;
        while (known < folders.size() && folders[known] != folder)
            ++known;
        if (known == folders.size()) {
            folders.push_back(folder);
            listed.push_back(store.list_effective_recursive(folder, "", true));
        }
        for (const std::string& name : listed[known]) {
            if (!oa::mutator_mask_matches(mask.pattern, name))
                continue;
            bool served = false;
            for (const oa::MutatorLayerEntry& entry : spec.entries) {
                if (same_path(entry.path, name)) {
                    served = true;
                    break;
                }
            }
            if (!served)
                entries.push_back(file_entry(*owner, name, Change::hidden, Phase::files));
        }
    }
    std::stable_sort(
        entries.begin(), entries.end(), [](const JournalEntry& left, const JournalEntry& right) {
            if (left.mutators.front() != right.mutators.front())
                return left.mutators.front() < right.mutators.front();
            return left.path < right.path;
        }
    );
    entries.erase(
        std::unique(
            entries.begin(),
            entries.end(),
            [](const JournalEntry& left, const JournalEntry& right) {
                return left.mutators.front() == right.mutators.front() && left.path == right.path;
            }
        ),
        entries.end()
    );
    return entries;
}

/// The position no failure has.
constexpr std::size_t k_no_slot = std::numeric_limits<std::size_t>::max();

/// The failure a pass keeps: the one nearest the top of the list.
///
/// A mutator's files, rules and hooks can depend on what the mutators above
/// it did, so only the first failure in list order is sure to be its own.
/// The pass keeps that one, and the next pass judges the rest without it.
struct FirstFailure {
    std::size_t slot{k_no_slot};
    Phase phase{};
    Cause cause{};
    std::string message;

    /// Keeps this failure when it is nearer the top than the one kept.
    void offer(std::size_t at, Phase in, Cause why, std::string text) {
        if (at >= slot)
            return;
        slot = at;
        phase = in;
        cause = why;
        message = std::move(text);
    }

    /// Reports whether the pass found a failure.
    [[nodiscard]] bool found() const noexcept { return slot != k_no_slot; }
};

/// Calls one hook. A script error, a spent allowance or a C++ exception fails
/// it, and the failure is offered as the pass's first.
bool call_hook(
    host::Sandbox& sandbox,
    Collections& collections,
    int function_ref,
    bool units,
    const ContextView& context,
    std::size_t slot,
    FirstFailure& first
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
        first.offer(
            slot,
            Phase::hooks,
            Cause::engine_error,
            ex.what() != nullptr ? ex.what() : "the hook stopped"
        );
        return false;
    }
    if (called)
        return true;
    first.offer(slot, Phase::hooks, cause_of(error.kind), error.message);
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
                    else if (results[other].outcome == Outcome::failed)
                        message += "failed";
                    else
                        message += "did not apply";
                    exclude_mutator(results[slot], std::move(message));
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
    const std::vector<LoadoutOption>& options,
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
    if (box.missing != 0) {
        fail_mutator(result, Phase::plan, Cause::invalid_plan, "mutate must return a table");
        sandbox.reset();
        return;
    }
    plan.rules.mutator = result.id;
    PlanFault fault;
    try {
        fault = read_returned_plan(
            sandbox->state(), box.shape, listing, entry, input.mutators[slot].manifest, layout, plan
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

/// The step state's id for one hook write, folded the way a rule step folds it.
///
/// @param write the hook write
/// @return the id
rules::FieldId field_id_of(const rules::FieldWrite& write) {
    rules::FieldId id;
    id.path.resize(write.path.size());
    for (std::size_t offset = 0; offset < write.path.size(); ++offset)
        id.path[offset] = fold_path_byte(write.path[offset]);
    id.record = fold_copy(write.record);
    id.block = fold_copy(write.block);
    id.key = fold_copy(write.key);
    return id;
}

/// One replaced-value note: the field name and the two mutators.
struct ReplacedField {
    std::string field;
    uint16_t earlier{};
    uint16_t later{};

    /// Orders by the earlier mutator, then the later one, then the field.
    ///
    /// @param other the other note
    /// @return true when this note sorts first
    bool operator<(const ReplacedField& other) const {
        if (earlier != other.earlier)
            return earlier < other.earlier;
        if (later != other.later)
            return later < other.later;
        return field < other.field;
    }
};

/// Adds one rules note to the report.
///
/// A replaced value is kept once for each field name and pair of mutators.
/// Further records of that same replacement would flood a deep stack.
///
/// @param report the pass report
/// @param note the rules note
/// @param ids mutator ids, in list order
/// @param[in,out] replaced_fields the replacements already kept
void append_rule_note(
    ConflictReport& report,
    const rules::RuleNote& note,
    const std::vector<std::string>& ids,
    std::set<ReplacedField>& replaced_fields
) {
    Conflict conflict = conflict_from_rule(note, ids);
    if (conflict.kind == ConflictKind::value_replaced && conflict.mutators.size() >= 2) {
        ReplacedField key;
        key.field = conflict.field;
        key.earlier = conflict.mutators.front();
        key.later = conflict.mutators.back();
        if (!replaced_fields.insert(std::move(key)).second)
            return;
    }
    report.conflicts.push_back(std::move(conflict));
}

/// Reports whether `note` names `mutator`.
///
/// @param note the hook note
/// @param mutator the id
/// @return true when the note names it
bool note_names(const HookNote& note, std::string_view mutator) {
    for (const std::string& name : note.mutators) {
        if (name == mutator)
            return true;
    }
    return false;
}

/// Records this mutator's new hook notes and writes.
///
/// A hook may write a field an earlier mutator locked. The write stands, and
/// the report names both mutators. The step state remembers the hook as the
/// last writer, and keeps the lock, so a later mutator's rules still leave
/// the field alone.
///
/// @param collections the hooks' collections
/// @param[in,out] state lock marks and last writers
/// @param[in,out] report the pass report
/// @param ids mutator ids, in list order
/// @param slot this mutator's index
/// @param notes_before how many hook notes existed before this mutator
/// @param writes_before how many hook writes existed before this mutator
void record_hook_turn(
    Collections& collections,
    rules::ApplyState& state,
    ConflictReport& report,
    const std::vector<std::string>& ids,
    std::size_t slot,
    std::size_t notes_before,
    std::size_t writes_before
) {
    const std::span<const HookNote> notes = collections.notes();
    for (std::size_t note_index = notes_before; note_index < notes.size(); ++note_index)
        report.conflicts.push_back(conflict_from_hook(notes[note_index], ids));

    const std::span<const rules::FieldWrite> writes = collections.writes();
    for (std::size_t write_index = writes_before; write_index < writes.size(); ++write_index) {
        const rules::FieldWrite& write = writes[write_index];
        const rules::FieldId id = field_id_of(write);
        const std::string& writer = write.mutators.empty() ? ids[slot] : write.mutators.front();
        state.writers.insert_or_assign(id, writer);
        const auto locker = state.locks.find(id);
        if (locker == state.locks.end() || locker->second == writer)
            continue;
        bool named = false;
        for (std::size_t seen = notes_before; seen < notes.size(); ++seen) {
            const HookNote& earlier = notes[seen];
            if (earlier.kind == HookNoteKind::hook_overwrote &&
                same_path(earlier.path, write.path) && same_text(earlier.record, write.record) &&
                same_text(earlier.key, write.key) && note_names(earlier, locker->second)) {
                named = true;
                break;
            }
        }
        if (named)
            continue;
        HookNote note;
        note.kind = HookNoteKind::hook_overwrote;
        note.mutators.push_back(locker->second);
        note.mutators.push_back(writer);
        note.path = write.path;
        note.record = write.record;
        note.key = write.key;
        note.message = writer;
        note.message += " wrote a field ";
        note.message += locker->second;
        note.message += " had locked";
        report.conflicts.push_back(conflict_from_hook(note, ids));
    }
}

/// Fails every mutator still in the pass with an engine error.
void fail_every(
    std::vector<MutatorResult>& results,
    const std::vector<uint8_t>& active,
    Phase phase,
    const std::string& message
) {
    for (std::size_t slot = 0; slot < results.size() && slot < active.size(); ++slot) {
        if (active[slot] != 0)
            fail_mutator(results[slot], phase, Cause::engine_error, message);
    }
}

/// One pass over the mutators that are still applied.
///
/// A clean pass publishes its layer and returns true. A pass that finds a
/// failure records it, leaves the store unmounted and returns false. The
/// pass's documents are its own, parsed from the store, so nothing needs
/// rolling back: the next pass parses them again without what failed.
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
    // Destroyed in reverse order: every sandbox closes before the collections
    // that were pushed into it, the collections go before the documents, and
    // the listings the sandboxes read from go last.
    rules::DocumentSet documents;
    std::optional<Collections> collections;
    std::vector<Listing> listings(count);
    std::vector<std::vector<LoadoutOption>> options(count);
    std::vector<std::optional<host::Sandbox>> sandboxes(count);
    std::vector<Plan> plans(count);
    std::vector<uint8_t> active(count, 0);
    for (std::size_t slot = 0; slot < count; ++slot)
        active[slot] = results[slot].outcome == Outcome::applied ? 1 : 0;

    // A plan sees only its own package and options, so each plan failure is
    // that mutator's own, and every one is kept.
    bool planned = true;
    for (std::size_t slot = 0; slot < count; ++slot) {
        if (active[slot] == 0)
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
        if (results[slot].outcome != Outcome::applied)
            planned = false;
    }
    if (!planned)
        return false;

    std::vector<std::string> stems = unit_stems(store, layout);
    PathContext paths;
    paths.layout = &layout;
    paths.is_unit_name = [&stems](std::string_view name) {
        const std::string folded = fold_copy(name);
        return std::binary_search(stems.begin(), stems.end(), folded);
    };

    // A rule target is judged by its own folder and the mod's unit names, so
    // these failures are each mutator's own too.
    bool targets_allowed = true;
    for (std::size_t slot = 0; slot < count; ++slot) {
        if (active[slot] == 0 || !plans[slot].has_rules)
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
                targets_allowed = false;
                break;
            }
        }
    }
    if (!targets_allowed)
        return false;

    // From here a mutator's outcome can depend on the mutators above it: a
    // mask or a layer-wide limit in the files layer, or the documents its
    // hooks read. Only the first failure in list order is kept. A mutator the
    // files layer refused sits out the rest of the pass, so the mutators
    // above it are judged as they would be without it.
    FirstFailure first;
    std::vector<MutatorFiles> files;
    for (std::size_t slot = 0; slot < count; ++slot) {
        if (active[slot] == 0 || !plans[slot].has_files)
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
        first.offer(*slot, Phase::files, Cause::files_refused, failure.message);
        active[*slot] = 0;
    }
    std::vector<JournalEntry> masked = mask_entries(store, built.spec, ids);

    oa::MutatorLayerSpec spec = built.spec;
    if (!spec.entries.empty() || !spec.masks.empty() || !spec.archives.empty()) {
        std::string mount_error;
        if (!store.mount_mutator_layer(spec, &mount_error)) {
            fail_every(
                results,
                active,
                Phase::files,
                mount_error.empty() ? "the mutator layer could not be mounted" : mount_error
            );
            return false;
        }
        guard.arm();
    }

    std::vector<rules::MutatorRules> rule_sets;
    for (std::size_t slot = 0; slot < count; ++slot) {
        if (active[slot] != 0 && plans[slot].has_rules)
            rule_sets.push_back(plans[slot].rules);
    }
    parse_documents(documents, report, store, layout, input.variant, rule_sets);
    rules::ApplyOptions apply_options;
    apply_options.layout = &layout;
    apply_options.check = [&layout](
                              const rules::FieldRef& field, const std::optional<std::string>& text
                          ) { return check_field(field, text, layout); };
    rules::ApplyState apply_state;
    std::vector<rules::FieldWrite> rule_writes;
    std::set<ReplacedField> replaced_fields;

    // Each mutator applies its rules, then its units hook and its weapons
    // hook, on the documents the mutators above it left. A plan that returns
    // map hooks is refused, so none run. The first hook that fails ends the
    // pass: no later mutator's rules or hooks run on what it left. A mutator
    // below a failure the files layer found does not run either.
    collections.emplace(documents, &layout);
    bool steps_done = true;
    for (std::size_t slot = 0; slot < count && slot < first.slot; ++slot) {
        if (active[slot] == 0)
            continue;
        if (plans[slot].has_rules) {
            rules::ApplyResult step =
                rules::apply_step(plans[slot].rules, documents, apply_state, apply_options);
            for (rules::FieldWrite& write : step.writes)
                rule_writes.push_back(std::move(write));
            for (const rules::RuleNote& note : step.notes)
                append_rule_note(report, note, ids, replaced_fields);
        }
        if (plans[slot].units_hook == LUA_NOREF && plans[slot].weapons_hook == LUA_NOREF)
            continue;
        if (!sandboxes[slot].has_value())
            continue;
        const std::size_t notes_before = collections->notes().size();
        const std::size_t writes_before = collections->writes().size();
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
                *sandboxes[slot], *collections, plans[slot].units_hook, true, context, slot, first
            );
        }
        if (hook_ok && plans[slot].weapons_hook != LUA_NOREF) {
            hook_ok = call_hook(
                *sandboxes[slot],
                *collections,
                plans[slot].weapons_hook,
                false,
                context,
                slot,
                first
            );
        }
        if (!hook_ok) {
            steps_done = false;
            break;
        }
        collections->end_mutator();
        record_hook_turn(*collections, apply_state, report, ids, slot, notes_before, writes_before);
    }
    if (!steps_done) {
        sandboxes.clear();
        collections.reset();
        guard.release();
        fail_mutator(results[first.slot], first.phase, first.cause, std::move(first.message));
        return false;
    }

    std::vector<std::vector<Writer>> writers(documents.size());
    std::vector<uint8_t> menus(documents.size(), 0);
    for (const rules::FieldWrite& write : rule_writes) {
        journal.entries.push_back(entry_from_write(write, ids, Phase::rules, layout));
        for (const uint16_t mutator : journal.entries.back().mutators) {
            note_writer(
                documents,
                writers,
                menus,
                write.path,
                mutator,
                Phase::rules,
                journal.entries.back().collection
            );
        }
    }
    for (const rules::FieldWrite& write : collections->writes()) {
        journal.entries.push_back(entry_from_write(write, ids, Phase::hooks, layout));
        for (const uint16_t mutator : journal.entries.back().mutators) {
            note_writer(
                documents,
                writers,
                menus,
                write.path,
                mutator,
                Phase::hooks,
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
        if (mutator.has_value()) {
            note_writer(
                documents, writers, menus, removal.path, *mutator, Phase::hooks, entry.collection
            );
        }
        journal.entries.push_back(std::move(entry));
    }

    for (const oa::MutatorLayerEntry& entry : spec.entries) {
        const std::optional<uint16_t> mutator = index_of(ids, entry.owner);
        if (!mutator.has_value())
            continue;
        const Change change =
            entry.kind == oa::MutatorSourceKind::hidden ? Change::hidden : Change::added;
        journal.entries.push_back(file_entry(*mutator, entry.path, change, Phase::files));
    }
    for (JournalEntry& entry : masked)
        journal.entries.push_back(std::move(entry));
    for (const FileNote& note : built.notes) {
        report.conflicts.push_back(conflict_from_file(note, ids));
        if (note.kind != FileNoteKind::add_masked)
            continue;
        const std::optional<uint16_t> masker = index_of(ids, note.mutator);
        if (!masker.has_value())
            continue;
        JournalEntry hidden = file_entry(*masker, note.path, Change::hidden, Phase::files);
        const std::optional<uint16_t> adder = index_of(ids, note.other);
        if (adder.has_value())
            hidden.mutators.insert(hidden.mutators.begin(), *adder);
        journal.entries.push_back(std::move(hidden));
    }

    for (std::size_t slot = 0; slot < documents.size(); ++slot) {
        if (!documents.changed(slot))
            continue;
        const rules::LoadedDocument& loaded = documents.at(slot);
        if (writers[slot].empty()) {
            for (std::size_t candidate = 0; candidate < count; ++candidate) {
                if (active[candidate] == 0)
                    continue;
                first.offer(
                    candidate, Phase::hooks, Cause::engine_error, "a changed document has no writer"
                );
                break;
            }
            continue;
        }
        const PathVerdict verdict = check_mutable_path(loaded.path, paths);
        const bool menus_pass = verdict.rule == PathRule::menus_only && menus[slot] != 0;
        if (!verdict.allowed() && !menus_pass) {
            for (const Writer& writer : writers[slot])
                first.offer(writer.mutator, writer.phase, Cause::protected_path, verdict.message);
            continue;
        }
        const uint16_t owner = owner_of(writers[slot]);
        const Phase owner_phase = phase_of(writers[slot], owner);
        oa::MutatorLayerEntry entry;
        entry.path = loaded.path;
        entry.owner = ids[owner];
        if (loaded.removed) {
            entry.kind = oa::MutatorSourceKind::hidden;
            journal.entries.push_back(file_entry(owner, loaded.path, Change::hidden, owner_phase));
        } else {
            const std::optional<std::string> text = tdf::write_text(loaded.document.get());
            if (!text.has_value()) {
                first.offer(
                    owner,
                    owner_phase,
                    Cause::engine_error,
                    "a changed document could not be written"
                );
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
    if (first.found()) {
        guard.release();
        fail_mutator(results[first.slot], first.phase, first.cause, std::move(first.message));
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
        fail_every(
            results,
            active,
            Phase::files,
            publish_error.empty() ? "the mutator layer could not be mounted" : publish_error
        );
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
