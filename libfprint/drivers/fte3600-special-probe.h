/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "fte3600-private.h"

/* Shared factory negotiation after FT9368 metadata probing. The caller owns
 * result until completion and chooses the current CS polarity. Each attempt
 * wakes the chip and executes two C6 helpers (each up to 31 write/read cycles)
 * before the repeated identity read, even if C6 readback never acknowledges.
 * A first ordinary UNKNOWN triggers H10/L20/H and 10 ms settle, then one full
 * attempt on the same CS. A second UNKNOWN returns without another reset.
 * A supported identity leaves the chip in its observed post-probe state;
 * successful identification alone does not prove idle or acknowledged C6.
 * Transport, cancellation and cleanup errors propagate and must not be
 * mistaken for a negative identity. A KNOWN_UNMAPPED_ID is retained for the
 * caller to reject instead of retrying or attempting legacy/firmware recovery.
 * Linux additionally performs one reset cleanup for these failures. A reset
 * already in progress finishes even on cancellation/GPIO error, without a
 * second cleanup pulse unless subsequent SPI I/O touched the chip again.
 * This child does not change CS, self->identity, pad voltage, IRQ masks, or
 * firmware. No inferred default identity is returned. */
FpiSsm *fpi_fte3600_special_probe_new (FpiDeviceFte3600 *self,
                                       Fte3600Identity *result);
