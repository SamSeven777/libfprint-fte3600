/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "fte3600-private.h"

/* Persistent flash update, never an unknown-device discovery fallback.
 * The caller must have just verified the FT9368 application information. */
FpiSsm *fpi_fte3600_ft9368_update_new (FpiDeviceFte3600 *self);
