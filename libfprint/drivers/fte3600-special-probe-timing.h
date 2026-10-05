/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

/* Observed command and C6 readback delays. */
#define FTE3600_SPECIAL_COMMAND_DELAY_MS 1
#define FTE3600_SPECIAL_MODE_DELAY_MS 4

/* The shared SPI factory calls transport reset H10/L20/H, then waits 10 ms
 * before its next probe. This is not the A8 firmware-startup delay. */
#define FTE3600_SPECIAL_RESET_SETTLE_MS 10

/* Each of the shared factory's two consecutive C6 helpers permits 31
 * write/delay/read attempts. Exhaustion does not suppress the ID read. */
#define FTE3600_SPECIAL_MODE_ATTEMPTS 31
