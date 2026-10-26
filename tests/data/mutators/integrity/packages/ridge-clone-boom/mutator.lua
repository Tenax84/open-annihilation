-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    units = function(units, ctx)
      units:clone("RGBOOM", "RGBOOM2", { maxdamage = 2000 })
    end
  }
end
