-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

local jet = units.RIDGEJET
local flying = units:where { canfly = true }
jet:remove()
flying:set { maxvelocity = 3 }
