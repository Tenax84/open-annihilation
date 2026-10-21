-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Each feature is tagged with its place in the first walk. Later walks,
-- lookups and sets hand out new handles; each must reach the same section.
local first = {}
local first_names = {}
for name, feature in pairs(features) do
    first[#first + 1] = feature
    first_names[#first_names + 1] = name
end
assert(#first == #features)
for index, feature in ipairs(first) do
    feature.tag = tostring(index)
end

for _ = 1, 30 do
    local index = 0
    for _, feature in pairs(features) do
        index = index + 1
        assert(feature.tag == tostring(index), index)
        assert(first[index].tag == feature.tag)
    end
    assert(index == #first)
    units:each(function(unit)
        assert(unit ~= nil)
    end)
    weapons:each(function(weapon)
        assert(weapon ~= nil)
    end)
end

local picked = features:where { tag = is("500") }
assert(#picked == 1)
picked:set { tag = "picked" }
assert(first[500].tag == "picked")
first[500].tag = "500"
assert(features.RIDGEROCK.tag == tostring(#first))

-- Remove every seventh feature through handles from a fresh walk.
local latest = {}
for _, feature in pairs(features) do
    latest[#latest + 1] = feature
end
local removed = 0
for index = 7, #latest, 7 do
    latest[index]:remove()
    removed = removed + 1
end

for index, feature in ipairs(first) do
    if index % 7 == 0 then
        local ok, err = pcall(function()
            return feature.tag
        end)
        assert(ok == false)
        assert(string.find(err, first_names[index] .. " was removed", 1, true) ~= nil, err)
    else
        assert(feature.tag == tostring(index), index)
    end
end

local expected = 0
for _, feature in pairs(features) do
    expected = expected + 1
    if expected % 7 == 0 then
        expected = expected + 1
    end
    assert(feature.tag == tostring(expected), expected)
end
assert(#features == #first - removed)
