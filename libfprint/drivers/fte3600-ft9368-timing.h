/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "fte3600-timing.h"

/* FT9368 observed waits, rounded up where Linux does not schedule in us. */
#define FTE3600_FT9368_WAKE_MS 10
#define FTE3600_FT9368_WAKE_ATTEMPTS 3
#define FTE3600_FT9368_START_MS 100
#define FTE3600_FT9368_SHORT_DELAY_MS 1
#define FTE3600_FT9368_PRAM_START_MS 10
#define FTE3600_FT9368_UPDATE_POLL_MS 10
#define FTE3600_FT9368_CHECKSUM_MS 5
#define FTE3600_FT9368_VERIFY_WAKE_MS 5
#define FTE3600_FT9368_FLASH_BOOT_MS 400

/* Host retry policy: bounded, independent of user cancellation after erase. */
#define FTE3600_FT9368_ERASE_ATTEMPTS 100
#define FTE3600_FT9368_PROGRAM_ATTEMPTS 10
#define FTE3600_FT9368_MAX_FALSE_IRQS 32
