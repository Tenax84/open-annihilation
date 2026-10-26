-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- The registry does not open setup.commander-warp to mutators, so this
-- mutator fails alone, in the plan phase.
function mutate(ctx)
  return { hacks = { ["setup.commander-warp"] = true } }
end
