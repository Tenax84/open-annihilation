-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    units = function(units, ctx)
      if units.RGCOM.maxdamage > 5000 then
        error("too tough to work with")
      end
      units.RGBOT.maxdamage = 1234
    end
  }
end
