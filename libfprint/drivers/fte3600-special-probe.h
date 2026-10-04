/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "fte3600-private.h"

/* State-changing fallback after read-only family probes have failed. The
 * caller owns result until completion and chooses the current CS polarity.
 * A successful identified probe leaves the chip awake with C6 == 1. An
 * unidentified probe completes with UNKNOWN only after a hardware reset.
 * Transport, cancellation and cleanup errors propagate and must not be
 * mistaken for a negative identity. A KNOWN_UNMAPPED_ID is retained for the
 * caller to reject instead of authorizing legacy firmware recovery.
 * This child does not change CS, self->identity, pad voltage, IRQ masks, or
 * firmware. No inferred default identity is returned. */
FpiSsm *fpi_fte3600_special_probe_new (FpiDeviceFte3600 *self,
                                       Fte3600Identity *result);
