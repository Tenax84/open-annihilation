-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

units:where { canfly = true }:remove()
assert(units.RIDGEJET == nil)
assert(units.RIDGECOM ~= nil)
assert(units.RIDGEFAB ~= nil)
