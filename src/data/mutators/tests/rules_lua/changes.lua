-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Every change form, on every Ridge unit.
return {
all():change {
plain = 5,
by_set = set(7),
words = "text",
by_set_text = set("note"),
flag = true,
off = false,
gone = "",
by_set_gone = set(""),
extra = append(" tail"),
factor = scale(2),
clamped = scale(3, 1, 9),
locked = lock(4),
bolt = "LIGHTNING ",
cleared = remove(),
quoted = "850",
yardmap = empty,
},
}
