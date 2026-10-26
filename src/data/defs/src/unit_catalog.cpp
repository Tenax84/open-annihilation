// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/defs/unit_catalog.hpp"
#include "oa/data/defs/layout.hpp"

#include "oa/data/defs/unit_records.hpp"
#include "oa/formats/tdf.hpp"
#include "oa/base/text.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace oa::data::defs {
namespace {

constexpr std::size_t key_capacity = 0x20;

struct NameList {
    char (*names)[path_capacity];
    uint32_t count{};
    uint32_t capacity{};
    bool failed{};
};

void collect_name(void* user, const char* name) {
    auto* list = static_cast<NameList*>(user);
    if (list->failed)
        return;
    if (list->count == list->capacity) {
        const uint32_t grown = list->capacity == 0 ? 64u : list->capacity * 2u;
        if (grown > max_download_files) {
            list->failed = true;
            return;
        }
        auto* names = static_cast<char (*)[path_capacity]>(
            std::realloc(list->names, sizeof *list->names * grown)
        );
        if (names == nullptr) {
            list->failed = true;
            return;
        }
        list->names = names;
        list->capacity = grown;
    }
    oa::base::text::copy_padded(list->names[list->count], name, path_capacity - 1);
    list->names[list->count][path_capacity - 1] = '\0';
    ++list->count;
}

/// Frees the download menu table and clears its pointer and count.
///
/// @param[in,out] downloads table to release
void download_menu_table_free(DownloadMenuTable* downloads) noexcept {
    std::free(downloads->groups);
    downloads->groups = nullptr;
    downloads->count = 0;
}

// Section i of one DOWNLOAD file: the entry names the first unit whose unitname matches UNITMENU.
// An entry whose UNITMENU is missing or names no unit is left zeroed.
void load_download_entry(
    const formats::tdf::Block* section, const UnitDefTables* tables, DownloadMenuEntry& entry
) noexcept {
    char builder[key_capacity];
    if (!formats::tdf::get_string(section, "UNITMENU", builder, sizeof builder, ""))
        return;
    for (uint32_t index = 0; index < tables->count; ++index) {
        if (formats::tdf::compare_nocase(tables->records[index].unit_name, builder) != 0)
            continue;
        entry.builder_index = static_cast<uint16_t>(index);
        entry.menu = static_cast<uint8_t>(formats::tdf::get_int(section, "MENU", 0));
        entry.button = static_cast<uint8_t>(formats::tdf::get_int(section, "BUTTON", 0));
        formats::tdf::get_string(
            section, "UNITNAME", entry.unit_name, download_unit_name_capacity, ""
        );
        return;
    }
}

} // namespace

void unit_def_tables_init(UnitDefTables* tables) noexcept {
    tables->records = nullptr;
    tables->count = 0;
    unit_def_blocks_init(&tables->blocks);
    category_registry_init(&tables->categories);
    tables->downloads = {nullptr, 0};
}

bool unit_def_tables_allocate(UnitDefTables* tables, uint32_t count) noexcept {
    unit_def_tables_free(tables);
    if (count == 0 || count > max_unit_defs)
        return false;
    tables->records = static_cast<UnitDef*>(std::calloc(count, sizeof(UnitDef)));
    if (tables->records == nullptr)
        return false;
    oa::base::text::copy_terminated(tables->records[0].unit_name, reserved_unit_name);
    tables->records[0].flags = OA_UNIT_DEF_FLAG_AVAILABLE;
    tables->count = count;
    return true;
}

void unit_def_tables_free(UnitDefTables* tables) noexcept {
    for (uint32_t index = 1; index < tables->count; ++index) {
        UnitDef& unit = tables->records[index];
        unit.yard_map = 0;
        unit.script = 0;
        if (unit.build_ids != 0) {
            unit.build_id_count = 0;
            unit.build_ids = 0;
        }
    }
    for (uint32_t block = 0; block < tables->blocks.count; ++block)
        std::free(tables->blocks.blocks[block].bytes);
    std::free(tables->blocks.blocks);
    std::free(tables->records);
    category_registry_clear(&tables->categories);
    download_menu_table_free(&tables->downloads);
    unit_def_tables_init(tables);
}

uint16_t* unit_def_build_ids(const UnitDefTables* tables, const UnitDef& unit) noexcept {
    return reinterpret_cast<uint16_t*>(unit_def_block(&tables->blocks, unit.build_ids));
}

bool load_build_lists(
    const Files* files,
    const char* variant,
    UnitDefTables* tables,
    const data::limits::BuildLists& lists
) noexcept {
    const uint32_t kept = data::limits::build_list_kept(lists);
    const auto list_bytes = static_cast<uint32_t>(build_list_entries(lists) * sizeof(uint16_t));
    char path[path_capacity];
    build_variant_path(
        files,
        path,
        sizeof path,
        directory_name(DataDirectory::gamedata),
        "sidedata",
        "tdf",
        variant
    );
    formats::tdf::Document sidedata;
    formats::tdf::document_init(&sidedata);
    if (!load_tdf_file(files, path, &sidedata, nullptr)) {
        formats::tdf::document_free(&sidedata);
        return false;
    }
    uint16_t scratch[data::limits::highest_build_list_copy + 1] = {};
    bool allocated = true;
    for (uint32_t index = 1; index < tables->count; ++index) {
        UnitDef& unit = tables->records[index];
        unit.build_id_count = 0;
        unit.build_ids = 0;
        if ((unit.flags & OA_UNIT_DEF_FLAG_BUILDER) == 0)
            continue;
        formats::tdf::reset_cursor(&sidedata);
        if (formats::tdf::select_section(&sidedata, "CANBUILD") &&
            formats::tdf::select_section(&sidedata, unit.unit_name)) {
            const formats::tdf::Block* list = formats::tdf::cursor(&sidedata);
            uint32_t found = 0;
            char key[key_capacity];
            char name[key_capacity];
            for (uint32_t entry = 1;; ++entry) {
                std::snprintf(key, sizeof key, "canbuild%u", entry);
                if (!formats::tdf::get_string(list, key, name, sizeof name, ""))
                    break;
                const uint16_t type_id = unit_defs_type_id(tables->records, tables->count, name);
                if (type_id != 0 && found < kept)
                    scratch[found++] = type_id;
            }
            unit.build_id_count = found;
        }
        unit.build_ids = unit_def_blocks_alloc(&tables->blocks, list_bytes);
        if (unit.build_ids == 0) {
            allocated = false;
            continue;
        }
        std::memcpy(unit_def_build_ids(tables, unit), scratch, kept * sizeof(uint16_t));
    }
    formats::tdf::document_free(&sidedata);
    return allocated;
}

bool load_download_menu(
    const Files* files,
    const char* variant,
    UnitDefTables* tables,
    const data::limits::BuildLists& lists
) noexcept {
    download_menu_table_free(&tables->downloads);
    NameList names{nullptr, 0, 0, false};
    files->list(
        files->context, directory_name(DataDirectory::download), "tdf", collect_name, &names
    );
    if (names.failed) {
        std::free(names.names);
        return false;
    }
    if (names.count != 0) {
        tables->downloads.groups =
            static_cast<DownloadMenuGroup*>(std::calloc(names.count, sizeof(DownloadMenuGroup)));
        if (tables->downloads.groups == nullptr) {
            std::free(names.names);
            return false;
        }
        tables->downloads.count = names.count;
    }
    for (uint32_t file = 0; file < names.count; ++file) {
        DownloadMenuGroup& group = tables->downloads.groups[file];
        char path[path_capacity];
        build_variant_path(
            files,
            path,
            sizeof path,
            directory_name(DataDirectory::download),
            names.names[file],
            "tdf",
            variant
        );
        formats::tdf::Document document;
        formats::tdf::document_init(&document);
        if (load_tdf_file(files, path, &document, nullptr)) {
            for (uint32_t section = 0; section < download_menu_entries; ++section) {
                formats::tdf::reset_cursor(&document);
                if (!formats::tdf::step_entry(&document, section))
                    break;
                group.count = static_cast<int32_t>(section + 1);
                load_download_entry(
                    formats::tdf::cursor(&document), tables, group.entries[section]
                );
            }
        }
        formats::tdf::document_free(&document);
    }
    std::free(names.names);
    for (uint32_t index = 0; index < tables->count; ++index) {
        UnitDef& unit = tables->records[index];
        for (uint32_t file = 0; file < tables->downloads.count; ++file) {
            const DownloadMenuGroup& group = tables->downloads.groups[file];
            for (int32_t entry = 0; entry < group.count; ++entry) {
                const DownloadMenuEntry& button = group.entries[entry];
                if (button.builder_index == static_cast<uint16_t>(index) &&
                    unit.gui_page_count < button.menu)
                    unit.gui_page_count = button.menu;
            }
        }
    }
    mark_downloadable_units(tables);
    append_download_build_ids(tables, lists);
    return true;
}

bool update_unit_def(
    const Files* files,
    UnitDefTables* tables,
    uint16_t type,
    const UnitDefSources& sources,
    const UnitScriptLoader& scripts
) noexcept {
    if (type == 0 || type >= tables->count)
        return false;
    UnitDef& unit = tables->records[type];
    if ((unit.flags & OA_UNIT_DEF_FLAG_AVAILABLE) == 0)
        return false;
    char path[path_capacity];
    build_variant_path(
        files,
        path,
        sizeof path,
        directory_name(DataDirectory::units),
        unit.unit_name,
        unit_extension(),
        nullptr
    );
    if (!load_unit_def(files, path, unit, sources))
        return false;
    build_variant_path(files, path, sizeof path, "scripts", unit.unit_name, "COB", nullptr);
    if (scripts.load != nullptr)
        scripts.load(scripts.context, type, path);
    return true;
}

void mark_downloadable_units(UnitDefTables* tables) noexcept {
    for (uint32_t index = 0; index < tables->count; ++index) {
        UnitDef& unit = tables->records[index];
        for (uint32_t file = 0; file < tables->downloads.count; ++file) {
            const char* name = tables->downloads.groups[file].entries[0].unit_name;
            if (formats::tdf::compare_nocase(unit.unit_name, name) == 0 &&
                (unit.flags & OA_UNIT_DEF_FLAG_DOWNLOADABLE) == 0)
                unit.flags |= OA_UNIT_DEF_FLAG_DOWNLOADABLE;
        }
    }
}

void append_download_build_ids(
    UnitDefTables* tables, const data::limits::BuildLists& lists
) noexcept {
    const uint32_t entries = download_build_list_entries(lists);
    for (uint32_t index = 0; index < tables->count; ++index) {
        UnitDef& unit = tables->records[index];
        uint16_t* list = unit_def_build_ids(tables, unit);
        if (list == nullptr)
            continue;
        for (uint32_t file = 0; file < tables->downloads.count; ++file) {
            const DownloadMenuGroup& group = tables->downloads.groups[file];
            for (int32_t entry = 0; entry < group.count; ++entry) {
                const DownloadMenuEntry& button = group.entries[entry];
                if (button.builder_index != index || unit.build_id_count >= entries)
                    continue;
                const uint16_t type_id =
                    unit_defs_type_id(tables->records, tables->count, button.unit_name);
                if (type_id != 0)
                    list[unit.build_id_count++] = type_id;
            }
        }
    }
}

} // namespace oa::data::defs
