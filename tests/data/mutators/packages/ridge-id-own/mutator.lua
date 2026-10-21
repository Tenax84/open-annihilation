-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    files = { add = { "files" } },
    rules = { named("RGOWN"):on("weapons", "tdf"):change { id = 9 } }
  }
end
