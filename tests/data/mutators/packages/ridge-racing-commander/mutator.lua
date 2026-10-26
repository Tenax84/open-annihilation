-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- The values Commander Radar, Krogoth Commander, Hypermobile Commanders 3x,
-- No Waste, Resource Commander, Super Commander (side all) and UberComm
-- (full), applied in that order, leave on ARMCOM.
function mutate(ctx)
  return {
    rules = {
      named("ARMCOM"):change {
        maxvelocity = set(42),
        turnrate = set(28800),
        acceleration = set(158.76),
        brakerate = set(335.16),
        footprintx = set(3),
        footprintz = set(3),
        movementclass = set("TANKDH3"),
        builddistance = set(720),
        workertime = set(10800),
      },
    },
  }
end
