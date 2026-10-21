-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

assert(type(features.RIDGEROCK.blocking) == "string")
assert(features.RIDGEROCK.blocking == "1")
features.RIDGEROCK.blocking = 0
assert(features.RIDGEROCK.blocking == "0")
assert(ctx.features.RIDGEROCK.blocking == "0")
