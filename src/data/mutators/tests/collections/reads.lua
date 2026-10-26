-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

local com = units.RIDGECOM
assert(com.maxvelocity == 1.6)
assert(units.ridgecom.MaxVelocity == 1.6)
assert(math.type(com.maxvelocity) == "float")
assert(type(com.canfly) == "boolean")
assert(com.canfly == false)
assert(com.workertime == 40)
assert(math.type(com.workertime) == "integer")
assert(com.ridgetag == "commander")
assert(type(com.ridgetag) == "string")
assert(units.RIDGEJET.footprintx == nil)
assert(units.NOPE == nil)
assert(units.nope == nil)
assert(#units == 5)
assert(#weapons == 4)
assert(#features == 1)
assert(ctx.units.RIDGECOM.name == "RIDGECOM")
assert(ctx.weapons.RIDGE_LASER.range == 250)
assert(ctx.features.RIDGEROCK.blocking == "1")
assert(weapons.RIDGE_LASER.range == 250)
assert(math.type(weapons.RIDGE_LASER.range) == "integer")
