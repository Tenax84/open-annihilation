-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- All, one name, and a group, on the Ridge units.
return {
all():change { maxvelocity = 5 },
named("RIDGECOM"):change { maxdamage = 9 },
where { builder = true }:change { workertime = scale(2) },
}
