// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Collections over a made-up Ridge corpus: reads, typed writes, selection,
// removal of units with their download entries, weapons, features, grants
// and the journal. Each Lua chunk is one file under tests/collections.

#include "oa/data/mutators/collections.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/formats/tdf.hpp"
#include "oa/test/check.hpp"

#include "lua.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef OA_MUTATORS_COLLECTIONS_DIR
#error OA_MUTATORS_COLLECTIONS_DIR is the fixture folder
#endif

namespace {

namespace rules = oa::data::mutation_rules;
namespace mutators = oa::data::mutators;
namespace host = oa::data::script_host;
namespace tdf = oa::formats::tdf;

constexpr mutators::WriteGrant k_all{true, true, true};
constexpr mutators::WriteGrant k_units{true, false, false};
constexpr mutators::WriteGrant k_weapons{false, true, false};
constexpr mutators::WriteGrant k_features{false, false, true};

constexpr std::string_view k_bare = R"([UNITINFO]
{
Name=RIDGEBARE;
MaxVelocity=1;
FootprintX=2;
}
)";

constexpr std::string_view k_com = R"([UNITINFO]
{
UnitName=RIDGECOM;
Name=RIDGECOM;
MaxVelocity=1.6;
CanFly=0;
WorkerTime=40;
MaxDamage=3000;
FootprintX=2;
Builder=1;
BMcode=1;
Weapon1=RIDGE_LASER;
AI_Weight=10;
RidgeTag=commander;
SoundCategory=CMDCK;
BankScale=1;
remove=9;
}
)";

constexpr std::string_view k_fab = R"([UNITINFO]
{
UnitName=RIDGEFAB;
Name=RIDGEFAB;
MaxVelocity=0;
CanFly=0;
Builder=1;
BMcode=0;
Weapon1=RIDGE_PEBBLE;
FootprintX=4;
}
)";

constexpr std::string_view k_jet = R"([UNITINFO]
{
UnitName=RIDGEJET;
Name=RIDGEJET;
MaxVelocity=10;
CanFly=1;
WorkerTime=5;
Builder=0;
Weapon1=RIDGE_LASER;
}
)";

constexpr std::string_view k_tank = R"([UNITINFO]
{
UnitName=RIDGETANK;
Name=RIDGETANK;
MaxVelocity=2;
CanFly=0;
BankScale=2;
Weapon1=RIDGE_CANNON;
SoundCategory=fast;
}
)";

constexpr std::string_view k_laser_a = R"([RIDGE_LASER]
{
Range=250;
}
)";

constexpr std::string_view k_laser_b = R"([RIDGE_LASER]
{
Range=10;
}
)";

constexpr std::string_view k_cannon = R"([RIDGE_CANNON]
{
Range=400;
}
[RIDGE_PEBBLE]
{
Range=80;
}
)";

constexpr std::string_view k_rock = R"([RIDGEROCK]
{
blocking=1;
}
)";

constexpr std::string_view k_menus = R"([MENU0]
{
UNITMENU=RIDGECOM;
UNITNAME=RIDGEJET;
}
[MENU1]
{
UNITNAME=RIDGETANK;
}
)";

constexpr std::string_view k_onlyjet = R"([ONLY]
{
UNITNAME=RIDGEJET;
}
)";

constexpr std::string_view k_builder = R"([MENU0]
{
UNITMENU=RIDGEJET;
UNITNAME=RIDGEFAB;
}
[MENU1]
{
UNITMENU=RIDGECOM;
UNITNAME=RIDGECOM;
}
)";

constexpr std::string_view k_case = R"([ONLY]
{
unitname=RidgeJet;
}
)";

constexpr std::string_view k_gui = R"([PAGE]
{
UNITNAME=RIDGEJET;
}
)";

constexpr std::string_view k_dup_weapon = R"([RIDGEDUP]
{
Range=1;
}
[RIDGEDUP]
{
Range=2;
}
)";

constexpr std::string_view k_dup_feature = R"([RIDGEDUP]
{
blocking=1;
}
[RIDGEDUP]
{
blocking=2;
}
)";

struct FileText {
    const char* path = nullptr;
    std::string_view text{};
};

const FileText k_files[] = {
    {"units/RIDGEBARE.fbi", k_bare},
    {"units/RIDGECOM.fbi", k_com},
    {"units/RIDGEFAB.fbi", k_fab},
    {"units/RIDGEJET.fbi", k_jet},
    {"units/RIDGETANK.fbi", k_tank},
    {"Weapons/a_laser.tdf", k_laser_a},
    {"Weapons/b_laser.tdf", k_laser_b},
    {"Weapons/cannon.tdf", k_cannon},
    {"features/rocks/ridge.tdf", k_rock},
    {"download/menus.tdf", k_menus},
    {"download/onlyjet.tdf", k_onlyjet},
    {"download/builder.tdf", k_builder},
    {"download/case.tdf", k_case},
    {"guis/ridge.tdf", k_gui},
};

struct Outcome {
    bool ok = false;
    host::ScriptError error;
};

struct Bind {
    mutators::Collections* collections = nullptr;
};

struct World {
    rules::DocumentSet documents;
    mutators::Collections collections;
    std::optional<host::Sandbox> sandbox;

    World() : collections(documents) {}
};

std::string& fixture_dir() {
    static std::string dir{OA_MUTATORS_COLLECTIONS_DIR};
    return dir;
}

bool read_fixture(void* context, std::string_view path, std::vector<uint8_t>& bytes) {
    if (context == nullptr || path.empty() || path.find('/') != std::string_view::npos ||
        path.find('\\') != std::string_view::npos || path.find("..") != std::string_view::npos)
        return false;
    const auto* root = static_cast<const std::string*>(context);
    const std::filesystem::path file = std::filesystem::path(*root) / std::string(path);
    std::ifstream input(file, std::ios::binary);
    if (!input)
        return false;
    input.seekg(0, std::ios::end);
    const std::streamoff end = input.tellg();
    if (end < 0)
        return false;
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(end));
    if (end > 0 && !input.read(reinterpret_cast<char*>(bytes.data()), end))
        return false;
    return true;
}

extern "C" {

static int host_collect(lua_State* state) {
    lua_gc(state, LUA_GCCOLLECT, 0);
    return 0;
}

} // extern "C"

void install_globals(void* context, lua_State* state) {
    auto* bind = static_cast<Bind*>(context);
    mutators::install_selection_forms(state);
    bind->collections->push_units(state);
    lua_setglobal(state, "units");
    bind->collections->push_weapons(state);
    lua_setglobal(state, "weapons");
    bind->collections->push_features(state);
    lua_setglobal(state, "features");
    lua_newtable(state);
    bind->collections->set_context_collections(state, -1);
    lua_setglobal(state, "ctx");
    // The host leaves this out. The weak-key fixture needs one full collection.
    lua_pushcfunction(state, host_collect);
    lua_setglobal(state, "collectgarbage");
}

bool load_ridge(rules::DocumentSet& documents) {
    for (const FileText& file : k_files) {
        std::string error;
        if (!documents.add(file.path, file.text, &error)) {
            std::fprintf(stderr, "%s: %s\n", file.path, error.c_str());
            return false;
        }
    }
    return true;
}

bool open_sandbox(World& world, host::Profile profile) {
    host::OpenOptions options;
    options.package_id = "ridge";
    options.profile = std::move(profile);
    options.sources.context = &fixture_dir();
    options.sources.read = read_fixture;
    host::ScriptError open_error;
    world.sandbox = host::Sandbox::open(options, open_error);
    if (!world.sandbox.has_value()) {
        std::fprintf(stderr, "open: %s\n", open_error.message.c_str());
        return false;
    }
    Bind bind;
    bind.collections = &world.collections;
    host::ScriptError install_error;
    if (!world.sandbox->run_host("install", &bind, install_globals, install_error)) {
        std::fprintf(stderr, "install: %s\n", install_error.message.c_str());
        return false;
    }
    return true;
}

bool prepare(World& world) {
    if (!load_ridge(world.documents))
        return false;
    return open_sandbox(world, host::mutator_profile());
}

bool ready(World& world) {
    if (!prepare(world)) {
        OA_CHECK(false);
        return false;
    }
    return true;
}

Outcome
play(World& world, const char* mutator, mutators::WriteGrant grant, bool begin, const char* file) {
    Outcome outcome;
    if (!world.sandbox.has_value()) {
        outcome.error.message = "no sandbox";
        return outcome;
    }
    if (begin)
        world.collections.begin_mutator(mutator, grant);
    outcome.ok = world.sandbox->run_file(file, file, nullptr, outcome.error);
    if (begin)
        world.collections.end_mutator();
    return outcome;
}

void expect_ok(World& world, const char* file, mutators::WriteGrant grant) {
    const Outcome outcome = play(world, "ridge", grant, true, file);
    if (!outcome.ok)
        std::fprintf(stderr, "%s: %s\n", file, outcome.error.message.c_str());
    OA_CHECK(outcome.ok);
}

void expect_fail(
    World& world,
    const char* file,
    mutators::WriteGrant grant,
    bool begin,
    int line,
    const char* part
) {
    const Outcome outcome = play(world, "ridge", grant, begin, file);
    if (outcome.ok)
        std::fprintf(stderr, "%s succeeded\n", file);
    OA_CHECK(!outcome.ok);
    if (outcome.error.chunk != file)
        std::fprintf(stderr, "%s chunk '%s'\n", file, outcome.error.chunk.c_str());
    OA_CHECK(outcome.error.chunk == file);
    if (outcome.error.line != line)
        std::fprintf(
            stderr,
            "%s line %d message '%s'\n",
            file,
            outcome.error.line,
            outcome.error.message.c_str()
        );
    OA_CHECK(outcome.error.line == line);
    if (outcome.error.message.find(part) == std::string::npos)
        std::fprintf(stderr, "%s message '%s'\n", file, outcome.error.message.c_str());
    OA_CHECK(outcome.error.message.find(part) != std::string::npos);
}

const tdf::Block*
section_of(rules::DocumentSet& documents, std::string_view path, std::string_view section) {
    rules::LoadedDocument* loaded = documents.find(path);
    if (loaded == nullptr)
        return nullptr;
    const std::string name(section);
    return tdf::find_child(loaded->document.root(), name.c_str());
}

const char* field_of(
    rules::DocumentSet& documents,
    std::string_view path,
    std::string_view section,
    std::string_view key
) {
    const std::string key_name(key);
    return tdf::find_value(section_of(documents, path, section), key_name.c_str());
}

bool removed_flag(rules::DocumentSet& documents, std::string_view path) {
    const rules::LoadedDocument* loaded = documents.find(path);
    return loaded != nullptr && loaded->removed;
}

uint32_t section_count(rules::DocumentSet& documents, std::string_view path) {
    const rules::LoadedDocument* loaded = documents.find(path);
    if (loaded == nullptr || loaded->document.root() == nullptr)
        return 0;
    return tdf::child_count(loaded->document.root());
}

bool any_changed(const rules::DocumentSet& documents) {
    for (std::size_t position = 0; position < documents.size(); ++position) {
        if (documents.changed(position))
            return true;
    }
    return false;
}

void expect_text(const char* got, const char* want, const char* label) {
    const bool match = (got == nullptr && want == nullptr) ||
                       (got != nullptr && want != nullptr && std::string_view(got) == want);
    if (!match) {
        std::fprintf(
            stderr,
            "%s: got '%s' want '%s'\n",
            label,
            got != nullptr ? got : "<absent>",
            want != nullptr ? want : "<absent>"
        );
    }
    OA_CHECK(match);
}

std::optional<std::string> maybe(const char* value) {
    if (value == nullptr)
        return std::nullopt;
    return std::string(value);
}

void expect_write(
    const mutators::Collections& collections,
    std::size_t index,
    const char* path,
    const char* record,
    const char* block,
    const char* key,
    const char* old_text,
    const char* new_text
) {
    const auto writes = collections.writes();
    if (index >= writes.size()) {
        std::fprintf(stderr, "missing write %zu of %zu\n", index, writes.size());
        OA_CHECK(false);
        return;
    }
    const rules::FieldWrite& write = writes[index];
    if (write.path != path || write.record != record || write.block != block || write.key != key ||
        write.old_text != maybe(old_text) || write.new_text != maybe(new_text) ||
        write.mutators.size() != 1 || write.mutators[0] != "ridge") {
        const char* old_shown = write.old_text.has_value() ? write.old_text->c_str() : "<absent>";
        const char* new_shown = write.new_text.has_value() ? write.new_text->c_str() : "<absent>";
        std::fprintf(
            stderr,
            "write %zu: %s %s block '%s' key '%s' old '%s' new '%s'\n",
            index,
            write.path.c_str(),
            write.record.c_str(),
            write.block.c_str(),
            write.key.c_str(),
            old_shown,
            new_shown
        );
    }
    OA_CHECK(write.path == path);
    OA_CHECK(write.record == record);
    OA_CHECK(write.block == block);
    OA_CHECK(write.key == key);
    OA_CHECK(write.old_text == maybe(old_text));
    OA_CHECK(write.new_text == maybe(new_text));
    OA_CHECK(write.mutators.size() == 1);
    if (!write.mutators.empty())
        OA_CHECK(write.mutators[0] == "ridge");
}

void expect_removal(
    const mutators::Collections& collections,
    std::size_t index,
    mutators::RemovalKind kind,
    const char* name,
    const char* path
) {
    const auto removals = collections.removals();
    if (index >= removals.size()) {
        std::fprintf(stderr, "missing removal %zu of %zu\n", index, removals.size());
        OA_CHECK(false);
        return;
    }
    const mutators::RecordRemoval& removal = removals[index];
    if (removal.kind != kind || removal.name != name || removal.path != path ||
        removal.mutator != "ridge") {
        std::fprintf(
            stderr,
            "removal %zu: kind %u name '%s' path '%s' mutator '%s'\n",
            index,
            static_cast<unsigned>(removal.kind),
            removal.name.c_str(),
            removal.path.c_str(),
            removal.mutator.c_str()
        );
    }
    OA_CHECK(removal.kind == kind);
    OA_CHECK(removal.name == name);
    OA_CHECK(removal.path == path);
    OA_CHECK(removal.mutator == "ridge");
}

bool same_letters(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        unsigned char left_byte = static_cast<unsigned char>(left[index]);
        unsigned char right_byte = static_cast<unsigned char>(right[index]);
        if (left_byte >= 'A' && left_byte <= 'Z')
            left_byte = static_cast<unsigned char>(left_byte - 'A' + 'a');
        if (right_byte >= 'A' && right_byte <= 'Z')
            right_byte = static_cast<unsigned char>(right_byte - 'A' + 'a');
        if (left_byte != right_byte)
            return false;
    }
    return true;
}

const char* stored_key(const tdf::Block* block, std::string_view key) {
    if (block == nullptr)
        return nullptr;
    const uint32_t count = tdf::property_count(block);
    for (uint32_t index = 0; index < count; ++index) {
        const char* stored = tdf::property_key_at(block, static_cast<int32_t>(index));
        if (stored != nullptr && same_letters(stored, key))
            return stored;
    }
    return nullptr;
}

void expect_jet_removed(World& world) {
    OA_CHECK(removed_flag(world.documents, "units/RIDGEJET.fbi"));
    OA_CHECK(!removed_flag(world.documents, "download/menus.tdf"));
    OA_CHECK(removed_flag(world.documents, "download/onlyjet.tdf"));
    OA_CHECK(!removed_flag(world.documents, "download/builder.tdf"));
    OA_CHECK(removed_flag(world.documents, "download/case.tdf"));
    OA_CHECK(!removed_flag(world.documents, "guis/ridge.tdf"));
    expect_text(
        field_of(world.documents, "guis/ridge.tdf", "PAGE", "UNITNAME"), "RIDGEJET", "guis"
    );
    expect_text(
        field_of(world.documents, "download/menus.tdf", "MENU1", "UNITNAME"), "RIDGETANK", "menus"
    );
    OA_CHECK(section_of(world.documents, "download/menus.tdf", "MENU0") == nullptr);
    expect_text(
        field_of(world.documents, "download/builder.tdf", "MENU1", "UNITNAME"),
        "RIDGECOM",
        "builder"
    );
    OA_CHECK(section_of(world.documents, "download/builder.tdf", "MENU0") == nullptr);
    OA_CHECK(world.collections.removals().size() == 7);
    expect_removal(
        world.collections, 0, mutators::RemovalKind::unit, "RIDGEJET", "units/RIDGEJET.fbi"
    );
    expect_removal(
        world.collections, 1, mutators::RemovalKind::download_entry, "MENU0", "download/menus.tdf"
    );
    expect_removal(
        world.collections, 2, mutators::RemovalKind::download_entry, "ONLY", "download/onlyjet.tdf"
    );
    expect_removal(
        world.collections, 3, mutators::RemovalKind::document, "onlyjet.tdf", "download/onlyjet.tdf"
    );
    expect_removal(
        world.collections, 4, mutators::RemovalKind::download_entry, "MENU0", "download/builder.tdf"
    );
    expect_removal(
        world.collections, 5, mutators::RemovalKind::download_entry, "ONLY", "download/case.tdf"
    );
    expect_removal(
        world.collections, 6, mutators::RemovalKind::document, "case.tdf", "download/case.tdf"
    );
    for (const mutators::RecordRemoval& removal : world.collections.removals()) {
        OA_CHECK(removal.path.find("guis/") != 0);
        const bool unit_file =
            removal.path == "units/RIDGEJET.fbi" && removal.kind == mutators::RemovalKind::document;
        OA_CHECK(!unit_file);
    }
    if (world.collections.notes().size() != 2) {
        OA_CHECK(false);
        return;
    }
    const auto notes = world.collections.notes();
    OA_CHECK(notes[0].kind == mutators::HookNoteKind::download_first_entry_changed);
    OA_CHECK(notes[0].path == "download/menus.tdf");
    OA_CHECK(notes[0].record == "RIDGEJET");
    OA_CHECK(notes[0].key == "UNITNAME");
    OA_CHECK(notes[0].message.find("RIDGETANK") != std::string::npos);
    OA_CHECK(notes[1].kind == mutators::HookNoteKind::download_first_entry_changed);
    OA_CHECK(notes[1].path == "download/builder.tdf");
    OA_CHECK(notes[1].message.find("RIDGECOM") != std::string::npos);
}

void test_reads() {
    World world;
    if (!ready(world))
        return;
    expect_ok(world, "reads.lua", k_all);
    OA_CHECK(world.collections.writes().empty());
    OA_CHECK(world.collections.removals().empty());
}

void test_writes() {
    World world;
    if (!ready(world))
        return;
    expect_ok(world, "writes.lua", k_units);
    OA_CHECK(world.collections.writes().size() == 5);
    expect_write(
        world.collections, 0, "units/RIDGECOM.fbi", "RIDGECOM", "", "MaxVelocity", "1.6", "3.2"
    );
    expect_write(
        world.collections, 1, "units/RIDGECOM.fbi", "RIDGECOM", "", "AI_Weight", "10", nullptr
    );
    expect_write(
        world.collections, 2, "units/RIDGEJET.fbi", "RIDGEJET", "", "NewRidge", nullptr, "kept"
    );
    expect_write(
        world.collections, 3, "units/RIDGECOM.fbi", "RIDGECOM", "", "MaxDamage", "3000", "40000"
    );
    expect_write(
        world.collections, 4, "units/RIDGEBARE.fbi", "RIDGEBARE", "", "canfly", nullptr, "1"
    );
    if (world.collections.notes().size() != 1) {
        OA_CHECK(false);
        return;
    }
    const mutators::HookNote& note = world.collections.notes()[0];
    OA_CHECK(note.kind == mutators::HookNoteKind::check_warned);
    OA_CHECK(note.record == "RIDGECOM");
    OA_CHECK(note.key == "MaxDamage");
    OA_CHECK(note.message.find("32767") != std::string::npos);
    OA_CHECK(note.mutators.size() == 1);
    OA_CHECK(note.mutators[0] == "ridge");
    expect_text(
        stored_key(section_of(world.documents, "units/RIDGEJET.fbi", "UNITINFO"), "NewRidge"),
        "NewRidge",
        "spelling"
    );
    expect_text(
        field_of(world.documents, "units/RIDGECOM.fbi", "UNITINFO", "AI_Weight"), nullptr, "weight"
    );
}

void test_write_failures() {
    World footprint;
    if (!ready(footprint))
        return;
    expect_fail(footprint, "writes_footprint.lua", k_units, true, 4, "RIDGECOM footprintx");
    OA_CHECK(footprint.collections.writes().empty());
    expect_text(
        field_of(footprint.documents, "units/RIDGECOM.fbi", "UNITINFO", "FootprintX"), "2", "foot"
    );

    World infinity;
    if (!ready(infinity))
        return;
    expect_fail(infinity, "writes_infinity.lua", k_units, true, 4, "not a finite number");
    OA_CHECK(infinity.collections.writes().empty());
    expect_text(
        field_of(infinity.documents, "units/RIDGECOM.fbi", "UNITINFO", "MaxVelocity"),
        "1.6",
        "speed"
    );

    World semi;
    if (!ready(semi))
        return;
    expect_fail(semi, "writes_semicolon.lua", k_units, true, 4, "semicolon");
    OA_CHECK(semi.collections.writes().empty());
    expect_text(
        field_of(semi.documents, "units/RIDGECOM.fbi", "UNITINFO", "SoundCategory"),
        "CMDCK",
        "sound"
    );
}

void test_methods() {
    World world;
    if (!ready(world))
        return;
    expect_ok(world, "methods.lua", k_units);
    expect_text(
        field_of(world.documents, "units/RIDGECOM.fbi", "UNITINFO", "remove"), "8", "remove"
    );
    expect_text(
        stored_key(section_of(world.documents, "units/RIDGECOM.fbi", "UNITINFO"), "remove"),
        "remove",
        "spelling"
    );
    OA_CHECK(world.collections.notes().empty());
}

void test_where() {
    World world;
    if (!ready(world))
        return;
    expect_ok(world, "where.lua", k_all);
    OA_CHECK(world.collections.writes().empty());
    OA_CHECK(world.collections.removals().empty());
}

void test_scale_and_set() {
    World scale;
    if (!ready(scale))
        return;
    expect_ok(scale, "scale.lua", k_units);
    OA_CHECK(scale.collections.writes().size() == 4);
    expect_write(
        scale.collections, 0, "units/RIDGECOM.fbi", "RIDGECOM", "", "MaxVelocity", "1.6", "3.2"
    );
    expect_write(
        scale.collections, 1, "units/RIDGEJET.fbi", "RIDGEJET", "", "MaxVelocity", "10", "20"
    );
    expect_write(
        scale.collections, 2, "units/RIDGECOM.fbi", "RIDGECOM", "", "WorkerTime", "40", "120"
    );
    expect_write(
        scale.collections, 3, "units/RIDGEJET.fbi", "RIDGEJET", "", "WorkerTime", "5", "15"
    );
    if (scale.collections.notes().size() != 1) {
        OA_CHECK(false);
        return;
    }
    const mutators::HookNote& note = scale.collections.notes()[0];
    OA_CHECK(note.kind == mutators::HookNoteKind::not_a_number);
    OA_CHECK(note.record == "RIDGECOM");
    OA_CHECK(note.key == "SoundCategory");
    OA_CHECK(note.message == "the field text is not a number");
    expect_text(
        field_of(scale.documents, "units/RIDGEJET.fbi", "UNITINFO", "BankScale"), nullptr, "bank"
    );

    World set;
    if (!ready(set))
        return;
    expect_ok(set, "set_order.lua", k_units);
    OA_CHECK(set.collections.writes().size() == 4);
    expect_write(
        set.collections, 0, "units/RIDGECOM.fbi", "RIDGECOM", "", "MaxVelocity", "1.6", "4"
    );
    expect_write(
        set.collections, 1, "units/RIDGEJET.fbi", "RIDGEJET", "", "MaxVelocity", "10", "4"
    );
    expect_write(set.collections, 2, "units/RIDGECOM.fbi", "RIDGECOM", "", "WorkerTime", "40", "7");
    expect_write(set.collections, 3, "units/RIDGEJET.fbi", "RIDGEJET", "", "WorkerTime", "5", "7");
    OA_CHECK(set.collections.notes().empty());
}

void test_snapshot() {
    World world;
    if (!ready(world))
        return;
    expect_ok(world, "snapshot.lua", k_units);
    expect_text(
        field_of(world.documents, "units/RIDGECOM.fbi", "UNITINFO", "MaxVelocity"), "0", "com"
    );
    expect_text(
        field_of(world.documents, "units/RIDGECOM.fbi", "UNITINFO", "FootprintX"), "2", "com foot"
    );
    expect_text(
        field_of(world.documents, "units/RIDGEFAB.fbi", "UNITINFO", "FootprintX"), "3", "fab foot"
    );
}

void test_remove_set() {
    World world;
    if (!ready(world))
        return;
    expect_ok(world, "remove_set.lua", k_units);
    OA_CHECK(removed_flag(world.documents, "units/RIDGEFAB.fbi"));
    OA_CHECK(world.collections.removals().size() == 2);
    expect_removal(
        world.collections, 0, mutators::RemovalKind::unit, "RIDGEFAB", "units/RIDGEFAB.fbi"
    );
    expect_removal(
        world.collections, 1, mutators::RemovalKind::download_entry, "MENU0", "download/builder.tdf"
    );
    if (world.collections.notes().size() != 1) {
        OA_CHECK(false);
        return;
    }
    OA_CHECK(
        world.collections.notes()[0].kind == mutators::HookNoteKind::download_first_entry_changed
    );
    OA_CHECK(world.collections.notes()[0].message.find("RIDGECOM") != std::string::npos);
    OA_CHECK(!removed_flag(world.documents, "guis/ridge.tdf"));
}

void test_remove_unit() {
    World raised;
    if (!ready(raised))
        return;
    expect_fail(raised, "remove_unit.lua", k_units, true, 7, "RIDGEJET was removed");
    expect_jet_removed(raised);
    OA_CHECK(raised.collections.writes().empty());

    World flying;
    if (!ready(flying))
        return;
    expect_ok(flying, "flying.lua", k_units);
    expect_jet_removed(flying);

    World stale;
    if (!ready(stale))
        return;
    expect_fail(stale, "stale.lua", k_units, true, 7, "RIDGEJET was removed");
    expect_jet_removed(stale);
    OA_CHECK(stale.collections.writes().empty());
    expect_text(
        field_of(stale.documents, "units/RIDGEJET.fbi", "UNITINFO", "MaxVelocity"), "10", "stale"
    );
}

void test_cross() {
    World world;
    if (!ready(world))
        return;
    expect_ok(world, "cross.lua", k_units);
    OA_CHECK(world.collections.writes().empty());
}

void test_weapons() {
    World world;
    if (!ready(world))
        return;
    expect_ok(world, "weapons.lua", k_weapons);
    OA_CHECK(world.collections.writes().size() == 2);
    expect_write(
        world.collections,
        0,
        "Weapons/a_laser.tdf",
        "RIDGE_LASER",
        "DAMAGE",
        "default",
        nullptr,
        "5"
    );
    expect_write(
        world.collections,
        1,
        "Weapons/a_laser.tdf",
        "RIDGE_LASER",
        "DAMAGE",
        "RIDGECOM",
        nullptr,
        "40000"
    );
    if (world.collections.notes().size() != 1) {
        OA_CHECK(false);
        return;
    }
    OA_CHECK(world.collections.notes()[0].kind == mutators::HookNoteKind::check_warned);
    OA_CHECK(world.collections.notes()[0].key == "RIDGECOM");
    OA_CHECK(world.collections.notes()[0].message.find("32767") != std::string::npos);
    const tdf::Block* damage = tdf::find_child(
        section_of(world.documents, "Weapons/a_laser.tdf", "RIDGE_LASER"), "DAMAGE"
    );
    OA_CHECK(damage != nullptr);
    expect_text(tdf::find_value(damage, "default"), "5", "default");
    expect_text(tdf::find_value(damage, "RIDGECOM"), "40000", "unit damage");
    OA_CHECK(
        tdf::find_child(
            section_of(world.documents, "Weapons/b_laser.tdf", "RIDGE_LASER"), "DAMAGE"
        ) == nullptr
    );
}

void test_weapon_removal() {
    World laser;
    if (!ready(laser))
        return;
    expect_fail(laser, "remove_laser.lua", k_weapons, true, 8, "RIDGE_LASER was removed");
    OA_CHECK(removed_flag(laser.documents, "Weapons/a_laser.tdf"));
    OA_CHECK(!removed_flag(laser.documents, "Weapons/b_laser.tdf"));
    expect_text(
        field_of(laser.documents, "Weapons/b_laser.tdf", "RIDGE_LASER", "Range"), "10", "other"
    );
    OA_CHECK(laser.collections.removals().size() == 2);
    expect_removal(
        laser.collections, 0, mutators::RemovalKind::weapon, "RIDGE_LASER", "Weapons/a_laser.tdf"
    );
    expect_removal(
        laser.collections, 1, mutators::RemovalKind::document, "a_laser.tdf", "Weapons/a_laser.tdf"
    );

    World pebble;
    if (!ready(pebble))
        return;
    expect_ok(pebble, "remove_pebble.lua", k_weapons);
    OA_CHECK(!removed_flag(pebble.documents, "Weapons/cannon.tdf"));
    OA_CHECK(pebble.collections.removals().size() == 1);
    expect_removal(
        pebble.collections, 0, mutators::RemovalKind::weapon, "RIDGE_PEBBLE", "Weapons/cannon.tdf"
    );
    expect_text(
        field_of(pebble.documents, "Weapons/cannon.tdf", "RIDGE_CANNON", "Range"), "400", "cannon"
    );
    OA_CHECK(section_of(pebble.documents, "Weapons/cannon.tdf", "RIDGE_PEBBLE") == nullptr);
}

void test_features() {
    World world;
    if (!ready(world))
        return;
    expect_ok(world, "features.lua", k_features);
    OA_CHECK(world.collections.writes().size() == 1);
    expect_write(
        world.collections, 0, "features/rocks/ridge.tdf", "RIDGEROCK", "", "blocking", "1", "0"
    );
    expect_text(
        field_of(world.documents, "features/rocks/ridge.tdf", "RIDGEROCK", "blocking"),
        "0",
        "blocking"
    );

    World removed;
    if (!ready(removed))
        return;
    expect_fail(removed, "remove_feature.lua", k_features, true, 7, "RIDGEROCK was removed");
    OA_CHECK(removed_flag(removed.documents, "features/rocks/ridge.tdf"));
    OA_CHECK(removed.collections.removals().size() == 2);
    expect_removal(
        removed.collections,
        0,
        mutators::RemovalKind::feature,
        "RIDGEROCK",
        "features/rocks/ridge.tdf"
    );
    expect_removal(
        removed.collections,
        1,
        mutators::RemovalKind::document,
        "ridge.tdf",
        "features/rocks/ridge.tdf"
    );
}

void test_grants() {
    World reading;
    if (!ready(reading))
        return;
    expect_ok(reading, "grant_read.lua", k_units);
    expect_text(
        field_of(reading.documents, "units/RIDGECOM.fbi", "UNITINFO", "MaxVelocity"), "2", "read"
    );

    World weapon;
    if (!ready(weapon))
        return;
    expect_fail(weapon, "grant_weapon.lua", k_units, true, 4, "does not declare the weapons layer");
    OA_CHECK(weapon.collections.writes().empty());
    expect_text(
        field_of(weapon.documents, "Weapons/a_laser.tdf", "RIDGE_LASER", "Range"), "250", "range"
    );

    World feature;
    if (!ready(feature))
        return;
    expect_fail(
        feature, "grant_feature.lua", k_units, true, 4, "does not declare the features layer"
    );
    OA_CHECK(feature.collections.writes().empty());
    expect_text(
        field_of(feature.documents, "features/rocks/ridge.tdf", "RIDGEROCK", "blocking"),
        "1",
        "block"
    );

    World unit;
    if (!ready(unit))
        return;
    expect_fail(unit, "grant_unit.lua", k_features, true, 4, "does not declare the units layer");
    OA_CHECK(unit.collections.writes().empty());
    expect_text(field_of(unit.documents, "units/RIDGECOM.fbi", "UNITINFO", "CanFly"), "0", "fly");

    World both;
    if (!ready(both))
        return;
    expect_fail(
        both, "feature_then_unit.lua", k_features, true, 5, "does not declare the units layer"
    );
    expect_text(
        field_of(both.documents, "features/rocks/ridge.tdf", "RIDGEROCK", "blocking"), "0", "wrote"
    );
    expect_text(
        field_of(both.documents, "units/RIDGECOM.fbi", "UNITINFO", "MaxVelocity"), "1.6", "kept"
    );
    OA_CHECK(both.collections.writes().size() == 1);

    World none;
    if (!ready(none))
        return;
    expect_fail(none, "nomutator.lua", k_all, false, 4, "no mutator has begun");
    OA_CHECK(none.collections.writes().empty());
    expect_text(
        field_of(none.documents, "units/RIDGECOM.fbi", "UNITINFO", "MaxVelocity"), "1.6", "none"
    );
}

void test_journal() {
    World twice_a;
    if (!ready(twice_a))
        return;
    expect_ok(twice_a, "writes.lua", k_units);
    World twice_b;
    if (!ready(twice_b))
        return;
    expect_ok(twice_b, "writes.lua", k_units);
    OA_CHECK(twice_a.collections.writes().size() == twice_b.collections.writes().size());
    OA_CHECK(twice_a.collections.notes().size() == twice_b.collections.notes().size());
    const auto left = twice_a.collections.writes();
    const auto right = twice_b.collections.writes();
    for (std::size_t index = 0; index < left.size() && index < right.size(); ++index) {
        OA_CHECK(left[index].mutators == right[index].mutators);
        OA_CHECK(left[index].path == right[index].path);
        OA_CHECK(left[index].record == right[index].record);
        OA_CHECK(left[index].block == right[index].block);
        OA_CHECK(left[index].key == right[index].key);
        OA_CHECK(left[index].old_text == right[index].old_text);
        OA_CHECK(left[index].new_text == right[index].new_text);
    }

    World same;
    if (!ready(same))
        return;
    expect_ok(same, "same_mutator.lua", k_units);
    OA_CHECK(same.collections.writes().size() == 2);
    expect_write(
        same.collections, 0, "units/RIDGECOM.fbi", "RIDGECOM", "", "MaxVelocity", "1.6", "4"
    );
    expect_write(
        same.collections, 1, "units/RIDGECOM.fbi", "RIDGECOM", "", "MaxVelocity", "4", "5"
    );
    OA_CHECK(same.collections.notes().empty());

    World over;
    if (!ready(over))
        return;
    const Outcome first = play(over, "ridge-a", k_units, true, "overwrite_once.lua");
    OA_CHECK(first.ok);
    const Outcome second = play(over, "ridge-b", k_units, true, "overwrite_five.lua");
    OA_CHECK(second.ok);
    if (over.collections.writes().size() != 2 || over.collections.notes().size() != 1) {
        OA_CHECK(over.collections.writes().size() == 2);
        OA_CHECK(over.collections.notes().size() == 1);
        return;
    }
    OA_CHECK(over.collections.writes()[0].mutators.size() == 1);
    OA_CHECK(over.collections.writes()[0].mutators[0] == "ridge-a");
    OA_CHECK(over.collections.writes()[0].old_text == maybe("1.6"));
    OA_CHECK(over.collections.writes()[0].new_text == maybe("4"));
    OA_CHECK(over.collections.writes()[1].mutators[0] == "ridge-b");
    OA_CHECK(over.collections.writes()[1].old_text == maybe("4"));
    OA_CHECK(over.collections.writes()[1].new_text == maybe("5"));
    const mutators::HookNote& note = over.collections.notes()[0];
    OA_CHECK(note.kind == mutators::HookNoteKind::hook_overwrote);
    OA_CHECK(note.mutators.size() == 2);
    OA_CHECK(note.mutators[0] == "ridge-a");
    OA_CHECK(note.mutators[1] == "ridge-b");
    OA_CHECK(
        note.message.find("ridge-b replaced the value written by ridge-a") != std::string::npos
    );
    OA_CHECK(note.key == "MaxVelocity");
    OA_CHECK(note.record == "RIDGECOM");

    World echoed;
    if (!ready(echoed))
        return;
    const Outcome echo_first = play(echoed, "ridge-a", k_units, true, "overwrite_once.lua");
    OA_CHECK(echo_first.ok);
    const Outcome echo_second = play(echoed, "ridge-b", k_units, true, "overwrite_once.lua");
    OA_CHECK(echo_second.ok);
    OA_CHECK(echoed.collections.writes().size() == 1);
    OA_CHECK(echoed.collections.notes().empty());
    if (!echoed.collections.writes().empty())
        OA_CHECK(echoed.collections.writes()[0].new_text == maybe("4"));
    expect_text(
        field_of(echoed.documents, "units/RIDGECOM.fbi", "UNITINFO", "MaxVelocity"), "4", "echo"
    );

    World order;
    if (!ready(order))
        return;
    expect_ok(order, "order.lua", k_all);
}

void test_sandbox() {
    World world;
    if (!ready(world))
        return;
    expect_ok(world, "sandbox.lua", k_all);
    OA_CHECK(world.collections.writes().empty());

    World raised;
    if (!ready(raised))
        return;
    expect_fail(raised, "sandbox_raise.lua", k_all, true, 4, "a collection cannot be assigned");

    World doubled;
    if (!ready(doubled))
        return;
    expect_fail(doubled, "not_double.lua", k_all, true, 4, "not_ of a not_");

    World bad;
    if (!ready(bad))
        return;
    expect_fail(bad, "where_bad.lua", k_all, true, 4, "canfly is not a number");

    World typed;
    if (!ready(typed))
        return;
    expect_fail(typed, "where_type.lua", k_all, true, 4, "where expects a table");
}

void test_padded_names() {
    World footprint;
    if (!ready(footprint))
        return;
    expect_fail(footprint, "pad_footprint.lua", k_units, true, 4, "RIDGECOM footprintx");
    OA_CHECK(footprint.collections.writes().empty());
    OA_CHECK(footprint.collections.notes().empty());
    expect_text(
        field_of(footprint.documents, "units/RIDGECOM.fbi", "UNITINFO", "FootprintX"), "2", "padded"
    );

    World set_pad;
    if (!ready(set_pad))
        return;
    expect_fail(set_pad, "pad_set.lua", k_units, true, 4, "RIDGEBARE footprintx");
    OA_CHECK(set_pad.collections.writes().empty());
    expect_text(
        field_of(set_pad.documents, "units/RIDGEBARE.fbi", "UNITINFO", "FootprintX"), "2", "bare"
    );
    expect_text(
        field_of(set_pad.documents, "units/RIDGECOM.fbi", "UNITINFO", "FootprintX"), "2", "com"
    );

    World tabbed;
    if (!ready(tabbed))
        return;
    expect_fail(tabbed, "pad_tab.lua", k_units, true, 4, "RIDGECOM footprintz");
    OA_CHECK(tabbed.collections.writes().empty());
    expect_text(
        field_of(tabbed.documents, "units/RIDGECOM.fbi", "UNITINFO", "FootprintZ"), nullptr, "tab"
    );

    World damage;
    if (!ready(damage))
        return;
    expect_ok(damage, "pad_maxdamage.lua", k_units);
    OA_CHECK(damage.collections.writes().size() == 1);
    expect_write(
        damage.collections, 0, "units/RIDGECOM.fbi", "RIDGECOM", "", "MaxDamage", "3000", "40000"
    );
    if (damage.collections.notes().size() != 1) {
        OA_CHECK(false);
        return;
    }
    OA_CHECK(damage.collections.notes()[0].kind == mutators::HookNoteKind::check_warned);
    OA_CHECK(damage.collections.notes()[0].key == "MaxDamage");
    OA_CHECK(damage.collections.notes()[0].message.find("32767") != std::string::npos);

    World weapon;
    if (!ready(weapon))
        return;
    expect_ok(weapon, "pad_weapon.lua", k_weapons);
    OA_CHECK(weapon.collections.writes().size() == 1);
    expect_write(
        weapon.collections,
        0,
        "Weapons/a_laser.tdf",
        "RIDGE_LASER",
        "DAMAGE",
        "default",
        nullptr,
        "40000"
    );
    if (weapon.collections.notes().size() != 1) {
        OA_CHECK(false);
        return;
    }
    OA_CHECK(weapon.collections.notes()[0].kind == mutators::HookNoteKind::check_warned);
    OA_CHECK(weapon.collections.notes()[0].key == "default");
    OA_CHECK(weapon.collections.notes()[0].message.find("32767") != std::string::npos);
}

void test_unchanged_write() {
    World world;
    if (!ready(world))
        return;
    expect_ok(world, "noop.lua", k_units);
    OA_CHECK(world.collections.writes().empty());
    OA_CHECK(world.collections.notes().empty());
    OA_CHECK(!any_changed(world.documents));
    expect_text(
        field_of(world.documents, "units/RIDGEJET.fbi", "UNITINFO", "WorkerTime"), "5", "same"
    );
}

void test_identity() {
    World world;
    if (!load_ridge(world.documents)) {
        OA_CHECK(false);
        return;
    }
    std::string error;
    if (!world.documents.add("Weapons/dup.tdf", k_dup_weapon, &error) ||
        !world.documents.add("features/dups/ridge.tdf", k_dup_feature, &error)) {
        std::fprintf(stderr, "dup: %s\n", error.c_str());
        OA_CHECK(false);
        return;
    }
    if (!open_sandbox(world, host::mutator_profile())) {
        OA_CHECK(false);
        return;
    }
    expect_ok(world, "identity.lua", k_all);
    OA_CHECK(!removed_flag(world.documents, "Weapons/dup.tdf"));
    OA_CHECK(section_count(world.documents, "Weapons/dup.tdf") == 1);
    expect_text(field_of(world.documents, "Weapons/dup.tdf", "RIDGEDUP", "Range"), "7", "range");
    OA_CHECK(!removed_flag(world.documents, "features/dups/ridge.tdf"));
    OA_CHECK(section_count(world.documents, "features/dups/ridge.tdf") == 1);
    expect_text(
        field_of(world.documents, "features/dups/ridge.tdf", "RIDGEDUP", "blocking"), "9", "block"
    );
    OA_CHECK(!removed_flag(world.documents, "units/RIDGECOM.fbi"));
    expect_text(
        field_of(world.documents, "units/RIDGECOM.fbi", "UNITINFO", "UnitName"), "RIDGENEW", "name"
    );
    expect_text(
        field_of(world.documents, "units/RIDGECOM.fbi", "UNITINFO", "MaxVelocity"), "3", "speed"
    );
    OA_CHECK(world.collections.removals().size() == 2);
    expect_removal(
        world.collections, 0, mutators::RemovalKind::weapon, "RIDGEDUP", "Weapons/dup.tdf"
    );
    expect_removal(
        world.collections, 1, mutators::RemovalKind::feature, "RIDGEDUP", "features/dups/ridge.tdf"
    );
    OA_CHECK(world.collections.writes().size() == 4);
    expect_write(world.collections, 0, "Weapons/dup.tdf", "RIDGEDUP", "", "Range", "2", "7");
    expect_write(
        world.collections, 1, "features/dups/ridge.tdf", "RIDGEDUP", "", "blocking", "2", "9"
    );
    expect_write(
        world.collections,
        2,
        "units/RIDGECOM.fbi",
        "RIDGECOM",
        "",
        "UnitName",
        "RIDGECOM",
        "RIDGENEW"
    );
    expect_write(
        world.collections, 3, "units/RIDGECOM.fbi", "RIDGENEW", "", "MaxVelocity", "1.6", "3"
    );
}

void test_rename_lookups() {
    World world;
    if (!ready(world))
        return;
    expect_ok(world, "rename_lookups.lua", k_units);
    expect_text(
        field_of(world.documents, "units/RIDGECOM.fbi", "UNITINFO", "UnitName"), "RIDGENEW", "name"
    );
    OA_CHECK(!removed_flag(world.documents, "units/RIDGECOM.fbi"));
    OA_CHECK(removed_flag(world.documents, "units/RIDGEBARE.fbi"));
    // Five renames a round for forty rounds, then the last one.
    OA_CHECK(world.collections.writes().size() == 201);
    OA_CHECK(world.collections.removals().size() == 1);
    expect_removal(
        world.collections, 0, mutators::RemovalKind::unit, "RIDGEBARE", "units/RIDGEBARE.fbi"
    );
    expect_write(
        world.collections,
        0,
        "units/RIDGECOM.fbi",
        "RIDGECOM",
        "",
        "UnitName",
        "RIDGECOM",
        "RIDGEAAA"
    );
    expect_write(
        world.collections,
        200,
        "units/RIDGECOM.fbi",
        "RIDGEJEU",
        "",
        "unitname",
        "RIDGEJEU",
        "RIDGENEW"
    );
}

/// The place of a generated feature section in collection order, from 1.
///
/// Twenty files of fifty sections each name `RIDGEGENnn` with nn the section
/// modulo 25, so every name appears twice in every file. RIDGEROCK follows.
int generated_place(int file, int section) {
    const int name = section % 25;
    const int ordinal = section / 25;
    return name * 40 + file * 2 + ordinal + 1;
}

std::string two_digits(int number) {
    std::string text = std::to_string(number);
    if (text.size() < 2)
        text.insert(0, "0");
    return text;
}

void test_identity_walks() {
    World world;
    if (!load_ridge(world.documents)) {
        OA_CHECK(false);
        return;
    }
    for (int file = 0; file < 20; ++file) {
        std::string text;
        for (int section = 0; section < 50; ++section) {
            text.append("[RIDGEGEN");
            text.append(two_digits(section % 25));
            text.append("]\n{\nblocking=1;\n}\n");
        }
        const std::string path = "features/gen/ridge" + two_digits(file) + ".tdf";
        std::string error;
        if (!world.documents.add(path, text, &error)) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str());
            OA_CHECK(false);
            return;
        }
    }
    if (!open_sandbox(world, host::mutator_profile())) {
        OA_CHECK(false);
        return;
    }
    expect_ok(world, "identity_walks.lua", k_all);
    // 1001 tags, then the set and the write that puts tag 500 back.
    OA_CHECK(world.collections.writes().size() == 1003);
    // Every seventh of 1001 features, and the rock file the last of them emptied.
    OA_CHECK(world.collections.removals().size() == 144);
    OA_CHECK(removed_flag(world.documents, "features/rocks/ridge.tdf"));
    for (int file = 0; file < 20; ++file) {
        const std::string path = "features/gen/ridge" + two_digits(file) + ".tdf";
        const rules::LoadedDocument* loaded = world.documents.find(path);
        if (loaded == nullptr || loaded->document.root() == nullptr) {
            OA_CHECK(false);
            continue;
        }
        const tdf::Block* root = loaded->document.root();
        uint32_t child = 0;
        for (int section = 0; section < 50; ++section) {
            const int place = generated_place(file, section);
            if (place % 7 == 0)
                continue;
            const tdf::Block* block = tdf::child_at(root, child);
            ++child;
            const std::string want = std::to_string(place);
            expect_text(
                block != nullptr ? tdf::find_value(block, "tag") : nullptr, want.c_str(), "tag"
            );
        }
        OA_CHECK(tdf::child_count(root) == child);
    }
}

void test_walk_skips_removed() {
    World world;
    if (!ready(world))
        return;
    expect_ok(world, "each_remove.lua", k_all);
    OA_CHECK(removed_flag(world.documents, "units/RIDGETANK.fbi"));
    OA_CHECK(removed_flag(world.documents, "units/RIDGEJET.fbi"));
    OA_CHECK(!removed_flag(world.documents, "units/RIDGEBARE.fbi"));
}

void test_weak_need() {
    World world;
    if (!ready(world))
        return;
    expect_ok(world, "p10_weak.lua", k_all);
}

void test_fixed() {
    World reads;
    if (!ready(reads))
        return;
    expect_ok(reads, "fixed_read.lua", k_all);
    OA_CHECK(reads.collections.writes().empty());
    const tdf::Block* com = section_of(reads.documents, "units/RIDGECOM.fbi", "UNITINFO");
    OA_CHECK(com != nullptr);
    OA_CHECK(tdf::get_fixed(com, "MaxVelocity", 0) == 104857);

    World velocity;
    if (!ready(velocity))
        return;
    expect_ok(velocity, "fixed_velocity.lua", k_units);
    OA_CHECK(velocity.collections.writes().size() == 1);
    expect_write(
        velocity.collections,
        0,
        "units/RIDGECOM.fbi",
        "RIDGECOM",
        "",
        "MaxVelocity",
        "1.6",
        "2.3999786376953125"
    );
    expect_text(
        field_of(velocity.documents, "units/RIDGECOM.fbi", "UNITINFO", "MaxVelocity"),
        "2.3999786376953125",
        "speed"
    );

    World assign;
    if (!ready(assign))
        return;
    expect_ok(assign, "fixed_assign.lua", k_units);
    OA_CHECK(assign.collections.writes().size() == 2);
    expect_write(
        assign.collections, 0, "units/RIDGEJET.fbi", "RIDGEJET", "", "MaxVelocity", "10", "3"
    );
    expect_write(
        assign.collections, 1, "units/RIDGEJET.fbi", "RIDGEJET", "", "bankscale", nullptr, "0.5"
    );
    expect_text(
        field_of(assign.documents, "units/RIDGEJET.fbi", "UNITINFO", "MaxVelocity"), "3", "jet"
    );
    expect_text(
        field_of(assign.documents, "units/RIDGEJET.fbi", "UNITINFO", "bankscale"), "0.5", "bank"
    );
    expect_text(
        field_of(assign.documents, "units/RIDGECOM.fbi", "UNITINFO", "MaxVelocity"), "1.6", "kept"
    );

    World footprint;
    if (!ready(footprint))
        return;
    expect_fail(footprint, "fixed_footprint.lua", k_units, true, 4, "RIDGECOM footprintx");
    OA_CHECK(footprint.collections.writes().empty());
    expect_text(
        field_of(footprint.documents, "units/RIDGECOM.fbi", "UNITINFO", "FootprintX"), "2", "foot"
    );
}

void test_memory_cap() {
    World world;
    if (!load_ridge(world.documents)) {
        OA_CHECK(false);
        return;
    }
    host::Profile profile = host::mutator_profile();
    profile.budget.memory_bytes = std::size_t{8} << 20;
    // Index and field charges spend the default allowance before 8 MiB is held.
    profile.budget.instructions = uint64_t{1} << 40;
    if (!open_sandbox(world, profile)) {
        OA_CHECK(false);
        return;
    }
    const Outcome outcome = play(world, "ridge", k_units, true, "cap.lua");
    if (outcome.ok)
        std::fprintf(stderr, "cap.lua succeeded\n");
    OA_CHECK(!outcome.ok);
    if (outcome.error.kind != host::ErrorKind::memory_budget) {
        std::fprintf(
            stderr,
            "cap kind %u message '%s'\n",
            static_cast<unsigned>(outcome.error.kind),
            outcome.error.message.c_str()
        );
    }
    OA_CHECK(outcome.error.kind == host::ErrorKind::memory_budget);
}

} // namespace

int main() {
    test_reads();
    test_writes();
    test_write_failures();
    test_methods();
    test_where();
    test_scale_and_set();
    test_snapshot();
    test_remove_set();
    test_remove_unit();
    test_cross();
    test_weapons();
    test_weapon_removal();
    test_features();
    test_grants();
    test_journal();
    test_sandbox();
    test_padded_names();
    test_unchanged_write();
    test_identity();
    test_rename_lookups();
    test_identity_walks();
    test_walk_skips_removed();
    test_weak_need();
    test_fixed();
    test_memory_cap();
    return oa::test::check_exit_status();
}
