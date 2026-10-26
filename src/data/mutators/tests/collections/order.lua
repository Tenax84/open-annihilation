-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

local names = {}
for name, _ in pairs(units) do
    names[#names + 1] = name
end
assert(names[1] == "RIDGEBARE")
assert(names[2] == "RIDGECOM")
assert(names[3] == "RIDGEFAB")
assert(names[4] == "RIDGEJET")
assert(names[5] == "RIDGETANK")

local walked = {}
units:each(function(unit)
    walked[#walked + 1] = unit.name
end)
assert(walked[1] == "RIDGEBARE")
assert(walked[2] == "RIDGECOM")
assert(walked[3] == "RIDGEFAB")
assert(walked[4] == "RIDGEJET")
assert(walked[5] == "RIDGETANK")

local ranges = {}
for name, weapon in pairs(weapons) do
    ranges[#ranges + 1] = name .. "=" .. tostring(weapon.range)
end
assert(ranges[1] == "RIDGE_CANNON=400")
assert(ranges[2] == "RIDGE_LASER=250")
assert(ranges[3] == "RIDGE_LASER=10")
assert(ranges[4] == "RIDGE_PEBBLE=80")
