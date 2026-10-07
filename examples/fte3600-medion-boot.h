/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Explicit Medion FT9338/FT9348 RAM-boot experiment.
 */
#pragma once

#include "fte3600-medion-identify.h"

/* Only FT9338 and FT9348 are accepted. The caller must obtain firmware through
 * the selected chip's catalog size/SHA-256 loader before calling. This engine
 * also checks size/capacity before I/O, but length alone is not authentication.
 *
 * FT9338 starts the selected download sequence without pre-boot application
 * queries. FT9348 retains its repeated pre-boot geometry checks.
 * It never tries a different chip, programs flash, or modifies automatic driver
 * identity/firmware policy. TRUE requires application idle, matching geometry
 * and firmware/AGC versions after RAM startup. FT9338 configuration-marker
 * mismatches are reported without failing, as in Windows. FALSE zeroes
 * result and preserves the first failure, including any cleanup failure.
 * A successful result describes runtime geometry, not immutable silicon ID.
 */
gboolean fte3600_medion_boot (const Fte3600MedionIdentifyIo *io,
                              Fte3600Sensor                  sensor,
                              GBytes                        *firmware,
                              Fte3600Identity               *result,
                              GError                       **error);
