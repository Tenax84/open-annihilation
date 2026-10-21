-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

local paired = {}
for name, _ in pairs(units) do
    if name == "RIDGEBARE" then
        units.RIDGETANK:remove()
    end
    paired[#paired + 1] = name
end
assert(paired[1] == "RIDGEBARE")
assert(paired[2] == "RIDGECOM")
assert(paired[3] == "RIDGEFAB")
assert(paired[4] == "RIDGEJET")
assert(paired[5] == nil)

local seen = {}
units:each(function(unit)
    if unit.name == "RIDGEBARE" then
        units.RIDGEJET:remove()
    end
    seen[#seen + 1] = unit.name
end)
assert(seen[1] == "RIDGEBARE")
assert(seen[2] == "RIDGECOM")
assert(seen[3] == "RIDGEFAB")
assert(seen[4] == nil)
