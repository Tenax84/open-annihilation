-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Scale, a clamped scale and a lock, each from a fixed value.
return {
all():change {
MaxVelocity = scale(fixed("0.75")),
WorkerTime = scale(fixed("1.5"), fixed("0.25"), fixed(4)),
MaxDamage = lock(fixed("2.5")),
},
}
