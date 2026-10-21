-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

local com = units.RIDGECOM
com.maxvelocity = com.maxvelocity * 2
com.ai_weight = nil
units.RIDGEJET.NewRidge = "kept"
com.maxdamage = 40000
units.RIDGEBARE.canfly = true
assert(com.maxvelocity == 3.2)
assert(com.ai_weight == nil)
assert(units.RIDGEJET.NewRidge == "kept")
assert(com.maxdamage == 40000)
assert(units.RIDGEBARE.canfly == true)
