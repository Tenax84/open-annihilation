-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    units = function(units, ctx)
      units.RGBOT.maxdamage = 9000
      units.RGBOT.maxvelocity = 3
    end
  }
end
