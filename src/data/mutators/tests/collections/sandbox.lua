-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

assert(type(getmetatable(units)) == "boolean")
assert(getmetatable(units) == false)
assert(getmetatable(weapons) == false)
assert(getmetatable(features) == false)
assert(getmetatable(units.RIDGECOM) == false)
local set = units:where {}
assert(getmetatable(set) == false)
local assigned, assign_error = pcall(function()
    units.RIDGECOM = 1
end)
assert(assigned == false)
assert(string.find(assign_error, "a collection cannot be assigned", 1, true) ~= nil)
local replaced = pcall(function()
    setmetatable(units, {})
end)
assert(replaced == false)
local set_assigned = pcall(function()
    set.RIDGECOM = 1
end)
assert(set_assigned == false)
assert(units[1] == nil)
assert(units[true] == nil)
