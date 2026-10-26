-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

local held = {}
while true do
    local set = units:where { canfly = true }
    held[#held + 1] = set
    local named = units.RIDGECOM
    held[#held + 1] = named
    local listed = {}
    for _, rec in pairs(units) do
        listed[#listed + 1] = rec
    end
    held[#held + 1] = listed
    local walked = {}
    units:each(function(rec)
        walked[#walked + 1] = rec
    end)
    held[#held + 1] = walked
    held[#held + 1] = named:keys()
    local edited = units:where { canfly = false }
    edited:set { maxvelocity = 1 }
    held[#held + 1] = edited
end
