/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_CALIBRATION_H
#define FTE3600_CALIBRATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FTE3600_CALIBRATION_CHANNELS 4
#define FTE3600_CALIBRATION_MAX_SAMPLES 31

struct fte3600_calibration_state
{
  uint8_t dac;
  bool    valid;
};

/* Project-local status values; these are not a vendor ABI. */
enum fte3600_calibration_status {
  FTE3600_CALIBRATION_OK = 0,
  FTE3600_CALIBRATION_FAILED = 1,
  FTE3600_CALIBRATION_INVALID_ARGUMENT = 2,
  FTE3600_CALIBRATION_SAMPLE_ERROR = 3
};

/*
 * Synchronously supply four unsigned 16-bit measurements at requested_dac.
 * Return zero after filling all four channels; nonzero indicates a local
 * sampling error. The channel buffer cannot be retained after returning.
 * No hardware operation or sampling duration is specified by this interface.
 */
typedef int (*fte3600_calibration_sample_fn)(void    *context,
                                             uint8_t  requested_dac,
                                             uint16_t channels[FTE3600_CALIBRATION_CHANNELS]);

/*
 * Implement specs/calibration-v1.md using caller-supplied measurements.
 * state and sample are required; context may be null. When state->valid is
 * false, state->dac is initialized to 27. Success sets valid; failure leaves
 * valid unchanged. The returned DAC may differ from the last sampled DAC and
 * is not guaranteed to be in 1..125. At most 31 callback calls are made.
 *
 * INVALID_ARGUMENT and SAMPLE_ERROR are local API extensions outside the
 * observed contract. Invalid arguments do not change state or invoke sample.
 * A sampling error stops immediately, preserves the input valid flag, and
 * leaves dac at the failed request; channel output from that call is ignored.
 *
 * The caller keeps state and callback context valid until return and must not
 * mutate state from the callback. Calls with separate state/context are
 * independent; this function allocates no memory and has no mutable globals.
 */
enum fte3600_calibration_status fte3600_calibrate (struct fte3600_calibration_state *state,
                                                   fte3600_calibration_sample_fn     sample,
                                                   void                             *context);

/* Four/eight-channel byte-state extension from fw9369-fdt-v1.md. sample fills
 * exactly channel_count values (4 or 8); its signature otherwise matches the
 * legacy callback. Zero valid starts at DAC 27 (4) or 56 (8). Nonzero valid is
 * retained byte-for-byte on failure and set to 1 on success. Invalid inputs
 * are unchanged. Pointer objects must be valid, separate and not changed by
 * the callback. SAMPLE_ERROR retains the DAC of the failed request. */
typedef int (*fte3600_calibration_channels_fn)(void     *context,
                                               uint8_t   requested_dac,
                                               uint16_t *channels);

enum fte3600_calibration_status fte3600_calibrate_channels (uint8_t                        *dac,
                                                            uint8_t                        *valid,
                                                            size_t                          channel_count,
                                                            fte3600_calibration_channels_fn sample,
                                                            void                           *context);

#ifdef __cplusplus
}
#endif

#endif
