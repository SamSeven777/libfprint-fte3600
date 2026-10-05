/*
 * FocalTech FTE3600 protocol timing and bounded host waits
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

/* Observed SPI timing. GPIO levels are controller pin levels: H10/L20/H.
 * A repeated pulse adds GAP_MS before the next HIGH_MS preamble, so the high
 * interval between low pulses totals 20 ms. These are minimum scheduled
 * delays, not a claim of realtime pulse precision in userspace. */
#define FTE3600_RESET_HIGH_MS 10
#define FTE3600_RESET_LOW_MS 20
#define FTE3600_RESET_GAP_MS 10
#define FTE3600_RESET_BOOT_MS 160
#define FTE3600_SOFT_RESET_INTERVAL_MS 5
#define FTE3600_A8_RESET_SETTLE_MS 2
#define FTE3600_FW_UPLOAD_SETTLE_MS 2
#define FTE3600_A8_CONFIG_DELAY_MS 2
#define FTE3600_38_CONFIG_DELAY_MS 1
#define FTE3600_FAMILY_QUERY_DELAY_MS 2
#define FTE3600_ARM_DELAY_MS 10
#define FTE3600_INIT_MCU_POLL_MS 2
#define FTE3600_INIT_MCU_MAX_ATTEMPTS 20

/* Linux retains the 2 ms post-pair reply wait used by the A1 wake path.
 * Windows Detect instead reads status immediately after the second 70.
 * Its six-attempt bound, failed-round interval and idle-to-geometry delay
 * supply the fallback for an application whose first response stays blank. */
#define FTE3600_LEGACY_WAKE_REPLY_MS 2
#define FTE3600_LEGACY_WAKE_MAX_ATTEMPTS 6
#define FTE3600_LEGACY_WAKE_RETRY_MS 5
#define FTE3600_LEGACY_WAKE_GEOMETRY_MS 350

/* Linux retry policy. These bounds prevent a stalled device or repeated
 * spurious IRQ from extending an action indefinitely; they are not silicon
 * specifications or build-time feature switches. */
#define FTE3600_ARM_TIMEOUT_MS 1000
#define FTE3600_ARM_MAX_ATTEMPTS 3
#define FTE3600_POLL_DELAY_MS 20
#define FTE3600_CAPTURE_READY_TIMEOUT_MS 5000
#define FTE3600_MAX_FALSE_IRQS 8
