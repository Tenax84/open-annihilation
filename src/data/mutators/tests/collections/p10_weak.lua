-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

local mt = {}
local wk = setmetatable({}, mt)
mt.__mode = "k"
local label = string.rep("n", 300)
wk[has(label)] = true
label = nil
collectgarbage("collect")
local key = next(wk)
if key ~= nil then
    local flipped = not_(key)
    local selected = units:where { canfly = key }
    local other = units:where { canfly = flipped }
    if selected == nil or other == nil then
        error("where rejected the need")
    end
end
local flying = units:where { canfly = true }
local count = 0
for _, unit in pairs(flying) do
    count = count + 1
    assert(unit.name == "RIDGEJET")
end
assert(count == 1)
