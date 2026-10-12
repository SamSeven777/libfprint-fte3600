/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_calibration.h"

#include <stddef.h>

enum fte3600_calibration_status
fte3600_calibrate_channels (
  uint8_t *stored_dac, uint8_t *valid, size_t channel_count,
  fte3600_calibration_channels_fn sample, void *context)
{
  unsigned int lower = 0;
  unsigned int upper = 127;
  unsigned int previous_dac = 0;
  unsigned int previous_measurement = 0;

  if (stored_dac == NULL || valid == NULL || sample == NULL ||
      (channel_count != 4 && channel_count != 8))
    return FTE3600_CALIBRATION_INVALID_ARGUMENT;

  if (*valid == 0)
    *stored_dac = channel_count == 4 ? 27 : 56;

  for (unsigned int count = 0; count < FTE3600_CALIBRATION_MAX_SAMPLES;
       ++count)
    {
      uint16_t channels[8];
      uint32_t sum = 0;
      unsigned int dac = *stored_dac;
      unsigned int measurement;

      if (sample (context, *stored_dac, channels) != 0)
        return FTE3600_CALIBRATION_SAMPLE_ERROR;
      for (size_t channel = 0; channel < channel_count;
           ++channel)
        sum += channels[channel];
      measurement = (unsigned int) ((sum & UINT32_C (65535)) / channel_count);

      if ((dac == previous_dac + 1 && previous_measurement <= 512 &&
           measurement > 512) ||
          (previous_dac == dac + 1 && measurement <= 512 &&
           previous_measurement > 512))
        {
          *stored_dac = (uint8_t) (dac > previous_dac ? dac : previous_dac);
          *valid = 1;
          return FTE3600_CALIBRATION_OK;
        }

      if (dac < 1 || dac > 125)
        return FTE3600_CALIBRATION_FAILED;

      previous_dac = dac;
      previous_measurement = measurement;
      if (measurement < 20)
        {
          if (dac == upper)
            return FTE3600_CALIBRATION_FAILED;
          lower = dac;
          *stored_dac = (uint8_t) ((dac + upper) / 2);
        }
      else if (measurement > 1000)
        {
          if (dac == lower)
            return FTE3600_CALIBRATION_FAILED;
          upper = dac;
          *stored_dac = (uint8_t) ((lower + dac) / 2);
        }
      else
        {
          *stored_dac = (uint8_t) (measurement <= 512 ? dac + 1 : dac - 1);
        }
    }

  return FTE3600_CALIBRATION_FAILED;
}

enum fte3600_calibration_status
fte3600_calibrate (
  struct fte3600_calibration_state *state,
  fte3600_calibration_sample_fn sample, void *context)
{
  enum fte3600_calibration_status status;
  uint8_t valid;

  if (state == NULL || sample == NULL)
    return FTE3600_CALIBRATION_INVALID_ARGUMENT;
  valid = state->valid;
  status = fte3600_calibrate_channels (&state->dac, &valid, 4, sample, context);
  state->valid = valid != 0;
  return status;
}
