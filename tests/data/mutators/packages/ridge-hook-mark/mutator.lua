-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    units = function(units, ctx)
      units.RGCOM.maxdamage = 4242
      units.RGBOT.maxdamage = 4242
      units.RGLAB.maxdamage = 4242
    end
  }
end
