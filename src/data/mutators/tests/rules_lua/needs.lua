-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

-- Needs on the made-up Ridge commander.
return {
where {
plain = 1,
words = "CMDCK",
flag = true,
off = false,
band = range(1, 100),
exact = is("RIDGECOM"),
part = has("CMD"),
missing = absent(),
flipped = not_(range(2, 4)),
flipped_text = not_(is("no")),
flipped_has = not_(has("zz")),
}:change { workertime = 4 },
}
