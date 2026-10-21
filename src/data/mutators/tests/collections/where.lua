-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

local function names_of(set)
    local names = {}
    for name, _ in pairs(set) do
        names[#names + 1] = name
    end
    return names
end

local function same(got, expect)
    assert(#got == #expect, "count")
    for index = 1, #expect do
        assert(got[index] == expect[index], got[index])
    end
end

same(names_of(units:where { canfly = true }), { "RIDGEJET" })
same(names_of(units:where { canfly = false }), { "RIDGEBARE", "RIDGECOM", "RIDGEFAB", "RIDGETANK" })
same(names_of(units:where { maxvelocity = 10 }), { "RIDGEJET" })
same(names_of(units:where { maxvelocity = range(1, 2) }), { "RIDGEBARE", "RIDGECOM", "RIDGETANK" })
same(names_of(units:where { weapon1 = "RIDGE_LASER" }), { "RIDGECOM", "RIDGEJET" })
same(names_of(units:where { ridgetag = is("commander") }), { "RIDGECOM" })
same(names_of(units:where { soundcategory = has("CMD") }), { "RIDGECOM" })
same(names_of(units:where { bankscale = absent() }), { "RIDGEBARE", "RIDGEFAB", "RIDGEJET" })
same(names_of(units:where { bankscale = 1 }), { "RIDGEBARE", "RIDGECOM", "RIDGEFAB", "RIDGEJET" })
same(
    names_of(units:where { weapon1 = not_(is("RIDGE_LASER")) }),
    { "RIDGEBARE", "RIDGEFAB", "RIDGETANK" }
)
same(names_of(units:where { canfly = false, maxvelocity = 2 }), { "RIDGETANK" })
assert(#units:where { canfly = true } == 1)
assert(#units:where {} == 5)

local walked = {}
units:where { canfly = false }:each(function(unit)
    walked[#walked + 1] = unit.name
end)
same(walked, { "RIDGEBARE", "RIDGECOM", "RIDGEFAB", "RIDGETANK" })

local none = units:where { maxvelocity = -1 }
assert(#none == 0)
none:set { maxvelocity = 9 }
none:scale { maxvelocity = 9 }
none:remove()
assert(units.RIDGECOM.maxvelocity == 1.6)
