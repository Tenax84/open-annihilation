-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

units:where { canfly = true }:set { maxvelocity = fixed(3) }
local jet = units.RIDGEJET
jet:set("bankscale", fixed("0.5"))
assert(jet:fixed("maxvelocity"):raw() == 3 * 65536)
assert(jet:fixed("bankscale"):raw() == 32768)
assert(jet:fixed("bankscale"):text() == "0.5")
assert(units.RIDGECOM:fixed("maxvelocity"):raw() == 104857)
