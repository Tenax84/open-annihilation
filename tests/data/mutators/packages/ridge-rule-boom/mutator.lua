-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    rules = { where { commander = 1 }:change { maxdamage = scale(2) } },
    units = function(units, ctx)
      error("rule-boom hook")
    end
  }
end
