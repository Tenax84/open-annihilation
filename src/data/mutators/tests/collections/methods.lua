-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

local com = units.RIDGECOM
assert(type(com.remove) == "function")
assert(type(com.get) == "function")
assert(type(com.set) == "function")
assert(type(com.keys) == "function")
assert(type(com:get("remove")) == "string")
assert(com:get("remove") == "9")
com:set("remove", 4)
assert(com:get("remove") == "4")
local refused = pcall(function()
    com.remove = 1
end)
assert(refused == false)
assert(com:get("remove") == "4")
com.Remove = 8
assert(com:get("Remove") == "8")
local missing = pcall(function()
    com:set("maxvelocity")
end)
assert(missing == false)
local keys = com:keys()
local saw = false
for index = 2, #keys do
    assert(string.lower(keys[index - 1]) <= string.lower(keys[index]))
end
for index = 1, #keys do
    if keys[index] == "remove" then
        saw = true
    end
end
assert(saw)
