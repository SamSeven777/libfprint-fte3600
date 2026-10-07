/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "fte3600-medion-identify.h"

/* Explicit, single-session FT9338 RAM startup and MCU configuration experiment.
 * The caller must authenticate the 14184-byte vendor firmware before calling.
 * This reproduces Windows factory selection, including its documented FT9338
 * defaults; it must not be used as the automatic driver's identity policy.
 *
 * Success requires full RAM readback, idle MCU, successful MCU configuration
 * transactions, geometry 58/58 and firmware/AGC versions 40/10. A non-BB
 * configuration marker is reported without failing, as in Windows.
 * No image is captured. Failure leaves result zeroed, disables an outstanding
 * OTP read if necessary, and releases reset without another startup attempt.
 */
gboolean fte3600_medion_test_ft9338 (const Fte3600MedionIdentifyIo *io,
                                     GBytes                        *firmware,
                                     Fte3600Identity               *result,
                                     GError                       **error);
