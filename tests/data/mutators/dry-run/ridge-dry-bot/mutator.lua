-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    rules = {
      where { unitname = is("RGBOT") }:change { maxdamage = 9000 }
    }
  }
end
