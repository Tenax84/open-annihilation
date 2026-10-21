-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

units:where { weapon1 = "RIDGE_PEBBLE" }:remove()
assert(units.RIDGEFAB == nil)
assert(units.RIDGECOM ~= nil)
