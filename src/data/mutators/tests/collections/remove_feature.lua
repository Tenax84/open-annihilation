-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

local rock = features.RIDGEROCK
rock:remove()
assert(features.RIDGEROCK == nil)
rock.blocking = "1"
