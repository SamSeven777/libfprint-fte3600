/* Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

/* Millisecond protocol requirements observed in the reference. */
#define FT93XX_COMMAND_SETTLE_MS 1
#define FT93XX_SFR_PROTOCOL_SETTLE_MS 4

/* Linux host bounds. These are not claimed hardware characteristics. */
#define FT93XX_IDLE_ATTEMPTS 5
#define FT93XX_SCAN_MODE_ATTEMPTS 10
#define FT93XX_SCAN_TIMEOUT_MS 200
#define FT93XX_FRAME_POLL_MS 100
#define FT93XX_FRAME_POLL_MAX_MS 500
#define FT93XX_RELEASE_EMPTY_FRAMES 3
#define FT93XX_EXPOSURE_ADJUSTMENTS 8
