-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- RIDGECOM is the only unit with WorkerTime 40, so a record that reads 40
-- is that unit whatever it is called.
local com = units.RIDGECOM

local function order()
    local names = {}
    for name, _ in pairs(units) do
        names[#names + 1] = name
    end
    return table.concat(names, " ")
end

local function check(want, now, before)
    local walked = order()
    assert(walked == want, walked)
    assert(#units == 5)
    local found = units[now]
    assert(found ~= nil and found.workertime == 40, now)
    local old = units[before]
    assert(old == nil or old.workertime ~= 40, before)
    for _, other in ipairs({ "RIDGEBARE", "RIDGEJET", "RIDGETANK" }) do
        local unit = units[other]
        assert(unit ~= nil and unit.workertime ~= 40, other)
    end
    local busy = units:where { workertime = 40 }
    assert(#busy == 1)
    for name, _ in pairs(busy) do
        assert(name == now, name)
    end
    assert(com.workertime == 40)
end

for _ = 1, 40 do
    com.unitname = "RIDGEAAA"
    check("RIDGEAAA RIDGEBARE RIDGEFAB RIDGEJET RIDGETANK", "RIDGEAAA", "RIDGECOM")
    com.unitname = "RIDGEZZZ"
    check("RIDGEBARE RIDGEFAB RIDGEJET RIDGETANK RIDGEZZZ", "RIDGEZZZ", "RIDGEAAA")
    -- The same name as RIDGEFAB: the path decides, and units/RIDGECOM.fbi comes first.
    com.unitname = "ridgefab"
    check("RIDGEBARE ridgefab RIDGEFAB RIDGEJET RIDGETANK", "ridgefab", "RIDGEZZZ")
    assert(units.RIDGEFAB.workertime == 40)
    -- No UnitName: the file stem names the unit again.
    com.unitname = nil
    check("RIDGEBARE RIDGECOM RIDGEFAB RIDGEJET RIDGETANK", "RIDGECOM", "ridgefab")
    assert(units.ridgefab ~= nil and units.ridgefab.workertime == nil)
    com.unitname = "RIDGEJEU"
    check("RIDGEBARE RIDGEFAB RIDGEJET RIDGEJEU RIDGETANK", "RIDGEJEU", "RIDGECOM")
end

com.unitname = "RIDGENEW"
check("RIDGEBARE RIDGEFAB RIDGEJET RIDGENEW RIDGETANK", "RIDGENEW", "RIDGEJEU")

-- A removal builds the index again from the documents. It keeps the order
-- the renames left.
units.RIDGEBARE:remove()
assert(order() == "RIDGEFAB RIDGEJET RIDGENEW RIDGETANK", order())
assert(units.RIDGENEW.workertime == 40)
assert(units.RIDGEJEU == nil)
assert(com.workertime == 40)
