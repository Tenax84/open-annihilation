-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    menus = function(menus, ctx)
      menus:add("RGLAB", "NOUNIT")
    end
  }
end
