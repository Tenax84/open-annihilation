-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

local function collect(group, want)
    local found = {}
    for name, record in pairs(group) do
        if name == want then
            found[#found + 1] = record
        end
    end
    return found
end

local dup_weapons = collect(weapons, "RIDGEDUP")
assert(#dup_weapons == 2)
assert(dup_weapons[1].range == 1)
assert(dup_weapons[2].range == 2)
dup_weapons[1]:remove()
local read_ok, read_err = pcall(function()
    return dup_weapons[1].range
end)
assert(read_ok == false)
assert(string.find(read_err, "RIDGEDUP was removed", 1, true) ~= nil)
local write_ok, write_err = pcall(function()
    dup_weapons[1].range = 99
end)
assert(write_ok == false)
assert(string.find(write_err, "RIDGEDUP was removed", 1, true) ~= nil)
assert(dup_weapons[2].range == 2)
dup_weapons[2].range = 7
assert(dup_weapons[2].range == 7)

local dup_features = collect(features, "RIDGEDUP")
assert(#dup_features == 2)
assert(dup_features[1].blocking == "1")
assert(dup_features[2].blocking == "2")
dup_features[1]:remove()
local feature_ok, feature_err = pcall(function()
    return dup_features[1].blocking
end)
assert(feature_ok == false)
assert(string.find(feature_err, "RIDGEDUP was removed", 1, true) ~= nil)
assert(dup_features[2].blocking == "2")
dup_features[2].blocking = "9"
assert(dup_features[2].blocking == "9")

local com = units.RIDGECOM
com.unitname = "RIDGENEW"
com.maxvelocity = 3
assert(units.RIDGECOM == nil)
assert(units.RIDGENEW.maxvelocity == 3)
assert(com.maxvelocity == 3)
