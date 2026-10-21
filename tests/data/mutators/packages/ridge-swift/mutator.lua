-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    rules = {
      where { maxvelocity = range(0.01, 100) }:change { maxvelocity = scale(ctx.options.factor) }
    }
  }
end
