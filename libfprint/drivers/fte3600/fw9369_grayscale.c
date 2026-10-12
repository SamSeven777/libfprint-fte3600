/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_fw9369_grayscale.h"

#include <fenv.h>
#include <float.h>
#include <limits.h>
#include <string.h>

#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__)
#error "FW9369 grayscale requires IEEE floating-point arithmetic"
#endif
_Static_assert (CHAR_BIT == 8 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 &&
                FLT_MAX_EXP == 128 && sizeof (float) == 4, "IEEE binary32 required");
_Static_assert (DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024 && sizeof (double) == 8,
                "IEEE binary64 required");

static float
add32 (float a, float b)
{
  volatile float v = a + b;

  return v;
}
static float
mul32 (float a, float b)
{
  volatile float v = a * b;

  return v;
}
static float
div32 (float a, float b)
{
  volatile float v = a / b;

  return v;
}
static double
add64 (double a, double b)
{
  volatile double v = a + b;

  return v;
}
static double
mul64 (double a, double b)
{
  volatile double v = a * b;

  return v;
}
static double
div64 (double a, double b)
{
  volatile double v = a / b;

  return v;
}

static int32_t
signed16 (uint16_t bits)
{
  return bits <= INT16_MAX ? (int32_t) bits : (int32_t) bits - 65536;
}

static void
borders (uint16_t *pixels)
{
  for (size_t y = 0; y < 80; ++y)
    {
      pixels[y * 64] = pixels[y * 64 + 1] = pixels[y * 64 + 2];
      pixels[y * 64 + 62] = pixels[y * 64 + 63] = pixels[y * 64 + 61];
    }
  memcpy (pixels, pixels + 64, 64 * sizeof (*pixels));
  memcpy (pixels + 79 * 64, pixels + 78 * 64, 64 * sizeof (*pixels));
}

static void
white_points (uint16_t *pixels)
{
  for (size_t y = 2; y <= 77; ++y)
    {
      for (size_t x = 2; x <= 61; ++x)
        {
          const size_t i = y * 64 + x;
          const uint16_t upper = (uint16_t) ((uint32_t) pixels[i] + 50);
          const uint16_t lower = pixels[i] < 50 ? 0 : (uint16_t) (pixels[i] - 50);
          const uint16_t neighbors[4] = {
            pixels[i - 1], pixels[i + 1], pixels[i - 64], pixels[i + 64]
          };
          uint32_t sum = 0;
          unsigned high = 0, low = 0;
          for (size_t j = 0; j < 4; ++j)
            {
              sum += neighbors[j];
              high += neighbors[j] > upper;
              low += neighbors[j] < lower;
            }
          if (high >= 3 || low >= 3)
            pixels[i] = (uint16_t) ((sum & UINT32_C (65535)) / 4);
        }
    }
}

static void
smooth (float *histogram, float *scratch, size_t length)
{
  for (size_t i = 0; i < length; ++i)
    {
      const size_t first = i > 2 ? i - 2 : 0;
      const size_t last = i + 2 < length ? i + 2 : length - 1;
      float total = 0.0f;
      for (size_t j = first; j <= last; ++j)
        total = add32 (total, histogram[j]);
      scratch[i] = div32 (total, (float) (last - first + 1));
    }
  memcpy (histogram, scratch, length * sizeof (*histogram));
}

static void
normalize (uint16_t *pixels, uint8_t *output,
           struct fte3600_fw9369_grayscale_workspace *workspace)
{
  int32_t minimum = INT16_MAX;

  for (size_t i = 0; i < FTE3600_IMAGE_PIXELS; ++i)
    {
      pixels[i] = (uint16_t) (0u - (uint32_t) pixels[i]);
      const int32_t value = signed16 (pixels[i]);
      if (value < minimum)
        minimum = value;
    }
  int32_t lower = INT16_MAX, upper = INT16_MIN;
  for (size_t i = 0; i < FTE3600_IMAGE_PIXELS; ++i)
    {
      pixels[i] = (uint16_t) (signed16 (pixels[i]) - minimum);
      const int32_t value = signed16 (pixels[i]);
      if (value < lower)
        lower = value;
      if (value > upper)
        upper = value;
    }
  uint32_t counts[256] = {0};
  const double span = add64 ((double) (upper - lower), 1e-6);
  for (size_t i = 0; i < FTE3600_IMAGE_PIXELS; ++i)
    {
      const int32_t value = signed16 (pixels[i]);
      const unsigned bin = (unsigned) div64 ((double) ((value - lower) * 256), span);
      ++counts[bin];
    }
  float *histogram = workspace->histogram;
  for (size_t i = 0; i < 256; ++i)
    histogram[i] = (float) counts[i];
  for (unsigned pass = 0; pass < 3; ++pass)
    smooth (histogram, workspace->scratch, 256);
  for (unsigned pass = 0; pass < 2; ++pass)
    smooth (histogram, workspace->scratch, 250);
  float a = 0.0f, b = 0.0f;
  for (size_t i = 0; i < 200; ++i)
    a = add32 (a, histogram[i]);
  for (size_t i = 200; i < 250; ++i)
    b = add32 (b, histogram[i]);
  int limit = 250;
  float threshold = 75.0f;
  if ((double) b > mul64 ((double) a, 0.8))
    {
      limit = 200;
      threshold = div32 (mul32 (a, 100.0f), b);
    }
  int cutoff = limit;
  unsigned consecutive = 0;
  for (int center = limit - 10; center >= 0; --center)
    {
      const int first = center > 2 ? center - 2 : 0;
      const int last = center + 2 < limit ? center + 2 : limit;
      float total = 0.0f;
      for (int j = first; j <= last; ++j)
        total = add32 (total, histogram[j]);
      if (total < threshold)
        {
          ++consecutive;
        }
      else if (consecutive > 5)
        {
          cutoff = center;
          break;
        }
      else
        {
          consecutive = 0;
        }
    }
  if (cutoff < 50)
    cutoff = 250;
  if (limit == 250)
    {
      cutoff += 30;
      if (cutoff > 250)
        cutoff = 250;
    }
  const int32_t denominator = cutoff * upper / 250 - lower;
  const double divisor = add64 ((double) denominator, 1e-6);
  for (size_t i = 0; i < FTE3600_IMAGE_PIXELS; ++i)
    {
      const int32_t value = signed16 (pixels[i]);
      const int64_t mapped = (int64_t) div64 ((double) ((value - lower) * 255), divisor);
      output[i] = mapped > 255 ? 255 : (uint8_t) ((uint64_t) mapped & UINT64_C (255));
    }
}

struct region { const void *pointer;
                size_t      length;
};
static int
overlapping (struct region a, struct region b)
{
  const uintptr_t first = (uintptr_t) a.pointer, second = (uintptr_t) b.pointer;

  return first >= second ? first - second < b.length : second - first < a.length;
}

static void
observe (const struct fte3600_fw9369_grayscale_observer *observer,
         enum fte3600_fw9369_grayscale_stage stage, const uint16_t *pixels)
{
  if (observer != NULL && observer->stage != NULL)
    observer->stage (observer->context, stage, pixels, FTE3600_IMAGE_PIXELS);
}

enum fte3600_fw9369_grayscale_status
fte3600_fw9369_grayscale_run (
  enum fte3600_fw9369_grayscale_action action, size_t width, size_t height,
  struct fte3600_fw9369_startup_state *state,
  const struct fte3600_fw9369_grayscale_buffers *buffers,
  struct fte3600_fw9369_grayscale_workspace *workspace, size_t workspace_bytes,
  const struct fte3600_capture_io *io,
  struct fte3600_fw9369_grayscale_diagnostics *diagnostics,
  const struct fte3600_fw9369_grayscale_observer *observer)
{
  if (action < FTE3600_FW9369_GRAYSCALE_CAPTURE || action > FTE3600_FW9369_GRAYSCALE_NORMALIZE ||
      width != 64 || height != 80 || buffers == NULL || buffers->processed == NULL)
    return FTE3600_FW9369_GRAYSCALE_INVALID_ARGUMENT;
  const int capture = action == FTE3600_FW9369_GRAYSCALE_CAPTURE;
  const int gray = action != FTE3600_FW9369_GRAYSCALE_WHITE_POINTS;
  if ((!capture && (buffers->input == NULL || buffers->input_samples != FTE3600_IMAGE_PIXELS)) ||
      (gray && (buffers->output == NULL || workspace == NULL)) ||
      (capture && (state == NULL || io == NULL || io->spi == NULL || io->wait_ms == NULL)))
    return FTE3600_FW9369_GRAYSCALE_INVALID_ARGUMENT;
  if (buffers->processed_samples < FTE3600_IMAGE_PIXELS ||
      (gray && (buffers->output_bytes < FTE3600_IMAGE_PIXELS ||
                workspace_bytes < sizeof (*workspace) ||
                (uintptr_t) workspace % _Alignof (struct fte3600_fw9369_grayscale_workspace) != 0)))
    return FTE3600_FW9369_GRAYSCALE_RESOURCE_ERROR;

  struct region regions[6];
  size_t regions_count = 0;
  regions[regions_count++] = (struct region){buffers->processed, FTE3600_CAPTURE_FRAME_BYTES};
  if (!capture)
    regions[regions_count++] = (struct region){buffers->input, FTE3600_CAPTURE_FRAME_BYTES}
  ;
  if (gray)
    {
      regions[regions_count++] = (struct region){buffers->output, FTE3600_IMAGE_PIXELS};
      regions[regions_count++] = (struct region){workspace, sizeof (*workspace)};
    }
  if (capture)
    regions[regions_count++] = (struct region){state, sizeof (*state)}
  ;
  if (diagnostics != NULL)
    regions[regions_count++] = (struct region){diagnostics, sizeof (*diagnostics)}
  ;
  for (size_t i = 0; i < regions_count; ++i)
    for (size_t j = 0; j < i; ++j)
      if (overlapping (regions[i], regions[j]))
        return FTE3600_FW9369_GRAYSCALE_INVALID_ARGUMENT;
  if (gray && fegetround () != FE_TONEAREST)
    return FTE3600_FW9369_GRAYSCALE_FLOAT_ENVIRONMENT_ERROR;
  if (diagnostics != NULL)
    memset (diagnostics, 0, sizeof (*diagnostics));

  if (capture)
    {
      struct fte3600_capture_state acquisition = {
        state->image_dac, state->shift, state->config.channel_mode,
        state->fdt.mode, state->fdt.configured, state->fdt.check_registers,
        state->fdt.dac_cache, state->period
      };
      const enum fte3600_capture_status status = fte3600_capture_frames (
        &acquisition, 1, workspace->raw, sizeof (workspace->raw), io,
        diagnostics == NULL ? NULL : &diagnostics->transport);
      state->fdt.mode = acquisition.mode;
      state->fdt.configured = acquisition.configured;
      state->fdt.check_registers = acquisition.check_registers;
      state->fdt.dac_cache = acquisition.dac_cache;
      if (status != FTE3600_CAPTURE_OK)
        return FTE3600_FW9369_GRAYSCALE_WORD_READ_ERROR;
      for (size_t i = 0; i < FTE3600_IMAGE_PIXELS; ++i)
        {
          const uint16_t raw = (uint16_t) ((uint32_t) workspace->raw[2 * i] * 256 + workspace->raw[2 * i + 1]);
          const int32_t difference = (int32_t) state->baseline[i] - raw;
          buffers->processed[i] = difference < 0 ? 0 : (uint16_t) difference;
        }
      borders (buffers->processed);
    }
  else
    {
      memcpy (buffers->processed, buffers->input, FTE3600_CAPTURE_FRAME_BYTES);
    }
  if (capture || action == FTE3600_FW9369_GRAYSCALE_WHITE_POINTS)
    {
      observe (observer, FTE3600_FW9369_GRAYSCALE_BEFORE_WHITE, buffers->processed);
      white_points (buffers->processed);
    }
  if (gray)
    {
      observe (observer, FTE3600_FW9369_GRAYSCALE_BEFORE_NORMALIZE, buffers->processed);
      normalize (buffers->processed, buffers->output, workspace);
    }
  observe (observer, FTE3600_FW9369_GRAYSCALE_FINAL_WORDS, buffers->processed);
  if (diagnostics != NULL)
    diagnostics->completed = 1;
  return FTE3600_FW9369_GRAYSCALE_OK;
}
