/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "fte3600-private.h"

/* boot_a requires the current discovery boot-probe response EF. The B38 path
 * requires a current-session runtime identity, preserved in ROM.response. */
FpiSsm *fpi_fte3600_legacy38_identify_new (FpiDeviceFte3600 *self,
                                           gboolean          boot_a);
/* Requires the positive identity above; validates the matching external RAM
 * firmware, uploads and compares every byte before restarting the app. */
FpiSsm *fpi_fte3600_legacy38_recovery_new (FpiDeviceFte3600 *self);
