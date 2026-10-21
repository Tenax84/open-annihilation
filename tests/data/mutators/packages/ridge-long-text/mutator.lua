-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    units = function(units, ctx)
      local text = string.rep("ab", 40000)
      units.RGBOT.name = text
    end
  }
end
