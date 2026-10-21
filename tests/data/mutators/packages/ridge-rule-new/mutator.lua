-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    files = { add = { "files" } },
    rules = {
      where { unitname = is("RGADDED") }:change { maxdamage = 4242 }
    }
  }
end
