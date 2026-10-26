-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    units = function(units, ctx)
      units:where { commander = true }:set { maxdamage = ctx.options.health }
    end
  }
end
