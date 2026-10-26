-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    rules = {
      where { unitname = is("NOUNIT") }:change { maxdamage = 1 }
    }
  }
end
