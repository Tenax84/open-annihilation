-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- A fixed product that is an exact whole number.
return {
all():change {
MaxVelocity = set(fixed("1.5") * 2),
},
}
