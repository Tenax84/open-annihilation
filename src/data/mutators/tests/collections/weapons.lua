-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

local laser = weapons.RIDGE_LASER
assert(laser.range == 250)
assert(laser:get("damage") == nil)
local damage = laser.damage
assert(#damage:keys() == 0)
damage.default = 5
damage.RIDGECOM = 40000
assert(damage.default == 5)
assert(math.type(damage.default) == "integer")
assert(damage.RIDGECOM == 40000)
assert(laser:get("damage") == nil)
assert(ctx.weapons.RIDGE_LASER.range == 250)
local keys = damage:keys()
assert(keys[1] == "default")
assert(keys[2] == "RIDGECOM")
local refused = pcall(function()
    damage:remove()
end)
assert(refused == false)
assert(weapons.RIDGE_LASER.range == 250)
