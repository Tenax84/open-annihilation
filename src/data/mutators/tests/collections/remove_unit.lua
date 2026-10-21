-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

local jet = units.RIDGEJET
jet:remove()
assert(units.RIDGEJET == nil)
jet.canfly = true
