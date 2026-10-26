-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

local found = {}
for name, unit in pairs(units) do
    local weapon_name = unit.weapon1
    if weapon_name ~= nil then
        local weapon = ctx.weapons[weapon_name]
        if weapon ~= nil and weapon.range > 200 then
            found[#found + 1] = name
        end
    end
end
assert(#found == 3)
assert(found[1] == "RIDGECOM")
assert(found[2] == "RIDGEJET")
assert(found[3] == "RIDGETANK")
