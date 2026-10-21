-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- The manifest names no clash mode, so the preference decides: Replace puts
-- the copy in place of ARMCOM, and Add keeps it beside ARMCOM under a
-- private name.
function mutate(ctx)
  return {
    units = function(units, ctx)
      units:clone("CORCOM", "ARMCOM", { maxdamage = 4321 })
    end
  }
end
