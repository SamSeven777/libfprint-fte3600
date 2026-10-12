/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_image.h"

#include <stddef.h>

enum fte3600_image_status
fte3600_image_measure (
  const uint16_t                   pixels[FTE3600_IMAGE_PIXELS],
  struct fte3600_image_statistics *statistics)
{
  uint16_t histogram[1024] = {0};
  struct fte3600_image_statistics result = {0, 0, 0};
  unsigned int cumulative = 0;

  if (pixels == NULL || statistics == NULL)
    return FTE3600_IMAGE_INVALID_ARGUMENT;
  for (size_t row = 1; row < FTE3600_IMAGE_HEIGHT - 1; ++row)
    {
      for (size_t column = 2; column < FTE3600_IMAGE_WIDTH - 2; ++column)
        {
          unsigned int raw = pixels[row * FTE3600_IMAGE_WIDTH + column];
          unsigned int scaled = raw / 4;
          if (scaled > 1023)
            scaled = 1023;
          ++histogram[scaled];
          if (raw < 500)
            ++result.low_count;
          if (raw > 4012)
            ++result.high_count;
        }
    }
  for (unsigned int value = 0; value < 1024; ++value)
    {
      cumulative += histogram[value];
      if (cumulative > FTE3600_IMAGE_REGION_PIXELS / 2)
        {
          result.median = (uint16_t) value;
          break;
        }
    }
  *statistics = result;
  return FTE3600_IMAGE_OK;
}

enum fte3600_image_status
fte3600_image_refine (
  struct fte3600_image_state *state,
  uint16_t pixels[FTE3600_IMAGE_PIXELS],
  fte3600_image_capture_fn capture, void *context)
{
  struct fte3600_image_statistics before, after;
  uint8_t original_dac;

  if (state == NULL || pixels == NULL || capture == NULL)
    return FTE3600_IMAGE_INVALID_ARGUMENT;
  (void) fte3600_image_measure (pixels, &before);
  original_dac = state->dac;
  if (before.low_count < 20 && before.high_count > 20)
    state->dac = (uint8_t) (state->dac - 1);
  else if (before.low_count > 20 && before.high_count < 20)
    state->dac = (uint8_t) (state->dac + 1);
  else
    return FTE3600_IMAGE_OK;
  if (capture (context, state->dac, pixels) != 0)
    return FTE3600_IMAGE_CAPTURE_ERROR;
  (void) fte3600_image_measure (pixels, &after);
  if ((unsigned int) after.low_count + after.high_count <
      (unsigned int) before.low_count + before.high_count)
    state->stored_median = after.median;
  else
    state->dac = original_dac;
  return FTE3600_IMAGE_OK;
}

enum fte3600_image_status
fte3600_image_search (
  struct fte3600_image_state *state,
  uint16_t pixels[FTE3600_IMAGE_PIXELS],
  fte3600_image_capture_fn capture, void *context)
{
  uint16_t previous_median = 0;
  uint8_t previous_dac, lower_endpoint = 0, upper_endpoint = 127;

  if (state == NULL || pixels == NULL || capture == NULL)
    return FTE3600_IMAGE_INVALID_ARGUMENT;
  if (!state->initialized)
    {
      state->dac = 54;
      state->initialized = true;
    }
  previous_dac = state->dac;

  for (unsigned int trial = 0; trial < FTE3600_IMAGE_MAX_SEARCH_CAPTURES; ++trial)
    {
      struct fte3600_image_statistics observed;
      unsigned int median;
      if (capture (context, state->dac, pixels) != 0)
        return FTE3600_IMAGE_CAPTURE_ERROR;
      (void) fte3600_image_measure (pixels, &observed);
      median = observed.median;

      /* Widen before comparing adjacency: 255 and 0 are not adjacent. */
      if (median > 512 && previous_median <= 512 &&
          (unsigned int) state->dac == (unsigned int) previous_dac + 1)
        {
          state->stored_median = observed.median;
          return fte3600_image_refine (state, pixels, capture, context);
        }
      if (median <= 512 && previous_median > 512 &&
          (unsigned int) previous_dac == (unsigned int) state->dac + 1)
        {
          state->dac = previous_dac;
          state->stored_median = previous_median;
          if (capture (context, state->dac, pixels) != 0)
            return FTE3600_IMAGE_CAPTURE_ERROR;
          return fte3600_image_refine (state, pixels, capture, context);
        }
      if (trial + 1 == FTE3600_IMAGE_MAX_SEARCH_CAPTURES)
        return FTE3600_IMAGE_FAILED;

      previous_dac = state->dac;
      previous_median = observed.median;
      if (median < 20)
        {
          if (state->dac == upper_endpoint)
            return FTE3600_IMAGE_FAILED;
          lower_endpoint = state->dac;
          state->dac = (uint8_t) (((unsigned int) state->dac + upper_endpoint) / 2);
        }
      else if (median > 1000)
        {
          if (state->dac == lower_endpoint)
            return FTE3600_IMAGE_FAILED;
          upper_endpoint = state->dac;
          state->dac = (uint8_t) (((unsigned int) state->dac + lower_endpoint) / 2);
        }
      else if (median <= 512)
        {
          unsigned int step = median <= 311 ? (512 - median) / 150 : 1;
          state->dac = (uint8_t) (state->dac + step);
        }
      else
        {
          unsigned int step = median >= 713 ? (median - 512) / 150 : 1;
          state->dac = (uint8_t) ((unsigned int) state->dac - step);
        }
      state->auxiliary_state = 0;
    }
  return FTE3600_IMAGE_FAILED;
}
