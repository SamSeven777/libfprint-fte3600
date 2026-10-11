/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "fte3600-medion-identify.h"

/* Explicit, single-session FT9338 factory startup experiment.
 * The caller must authenticate the 14184-byte vendor firmware before calling.
 * This reproduces Windows factory selection, including its documented FT9338
 * defaults; it must not be used as the automatic driver's identity policy.
 *
 * Matching running firmware is reused. A download requires full RAM readback
 * and MCU idle before configuration. Success requires configuration and
 * sensor-mode transactions, geometry 58/58 and firmware/AGC versions 40/10.
 * A non-BB configuration marker or an MCU that stays idle after sensor-mode
 * entry is reported without failing, as in Windows. Neither proves capture.
 * The first three vendor factory rounds are supported; later forced FT9368
 * downloads are outside this FT9338-only experiment.
 * No image is captured. Failure leaves result zeroed, disables an outstanding
 * OTP read if necessary, and releases reset without another startup attempt.
 */
gboolean fte3600_medion_test_ft9338 (const Fte3600MedionIdentifyIo *io,
                                     GBytes                        *firmware,
                                     Fte3600Identity               *result,
                                     GError                       **error);

/* Explicit FT9338 selection, for --boot ft9338. Same preparation, LoadFW,
 * configuration, sensor-mode entry and validation, without ROM/OTP selection.
 * Positive application evidence of another chip still stops the test. */
gboolean fte3600_medion_start_ft9338 (const Fte3600MedionIdentifyIo *io,
                                      GBytes                        *firmware,
                                      Fte3600Identity               *result,
                                      GError                       **error);
