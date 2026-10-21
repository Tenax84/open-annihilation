-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    weapons = function(weapons, ctx)
      weapons.RGGUN.id = 2
    end
  }
end
