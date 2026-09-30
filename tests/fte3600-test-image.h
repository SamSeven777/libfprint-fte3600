/* Mathematical image fixtures, shared by extractor and driver tests.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include <math.h>

static void
make_visual_pattern (guint8 *image)
{
  static const struct
  {
    gdouble x;
    gdouble y;
    gdouble sigma;
    gdouble amplitude;
  } spots[] = {
    { 17, 18, 1.5,  58 }, { 29, 17, 2.2, -54 },
    { 43, 19, 3.1,  61 }, { 51, 29, 1.8, -48 },
    { 18, 32, 2.7, -61 }, { 32, 31, 1.4,  52 },
    { 43, 39, 2.4, -57 }, { 17, 47, 1.9,  55 },
    { 31, 49, 3.2,  63 }, { 49, 52, 1.5, -53 },
    { 20, 63, 2.3, -58 }, { 36, 63, 1.7,  57 },
    { 48, 66, 2.8,  50 },
  };

  for (guint y = 0; y < FTE3600_BRISK_HEIGHT; y++)
    for (guint x = 0; x < FTE3600_BRISK_WIDTH; x++)
      {
        gdouble value = 126.0 + 13.0 * sin (0.29 * x + 0.17 * y) +
                        8.0 * cos (0.13 * x - 0.23 * y);

        for (guint i = 0; i < G_N_ELEMENTS (spots); i++)
          {
            const gdouble dx = x - spots[i].x;
            const gdouble dy = y - spots[i].y;

            value += spots[i].amplitude *
                     exp (-(dx * dx + dy * dy) /
                          (2.0 * spots[i].sigma * spots[i].sigma));
          }
        image[y * FTE3600_BRISK_WIDTH + x] = CLAMP (floor (value + 0.5), 2, 253);
      }
}

static void
translate_pattern (const guint8 *source,
                   guint8       *destination,
                   gint          translate_x,
                   gint          translate_y,
                   gboolean      add_noise)
{
  for (guint y = 0; y < FTE3600_BRISK_HEIGHT; y++)
    for (guint x = 0; x < FTE3600_BRISK_WIDTH; x++)
      {
        const gint source_x = (gint) x - translate_x;
        const gint source_y = (gint) y - translate_y;
        gint value = 126;

        if (source_x >= 0 && source_x < FTE3600_BRISK_WIDTH &&
            source_y >= 0 && source_y < FTE3600_BRISK_HEIGHT)
          value = source[source_y * FTE3600_BRISK_WIDTH + source_x];
        if (add_noise)
          value += ((17 * x + 31 * y) % 3) - 1;
        destination[y * FTE3600_BRISK_WIDTH + x] = CLAMP (value, 0, 255);
      }
}
