-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    files = { add = { "files" } },
    units = function(units, ctx)
      error("files-boom hook")
    end
  }
end
