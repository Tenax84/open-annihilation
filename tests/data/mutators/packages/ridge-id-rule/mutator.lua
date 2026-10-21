-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    rules = { named("RGGUN"):on("weapons", "tdf"):change { id = 2 } }
  }
end
