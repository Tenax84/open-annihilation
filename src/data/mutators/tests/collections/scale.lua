-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

units:where { weapon1 = "RIDGE_LASER" }:scale { maxvelocity = 2, soundcategory = 2, workertime = 3 }
units:where { unitname = "RIDGEJET" }:scale { bankscale = 4 }
assert(units.RIDGECOM.maxvelocity == 3.2)
assert(units.RIDGEJET.maxvelocity == 20)
assert(units.RIDGECOM.workertime == 120)
assert(units.RIDGEJET.workertime == 15)
assert(units.RIDGECOM.soundcategory == "CMDCK")
assert(units.RIDGEJET.bankscale == nil)
