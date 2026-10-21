-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

units:where { weapon1 = "RIDGE_LASER" }:set { maxvelocity = 4, workertime = 7 }
assert(units.RIDGECOM.maxvelocity == 4)
assert(units.RIDGEJET.maxvelocity == 4)
assert(units.RIDGECOM.workertime == 7)
assert(units.RIDGEJET.workertime == 7)
