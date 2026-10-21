// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Map rules apply to the chosen map only. Fixtures are made-up Ridge OTAs.
#include "oa/data/mutators/map_rules.hpp"
#include "oa/data/mutators/pipeline.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/soft_mutator.hpp"
#include "oa/formats/tdf.hpp"
#include "oa/formats/tdf/number.hpp"
#include "oa/formats/tdf/write.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef OA_MUTATORS_MAP_RULES_DIR
#error OA_MUTATORS_MAP_RULES_DIR is the fixture folder
#endif

namespace {

namespace fs = std::filesystem;
namespace mutators = oa::data::mutators;
namespace rules = oa::data::mutation_rules;
namespace sm = oa::formats::soft_mutator;
namespace tdf = oa::formats::tdf;
namespace oamut = oa::formats::oamut;

const fs::path k_fixtures = OA_MUTATORS_MAP_RULES_DIR;

struct Scratch {
    fs::path path;

    explicit Scratch(std::string_view name) : path(oa::test::make_scratch_directory(name)) {}

    ~Scratch() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void write_text(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    OA_CHECK(out.good());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    OA_CHECK(in.good());
    if (!in.good())
        return {};
    in.seekg(0, std::ios::end);
    const std::streamoff length = in.tellg();
    in.seekg(0);
    if (length <= 0)
        return {};
    std::string text(static_cast<std::size_t>(length), '\0');
    in.read(text.data(), length);
    return text;
}

std::string fixture(const char* name) {
    return read_text(k_fixtures / name);
}

rules::MutatorRules rules_from(std::string id, const char* file) {
    const std::string text = fixture(file);
    const auto* bytes = reinterpret_cast<const uint8_t*>(text.data());
    const sm::ParseResult parsed = sm::parse_rules(std::span<const uint8_t>(bytes, text.size()));
    if (parsed.has_errors()) {
        for (const sm::Diagnostic& diagnostic : parsed.diagnostics)
            std::fprintf(stderr, "%s:%u: %s\n", file, diagnostic.line, diagnostic.message.c_str());
    }
    OA_CHECK(!parsed.has_errors());
    rules::MutatorRules one;
    one.mutator = std::move(id);
    rules::RuleSource source;
    source.name = std::string("rules/") + file;
    source.rules = parsed.rules;
    one.sources.push_back(std::move(source));
    return one;
}

mutators::ChosenMap chosen(std::string key, std::string text) {
    mutators::ChosenMap map;
    map.key = std::move(key);
    map.ota_path = "maps/" + map.key + ".ota";
    map.ota = std::make_shared<const std::vector<uint8_t>>(text.begin(), text.end());
    return map;
}

std::string applied_text(const rules::DocumentSet& documents) {
    if (documents.size() == 0)
        return {};
    const std::optional<std::string> written = tdf::write_text(documents.at(0).document.get());
    OA_CHECK(written.has_value());
    if (!written.has_value())
        return {};
    return *written;
}

const tdf::Block* header_of(const tdf::OwnedDocument& document) {
    return tdf::find_child(document.root(), "GlobalHeader");
}

const char* field_of(const tdf::Block* block, const char* key) {
    if (block == nullptr)
        return nullptr;
    return tdf::find_value(block, key);
}

bool field_is(const tdf::Block* block, const char* key, std::string_view expected) {
    const char* value = field_of(block, key);
    return value != nullptr && std::string_view(value) == expected;
}

bool note_has(const rules::ApplyResult& result, rules::NoteKind kind, std::string_view message) {
    for (const rules::RuleNote& note : result.notes) {
        if (note.kind == kind && note.message == message)
            return true;
    }
    return false;
}

bool detail_has(const mutators::PipelineResult& result, std::string_view fragment) {
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        if (conflict.detail.find(fragment) != std::string::npos)
            return true;
    }
    return false;
}

int detail_count(const mutators::PipelineResult& result, std::string_view fragment) {
    int found = 0;
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        if (conflict.detail.find(fragment) != std::string::npos)
            ++found;
    }
    return found;
}

const oa::MutatorLayerEntry*
served_entry(const mutators::PipelineResult& result, std::string_view path) {
    for (const oa::MutatorLayerEntry& entry : result.layer.entries) {
        if (entry.path == path && entry.kind == oa::MutatorSourceKind::contents &&
            entry.contents != nullptr)
            return &entry;
    }
    return nullptr;
}

std::string served_text(const mutators::PipelineResult& result, std::string_view path) {
    const oa::MutatorLayerEntry* entry = served_entry(result, path);
    if (entry == nullptr)
        return {};
    return std::string(entry->contents->begin(), entry->contents->end());
}

void release(oa::AssetStore& store) {
    if (store.mutator_layer_mounted())
        store.unmount_mutator_layer();
}

mutators::PipelineMutator load_package(const fs::path& package) {
    mutators::PipelineMutator mutator;
    mutator.package = package;
    const std::string yaml = read_text(package / "oamut.yaml");
    const auto* bytes = reinterpret_cast<const uint8_t*>(yaml.data());
    std::vector<oamut::Problem> problems;
    const bool read = oamut::read_manifest(
        std::span<const uint8_t>(bytes, yaml.size()), mutator.manifest, problems
    );
    OA_CHECK(read);
    return mutator;
}

void write_package(
    const fs::path& package,
    std::string_view id,
    std::string_view layers,
    std::string_view sm_name,
    std::string_view sm_text,
    std::string_view maps_entry
) {
    std::string yaml;
    yaml += "oamut: 1\n";
    yaml += "id: ";
    yaml += id;
    yaml += "\nname: \"Ridge Maps\"\nversion: \"1.0\"\nauthor:\n  name: \"Ridge Maker\"\n";
    yaml += "tags: [\"gameplay\"]\nworks-with: [\"*\"]\nlayers: ";
    yaml += layers;
    yaml += "\n";
    write_text(package / "oamut.yaml", yaml);
    std::string lua = "function mutate(ctx)\n  return {\n    rules = { sm \"rules/";
    lua += sm_name;
    lua += "\" }";
    if (!maps_entry.empty()) {
        lua += ",\n    maps = ";
        lua += maps_entry;
    }
    lua += "\n  }\nend\n";
    write_text(package / "mutator.lua", lua);
    write_text(package / "rules" / std::string(sm_name), sm_text);
}

mutators::PipelineInput make_input(std::vector<mutators::PipelineMutator> list) {
    mutators::PipelineInput input;
    input.mutators = std::move(list);
    input.mod.id = "ridge";
    input.mod.version = "1.0";
    input.mod.game = "ridge";
    input.engine_version = "0.8.5";
    input.seed = 1;
    return input;
}

mutators::PipelineResult run_packages(
    oa::AssetStore& store,
    const fs::path& scratch,
    const std::vector<std::pair<std::string, std::string>>& scripts,
    std::string_view layers,
    const std::optional<mutators::ChosenMap>& map,
    std::string_view maps_entry
) {
    release(store);
    std::vector<mutators::PipelineMutator> list;
    for (std::size_t index = 0; index < scripts.size(); ++index) {
        const fs::path package =
            scratch / ("pkg-" + std::to_string(index) + "-" + scripts[index].first);
        write_package(
            package,
            scripts[index].first,
            layers,
            scripts[index].first + ".sm",
            scripts[index].second,
            maps_entry
        );
        list.push_back(load_package(package));
    }
    mutators::PipelineInput input = make_input(std::move(list));
    input.map = map;
    return mutators::run_pipeline(store, input);
}

uint32_t line_of(std::string_view text, std::string_view needle) {
    uint32_t line = 1;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = text.find('\n', at);
        const std::size_t stop = end == std::string_view::npos ? text.size() : end;
        if (text.substr(at, stop - at).find(needle) != std::string_view::npos)
            return line;
        if (end == std::string_view::npos)
            break;
        at = end + 1;
        ++line;
    }
    return 0;
}

void test_split() {
    const rules::MutatorRules mixed = rules_from("ridge-mixed", "mixed.sm");
    const std::vector<rules::MutatorRules> incoming{mixed};
    const mutators::SplitRules split = mutators::split_map_rules(incoming);
    OA_CHECK(split.others.size() == 1);
    OA_CHECK(split.maps.size() == 1);
    OA_CHECK(split.refusals.size() == 1);
    if (!split.others.empty()) {
        OA_CHECK(split.others.front().mutator == "ridge-mixed");
        OA_CHECK(split.others.front().sources.size() == 1);
        OA_CHECK(split.others.front().sources.front().rules.blocks.size() == 1);
        if (!split.others.front().sources.empty() &&
            !split.others.front().sources.front().rules.blocks.empty())
            OA_CHECK(
                split.others.front().sources.front().rules.blocks.front().target.extension == "fbi"
            );
    }
    if (!split.maps.empty()) {
        OA_CHECK(split.maps.front().mutator == "ridge-mixed");
        OA_CHECK(split.maps.front().sources.size() == 1);
        OA_CHECK(split.maps.front().sources.front().rules.blocks.size() == 1);
        if (!split.maps.front().sources.empty() &&
            !split.maps.front().sources.front().rules.blocks.empty())
            OA_CHECK(
                split.maps.front().sources.front().rules.blocks.front().target.extension == "ota"
            );
    }
    OA_CHECK(!split.refusals.empty());
    if (!split.refusals.empty()) {
        OA_CHECK(split.refusals.front().find("ridge-mixed") != std::string::npos);
        OA_CHECK(
            split.refusals.front().find("rules can change a map's OTA only") != std::string::npos
        );
    }
}

rules::ApplyResult
apply_one(const char* file, const mutators::ChosenMap& map, rules::DocumentSet& documents) {
    const rules::MutatorRules one = rules_from("ridge-one", file);
    const std::vector<rules::MutatorRules> list{one};
    return mutators::apply_map_rules(list, map, documents);
}

void test_lava_group() {
    const mutators::ChosenMap lava = chosen("ridge-lava", fixture("lava.ota"));
    const mutators::ChosenMap plain = chosen("ridge-plain", fixture("plain.ota"));
    rules::DocumentSet lava_docs;
    rules::DocumentSet plain_docs;
    const rules::ApplyResult lava_result = apply_one("lava.sm", lava, lava_docs);
    const rules::ApplyResult plain_result = apply_one("lava.sm", plain, plain_docs);
    tdf::OwnedDocument lava_tree;
    tdf::OwnedDocument plain_tree;
    OA_CHECK(lava_tree.parse(applied_text(lava_docs)));
    OA_CHECK(plain_tree.parse(applied_text(plain_docs)));
    const tdf::Block* lava_header = header_of(lava_tree);
    const tdf::Block* plain_header = header_of(plain_tree);
    OA_CHECK(field_is(lava_header, "lavaworld", "0"));
    OA_CHECK(field_is(lava_header, "waterdamage", "30"));
    OA_CHECK(field_is(plain_header, "lavaworld", "0"));
    OA_CHECK(field_of(plain_header, "waterdamage") == nullptr);
    OA_CHECK(plain_result.writes.empty());
    bool created = false;
    for (const rules::FieldWrite& write : lava_result.writes) {
        if (write.key == "waterdamage") {
            created = !write.old_text.has_value() && write.new_text == "30";
        }
    }
    OA_CHECK(created);
}

void test_wind_clamp() {
    rules::DocumentSet documents;
    apply_one("wind.sm", chosen("ridge-metal", fixture("schemas.ota")), documents);
    tdf::OwnedDocument tree;
    OA_CHECK(tree.parse(applied_text(documents)));
    OA_CHECK(field_is(header_of(tree), "minwindspeed", "4000"));
}

void test_wind_sequence() {
    const rules::MutatorRules still = rules_from("ridge-still", "assign0.sm");
    const rules::MutatorRules breeze = rules_from("ridge-breeze", "wind.sm");
    const mutators::ChosenMap map = chosen("ridge-metal", fixture("schemas.ota"));
    for (int order = 0; order < 2; ++order) {
        std::vector<rules::MutatorRules> list;
        const char* expect = "4000";
        if (order == 0) {
            list.push_back(still);
            list.push_back(breeze);
        } else {
            list.push_back(breeze);
            list.push_back(still);
            expect = "0";
        }
        rules::DocumentSet documents;
        const rules::ApplyResult result = mutators::apply_map_rules(list, map, documents);
        tdf::OwnedDocument tree;
        OA_CHECK(tree.parse(applied_text(documents)));
        OA_CHECK(field_is(header_of(tree), "minwindspeed", expect));
        std::string last;
        for (const rules::FieldWrite& write : result.writes) {
            if (write.key == "minwindspeed" && write.new_text.has_value())
                last = *write.new_text;
        }
        OA_CHECK(last == expect);
    }
}

void test_metal_sequence() {
    const rules::MutatorRules light = rules_from("ridge-light", "metal32.sm");
    const rules::MutatorRules heavy = rules_from("ridge-heavy", "metal128.sm");
    for (int order = 0; order < 2; ++order) {
        std::vector<rules::MutatorRules> list;
        const char* expect = "128";
        const char* earlier = "32";
        if (order == 0) {
            list.push_back(light);
            list.push_back(heavy);
        } else {
            list.push_back(heavy);
            list.push_back(light);
            expect = "32";
            earlier = "128";
        }
        rules::DocumentSet documents;
        const rules::ApplyResult result = mutators::apply_map_rules(
            list, chosen("ridge-metal", fixture("schemas.ota")), documents
        );
        tdf::OwnedDocument tree;
        OA_CHECK(tree.parse(applied_text(documents)));
        const tdf::Block* header = header_of(tree);
        OA_CHECK(field_of(header, "SurfaceMetal") == nullptr);
        OA_CHECK(field_is(tdf::find_child(header, "Schema 0"), "SurfaceMetal", expect));
        OA_CHECK(field_is(tdf::find_child(header, "Schema 1"), "SurfaceMetal", expect));
        std::string last0;
        std::string last1;
        bool saw_earlier = false;
        for (const rules::FieldWrite& write : result.writes) {
            if (write.key != "SurfaceMetal" || !write.new_text.has_value())
                continue;
            if (*write.new_text == earlier)
                saw_earlier = true;
            if (write.block == "Schema 0")
                last0 = *write.new_text;
            if (write.block == "Schema 1")
                last1 = *write.new_text;
        }
        OA_CHECK(saw_earlier);
        OA_CHECK(last0 == expect);
        OA_CHECK(last1 == expect);
        OA_CHECK(note_has(
            result, rules::NoteKind::check_refused, "copied to every schema's SurfaceMetal"
        ));
    }
}

void test_later_block_sees_write() {
    rules::DocumentSet documents;
    apply_one("sees.sm", chosen("ridge-lava", fixture("lava.ota")), documents);
    tdf::OwnedDocument tree;
    OA_CHECK(tree.parse(applied_text(documents)));
    const tdf::Block* header = header_of(tree);
    OA_CHECK(field_is(header, "lavaworld", "0"));
    OA_CHECK(field_is(header, "waterdamage", "30"));
}

void test_absent_factor_and_moho() {
    rules::DocumentSet scaled;
    const rules::ApplyResult scale =
        apply_one("metalscale.sm", chosen("ridge-metal", fixture("schemas.ota")), scaled);
    tdf::OwnedDocument scale_tree;
    OA_CHECK(scale_tree.parse(applied_text(scaled)));
    const tdf::Block* scale_header = header_of(scale_tree);
    OA_CHECK(field_of(scale_header, "SurfaceMetal") == nullptr);
    OA_CHECK(field_is(tdf::find_child(scale_header, "Schema 0"), "SurfaceMetal", "4"));
    OA_CHECK(field_is(tdf::find_child(scale_header, "Schema 1"), "SurfaceMetal", "6"));
    OA_CHECK(note_has(
        scale,
        rules::NoteKind::factor_on_absent_field,
        "a factor did nothing because the field is absent"
    ));
    OA_CHECK(scale.writes.empty());

    rules::DocumentSet moho_docs;
    const rules::ApplyResult moho =
        apply_one("moho.sm", chosen("ridge-metal", fixture("schemas.ota")), moho_docs);
    tdf::OwnedDocument moho_tree;
    OA_CHECK(moho_tree.parse(applied_text(moho_docs)));
    const tdf::Block* moho_header = header_of(moho_tree);
    OA_CHECK(field_of(moho_header, "MohoMetal") == nullptr);
    OA_CHECK(field_of(tdf::find_child(moho_header, "Schema 0"), "MohoMetal") == nullptr);
    OA_CHECK(
        note_has(moho, rules::NoteKind::check_refused, "nothing in OA reads MohoMetal; skipped")
    );
    OA_CHECK(moho.writes.empty());
}

void test_fractional_whole() {
    rules::DocumentSet documents;
    apply_one("fraction.sm", chosen("ridge-acid", fixture("fraction.ota")), documents);
    tdf::OwnedDocument tree;
    OA_CHECK(tree.parse(applied_text(documents)));
    const char* damage = field_of(header_of(tree), "waterdamage");
    OA_CHECK(damage != nullptr);
    if (damage == nullptr)
        return;
    const double product = 6.0 * 0.1;
    const std::optional<std::string> spelled = tdf::format_number(product);
    OA_CHECK(spelled.has_value());
    if (spelled.has_value())
        OA_CHECK(std::string_view(damage) == *spelled);
}

bool journal_schema(
    const mutators::PipelineResult& result, std::string_view record, std::string_view text
) {
    for (const mutators::JournalEntry& entry : result.journal.entries) {
        if (entry.record == record && entry.field == "surfacemetal" && entry.block.empty() &&
            entry.new_text == text && entry.phase == mutators::Phase::rules &&
            entry.collection == mutators::Collection::other)
            return true;
    }
    return false;
}

void test_pipeline_metal(oa::AssetStore& store, const fs::path& scratch) {
    const std::string sm = fixture("metal32.sm");
    std::vector<std::pair<std::string, std::string>> scripts;
    scripts.emplace_back("ridge-metal", sm);
    const mutators::PipelineResult result = run_packages(
        store, scratch, scripts, "[\"maps\"]", chosen("ridge-metal", fixture("schemas.ota")), {}
    );
    OA_CHECK(result.playable());
    const std::string served = served_text(result, "maps/ridge-metal.ota");
    tdf::OwnedDocument tree;
    OA_CHECK(tree.parse(served));
    const tdf::Block* header = header_of(tree);
    OA_CHECK(field_of(header, "SurfaceMetal") == nullptr);
    OA_CHECK(field_is(tdf::find_child(header, "Schema 0"), "SurfaceMetal", "32"));
    OA_CHECK(field_is(tdf::find_child(header, "Schema 1"), "SurfaceMetal", "32"));
    OA_CHECK(journal_schema(result, "map:ridge-metal/Schema 0", "32"));
    OA_CHECK(journal_schema(result, "map:ridge-metal/Schema 1", "32"));
    OA_CHECK(detail_has(result, "copied to every schema's SurfaceMetal"));
}

void test_pipeline_refused_metal(oa::AssetStore& store, const fs::path& scratch) {
    std::vector<std::pair<std::string, std::string>> scripts;
    scripts.emplace_back("ridge-hot", fixture("metal300.sm"));
    const mutators::PipelineResult result = run_packages(
        store, scratch, scripts, "[\"maps\"]", chosen("ridge-metal", fixture("schemas.ota")), {}
    );
    OA_CHECK(result.playable());
    OA_CHECK(served_entry(result, "maps/ridge-metal.ota") == nullptr);
    OA_CHECK(detail_has(result, "copied to every schema's SurfaceMetal"));
    OA_CHECK(
        detail_count(result, "map.schemas.surfacemetal must be a whole number from 0 to 255") == 2
    );
    OA_CHECK(detail_has(result, "Schema 0"));
    OA_CHECK(detail_has(result, "Schema 1"));
}

void test_hook_sees_rules(oa::AssetStore& store, const fs::path& scratch) {
    std::vector<std::pair<std::string, std::string>> scripts;
    scripts.emplace_back("ridge-hook", fixture("wind0.sm"));
    const mutators::PipelineResult result = run_packages(
        store,
        scratch,
        scripts,
        "[\"maps\"]",
        chosen("ridge-metal", fixture("schemas.ota")),
        "function(map, ctx)\n      map.header.gravity = map.header.minwindspeed\n    end"
    );
    OA_CHECK(result.playable());
    tdf::OwnedDocument tree;
    OA_CHECK(tree.parse(served_text(result, "maps/ridge-metal.ota")));
    const tdf::Block* header = header_of(tree);
    OA_CHECK(field_is(header, "minwindspeed", "0"));
    OA_CHECK(field_is(header, "gravity", "0"));
}

void test_no_map(oa::AssetStore& store, const fs::path& scratch) {
    std::vector<std::pair<std::string, std::string>> scripts;
    scripts.emplace_back("ridge-calm-a", fixture("wind0.sm"));
    scripts.emplace_back("ridge-calm-b", fixture("assign0.sm"));
    const mutators::PipelineResult result =
        run_packages(store, scratch, scripts, "[\"maps\"]", std::nullopt, {});
    OA_CHECK(result.playable());
    OA_CHECK(served_entry(result, "maps/ridge-metal.ota") == nullptr);
    OA_CHECK(detail_count(result, "applies to the chosen map only") == 2);
    int matched = 0;
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        if (conflict.detail == "applies to the chosen map only" &&
            conflict.kind == mutators::ConflictKind::matched_nothing)
            ++matched;
    }
    OA_CHECK(matched == 2);
}

void test_tnt_refused(oa::AssetStore& store, const fs::path& scratch) {
    const std::string sm = fixture("tnt.sm");
    const uint32_t line = line_of(sm, "[*All*]");
    std::vector<std::pair<std::string, std::string>> scripts;
    scripts.emplace_back("ridge-tnt", sm);
    const mutators::PipelineResult result = run_packages(
        store, scratch, scripts, "[\"maps\"]", chosen("ridge-metal", fixture("schemas.ota")), {}
    );
    OA_CHECK(!result.playable());
    OA_CHECK(!result.mutators.empty());
    if (result.mutators.empty())
        return;
    OA_CHECK(result.mutators.front().outcome == mutators::Outcome::failed);
    OA_CHECK(result.mutators.front().cause == mutators::Cause::invalid_plan);
    OA_CHECK(result.mutators.front().phase == mutators::Phase::plan);
    const std::string& message = result.mutators.front().message;
    OA_CHECK(message.find("ridge-tnt") != std::string::npos);
    OA_CHECK(message.find("rules/ridge-tnt.sm:" + std::to_string(line) + ":") != std::string::npos);
    OA_CHECK(message.find("rules can change a map's OTA only") != std::string::npos);
    if (message.find("rules can change a map's OTA only") == std::string::npos)
        std::fprintf(stderr, "plan said: %s\n", message.c_str());
}

void test_missing_layer(oa::AssetStore& store, const fs::path& scratch) {
    std::vector<std::pair<std::string, std::string>> scripts;
    scripts.emplace_back("ridge-bare", fixture("metal32.sm"));
    const mutators::PipelineResult result = run_packages(
        store, scratch, scripts, "[\"tables\"]", chosen("ridge-metal", fixture("schemas.ota")), {}
    );
    OA_CHECK(!result.playable());
    OA_CHECK(!result.mutators.empty());
    if (result.mutators.empty())
        return;
    OA_CHECK(result.mutators.front().cause == mutators::Cause::undeclared_layer);
    OA_CHECK(
        result.mutators.front().message.find(
            "returns a rule for the maps layer but its manifest does not declare the maps layer"
        ) != std::string::npos
    );
}

struct Watch {
    bool maps{false};
};

void on_lookup(void* context, std::string_view name) {
    auto* watch = static_cast<Watch*>(context);
    std::string folded;
    folded.resize(name.size());
    for (std::size_t offset = 0; offset < name.size(); ++offset) {
        char byte = name[offset];
        if (byte == '\\')
            byte = '/';
        if (byte >= 'A' && byte <= 'Z')
            byte = static_cast<char>(byte - 'A' + 'a');
        folded[offset] = byte;
    }
    if (folded == "maps" || folded.starts_with("maps/"))
        watch->maps = true;
}

void test_rules_skip_maps(oa::AssetStore& store, const fs::path& scratch) {
    Watch watch;
    oa::LookupObserver observer;
    observer.context = &watch;
    observer.looked_up = &on_lookup;
    store.observe_lookups(observer);
    std::vector<std::pair<std::string, std::string>> scripts;
    scripts.emplace_back("ridge-watch", fixture("wind.sm"));
    const mutators::PipelineResult result = run_packages(
        store, scratch, scripts, "[\"maps\"]", chosen("ridge-metal", fixture("schemas.ota")), {}
    );
    store.observe_lookups({});
    OA_CHECK(result.playable());
    OA_CHECK(!watch.maps);
    tdf::OwnedDocument tree;
    OA_CHECK(tree.parse(served_text(result, "maps/ridge-metal.ota")));
    OA_CHECK(field_is(header_of(tree), "minwindspeed", "4000"));
}

} // namespace

int main() {
    test_split();
    test_lava_group();
    test_wind_clamp();
    test_wind_sequence();
    test_metal_sequence();
    test_later_block_sees_write();
    test_absent_factor_and_moho();
    test_fractional_whole();
    const Scratch scratch("R04-map-rules");
    write_text(
        scratch.path / "store" / "units" / "RGCOM.fbi", "[UNITINFO]\n{\nUnitName=RGCOM;\n}\n"
    );
    oa::AssetStore store(scratch.path / "store");
    test_pipeline_metal(store, scratch.path);
    test_pipeline_refused_metal(store, scratch.path);
    test_hook_sees_rules(store, scratch.path);
    test_no_map(store, scratch.path);
    test_tnt_refused(store, scratch.path);
    test_missing_layer(store, scratch.path);
    test_rules_skip_maps(store, scratch.path);
    release(store);
    return oa::test::check_exit_status();
}
