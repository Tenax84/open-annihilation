-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

return {
all():change { marker = 1 },
where { builder = true }:change { workertime = 4 },
}
