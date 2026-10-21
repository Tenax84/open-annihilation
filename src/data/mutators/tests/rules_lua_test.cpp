// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Lua rule blocks against the rule-file spelling of the same blocks, and the
// refusals of a plan's rules list. The units are the made-up mod Ridge.

#include "oa/data/mutators/rules_lua.hpp"
#include "oa/data/mutation_rules/apply.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/formats/soft_mutator.hpp"
#include "oa/test/check.hpp"

#include "lua.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef OA_MUTATORS_RULES_DIR
#error OA_MUTATORS_RULES_DIR is the fixture folder
#endif

namespace {

namespace rules = oa::data::mutation_rules;
namespace mutators = oa::data::mutators;
namespace host = oa::data::script_host;
namespace sm = oa::formats::soft_mutator;

constexpr const char* k_cases[] = {
    "needs", "changes", "selectors", "targets", "depth", "line", "fixed_set", "fixed_scale"
};

constexpr std::string_view k_com = R"([UNITINFO]
{
UnitName=RIDGECOM;
Name=Ridge;
Builder=1;
WorkerTime=10;
MaxVelocity=2;
MaxDamage=3000;
plain=1;
words=CMDCK;
flag=1;
off=0;
band=10;
exact=RIDGECOM;
part=CMDCK;
flipped=10;
flipped_text=yes;
flipped_has=CMD;
}
)";

constexpr std::string_view k_fab = R"([UNITINFO]
{
UnitName=RIDGEFAB;
Builder=1;
WorkerTime=20;
MaxVelocity=1;
MaxDamage=1000;
}
)";

constexpr std::string_view k_weapon = R"([RIDGE_LASER]
{
range=100;
reloadtime=8;
[DAMAGE]
{
default=10;
RIDGECOM=4;
}
}
)";

constexpr std::string_view k_wreck = R"([RIDGE_WRECK]
{
metal=5;
}
)";

constexpr std::string_view k_stone = R"([RIDGE_STONE]
{
metal=5;
}
)";

constexpr std::string_view k_sides = R"([ARM]
{
commander=ARMCOM;
}
[CORE]
{
commander=CORCOM;
}
)";

struct FileText {
    const char* path = nullptr;
    std::string_view text{};
};

constexpr FileText k_files[] = {
    {"units/RIDGECOM.fbi", k_com},
    {"units/RIDGEFAB.fbi", k_fab},
    {"weapons/ridge.tdf", k_weapon},
    {"features/corpses/ridge.tdf", k_wreck},
    {"features/rocks/ridge.tdf", k_stone},
    {"gamedata/sidedata.tdf", k_sides},
};

struct HostBox {
    std::optional<host::Sandbox> sandbox;
};

struct Eval {
    const char* source{nullptr};
    const char* chunk{nullptr};
    bool want_plan{true};
    bool script_ok{false};
    bool flag{false};
    std::string script_error;
    mutators::PlanRules plan;
    mutators::PackageReader reader;
    std::string entry{"mutator.lua"};
};

struct FileCapture {
    mutators::PlanRules plan;
    mutators::PackageReader reader;
    std::string entry;
    bool saw{false};
};

struct Asked {
    std::map<std::string, std::vector<uint8_t>> files;
    std::vector<std::string> paths;
    bool called{false};
};

std::string& fixture_dir() {
    static std::string dir{OA_MUTATORS_RULES_DIR};
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
    const std::streamoff size = input.tellg();
    if (size < 0)
        return false;
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    if (size > 0 && !input.read(reinterpret_cast<char*>(bytes.data()), size))
        return false;
    return true;
}

bool read_bytes(
    const std::filesystem::path& path, std::vector<uint8_t>& bytes, std::string& failure
) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        failure = "cannot open " + path.string();
        return false;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        failure = "cannot size " + path.string();
        return false;
    }
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    if (size > 0 && !input.read(reinterpret_cast<char*>(bytes.data()), size)) {
        failure = "cannot read " + path.string();
        return false;
    }
    return true;
}

void install_forms(void*, lua_State* state) {
    mutators::install_rule_forms(state);
}

bool open_host(HostBox& box) {
    host::OpenOptions options;
    options.package_id = "ridge";
    options.profile = host::mutator_profile();
    options.sources.context = &fixture_dir();
    options.sources.read = read_fixture;
    host::ScriptError failure;
    box.sandbox = host::Sandbox::open(options, failure);
    if (!box.sandbox.has_value()) {
        std::fprintf(stderr, "open: %s\n", failure.message.c_str());
        return false;
    }
    host::ScriptError install_failure;
    if (!box.sandbox->run_host("install", nullptr, install_forms, install_failure)) {
        std::fprintf(stderr, "install: %s\n", install_failure.message.c_str());
        return false;
    }
    return true;
}

bool ensure_host(HostBox& box) {
    if (box.sandbox.has_value() && !box.sandbox->failed())
        return true;
    return open_host(box);
}

void eval_work(void* context, lua_State* state) {
    auto* eval = static_cast<Eval*>(context);
    const std::size_t length = std::strlen(eval->source);
    if (luaL_loadbuffer(state, eval->source, length, eval->chunk) != LUA_OK) {
        const char* message = lua_tostring(state, -1);
        eval->script_error = message != nullptr ? message : "load failed";
        lua_pop(state, 1);
        return;
    }
    if (lua_pcall(state, 0, LUA_MULTRET, 0) != LUA_OK) {
        const char* message = lua_tostring(state, -1);
        eval->script_error = message != nullptr ? message : "run failed";
        lua_pop(state, 1);
        return;
    }
    if (!eval->want_plan) {
        if (lua_gettop(state) != 1 || !lua_isboolean(state, -1))
            eval->script_error = "expected a boolean";
        else
            eval->flag = lua_toboolean(state, -1) != 0;
        lua_settop(state, 0);
        eval->script_ok = eval->script_error.empty();
        return;
    }
    if (lua_gettop(state) != 1) {
        eval->script_error = "the chunk did not return one value";
        lua_settop(state, 0);
        return;
    }
    eval->plan = mutators::read_plan_rules(state, -1, eval->reader, eval->entry);
    eval->script_ok = true;
    lua_settop(state, 0);
}

bool run_source(HostBox& box, Eval& eval) {
    if (!ensure_host(box)) {
        eval.script_error = "no sandbox";
        return false;
    }
    host::ScriptError failure;
    if (!box.sandbox->run_host("rules", &eval, eval_work, failure)) {
        eval.script_error = failure.message;
        return false;
    }
    return eval.script_ok;
}

void read_capture(void* context, lua_State* state, int results) {
    auto* capture = static_cast<FileCapture*>(context);
    capture->saw = true;
    if (results != 1) {
        capture->plan.error = "the chunk did not return one value";
        return;
    }
    capture->plan = mutators::read_plan_rules(state, -1, capture->reader, capture->entry);
}

bool run_fixture(HostBox& box, const char* file, FileCapture& capture) {
    if (!ensure_host(box)) {
        capture.plan.error = "no sandbox";
        return false;
    }
    host::CallHooks hooks;
    hooks.context = &capture;
    hooks.read = read_capture;
    host::ScriptError failure;
    if (!box.sandbox->run_file(file, file, &hooks, failure)) {
        std::fprintf(stderr, "%s: %s\n", file, failure.message.c_str());
        capture.plan.error = failure.message;
        return false;
    }
    return capture.saw && capture.plan.ok();
}

bool fill_ridge(rules::DocumentSet& documents) {
    for (const FileText& file : k_files) {
        std::string failure;
        if (!documents.add(file.path, file.text, &failure)) {
            std::fprintf(stderr, "%s: %s\n", file.path, failure.c_str());
            return false;
        }
    }
    return true;
}

rules::ApplyResult apply_file(const sm::RuleFile& file, const std::string& source_name) {
    rules::DocumentSet documents;
    if (!fill_ridge(documents))
        OA_CHECK(false);
    rules::MutatorRules mutator;
    mutator.mutator = "ridge";
    rules::RuleSource source;
    source.name = source_name;
    source.rules = file;
    mutator.sources.push_back(std::move(source));
    rules::ApplyState state;
    return rules::apply_step(mutator, documents, state, {});
}

void dump_rules(const char* label, const sm::RuleFile& file) {
    std::fprintf(stderr, "%s blocks %zu\n", label, file.blocks.size());
    for (const sm::Block& block : file.blocks) {
        std::fprintf(
            stderr,
            "  selector %u name '%s' folder '%s' extension '%s' needs %zu changes %zu line %u\n",
            static_cast<unsigned>(block.selector),
            block.name.c_str(),
            block.target.folder.c_str(),
            block.target.extension.c_str(),
            block.needs.size(),
            block.changes.size(),
            block.line
        );
        for (const sm::Need& need : block.needs) {
            std::fprintf(
                stderr,
                "    need '%s' kind %u negated %d low %g high %g text '%s'\n",
                need.field.c_str(),
                static_cast<unsigned>(need.kind),
                need.negated ? 1 : 0,
                need.low,
                need.high,
                need.text.c_str()
            );
        }
        for (const sm::Change& change : block.changes) {
            std::fprintf(
                stderr,
                "    change '%s' kind %u depth %d value %g low %g high %g text '%s'\n",
                change.field.c_str(),
                static_cast<unsigned>(change.kind),
                change.all_depth2 ? 1 : 0,
                change.value,
                change.low,
                change.high,
                change.text.c_str()
            );
        }
    }
}

void dump_diagnostics(const char* label, const sm::ParseResult& parsed) {
    for (const sm::Diagnostic& item : parsed.diagnostics) {
        std::fprintf(
            stderr,
            "%s diagnostic %s line %u %s\n",
            label,
            std::string(sm::code_name(item.code)).c_str(),
            item.line,
            item.message.c_str()
        );
    }
}

bool same_fold(std::string_view first, std::string_view second) {
    if (first.size() != second.size())
        return false;
    for (std::size_t cursor = 0; cursor < first.size(); ++cursor) {
        const auto lower = [](unsigned char byte) noexcept {
            if (byte >= 'A' && byte <= 'Z')
                return static_cast<char>(byte - 'A' + 'a');
            return static_cast<char>(byte);
        };
        if (lower(static_cast<unsigned char>(first[cursor])) !=
            lower(static_cast<unsigned char>(second[cursor])))
            return false;
    }
    return true;
}

bool has_write(
    const std::vector<rules::FieldWrite>& writes, std::string_view path, std::string_view key
) {
    for (const rules::FieldWrite& write : writes) {
        if (write.path == path && same_fold(write.key, key))
            return true;
    }
    return false;
}

bool has_block_write(
    const std::vector<rules::FieldWrite>& writes,
    std::string_view path,
    std::string_view block,
    std::string_view key
) {
    for (const rules::FieldWrite& write : writes) {
        if (write.path == path && same_fold(write.block, block) && same_fold(write.key, key))
            return true;
    }
    return false;
}

void expect_same_writes(
    const char* label, const rules::ApplyResult& lua_result, const rules::ApplyResult& file_result
) {
    const std::vector<rules::FieldWrite>& lua_writes = lua_result.writes;
    const std::vector<rules::FieldWrite>& file_writes = file_result.writes;
    if (lua_writes.size() != file_writes.size()) {
        std::fprintf(
            stderr,
            "%s write count lua %zu file %zu\n",
            label,
            lua_writes.size(),
            file_writes.size()
        );
    }
    OA_CHECK(lua_writes.size() == file_writes.size());
    if (lua_writes.empty())
        std::fprintf(stderr, "%s wrote nothing\n", label);
    OA_CHECK(!lua_writes.empty());
    const std::size_t count =
        lua_writes.size() < file_writes.size() ? lua_writes.size() : file_writes.size();
    for (std::size_t cursor = 0; cursor < count; ++cursor) {
        const rules::FieldWrite& lua_write = lua_writes[cursor];
        const rules::FieldWrite& file_write = file_writes[cursor];
        const bool match =
            lua_write.mutators == file_write.mutators && lua_write.path == file_write.path &&
            lua_write.record == file_write.record && lua_write.block == file_write.block &&
            lua_write.key == file_write.key && lua_write.old_text == file_write.old_text &&
            lua_write.new_text == file_write.new_text;
        if (!match) {
            std::fprintf(
                stderr,
                "%s write %zu lua '%s' '%s' '%s' '%s' file '%s' '%s' '%s' '%s'\n",
                label,
                cursor,
                lua_write.path.c_str(),
                lua_write.record.c_str(),
                lua_write.block.c_str(),
                lua_write.key.c_str(),
                file_write.path.c_str(),
                file_write.record.c_str(),
                file_write.block.c_str(),
                file_write.key.c_str()
            );
        }
        OA_CHECK(match);
    }
}

mutators::PackageReader asking_reader(Asked& asked) {
    return [&asked](
               std::string_view path, std::string* failure
           ) -> std::optional<std::vector<uint8_t>> {
        asked.called = true;
        asked.paths.emplace_back(path);
        const auto found = asked.files.find(std::string(path));
        if (found == asked.files.end()) {
            if (failure != nullptr)
                *failure = "no such file";
            return std::nullopt;
        }
        return found->second;
    };
}

void expect_script_error(HostBox& box, const char* chunk, const char* source, const char* part) {
    Eval eval;
    eval.source = source;
    eval.chunk = chunk;
    const bool ran = run_source(box, eval);
    if (ran)
        std::fprintf(stderr, "%s succeeded\n", chunk);
    OA_CHECK(!ran);
    if (eval.script_error.find(part) == std::string::npos)
        std::fprintf(stderr, "%s message '%s'\n", chunk, eval.script_error.c_str());
    OA_CHECK(eval.script_error.find(part) != std::string::npos);
}

void expect_plan_error(HostBox& box, const char* chunk, const char* source, const char* part) {
    Eval eval;
    eval.source = source;
    eval.chunk = chunk;
    if (!run_source(box, eval)) {
        std::fprintf(stderr, "%s script '%s'\n", chunk, eval.script_error.c_str());
        OA_CHECK(false);
        return;
    }
    if (eval.plan.ok())
        std::fprintf(stderr, "%s plan succeeded\n", chunk);
    OA_CHECK(!eval.plan.ok());
    if (eval.plan.error.find(part) == std::string::npos)
        std::fprintf(stderr, "%s plan '%s'\n", chunk, eval.plan.error.c_str());
    OA_CHECK(eval.plan.error.find(part) != std::string::npos);
}

void test_pairs(HostBox& box) {
    for (const char* name : k_cases) {
        const std::string lua_name = std::string(name) + ".lua";
        const std::filesystem::path sm_path =
            std::filesystem::path(fixture_dir()) / (std::string(name) + ".sm");
        std::vector<uint8_t> bytes;
        std::string failure;
        if (!read_bytes(sm_path, bytes, failure)) {
            std::fprintf(stderr, "%s\n", failure.c_str());
            OA_CHECK(false);
            continue;
        }
        const sm::ParseResult parsed = sm::parse_rules(bytes);
        if (parsed.has_errors())
            dump_diagnostics(name, parsed);
        OA_CHECK(!parsed.has_errors());
        FileCapture capture;
        capture.entry = lua_name;
        if (!run_fixture(box, lua_name.c_str(), capture)) {
            std::fprintf(stderr, "%s plan '%s'\n", name, capture.plan.error.c_str());
            OA_CHECK(false);
            continue;
        }
        OA_CHECK(capture.plan.sources.size() == 1);
        if (capture.plan.sources.size() != 1)
            continue;
        OA_CHECK(capture.plan.sources[0].name == lua_name);
        if (!sm::equivalent(capture.plan.sources[0].rules, parsed.rules)) {
            dump_rules("lua", capture.plan.sources[0].rules);
            dump_rules("file", parsed.rules);
        }
        OA_CHECK(sm::equivalent(capture.plan.sources[0].rules, parsed.rules));
        const rules::ApplyResult lua_result =
            apply_file(capture.plan.sources[0].rules, capture.plan.sources[0].name);
        const rules::ApplyResult file_result = apply_file(parsed.rules, std::string(name) + ".sm");
        expect_same_writes(name, lua_result, file_result);
        if (std::string(name) == "needs") {
            OA_CHECK(has_write(lua_result.writes, "units/RIDGECOM.fbi", "WorkerTime"));
        } else if (std::string(name) == "selectors") {
            OA_CHECK(has_write(lua_result.writes, "units/RIDGECOM.fbi", "MaxDamage"));
            OA_CHECK(!has_write(lua_result.writes, "units/RIDGEFAB.fbi", "MaxDamage"));
            OA_CHECK(has_write(lua_result.writes, "units/RIDGECOM.fbi", "WorkerTime"));
            OA_CHECK(has_write(lua_result.writes, "units/RIDGECOM.fbi", "MaxVelocity"));
        } else if (std::string(name) == "targets") {
            OA_CHECK(has_write(lua_result.writes, "weapons/ridge.tdf", "range"));
            OA_CHECK(has_write(lua_result.writes, "features/corpses/ridge.tdf", "metal"));
            OA_CHECK(!has_write(lua_result.writes, "features/rocks/ridge.tdf", "metal"));
            OA_CHECK(has_write(lua_result.writes, "gamedata/sidedata.tdf", "commander"));
            OA_CHECK(has_write(lua_result.writes, "weapons/ridge.tdf", "reloadtime"));
        } else if (std::string(name) == "depth") {
            OA_CHECK(has_block_write(lua_result.writes, "weapons/ridge.tdf", "DAMAGE", "default"));
        } else if (std::string(name) == "line") {
            OA_CHECK(capture.plan.sources[0].rules.blocks.size() == 2);
            if (capture.plan.sources[0].rules.blocks.size() == 2) {
                const sm::Block& all_block = capture.plan.sources[0].rules.blocks[0];
                const sm::Block& where_block = capture.plan.sources[0].rules.blocks[1];
                OA_CHECK(all_block.line == 5);
                OA_CHECK(where_block.line == 6);
                OA_CHECK(where_block.needs.size() == 1);
                if (where_block.needs.size() == 1)
                    OA_CHECK(where_block.needs[0].line == 6);
                OA_CHECK(all_block.changes.size() == 1);
                if (all_block.changes.size() == 1)
                    OA_CHECK(all_block.changes[0].line == 5);
                OA_CHECK(where_block.changes.size() == 1);
                if (where_block.changes.size() == 1)
                    OA_CHECK(where_block.changes[0].line == 6);
            }
        }
        if (std::string(name) == "changes") {
            bool quoted = false;
            for (const sm::Diagnostic& item : parsed.diagnostics) {
                if (item.code == sm::DiagnosticCode::quoted_number)
                    quoted = true;
            }
            OA_CHECK(quoted);
            bool empty_text = false;
            for (const sm::Diagnostic& item : parsed.diagnostics) {
                if (item.code == sm::DiagnosticCode::empty_value)
                    empty_text = true;
            }
            OA_CHECK(empty_text);
            bool saw_empty = false;
            bool saw_remove = false;
            const std::vector<sm::Block>& blocks = capture.plan.sources[0].rules.blocks;
            if (blocks.size() == 1) {
                for (const sm::Change& change : blocks[0].changes) {
                    if (change.field == "yardmap")
                        saw_empty =
                            change.kind == sm::ChangeKind::assign_text && change.text.empty();
                    if (change.field == "gone")
                        saw_remove = change.kind == sm::ChangeKind::remove;
                }
            }
            OA_CHECK(saw_empty);
            OA_CHECK(saw_remove);
        }
    }
}

void test_order(HostBox& box) {
    constexpr const char* k_source = R"(local function make(first_key, second_key)
  local needs = {}
  local changes = {}
  local function put(key)
    if key == "alpha" then
      needs[key] = 1
      changes[key] = set(5)
    else
      needs[key] = "ridge"
      changes[key] = scale(2)
    end
  end
  put(first_key)
  put(second_key)
  return where(needs):change(changes)
end
return { make("Zeta", "alpha"), make("alpha", "Zeta") }
)";
    Eval eval;
    eval.source = k_source;
    eval.chunk = "order.lua";
    if (!run_source(box, eval)) {
        std::fprintf(stderr, "order script '%s'\n", eval.script_error.c_str());
        OA_CHECK(false);
        return;
    }
    OA_CHECK(eval.plan.ok());
    OA_CHECK(eval.plan.sources.size() == 1);
    if (!eval.plan.ok() || eval.plan.sources.size() != 1)
        return;
    const std::vector<sm::Block>& blocks = eval.plan.sources[0].rules.blocks;
    OA_CHECK(blocks.size() == 2);
    if (blocks.size() != 2)
        return;
    OA_CHECK(sm::equivalent(blocks[0], blocks[1]));
    for (const sm::Block& block : blocks) {
        OA_CHECK(block.needs.size() == 2);
        OA_CHECK(block.changes.size() == 2);
        if (block.needs.size() != 2 || block.changes.size() != 2)
            continue;
        OA_CHECK(block.needs[0].field == "alpha");
        OA_CHECK(block.needs[0].kind == sm::NeedKind::range);
        OA_CHECK(block.needs[0].low == 1);
        OA_CHECK(block.needs[0].high == 1);
        OA_CHECK(block.needs[1].field == "Zeta");
        OA_CHECK(block.needs[1].kind == sm::NeedKind::equals);
        OA_CHECK(block.needs[1].text == "ridge");
        OA_CHECK(block.changes[0].field == "alpha");
        OA_CHECK(block.changes[0].kind == sm::ChangeKind::assign);
        OA_CHECK(block.changes[0].value == 5);
        OA_CHECK(block.changes[1].field == "Zeta");
        OA_CHECK(block.changes[1].kind == sm::ChangeKind::scale);
        OA_CHECK(block.changes[1].value == 2);
    }
}

void test_reused_tables(HostBox& box) {
    constexpr const char* k_source = R"(local rules = {}
local needs, changes = {}, {}
for level = 1, 3 do
  needs.techlevel = level
  changes.workertime = scale(level)
  rules[#rules + 1] = where(needs):change(changes)
end
local held = { canfly = true }
local open = where(held)
held.canfly = nil
held[1] = "x"
rules[#rules + 1] = open:change { maxvelocity = 2 }
needs.techlevel = 9
needs[1] = "x"
changes.workertime = nil
for n = 1, 5000 do
  changes["f" .. n] = n
end
return rules
)";
    Eval eval;
    eval.source = k_source;
    eval.chunk = "reused.lua";
    if (!run_source(box, eval)) {
        std::fprintf(stderr, "reused script '%s'\n", eval.script_error.c_str());
        OA_CHECK(false);
        return;
    }
    if (!eval.plan.ok())
        std::fprintf(stderr, "reused plan '%s'\n", eval.plan.error.c_str());
    OA_CHECK(eval.plan.ok());
    OA_CHECK(eval.plan.sources.size() == 1);
    if (!eval.plan.ok() || eval.plan.sources.size() != 1)
        return;
    const std::vector<sm::Block>& blocks = eval.plan.sources[0].rules.blocks;
    OA_CHECK(blocks.size() == 4);
    if (blocks.size() != 4)
        return;
    for (std::size_t block_index = 0; block_index < 3; ++block_index) {
        const sm::Block& block = blocks[block_index];
        const double level = static_cast<double>(block_index + 1);
        OA_CHECK(block.selector == sm::Selector::group);
        OA_CHECK(block.needs.size() == 1);
        OA_CHECK(block.changes.size() == 1);
        if (block.needs.size() != 1 || block.changes.size() != 1)
            continue;
        OA_CHECK(block.needs[0].field == "techlevel");
        OA_CHECK(block.needs[0].kind == sm::NeedKind::range);
        OA_CHECK(block.needs[0].low == level);
        OA_CHECK(block.needs[0].high == level);
        OA_CHECK(block.changes[0].field == "workertime");
        OA_CHECK(block.changes[0].kind == sm::ChangeKind::scale);
        OA_CHECK(block.changes[0].value == level);
        OA_CHECK(block.changes[0].line == 6);
    }
    const sm::Block& held = blocks[3];
    OA_CHECK(held.needs.size() == 1);
    OA_CHECK(held.changes.size() == 1);
    if (held.needs.size() == 1 && held.changes.size() == 1) {
        OA_CHECK(held.needs[0].field == "canfly");
        OA_CHECK(held.needs[0].kind == sm::NeedKind::range);
        OA_CHECK(!held.needs[0].negated);
        OA_CHECK(held.needs[0].line == 9);
        OA_CHECK(held.changes[0].field == "maxvelocity");
        OA_CHECK(held.changes[0].value == 2);
    }
}

void test_depth_order(HostBox& box) {
    constexpr const char* k_source = R"(local changes = {}
changes["<alldepth2>"] = scale(2)
changes["<AllDepth2>"] = scale(3)
return { on("weapons", "tdf"):change(changes) }
)";
    Eval eval;
    eval.source = k_source;
    eval.chunk = "depth-order.lua";
    if (!run_source(box, eval)) {
        std::fprintf(stderr, "depth order script '%s'\n", eval.script_error.c_str());
        OA_CHECK(false);
        return;
    }
    OA_CHECK(eval.plan.ok());
    if (!eval.plan.ok() || eval.plan.sources.size() != 1)
        return;
    const std::vector<sm::Block>& blocks = eval.plan.sources[0].rules.blocks;
    OA_CHECK(blocks.size() == 1);
    if (blocks.size() != 1 || blocks[0].changes.size() != 2)
        return;
    OA_CHECK(blocks[0].changes[0].field == "<AllDepth2>");
    OA_CHECK(blocks[0].changes[0].all_depth2);
    OA_CHECK(blocks[0].changes[0].value == 3);
    OA_CHECK(blocks[0].changes[1].field == "<alldepth2>");
    OA_CHECK(blocks[0].changes[1].all_depth2);
    OA_CHECK(blocks[0].changes[1].value == 2);
    OA_CHECK(blocks[0].target.folder == "weapons");
    OA_CHECK(blocks[0].target.extension == "tdf");
}

void test_lines_and_targets(HostBox& box) {
    constexpr const char* k_kept = R"(return {
where { builder = true }
:on("units", "fbi"):change { workertime = 4 },
}
)";
    Eval kept;
    kept.source = k_kept;
    kept.chunk = "kept-line.lua";
    if (!run_source(box, kept)) {
        std::fprintf(stderr, "kept line script '%s'\n", kept.script_error.c_str());
        OA_CHECK(false);
    } else {
        OA_CHECK(kept.plan.ok());
        OA_CHECK(kept.plan.sources.size() == 1);
        if (kept.plan.ok() && kept.plan.sources.size() == 1 &&
            kept.plan.sources[0].rules.blocks.size() == 1) {
            const sm::Block& block = kept.plan.sources[0].rules.blocks[0];
            OA_CHECK(block.line == 2);
            OA_CHECK(block.selector == sm::Selector::group);
            OA_CHECK(block.target.folder == "units");
            OA_CHECK(block.target.extension == "fbi");
            OA_CHECK(block.changes.size() == 1);
            if (block.changes.size() == 1)
                OA_CHECK(block.changes[0].line == 3);
        } else {
            OA_CHECK(false);
        }
    }

    constexpr const char* k_canon = R"(return {
on("weapons", ".TDF"):change { range = 1 },
on("Features\\Corpses", "tdf"):change { metal = 1 },
}
)";
    Eval canon;
    canon.source = k_canon;
    canon.chunk = "canon.lua";
    if (!run_source(box, canon)) {
        std::fprintf(stderr, "canon script '%s'\n", canon.script_error.c_str());
        OA_CHECK(false);
        return;
    }
    OA_CHECK(canon.plan.ok());
    if (!canon.plan.ok() || canon.plan.sources.size() != 1 ||
        canon.plan.sources[0].rules.blocks.size() != 2) {
        OA_CHECK(false);
        return;
    }
    const sm::Block& weapons = canon.plan.sources[0].rules.blocks[0];
    const sm::Block& corpses = canon.plan.sources[0].rules.blocks[1];
    OA_CHECK(weapons.target.folder == "weapons");
    OA_CHECK(weapons.target.extension == "tdf");
    OA_CHECK(corpses.target.folder == "features/corpses");
    OA_CHECK(corpses.target.extension == "tdf");
    OA_CHECK(weapons.selector == sm::Selector::all);
}

void test_mixed(HostBox& box) {
    Asked asked;
    asked.files["rules/mid.sm"] =
        std::vector<uint8_t>{'[', '*', 'A', 'l', 'l', '*', ']', '{', 'M', 'a', 'x', 'D',
                             'a', 'm', 'a', 'g', 'e', '=', '8', '5', '0', ';', '}', '\n'};
    constexpr const char* k_mixed = R"(return {
all():change { maxdamage = 1 },
sm("rules/mid.sm"),
named("RIDGECOM"):change { maxvelocity = 3 },
}
)";
    Eval mixed;
    mixed.source = k_mixed;
    mixed.chunk = "mixed.lua";
    mixed.reader = asking_reader(asked);
    mixed.entry = "mutator.lua";
    if (!run_source(box, mixed)) {
        std::fprintf(stderr, "mixed script '%s'\n", mixed.script_error.c_str());
        OA_CHECK(false);
        return;
    }
    if (!mixed.plan.ok())
        std::fprintf(stderr, "mixed plan '%s'\n", mixed.plan.error.c_str());
    OA_CHECK(mixed.plan.ok());
    OA_CHECK(mixed.plan.sources.size() == 3);
    if (mixed.plan.sources.size() != 3)
        return;
    OA_CHECK(mixed.plan.sources[0].name == "mutator.lua");
    OA_CHECK(mixed.plan.sources[0].rules.blocks.size() == 1);
    OA_CHECK(mixed.plan.sources[1].name == "rules/mid.sm");
    OA_CHECK(mixed.plan.sources[1].rules.blocks.size() == 1);
    OA_CHECK(mixed.plan.sources[2].name == "mutator.lua");
    OA_CHECK(mixed.plan.sources[2].rules.blocks.size() == 1);
    OA_CHECK(asked.paths.size() == 1);
    if (asked.paths.size() == 1)
        OA_CHECK(asked.paths[0] == "rules/mid.sm");

    asked.paths.clear();
    constexpr const char* k_grouped = R"(return {
all():change { marker = 1 },
all():change { marker = 2 },
sm("rules/mid.sm"),
}
)";
    Eval grouped;
    grouped.source = k_grouped;
    grouped.chunk = "grouped.lua";
    grouped.reader = asking_reader(asked);
    grouped.entry = "mutator.lua";
    if (!run_source(box, grouped)) {
        std::fprintf(stderr, "grouped script '%s'\n", grouped.script_error.c_str());
        OA_CHECK(false);
        return;
    }
    OA_CHECK(grouped.plan.ok());
    OA_CHECK(grouped.plan.sources.size() == 2);
    if (grouped.plan.sources.size() == 2) {
        OA_CHECK(grouped.plan.sources[0].name == "mutator.lua");
        OA_CHECK(grouped.plan.sources[0].rules.blocks.size() == 2);
        OA_CHECK(grouped.plan.sources[1].name == "rules/mid.sm");
    }

    asked.paths.clear();
    constexpr const char* k_slash = R"(return { sm("rules\\mid.sm") }
)";
    Eval slash;
    slash.source = k_slash;
    slash.chunk = "slash.lua";
    slash.reader = asking_reader(asked);
    if (!run_source(box, slash)) {
        std::fprintf(stderr, "slash script '%s'\n", slash.script_error.c_str());
        OA_CHECK(false);
        return;
    }
    if (!slash.plan.ok())
        std::fprintf(stderr, "slash plan '%s'\n", slash.plan.error.c_str());
    OA_CHECK(slash.plan.ok());
    OA_CHECK(slash.plan.sources.size() == 1);
    if (slash.plan.sources.size() == 1)
        OA_CHECK(slash.plan.sources[0].name == "rules/mid.sm");
    OA_CHECK(asked.paths.size() == 1);
    if (asked.paths.size() == 1)
        OA_CHECK(asked.paths[0] == "rules/mid.sm");
}

void test_warnings_and_errors(HostBox& box) {
    Asked asked;
    const char* warning_text = "[*All*]{MaxDamage=\"850\";}\n";
    asked.files["rules/mid.sm"] =
        std::vector<uint8_t>(warning_text, warning_text + std::strlen(warning_text));
    constexpr const char* k_warn = R"(return { sm("rules/mid.sm") }
)";
    Eval warn;
    warn.source = k_warn;
    warn.chunk = "warn.lua";
    warn.reader = asking_reader(asked);
    if (!run_source(box, warn)) {
        std::fprintf(stderr, "warn script '%s'\n", warn.script_error.c_str());
        OA_CHECK(false);
    } else {
        if (!warn.plan.ok())
            std::fprintf(stderr, "warn plan '%s'\n", warn.plan.error.c_str());
        OA_CHECK(warn.plan.ok());
        OA_CHECK(warn.plan.sources.size() == 1);
        OA_CHECK(!warn.plan.diagnostics.empty());
        OA_CHECK(warn.plan.diagnostic_sources.size() == warn.plan.diagnostics.size());
        if (!warn.plan.diagnostic_sources.empty())
            OA_CHECK(warn.plan.diagnostic_sources[0] == "rules/mid.sm");
        bool quoted = false;
        for (const sm::Diagnostic& item : warn.plan.diagnostics) {
            if (item.code == sm::DiagnosticCode::quoted_number &&
                item.severity == sm::Severity::warning)
                quoted = true;
        }
        OA_CHECK(quoted);
    }

    const char* broken_text = "[RIDGECOM]{Speed=*1e2;}\n";
    asked.files["rules/broken.sm"] =
        std::vector<uint8_t>(broken_text, broken_text + std::strlen(broken_text));
    constexpr const char* k_broken = R"(return { sm("rules/broken.sm") }
)";
    Eval broken;
    broken.source = k_broken;
    broken.chunk = "broken.lua";
    broken.reader = asking_reader(asked);
    if (!run_source(box, broken)) {
        std::fprintf(stderr, "broken script '%s'\n", broken.script_error.c_str());
        OA_CHECK(false);
    } else {
        OA_CHECK(!broken.plan.ok());
        if (broken.plan.error.find("rules/broken.sm:1:") == std::string::npos)
            std::fprintf(stderr, "broken plan '%s'\n", broken.plan.error.c_str());
        OA_CHECK(broken.plan.error.find("rules/broken.sm:1:") != std::string::npos);
        OA_CHECK(broken.plan.error.find("bad number") != std::string::npos);
        OA_CHECK(broken.plan.sources.empty());
        OA_CHECK(!broken.plan.diagnostics.empty());
    }

    asked.paths.clear();
    asked.called = false;
    constexpr const char* k_missing = R"(return { sm("rules/missing.sm") }
)";
    Eval missing;
    missing.source = k_missing;
    missing.chunk = "missing.lua";
    missing.reader = asking_reader(asked);
    if (!run_source(box, missing)) {
        std::fprintf(stderr, "missing script '%s'\n", missing.script_error.c_str());
        OA_CHECK(false);
    } else {
        OA_CHECK(!missing.plan.ok());
        if (missing.plan.error.find("cannot read \"rules/missing.sm\"") == std::string::npos)
            std::fprintf(stderr, "missing plan '%s'\n", missing.plan.error.c_str());
        OA_CHECK(missing.plan.error.find("cannot read \"rules/missing.sm\"") != std::string::npos);
        OA_CHECK(asked.called);
    }
}

void test_outside(HostBox& box) {
    constexpr const char* k_paths[] = {
        "../x.sm",
        "/x.sm",
        "C:/x.sm",
        "rules/../x.sm",
    };
    for (const char* path : k_paths) {
        Asked asked;
        const std::string source = std::string("return { sm(\"") + path + "\") }\n";
        Eval eval;
        eval.source = source.c_str();
        eval.chunk = "outside.lua";
        eval.reader = asking_reader(asked);
        if (!run_source(box, eval)) {
            std::fprintf(stderr, "outside %s script '%s'\n", path, eval.script_error.c_str());
            OA_CHECK(false);
            continue;
        }
        OA_CHECK(!eval.plan.ok());
        OA_CHECK(!asked.called);
        const std::string part = std::string("sm path \"") + path + "\" is outside the package";
        if (eval.plan.error.find(part) == std::string::npos)
            std::fprintf(stderr, "outside %s plan '%s'\n", path, eval.plan.error.c_str());
        OA_CHECK(eval.plan.error.find(part) != std::string::npos);
    }
}

void test_refusals(HostBox& box) {
    expect_plan_error(
        box,
        "table.lua",
        "return { all():change { marker = 1 }, {}, named(\"RIDGECOM\"):change { marker = 2 } }\n",
        "rules[2] is not a rule: a table"
    );
    expect_plan_error(
        box,
        "incomplete.lua",
        "return { where { builder = true } }\n",
        "rules[1]: the where on line 1 has no change"
    );
    expect_plan_error(
        box,
        "on-open.lua",
        "return { on(\"weapons\", \"tdf\") }\n",
        "rules[1]: the on on line 1 has no change"
    );
    expect_script_error(
        box,
        "lock.lua",
        "return { all():change { marker = lock(\"text\") } }\n",
        "lock expects a number"
    );
    expect_script_error(
        box, "on.lua", "return { on(\"weapons\") }\n", "on expects a folder and an extension"
    );
    expect_script_error(
        box,
        "twice.lua",
        "local rule = all():change { marker = 1 }\nrule:change { marker = 2 }\nreturn { rule }\n",
        "change was already called"
    );
}

void test_limits(HostBox& box) {
    constexpr const char* k_blocks = R"(local rules = {}
for n = 1, 4097 do
  rules[n] = all():change { marker = 1 }
end
return rules
)";
    expect_plan_error(box, "blocks.lua", k_blocks, "too many blocks");

    constexpr const char* k_statements = R"(local changes = {}
for n = 1, 4097 do
  changes["f" .. n] = 1
end
return { all():change(changes) }
)";
    expect_script_error(box, "statements.lua", k_statements, "too many statements");

    constexpr const char* k_sum = R"(local needs = {}
for n = 1, 4096 do
  needs["n" .. n] = 1
end
return { where(needs):change { extra = 1 } }
)";
    expect_script_error(box, "sum.lua", k_sum, "too many statements");

    constexpr const char* k_text = R"(local changes = {}
changes[string.rep("a", 1025)] = 1
return { all():change(changes) }
)";
    expect_script_error(box, "text.lua", k_text, "text is longer than 1024 bytes");

    Asked wide_asked;
    std::string wide_text;
    wide_text.reserve(static_cast<std::size_t>(3000) * 20);
    for (int count = 0; count < 3000; ++count)
        wide_text += "[*All*]{marker=1;}\n";
    wide_asked.files["rules/wide.sm"] = std::vector<uint8_t>(wide_text.begin(), wide_text.end());
    constexpr const char* k_wide = R"(local rules = {}
for n = 1, 2000 do
  rules[n] = all():change { marker = 1 }
end
rules[2001] = sm("rules/wide.sm")
return rules
)";
    Eval wide;
    wide.source = k_wide;
    wide.chunk = "wide.lua";
    wide.reader = asking_reader(wide_asked);
    if (!run_source(box, wide)) {
        std::fprintf(stderr, "wide script '%s'\n", wide.script_error.c_str());
        OA_CHECK(false);
    } else {
        OA_CHECK(!wide.plan.ok());
        OA_CHECK(wide.plan.error.find("too many blocks") != std::string::npos);
        OA_CHECK(wide_asked.paths.size() == 1);
    }

    Asked once_asked;
    const std::string one_text = "[*All*]{marker=1;}\n";
    once_asked.files["rules/one.sm"] = std::vector<uint8_t>(one_text.begin(), one_text.end());
    const std::string source_list = "local rules = {}\nfor n = 1, " +
                                    std::to_string(mutators::max_plan_sources + 1) +
                                    " do\n  rules[n] = sm(\"rules/one.sm\")\nend\nreturn rules\n";
    Eval sources;
    sources.source = source_list.c_str();
    sources.chunk = "sources.lua";
    sources.reader = asking_reader(once_asked);
    if (!run_source(box, sources)) {
        std::fprintf(stderr, "sources script '%s'\n", sources.script_error.c_str());
        OA_CHECK(false);
    } else {
        OA_CHECK(!sources.plan.ok());
        OA_CHECK(sources.plan.error.find("too many sources") != std::string::npos);
        OA_CHECK(once_asked.paths.size() == 1);
    }

    Asked repeated;
    const std::string quoted = "[*All*]{MaxDamage=\"850\";}\n";
    repeated.files["rules/mid.sm"] = std::vector<uint8_t>(quoted.begin(), quoted.end());
    constexpr const char* k_repeat = R"(return {
sm("rules/mid.sm"),
sm("rules/mid.sm"),
}
)";
    Eval repeat;
    repeat.source = k_repeat;
    repeat.chunk = "repeat.lua";
    repeat.reader = asking_reader(repeated);
    if (!run_source(box, repeat)) {
        std::fprintf(stderr, "repeat script '%s'\n", repeat.script_error.c_str());
        OA_CHECK(false);
    } else {
        OA_CHECK(repeat.plan.ok());
        OA_CHECK(repeat.plan.sources.size() == 2);
        OA_CHECK(repeated.paths.size() == 1);
        OA_CHECK(repeat.plan.diagnostics.size() == 1);
        OA_CHECK(repeat.plan.diagnostic_sources.size() == 1);
    }
}

void test_metatable(HostBox& box) {
    constexpr const char* k_source = R"(local rule = all()
local change = scale(1)
local source = sm("rules/mid.sm")
local blank = empty
local hidden = getmetatable(rule) == false and getmetatable(change) == false and getmetatable(source) == false and getmetatable(blank) == false
local rule_ok = pcall(setmetatable, rule, {})
local change_ok = pcall(setmetatable, change, {})
local source_ok = pcall(setmetatable, source, {})
local blank_ok = pcall(setmetatable, blank, {})
return hidden and not rule_ok and not change_ok and not source_ok and not blank_ok
)";
    Eval eval;
    eval.source = k_source;
    eval.chunk = "meta.lua";
    eval.want_plan = false;
    if (!run_source(box, eval)) {
        std::fprintf(stderr, "meta script '%s'\n", eval.script_error.c_str());
        OA_CHECK(false);
        return;
    }
    OA_CHECK(eval.flag);
}

void test_selectors(HostBox& box) {
    expect_script_error(
        box,
        "named-where.lua",
        "return { named(\"RIDGECOM\"):where { builder = true }:change { workertime = 1 } }\n",
        "already has a selector"
    );
    expect_script_error(
        box,
        "where-named.lua",
        "return { where { builder = true }:named(\"RIDGECOM\"):change { workertime = 1 } }\n",
        "already has a selector"
    );
    expect_script_error(
        box,
        "selector-line.lua",
        "return { named(\"RIDGECOM\")\n:where { builder = true } }\n",
        "where on line 2: a rule already has a selector"
    );
}

void test_leaks(HostBox& box) {
    constexpr const char* k_source = R"(local ok = true
for n = 1, 5000 do
  if pcall(function() all():change { a = 1 }:change { a = 2 } end) then
    ok = false
  end
  if pcall(function() all():change { maxvelocity = {} } end) then
    ok = false
  end
end
return ok
)";
    Eval eval;
    eval.source = k_source;
    eval.chunk = "leaks.lua";
    eval.want_plan = false;
    if (!run_source(box, eval)) {
        std::fprintf(stderr, "leaks script '%s'\n", eval.script_error.c_str());
        OA_CHECK(false);
        return;
    }
    OA_CHECK(eval.flag);
}

void test_throwing_reader(HostBox& box) {
    Eval eval;
    eval.source = "return { sm(\"rules/mid.sm\") }\n";
    eval.chunk = "throw.lua";
    eval.reader = [](std::string_view, std::string*) -> std::optional<std::vector<uint8_t>> {
        throw std::runtime_error("disk");
    };
    if (!run_source(box, eval)) {
        std::fprintf(stderr, "throw script '%s'\n", eval.script_error.c_str());
        OA_CHECK(false);
        return;
    }
    OA_CHECK(!eval.plan.ok());
    OA_CHECK(eval.plan.error.find("cannot read \"rules/mid.sm\"") != std::string::npos);
    OA_CHECK(eval.plan.error.find("disk") != std::string::npos);
}

} // namespace

int main() {
    HostBox box;
    if (!open_host(box)) {
        OA_CHECK(false);
        return oa::test::check_exit_status();
    }
    test_pairs(box);
    test_order(box);
    test_reused_tables(box);
    test_depth_order(box);
    test_lines_and_targets(box);
    test_mixed(box);
    test_warnings_and_errors(box);
    test_outside(box);
    test_refusals(box);
    test_limits(box);
    test_metatable(box);
    test_selectors(box);
    test_leaks(box);
    test_throwing_reader(box);
    return oa::test::check_exit_status();
}
