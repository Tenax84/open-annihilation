-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Weapons, corpse features and side data for Ridge.
return {
on("weapons", "tdf"):change { range = scale(2) },
on("features/corpses", "tdf"):all():change { metal = 7 },
on("gamedata", "sidedata.tdf"):named("ARM"):change { commander = "RIDGECOM" },
on("weapons", "tdf"):where { range = range(1, 200) }:change { reloadtime = 9 },
}
