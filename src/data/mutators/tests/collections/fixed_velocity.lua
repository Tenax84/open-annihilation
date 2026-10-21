-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

local com = units.RIDGECOM
com.maxvelocity = com:fixed("maxvelocity") * fixed("1.5")
assert(com:fixed("maxvelocity"):raw() == 157285)
assert(com:fixed("maxvelocity"):text() == "2.3999786376953125")
