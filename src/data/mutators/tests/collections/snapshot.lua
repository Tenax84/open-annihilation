-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

local slow = units:where { maxvelocity = 0 }
units.RIDGECOM.maxvelocity = 0
local names = {}
for name, _ in pairs(slow) do
    names[#names + 1] = name
end
assert(#names == 1)
assert(names[1] == "RIDGEFAB")
slow:set { footprintx = 3 }
assert(units.RIDGEFAB.footprintx == 3)
assert(units.RIDGECOM.footprintx == 2)
