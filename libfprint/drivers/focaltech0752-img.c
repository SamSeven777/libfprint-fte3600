/*
 * FocalTech FT9362 (2808:0752) Image Preprocessing
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "focaltech0752-img.h"
#include <string.h>
#include <stdlib.h>

/* 3x3 median filter to eliminate capacitive sensor grid noise */
static void
median_filter_3x3 (const float *input, float *output, int height, int width)
{
  for (int y = 0; y < height; y++)
    {
      for (int x = 0; x < width; x++)
        {
          float window[9];
          int count = 0;

          for (int dy = -1; dy <= 1; dy++)
            {
              int ny = y + dy;
              if (ny < 0 || ny >= height)
                continue;
              for (int dx = -1; dx <= 1; dx++)
                {
                  int nx = x + dx;
                  if (nx >= 0 && nx < width)
                    window[count++] = input[ny * width + nx];
                }
            }

          for (int i = 1; i < count; i++)
            {
              float key = window[i];
              int j = i - 1;
              while (j >= 0 && window[j] > key)
                {
                  window[j + 1] = window[j];
                  j--;
                }
              window[j + 1] = key;
            }

          output[y * width + x] = window[count / 2];
        }
    }
}

static int
float_compare (const void *a, const void *b)
{
  float fa = *(const float *) a;
  float fb = *(const float *) b;
  return (fa > fb) - (fa < fb);
}

/* Contrast stretch & center the 40x76 sensor frame into 64x80 BRISK coordinates */
void
focaltech0752_process_raw_to_brisk (const uint8_t *raw_data,
                                    guint8 brisk_image[FTE3600_BRISK_IMAGE_SIZE])
{
  const int16_t *pixels = (const int16_t *) (raw_data + FT_RAW_HEADER);
  float temp[FT9362_ACTIVE_SIZE];
  float filtered[FT9362_ACTIVE_SIZE];
  float sorted[FT9362_ACTIVE_SIZE];
  float p5, p95, range;

  /* Extract active region (3040 signal samples at offset 3040) */
  for (int i = 0; i < FT9362_ACTIVE_SIZE; i++)
    temp[i] = (float) pixels[3040 + i];

  median_filter_3x3 (temp, filtered, FT9362_ACTIVE_HEIGHT, FT9362_ACTIVE_WIDTH);

  memcpy (sorted, filtered, sizeof (sorted));
  qsort (sorted, FT9362_ACTIVE_SIZE, sizeof (float), float_compare);

  p5 = sorted[(int) (0.05f * (FT9362_ACTIVE_SIZE - 1))];
  p95 = sorted[(int) (0.95f * (FT9362_ACTIVE_SIZE - 1))];
  range = p95 - p5 + 1e-6f;

  /* Fill BRISK image buffer with neutral gray background (128) */
  memset (brisk_image, 128, FTE3600_BRISK_IMAGE_SIZE);

  /* Center 40x76 into 64x80:
   * x offset = (64 - 40) / 2 = 12
   * y offset = (80 - 76) / 2 = 2
   */
  const int x_offset = (FTE3600_BRISK_WIDTH - FT9362_ACTIVE_WIDTH) / 2;
  const int y_offset = (FTE3600_BRISK_HEIGHT - FT9362_ACTIVE_HEIGHT) / 2;

  for (int y = 0; y < FT9362_ACTIVE_HEIGHT; y++)
    {
      for (int x = 0; x < FT9362_ACTIVE_WIDTH; x++)
        {
          float val = (filtered[y * FT9362_ACTIVE_WIDTH + x] - p5) / range;
          if (val < 0.0f)
            val = 0.0f;
          if (val > 1.0f)
            val = 1.0f;
          /* Invert normalized float: valleys light, ridges dark */
          guint8 u8_val = (guint8) ((1.0f - val) * 255.0f);
          brisk_image[(y + y_offset) * FTE3600_BRISK_WIDTH + (x + x_offset)] = u8_val;
        }
    }
}
