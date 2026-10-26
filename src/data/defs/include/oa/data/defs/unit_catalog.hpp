// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The unit table after its FBI loads: CANBUILD build lists, DOWNLOAD menus
// and the build ids they add, and releasing everything the records point at.
#pragma once

#include "oa/core/unit_def.h"
#include "oa/data/defs/categories.hpp"
#include "oa/data/defs/files.hpp"
#include "oa/data/defs/unit_def_loader.hpp"
#include "oa/data/limits.hpp"

#include <cstdint>

namespace oa::data::defs {

/// Returns how many type ids a builder's UnitDef.build_ids holds: the CANBUILD
/// entries it keeps and one more, which only the download menus may fill. 31
/// for 3.1c's 30 CANBUILD entries.
///
/// @param lists the build-list limits
/// @return type ids a list holds
[[nodiscard]] constexpr uint32_t
build_list_entries(const data::limits::BuildLists& lists) noexcept {
    return data::limits::build_list_kept(lists) + 1;
}

/// The type ids a builder's list holds before the download menus stop adding theirs:
/// they add entries only while it holds fewer.
inline constexpr uint32_t download_build_list_limit = 31;

/// Returns how many type ids a builder's list may hold once the download menus have
/// added theirs.
///
/// The download menus add entries while a list holds fewer than 31, whatever its
/// CANBUILD copy keeps, so a raised copy that keeps 31 entries or more takes none of
/// theirs. A list that keeps every entry (BuildListOverflow::dynamic) takes them until
/// it holds build_list_entries.
///
/// @param lists the build-list limits
/// @return the most type ids a list holds after the download menus
[[nodiscard]] constexpr uint32_t
download_build_list_entries(const data::limits::BuildLists& lists) noexcept {
    const uint32_t entries = build_list_entries(lists);
    if (lists.overflow == data::limits::BuildListOverflow::dynamic)
        return entries;
    return entries < download_build_list_limit ? entries : download_build_list_limit;
}

/// Unit type records the catalog allocates, including the reserved slot.
/// A match can load one fewer unit file than this.
inline constexpr uint32_t max_unit_defs = 0x10000;

/// Download files the catalog reads. One more is refused.
inline constexpr uint32_t max_download_files = 0x1000;

inline constexpr uint32_t download_menu_entries = 5;
inline constexpr uint32_t download_unit_name_capacity = 0x20;

#pragma pack(push, 1)

// One DOWNLOAD\*.TDF section.
struct DownloadMenuEntry {
    uint16_t builder_index{};                          // table index of the UNITMENU unit
    uint8_t menu{};                                    // MENU: 1-based build page
    uint8_t button{};                                  // BUTTON: slot on that page
    char unit_name[download_unit_name_capacity + 1]{}; // UNITNAME
};

// One DOWNLOAD file: its section count and first five sections.
struct DownloadMenuGroup {
    int32_t count{};
    DownloadMenuEntry entries[download_menu_entries];
};

#pragma pack(pop)

static_assert(sizeof(DownloadMenuEntry) == 0x25);
static_assert(sizeof(DownloadMenuGroup) == 0xbd);

// The DOWNLOAD menus; ? 3.1c keeps their count and table in Game.download_menus.
struct DownloadMenuTable {
    DownloadMenuGroup* groups{};
    uint32_t count{};
};

// Game.unit_defs (records, slot 0 reserved) and Game.unit_def_count, with the
// blocks and category masks the records reference.
struct UnitDefTables {
    UnitDef* records{};
    uint32_t count{};
    UnitDefBlocks blocks;
    CategoryRegistry categories;
    DownloadMenuTable downloads;
};

// Unit name of reserved record 0, which stays in the catalog.
inline constexpr const char* reserved_unit_name = "None";

/// Empties the unit tables without freeing anything.
///
/// @param[out] tables tables to reset; their previous storage is not released
void unit_def_tables_init(UnitDefTables* tables) noexcept;

/// Frees the tables and allocates zeroed records[0..count) with record 0 reserved.
///
/// Record 0 is named reserved_unit_name and marked available.
///
/// @param[in,out] tables tables to reallocate
/// @param count number of records including slot 0; at most 65536
/// @return false when `count` is 0 or too large or the allocation fails
///     (the tables are left empty)
bool unit_def_tables_allocate(UnitDefTables* tables, uint32_t count) noexcept;

/// Releases the unit tables and everything their records point at.
///
/// Each record's yard map, script and build list are dropped, then the data
/// blocks, the records, the category registry and the download menus; the
/// tables end up as unit_def_tables_init leaves them. Model pointer arrays and
/// COB scripts belong to the runtime types and are not freed here.
///
/// @param[in,out] tables tables to release
void unit_def_tables_free(UnitDefTables* tables) noexcept;

/// Returns a builder's build list.
///
/// @param tables tables holding the record's data blocks
/// @param unit unit record
/// @return build_list_entries type ids, for the limits the lists were loaded
///     with, of which build_id_count are used; null when the unit has no list
[[nodiscard]] uint16_t*
unit_def_build_ids(const UnitDefTables* tables, const UnitDef& unit) noexcept;

/// Gives every builder a build list from GAMEDATA/SIDEDATA.TDF [CANBUILD].
///
/// Each builder's [CANBUILD] [<unitname>] canbuild1, canbuild2, ... entries
/// that name a loaded unit are collected, at most build_list_kept(lists) of
/// them (30 in 3.1c); non-builders lose any list they had.
///
/// @param files file boundary
/// @param variant game-data variant suffix; null or empty for none
/// @param[in,out] tables unit tables with a sorted catalog
/// @param lists how many entries a list keeps
/// @return false when SIDEDATA.TDF is missing or a list cannot be allocated
/// @quirk Every list is copied whole from one scratch list reused across
///     builders, so the slots past a builder's count hold the ids of earlier
///     builders.
bool load_build_lists(
    const Files* files,
    const char* variant,
    UnitDefTables* tables,
    const data::limits::BuildLists& lists = {}
) noexcept;

/// Reads every DOWNLOAD\*.TDF into tables->downloads and applies the menus.
///
/// Files enumerate in VFS order (at most 4096), one group each; sections past
/// the five a group holds are dropped. A
/// builder's gui_page_count is raised to the highest MENU naming it, then
/// mark_downloadable_units and append_download_build_ids run.
///
/// @param files file boundary
/// @param variant game-data variant suffix; null or empty for none
/// @param[in,out] tables unit tables with a sorted catalog and build lists
/// @param lists the build-list limits the lists were loaded with
/// @return false when the listing or the group allocation fails
bool load_download_menu(
    const Files* files,
    const char* variant,
    UnitDefTables* tables,
    const data::limits::BuildLists& lists = {}
) noexcept;

// Replaces one type's COB with the named scripts\<unitname>.COB.
struct UnitScriptLoader {
    void* context{};
    void (*load)(void* context, uint16_t type, const char* path) = nullptr;
};

/// Reloads a catalog type's units\<unitname>.FBI over its record, then its COB, as the console's Reload does.
///
/// @param files file boundary
/// @param[in,out] tables unit tables holding the type
/// @param type catalog type id
/// @param sources language, move classes, weapons and sounds the FBI load uses
/// @param scripts loader handed scripts\<unitname>.COB; a null callback skips it
/// @return false when the type is 0, outside the catalog or unavailable, or its
///     FBI fails to load
bool update_unit_def(
    const Files* files,
    UnitDefTables* tables,
    uint16_t type,
    const UnitDefSources& sources,
    const UnitScriptLoader& scripts
) noexcept;

/// Marks as downloadable each unit named by the first section of a download menu file.
///
/// Only a file's first section counts.
///
/// @param[in,out] tables unit tables with loaded download menus
void mark_downloadable_units(UnitDefTables* tables) noexcept;

/// Appends each download menu unit to the build list of the builder its UNITMENU names.
///
/// Type ids are appended in file and section order while the list holds fewer than
/// download_build_list_entries; names are not deduplicated and unknown ones are skipped.
///
/// @param[in,out] tables unit tables with build lists and loaded download menus
/// @param lists the build-list limits the lists were loaded with
void append_download_build_ids(
    UnitDefTables* tables, const data::limits::BuildLists& lists = {}
) noexcept;

} // namespace oa::data::defs
