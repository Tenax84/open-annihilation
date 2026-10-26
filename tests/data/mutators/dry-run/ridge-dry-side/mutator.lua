-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    sides = function(sides, ctx)
      sides[1].commander = "RGBOT"
    end
  }
end
