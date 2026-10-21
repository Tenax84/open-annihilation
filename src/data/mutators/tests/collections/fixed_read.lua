-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

local com = units.RIDGECOM
assert(type(com.fixed) == "function")
assert(com:get("fixed") == nil)
assert(com:fixed("maxvelocity"):raw() == 104857)
assert(com:fixed("moverate1"):raw() == 209714)
assert(com:fixed("nope") == nil)
local refused = pcall(function()
    com.fixed = 1
end)
assert(refused == false)
assert(com:get("fixed") == nil)

local jet = units.RIDGEJET
assert(jet:fixed("bankscale") == fixed(1))
assert(jet:fixed("bankscale"):raw() == 65536)
assert(jet:fixed("moverate1"):raw() == 1310720)

local laser = weapons.RIDGE_LASER
assert(laser.range == 250)
assert(laser.damage:fixed("default") == fixed(0))
assert(laser.damage:fixed("RIDGECOM") == fixed(0))
assert(laser.damage:fixed("default"):raw() == 0)

local rock = features.RIDGEROCK
assert(rock:fixed("blocking"):raw() == 65536)
assert(rock:fixed("nope") == nil)
