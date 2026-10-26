-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    weapons = function(weapons, ctx)
      weapons.RGGUN:remove()
      weapons.RGBLAST:remove()
    end,
    units = function(units, ctx)
      units.RGBOT.weapon1 = nil
    end
  }
end
