-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    rules = { named("RGCOM"):change { maxdamage = 9999 } },
    units = function(units, ctx)
      units.RGCOM.maxdamage = 7777
    end
  }
end
