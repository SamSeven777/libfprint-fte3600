/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

/* Observed command and C6 readback delays. */
#define FTE3600_SPECIAL_COMMAND_DELAY_MS 1
#define FTE3600_SPECIAL_MODE_DELAY_MS 4

/* Independent bounded retry policy. The reference FW9369 helper uses 31
 * attempts; its FT93xx counterpart uses four. */
#define FTE3600_SPECIAL_MODE_ATTEMPTS 4
