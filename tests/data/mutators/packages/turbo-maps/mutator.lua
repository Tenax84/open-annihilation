-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    files = { add = { "files" } },
    maps = function(map, ctx)
      map.header.gravity = 50
      map.schemas:set { aiprofile = "TURBOTEST" }
    end
  }
end
