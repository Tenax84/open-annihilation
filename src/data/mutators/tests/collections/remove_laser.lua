-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

local first = weapons.RIDGE_LASER
assert(first.range == 250)
first:remove()
assert(weapons.RIDGE_LASER.range == 10)
first.range = 1
