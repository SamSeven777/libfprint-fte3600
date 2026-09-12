/*
 * Clean-room BRISK-style feature matcher prototype for the FocalTech FT9361
 *
 * The frozen point-pair table in this file was generated from a documented
 * public seed.  It is not copied from a vendor driver.  Likewise, the scale-space,
 * detector, descriptor, and matcher below are an independent implementation
 * of published computer-vision techniques.
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "fte3600-brisk.h"

#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BRISK_PI                 3.14159265358979323846
#define BRISK_TWO_PI             (2.0 * BRISK_PI)
#define BRISK_UPSCALE            2
#define BRISK_OCTAVES            2
#define BRISK_GAUSSIAN_LEVELS    6
#define BRISK_DOG_LEVELS         5
#define BRISK_ORIENTATION_BINS   36
#define BRISK_DESCRIPTOR_MARGIN  14
#define BRISK_DOG_PRETHRESHOLD_U8 0.85
#define BRISK_DOG_CONTRAST_NORMALIZED 0.02
#define BRISK_U8_RANGE           255.0
#define BRISK_MAX_REFINEMENT     5
#define BRISK_AXIS_INLIER_LIMIT  1.2
#define BRISK_ORIENTATION_LIMIT  (20.0 * BRISK_PI / 180.0)

typedef struct {
  guint8 first;
  guint8 second;
} DescriptorPair;

typedef struct {
  guint width;
  guint height;
  gfloat *gaussian[BRISK_GAUSSIAN_LEVELS];
  gfloat *dog[BRISK_DOG_LEVELS];
} BriskOctave;

typedef struct {
  gfloat x;
  gfloat y;
  gfloat image_x;
  gfloat image_y;
  gfloat response;
  guint  octave;
  guint  level;
} BriskKeypoint;

typedef struct {
  gdouble angle;
  gdouble translate_x;
  gdouble translate_y;
} RigidModel;

typedef struct {
  guint n_groups;
  guint group_for_feature[FTE3600_BRISK_MAX_FEATURES];
} PhysicalGroups;

typedef struct {
  guint hamming;
  guint query_feature;
  guint reference_feature;
} GroupPairDistance;

typedef struct {
  gint     previous_mode;
  gboolean changed;
} BriskRoundingGuard;

static gboolean
brisk_rounding_guard_enter (BriskRoundingGuard *guard)
{
  memset (guard, 0, sizeof (*guard));
  guard->previous_mode = fegetround ();
  if (guard->previous_mode == -1)
    return FALSE;
  if (guard->previous_mode == FE_TONEAREST)
    return TRUE;
  if (fesetround (FE_TONEAREST) != 0)
    return FALSE;
  guard->changed = TRUE;
  return TRUE;
}

static void
brisk_rounding_guard_clear (BriskRoundingGuard *guard)
{
  if (guard->changed && fesetround (guard->previous_mode) != 0)
    g_warning ("Failed to restore caller floating-point rounding mode");
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (BriskRoundingGuard,
                                  brisk_rounding_guard_clear)

/*
 * Clean-room descriptor table, generated once from
 * FTE3600_BRISK_PAIR_SEED with xorshift32 and the following schema-v1 process:
 * enumerate the 990 unordered pairs lexicographically; Fisher-Yates shuffle
 * from the end using xorshift32(state) % (i + 1); then repeatedly choose the
 * first unused minimum-cost pair in shuffled order, where cost is
 * 24*(endpoint uses) + 3*(length-bin uses) + 2*(undirected angle-bin uses) +
 * 6 for a same-ring pair.  Length-squared boundaries are 36, 100, and 225;
 * there are 12 angle bins over [0, pi).  A further xorshift bit directs each
 * selected pair.  It is frozen here so libm differences at angle-bin
 * boundaries cannot silently change a persisted template.  This is not a
 * vendor pair table.
 *
 * SHA-256 over the 512 interleaved first/second bytes is asserted by tests.
 */
static const DescriptorPair descriptor_pairs[] = {
  { 22, 40 }, { 5, 27 }, { 9, 13 }, { 30, 11 }, { 38, 0 }, { 16, 1 },
  { 6, 14 }, { 34, 17 }, { 28, 20 }, { 7, 24 }, { 33, 19 }, { 32, 15 },
  { 21, 37 }, { 10, 31 }, { 3, 35 }, { 8, 23 }, { 2, 18 }, { 43, 4 },
  { 12, 29 }, { 25, 44 }, { 42, 39 }, { 26, 41 }, { 17, 36 }, { 22, 8 },
  { 9, 33 }, { 2, 30 }, { 6, 12 }, { 14, 3 }, { 16, 31 }, { 28, 7 },
  { 20, 43 }, { 18, 35 }, { 15, 34 }, { 26, 5 }, { 40, 19 }, { 23, 42 },
  { 44, 1 }, { 13, 41 }, { 11, 0 }, { 4, 32 }, { 36, 21 }, { 38, 24 },
  { 25, 10 }, { 27, 29 }, { 39, 37 }, { 18, 9 }, { 43, 14 }, { 20, 39 },
  { 38, 6 }, { 36, 15 }, { 5, 28 }, { 21, 8 }, { 4, 22 }, { 17, 26 },
  { 3, 16 }, { 13, 0 }, { 24, 34 }, { 7, 10 }, { 19, 2 }, { 11, 1 },
  { 12, 41 }, { 23, 40 }, { 31, 32 }, { 25, 42 }, { 44, 30 }, { 35, 37 },
  { 27, 33 }, { 16, 29 }, { 23, 9 }, { 14, 26 }, { 15, 4 }, { 2, 11 },
  { 13, 40 }, { 5, 31 }, { 20, 36 }, { 22, 7 }, { 18, 33 }, { 17, 28 },
  { 21, 1 }, { 24, 44 }, { 19, 35 }, { 12, 37 }, { 0, 27 }, { 30, 6 },
  { 8, 42 }, { 29, 10 }, { 38, 39 }, { 3, 43 }, { 32, 34 }, { 25, 41 },
  { 9, 24 }, { 21, 4 }, { 14, 30 }, { 37, 6 }, { 7, 25 }, { 2, 12 },
  { 27, 22 }, { 13, 31 }, { 33, 5 }, { 10, 1 }, { 39, 16 }, { 44, 8 },
  { 18, 3 }, { 11, 26 }, { 38, 15 }, { 0, 19 }, { 41, 20 }, { 35, 17 },
  { 23, 32 }, { 34, 36 }, { 43, 42 }, { 28, 40 }, { 5, 29 }, { 6, 21 },
  { 7, 35 }, { 4, 17 }, { 9, 31 }, { 25, 1 }, { 42, 24 }, { 28, 12 },
  { 38, 14 }, { 40, 18 }, { 10, 2 }, { 19, 37 }, { 27, 16 }, { 11, 8 },
  { 30, 0 }, { 13, 3 }, { 22, 34 }, { 41, 23 }, { 20, 32 }, { 33, 15 },
  { 43, 26 }, { 36, 39 }, { 29, 44 }, { 5, 18 }, { 7, 21 }, { 9, 37 },
  { 26, 12 }, { 43, 24 }, { 0, 35 }, { 28, 3 }, { 19, 4 }, { 13, 1 },
  { 25, 17 }, { 40, 6 }, { 8, 10 }, { 22, 42 }, { 38, 11 }, { 27, 14 },
  { 2, 29 }, { 16, 34 }, { 20, 30 }, { 15, 31 }, { 23, 39 }, { 44, 32 },
  { 33, 36 }, { 41, 8 }, { 2, 24 }, { 1, 12 }, { 28, 16 }, { 6, 43 },
  { 44, 9 }, { 5, 19 }, { 15, 42 }, { 37, 22 }, { 4, 33 }, { 7, 20 },
  { 14, 40 }, { 31, 18 }, { 0, 10 }, { 25, 11 }, { 21, 26 }, { 3, 27 },
  { 32, 17 }, { 30, 13 }, { 36, 23 }, { 39, 41 }, { 38, 35 }, { 29, 34 },
  { 24, 0 }, { 20, 1 }, { 30, 12 }, { 18, 6 }, { 16, 43 }, { 35, 14 },
  { 42, 21 }, { 15, 3 }, { 13, 37 }, { 9, 40 }, { 38, 19 }, { 17, 7 },
  { 31, 4 }, { 5, 44 }, { 41, 22 }, { 23, 25 }, { 36, 10 }, { 2, 33 },
  { 27, 11 }, { 8, 32 }, { 26, 28 }, { 39, 29 }, { 34, 14 }, { 15, 6 },
  { 22, 9 }, { 5, 37 }, { 4, 16 }, { 7, 23 }, { 33, 12 }, { 44, 21 },
  { 39, 19 }, { 10, 26 }, { 29, 24 }, { 17, 1 }, { 8, 40 }, { 13, 2 },
  { 18, 42 }, { 3, 20 }, { 25, 0 }, { 11, 36 }, { 28, 27 }, { 38, 41 },
  { 30, 32 }, { 35, 34 }, { 43, 31 }, { 26, 9 }, { 14, 5 }, { 39, 21 },
  { 31, 8 }, { 1, 41 }, { 6, 23 }, { 20, 38 }, { 10, 30 }, { 15, 7 },
  { 18, 37 }, { 36, 19 }, { 43, 13 }, { 40, 24 }, { 12, 25 }, { 16, 33 },
  { 11, 35 }, { 22, 44 }, { 32, 3 }, { 2, 42 }, { 28, 0 }, { 4, 34 },
  { 29, 17 }, { 27, 12 }, { 40, 21 }, { 26, 2 }, { 20, 4 }, { 43, 23 },
  { 18, 29 }, { 8, 17 }, { 0, 22 }, { 5, 16 },
};

G_STATIC_ASSERT (G_N_ELEMENTS (descriptor_pairs) == FTE3600_BRISK_DESCRIPTOR_BITS);

static gdouble
wrap_angle (gdouble angle)
{
  while (angle <= -BRISK_PI)
    angle += BRISK_TWO_PI;
  while (angle > BRISK_PI)
    angle -= BRISK_TWO_PI;

  return angle;
}

/* Unlike lrint(), this is independent of the calling thread's floating-point
 * rounding mode.  Extractor schema v1 specifies halves away from zero. */
static gint
round_half_away_from_zero (gdouble value)
{
  return value < 0.0 ? (gint) ceil (value - 0.5) : (gint) floor (value + 0.5);
}

static void
get_pattern_point (guint    index,
                   gdouble *x,
                   gdouble *y)
{
  guint ring_index;
  guint count;
  gdouble radius;
  gdouble angle;

  if (index < 10)
    {
      ring_index = index;
      count = 10;
      radius = 4.0;
    }
  else if (index < 25)
    {
      ring_index = index - 10;
      count = 15;
      radius = 8.0;
    }
  else
    {
      ring_index = index - 25;
      count = 20;
      radius = 13.0;
    }

  /* Every ring deliberately starts at angle zero. */
  angle = BRISK_TWO_PI * ring_index / count;
  *x = radius * cos (angle);
  *y = radius * sin (angle);
}

gboolean
fte3600_brisk_pattern_point (guint   index,
                             gfloat *x,
                             gfloat *y)
{
  g_auto(BriskRoundingGuard) rounding_guard = { 0 };
  gdouble point_x;
  gdouble point_y;

  if (!brisk_rounding_guard_enter (&rounding_guard))
    return FALSE;
  if (index >= FTE3600_BRISK_PATTERN_POINTS || x == NULL || y == NULL)
    return FALSE;

  get_pattern_point (index, &point_x, &point_y);
  *x = point_x;
  *y = point_y;
  return TRUE;
}

gboolean
fte3600_brisk_descriptor_pair (guint  bit,
                               guint *first,
                               guint *second)
{
  if (bit >= FTE3600_BRISK_DESCRIPTOR_BITS || first == NULL || second == NULL)
    return FALSE;

  *first = descriptor_pairs[bit].first;
  *second = descriptor_pairs[bit].second;
  return TRUE;
}

static gdouble
cubic_weight (gdouble value)
{
  const gdouble absolute = fabs (value);

  if (absolute <= 1.0)
    return 1.5 * absolute * absolute * absolute -
           2.5 * absolute * absolute + 1.0;
  if (absolute < 2.0)
    return -0.5 * absolute * absolute * absolute +
           2.5 * absolute * absolute - 4.0 * absolute + 2.0;
  return 0.0;
}

static void
bicubic_upscale_2x (const guint8 *source,
                    gfloat       *destination)
{
  const guint destination_width = FTE3600_BRISK_WIDTH * BRISK_UPSCALE;
  const guint destination_height = FTE3600_BRISK_HEIGHT * BRISK_UPSCALE;

  for (guint y = 0; y < destination_height; y++)
    for (guint x = 0; x < destination_width; x++)
      {
        const gdouble source_x = (x + 0.5) / BRISK_UPSCALE - 0.5;
        const gdouble source_y = (y + 0.5) / BRISK_UPSCALE - 0.5;
        const gint base_x = floor (source_x);
        const gint base_y = floor (source_y);
        gdouble sum = 0.0;
        gdouble weight_sum = 0.0;

        for (gint offset_y = -1; offset_y <= 2; offset_y++)
          for (gint offset_x = -1; offset_x <= 2; offset_x++)
            {
              const gint image_x = CLAMP (base_x + offset_x, 0,
                                          FTE3600_BRISK_WIDTH - 1);
              const gint image_y = CLAMP (base_y + offset_y, 0,
                                          FTE3600_BRISK_HEIGHT - 1);
              const gdouble weight = cubic_weight (source_x - (base_x + offset_x)) *
                                     cubic_weight (source_y - (base_y + offset_y));

              sum += weight * source[image_y * FTE3600_BRISK_WIDTH + image_x];
              weight_sum += weight;
            }

        destination[y * destination_width + x] =
          CLAMP (sum / weight_sum, 0.0, 255.0);
      }
}

static void
box_blur (const gfloat *source,
          gfloat       *destination,
          guint         width,
          guint         height,
          guint         box_width)
{
  const gint radius = (box_width - 1) / 2;
  const gdouble normalization = 1.0 / (box_width * box_width);

  for (guint y = 0; y < height; y++)
    for (guint x = 0; x < width; x++)
      {
        gdouble sum = 0.0;

        for (gint offset_y = -radius; offset_y <= radius; offset_y++)
          for (gint offset_x = -radius; offset_x <= radius; offset_x++)
            {
              const guint image_x = CLAMP ((gint) x + offset_x, 0, (gint) width - 1);
              const guint image_y = CLAMP ((gint) y + offset_y, 0, (gint) height - 1);

              sum += source[image_y * width + image_x];
            }

        destination[y * width + x] = sum * normalization;
      }
}

static void
apply_box_sequence (const gfloat *source,
                    gfloat       *destination,
                    guint         width,
                    guint         height,
                    const guint  *box_widths,
                    guint         n_boxes)
{
  const gsize bytes = width * height * sizeof (gfloat);
  g_autofree gfloat *first = g_new (gfloat, width * height);
  g_autofree gfloat *second = g_new (gfloat, width * height);
  gfloat *current = first;
  gfloat *next = second;

  memcpy (current, source, bytes);
  for (guint i = 0; i < n_boxes; i++)
    {
      gfloat *temporary;

      box_blur (current, next, width, height, box_widths[i]);
      temporary = current;
      current = next;
      next = temporary;
    }
  memcpy (destination, current, bytes);
}

static void
octave_clear (BriskOctave *octave)
{
  for (guint level = 0; level < BRISK_GAUSSIAN_LEVELS; level++)
    g_clear_pointer (&octave->gaussian[level], g_free);
  for (guint level = 0; level < BRISK_DOG_LEVELS; level++)
    g_clear_pointer (&octave->dog[level], g_free);
}

static void
octave_build_levels (BriskOctave *octave)
{
  static const guint narrow_boxes[] = { 3, 3, 3 };
  static const guint wide_boxes[] = { 3, 3, 5 };
  const gsize pixels = octave->width * octave->height;

  for (guint level = 1; level < BRISK_GAUSSIAN_LEVELS; level++)
    {
      const guint *boxes = level <= 2 ? narrow_boxes : wide_boxes;

      octave->gaussian[level] = g_new (gfloat, pixels);
      apply_box_sequence (octave->gaussian[level - 1], octave->gaussian[level],
                          octave->width, octave->height, boxes,
                          G_N_ELEMENTS (narrow_boxes));
    }

  for (guint level = 0; level < BRISK_DOG_LEVELS; level++)
    {
      octave->dog[level] = g_new (gfloat, pixels);
      for (gsize i = 0; i < pixels; i++)
        octave->dog[level][i] = octave->gaussian[level + 1][i] -
                                octave->gaussian[level][i];
    }
}

static void
build_scale_space (const guint8 *image,
                   BriskOctave   octaves[BRISK_OCTAVES])
{
  static const guint initial_boxes[] = { 3, 3, 3, 3 };
  BriskOctave *first = &octaves[0];
  BriskOctave *second = &octaves[1];
  g_autofree gfloat *upscaled = NULL;

  memset (octaves, 0, sizeof (BriskOctave) * BRISK_OCTAVES);

  first->width = FTE3600_BRISK_WIDTH * BRISK_UPSCALE;
  first->height = FTE3600_BRISK_HEIGHT * BRISK_UPSCALE;
  upscaled = g_new (gfloat, first->width * first->height);
  first->gaussian[0] = g_new (gfloat, first->width * first->height);
  bicubic_upscale_2x (image, upscaled);
  apply_box_sequence (upscaled, first->gaussian[0], first->width, first->height,
                      initial_boxes, G_N_ELEMENTS (initial_boxes));
  octave_build_levels (first);

  second->width = FTE3600_BRISK_WIDTH;
  second->height = FTE3600_BRISK_HEIGHT;
  second->gaussian[0] = g_new (gfloat, second->width * second->height);
  for (guint y = 0; y < second->height; y++)
    for (guint x = 0; x < second->width; x++)
      second->gaussian[0][y * second->width + x] =
        first->gaussian[3][(2 * y) * first->width + 2 * x];
  octave_build_levels (second);
}

static gboolean
image_has_contrast (const guint8 *image)
{
  gdouble sum = 0.0;
  gdouble square_sum = 0.0;

  for (guint i = 0; i < FTE3600_BRISK_IMAGE_SIZE; i++)
    {
      sum += image[i];
      square_sum += image[i] * image[i];
    }

  sum /= FTE3600_BRISK_IMAGE_SIZE;
  square_sum = square_sum / FTE3600_BRISK_IMAGE_SIZE - sum * sum;
  return square_sum >= 64.0;
}

static gboolean
is_strict_extremum (const BriskOctave *octave,
                    guint              level,
                    guint              x,
                    guint              y)
{
  const gfloat value = octave->dog[level][y * octave->width + x];
  const gboolean maximum = value > 0.0;

  for (gint level_offset = -1; level_offset <= 1; level_offset++)
    for (gint y_offset = -1; y_offset <= 1; y_offset++)
      for (gint x_offset = -1; x_offset <= 1; x_offset++)
        {
          const gfloat neighbor = octave->dog[level + level_offset]
            [(y + y_offset) * octave->width + x + x_offset];

          if (level_offset == 0 && y_offset == 0 && x_offset == 0)
            continue;
          if ((maximum && value <= neighbor) || (!maximum && value >= neighbor))
            return FALSE;
        }

  return TRUE;
}

static gboolean
solve_symmetric_3x3 (gdouble  hxx,
                     gdouble  hyy,
                     gdouble  hss,
                     gdouble  hxy,
                     gdouble  hxs,
                     gdouble  hys,
                     gdouble  gx,
                     gdouble  gy,
                     gdouble  gs,
                     gdouble *offset_x,
                     gdouble *offset_y,
                     gdouble *offset_s)
{
  const gdouble determinant =
    hxx * (hyy * hss - hys * hys) -
    hxy * (hxy * hss - hys * hxs) +
    hxs * (hxy * hys - hyy * hxs);
  gdouble inverse00;
  gdouble inverse01;
  gdouble inverse02;
  gdouble inverse11;
  gdouble inverse12;
  gdouble inverse22;

  if (fabs (determinant) < 1e-9)
    return FALSE;

  inverse00 = (hyy * hss - hys * hys) / determinant;
  inverse01 = (hxs * hys - hxy * hss) / determinant;
  inverse02 = (hxy * hys - hxs * hyy) / determinant;
  inverse11 = (hxx * hss - hxs * hxs) / determinant;
  inverse12 = (hxy * hxs - hxx * hys) / determinant;
  inverse22 = (hxx * hyy - hxy * hxy) / determinant;

  *offset_x = -(inverse00 * gx + inverse01 * gy + inverse02 * gs);
  *offset_y = -(inverse01 * gx + inverse11 * gy + inverse12 * gs);
  *offset_s = -(inverse02 * gx + inverse12 * gy + inverse22 * gs);
  return isfinite (*offset_x) && isfinite (*offset_y) && isfinite (*offset_s);
}

static gboolean
refine_keypoint (const BriskOctave *octave,
                 guint             initial_level,
                 guint             initial_x,
                 guint             initial_y,
                 BriskKeypoint    *keypoint)
{
  gint level = initial_level;
  gint x = initial_x;
  gint y = initial_y;

  for (guint iteration = 0; iteration < BRISK_MAX_REFINEMENT; iteration++)
    {
      const guint width = octave->width;
      const gfloat *current;
      gdouble value;
      gdouble gx;
      gdouble gy;
      gdouble gs;
      gdouble hxx;
      gdouble hyy;
      gdouble hss;
      gdouble hxy;
      gdouble hxs;
      gdouble hys;
      gdouble offset_x;
      gdouble offset_y;
      gdouble offset_s;
      gdouble determinant;
      gdouble trace;
      gdouble interpolated;

      if (level < 1 || level > 3 || x < 2 || y < 2 ||
          x >= (gint) octave->width - 2 || y >= (gint) octave->height - 2)
        return FALSE;

      current = octave->dog[level];
      value = current[y * width + x];
      gx = 0.5 * (current[y * width + x + 1] - current[y * width + x - 1]);
      gy = 0.5 * (current[(y + 1) * width + x] - current[(y - 1) * width + x]);
      gs = 0.5 * (octave->dog[level + 1][y * width + x] -
                  octave->dog[level - 1][y * width + x]);
      hxx = current[y * width + x + 1] + current[y * width + x - 1] - 2.0 * value;
      hyy = current[(y + 1) * width + x] + current[(y - 1) * width + x] - 2.0 * value;
      hss = octave->dog[level + 1][y * width + x] +
            octave->dog[level - 1][y * width + x] - 2.0 * value;
      hxy = 0.25 * (current[(y + 1) * width + x + 1] -
                           current[(y + 1) * width + x - 1] -
                           current[(y - 1) * width + x + 1] +
                           current[(y - 1) * width + x - 1]);
      hxs = 0.25 * (octave->dog[level + 1][y * width + x + 1] -
                    octave->dog[level + 1][y * width + x - 1] -
                    octave->dog[level - 1][y * width + x + 1] +
                    octave->dog[level - 1][y * width + x - 1]);
      hys = 0.25 * (octave->dog[level + 1][(y + 1) * width + x] -
                    octave->dog[level + 1][(y - 1) * width + x] -
                    octave->dog[level - 1][(y + 1) * width + x] +
                    octave->dog[level - 1][(y - 1) * width + x]);

      if (!solve_symmetric_3x3 (hxx, hyy, hss, hxy, hxs, hys,
                                gx, gy, gs, &offset_x, &offset_y, &offset_s))
        return FALSE;
      if (fabs (offset_x) > 1.5 || fabs (offset_y) > 1.5 || fabs (offset_s) > 1.5)
        return FALSE;

      if (fabs (offset_x) >= 0.5 || fabs (offset_y) >= 0.5 || fabs (offset_s) >= 0.5)
        {
          x += offset_x >= 0.5 ? 1 : offset_x <= -0.5 ? -1 : 0;
          y += offset_y >= 0.5 ? 1 : offset_y <= -0.5 ? -1 : 0;
          level += offset_s >= 0.5 ? 1 : offset_s <= -0.5 ? -1 : 0;
          continue;
        }

      interpolated = value + 0.5 * (gx * offset_x + gy * offset_y + gs * offset_s);
      /* DoG samples remain in input (0..255) intensity units.  The initial
       * 0.85 gate is one raw-intensity-unit scale; the interpolated contrast
       * equation is explicitly normalized to 0..1 before applying 0.02. */
      if (3.0 * fabs (interpolated) / BRISK_U8_RANGE <
          BRISK_DOG_CONTRAST_NORMALIZED)
        return FALSE;

      determinant = hxx * hyy - hxy * hxy;
      trace = hxx + hyy;
      if (determinant <= 0.0 || 15.0 * trace * trace >= 256.0 * determinant)
        return FALSE;

      keypoint->x = x + offset_x;
      keypoint->y = y + offset_y;
      keypoint->level = level;
      keypoint->response = fabs (interpolated);
      return TRUE;
    }

  return FALSE;
}

static gint
compare_keypoint_response (gconstpointer first,
                           gconstpointer second)
{
  const BriskKeypoint *a = first;
  const BriskKeypoint *b = second;

  if (a->response < b->response)
    return 1;
  if (a->response > b->response)
    return -1;
  if (a->image_y < b->image_y)
    return -1;
  if (a->image_y > b->image_y)
    return 1;
  if (a->image_x < b->image_x)
    return -1;
  if (a->image_x > b->image_x)
    return 1;
  if (a->octave < b->octave)
    return -1;
  if (a->octave > b->octave)
    return 1;
  if (a->level < b->level)
    return -1;
  if (a->level > b->level)
    return 1;
  if (a->y < b->y)
    return -1;
  if (a->y > b->y)
    return 1;
  if (a->x < b->x)
    return -1;
  if (a->x > b->x)
    return 1;
  return 0;
}

static GArray *
detect_keypoints (BriskOctave octaves[BRISK_OCTAVES])
{
  static const guint level_borders[] = { 0, 7, 8, 10 };
  g_autoptr(GArray) candidates = g_array_new (FALSE, FALSE, sizeof (BriskKeypoint));
  GArray *selected = g_array_new (FALSE, FALSE, sizeof (BriskKeypoint));

  for (guint octave_index = 0; octave_index < BRISK_OCTAVES; octave_index++)
    {
      BriskOctave *octave = &octaves[octave_index];

      for (guint level = 1; level <= 3; level++)
        {
          const guint border = MAX (level_borders[level], BRISK_DESCRIPTOR_MARGIN);

          for (guint y = border; y + border < octave->height; y++)
            for (guint x = border; x + border < octave->width; x++)
              {
                const gfloat value = octave->dog[level][y * octave->width + x];
                BriskKeypoint keypoint = { 0 };

                if (fabs (value) < BRISK_DOG_PRETHRESHOLD_U8 ||
                    !is_strict_extremum (octave, level, x, y) ||
                    !refine_keypoint (octave, level, x, y, &keypoint))
                  continue;

                if (keypoint.x < BRISK_DESCRIPTOR_MARGIN ||
                    keypoint.y < BRISK_DESCRIPTOR_MARGIN ||
                    keypoint.x + BRISK_DESCRIPTOR_MARGIN >= octave->width ||
                    keypoint.y + BRISK_DESCRIPTOR_MARGIN >= octave->height)
                  continue;

                keypoint.octave = octave_index;
                keypoint.image_x = octave_index == 0 ? keypoint.x / 2.0 : keypoint.x;
                keypoint.image_y = octave_index == 0 ? keypoint.y / 2.0 : keypoint.y;
                g_array_append_val (candidates, keypoint);
              }
        }
    }

  g_array_sort (candidates, compare_keypoint_response);
  for (guint i = 0; i < candidates->len; i++)
    {
      const BriskKeypoint candidate = g_array_index (candidates, BriskKeypoint, i);
      gboolean duplicate = FALSE;

      for (guint j = 0; j < selected->len; j++)
        {
          const BriskKeypoint existing = g_array_index (selected, BriskKeypoint, j);
          const gdouble dx = candidate.image_x - existing.image_x;
          const gdouble dy = candidate.image_y - existing.image_y;

          if (dx * dx + dy * dy < 2.25)
            {
              duplicate = TRUE;
              break;
            }
        }

      if (!duplicate)
        g_array_append_val (selected, candidate);
      if (selected->len >= FTE3600_BRISK_MAX_FEATURES)
        break;
    }

  return selected;
}

static guint
orientation_peaks (const gfloat *image,
                   guint         width,
                   guint         height,
                   gfloat        center_x,
                   gfloat        center_y,
                   guint         level,
                   gdouble      *angles,
                   guint         maximum_angles)
{
  gdouble bins[BRISK_ORIENTATION_BINS] = { 0.0 };
  gdouble scratch[BRISK_ORIENTATION_BINS];
  const gint radius = level == 1 ? 9 : level == 2 ? 11 : 14;
  const gint rounded_x = round_half_away_from_zero (center_x);
  const gint rounded_y = round_half_away_from_zero (center_y);
  gdouble maximum = 0.0;
  guint n_angles = 0;

  for (gint offset_y = -radius; offset_y <= radius; offset_y++)
    for (gint offset_x = -radius; offset_x <= radius; offset_x++)
      {
        const gint x = rounded_x + offset_x;
        const gint y = rounded_y + offset_y;
        gdouble gx;
        gdouble gy;
        gdouble magnitude;
        gdouble angle;
        guint bin;

        if (x <= 0 || y <= 0 || x >= (gint) width - 1 || y >= (gint) height - 1)
          continue;

        gx = image[y * width + x + 1] - image[y * width + x - 1];
        gy = image[(y + 1) * width + x] - image[(y - 1) * width + x];
        magnitude = hypot (gx, gy);
        if (magnitude <= DBL_EPSILON)
          continue;
        angle = atan2 (gy, gx);
        bin = MIN ((guint) floor ((angle + BRISK_PI) *
                                  BRISK_ORIENTATION_BINS / BRISK_TWO_PI),
                   BRISK_ORIENTATION_BINS - 1);
        bins[bin] += magnitude;
      }

  for (guint pass = 0; pass < 2; pass++)
    {
      for (guint bin = 0; bin < BRISK_ORIENTATION_BINS; bin++)
        scratch[bin] = (bins[(bin + BRISK_ORIENTATION_BINS - 1) % BRISK_ORIENTATION_BINS] +
                        2.0 * bins[bin] + bins[(bin + 1) % BRISK_ORIENTATION_BINS]) / 4.0;
      memcpy (bins, scratch, sizeof (bins));
    }

  for (guint bin = 0; bin < BRISK_ORIENTATION_BINS; bin++)
    maximum = MAX (maximum, bins[bin]);
  if (maximum <= DBL_EPSILON)
    return 0;

  for (guint bin = 0; bin < BRISK_ORIENTATION_BINS && n_angles < maximum_angles; bin++)
    {
      const gdouble previous = bins[(bin + BRISK_ORIENTATION_BINS - 1) % BRISK_ORIENTATION_BINS];
      const gdouble current = bins[bin];
      const gdouble next = bins[(bin + 1) % BRISK_ORIENTATION_BINS];
      const gdouble denominator = previous - 2.0 * current + next;
      gdouble offset = 0.0;

      if (current < 0.8 * maximum || current <= previous || current <= next)
        continue;
      if (fabs (denominator) > DBL_EPSILON)
        offset = CLAMP (0.5 * (previous - next) / denominator, -0.5, 0.5);
      angles[n_angles++] = wrap_angle (-BRISK_PI +
                                       BRISK_TWO_PI * (bin + 0.5 + offset) /
                                       BRISK_ORIENTATION_BINS);
    }

  return n_angles;
}

static void
describe_float_image (const gfloat          *image,
                      guint                  width,
                      guint                  height,
                      gfloat                 center_x,
                      gfloat                 center_y,
                      gdouble                orientation,
                      Fte3600BriskFeature   *feature)
{
  gfloat samples[FTE3600_BRISK_PATTERN_POINTS] = { 0.0 };
  gboolean valid[FTE3600_BRISK_PATTERN_POINTS] = { FALSE };
  const gdouble cosine = cos (orientation);
  const gdouble sine = sin (orientation);

  memset (feature->descriptor, 0, sizeof (feature->descriptor));

  for (guint point = 0; point < FTE3600_BRISK_PATTERN_POINTS; point++)
    {
      gdouble pattern_x;
      gdouble pattern_y;
      gint sample_x;
      gint sample_y;

      get_pattern_point (point, &pattern_x, &pattern_y);
      sample_x = round_half_away_from_zero (center_x + cosine * pattern_x -
                                            sine * pattern_y);
      sample_y = round_half_away_from_zero (center_y + sine * pattern_x +
                                            cosine * pattern_y);
      if (sample_x < 0 || sample_y < 0 ||
          sample_x >= (gint) width || sample_y >= (gint) height)
        continue;
      samples[point] = image[sample_y * width + sample_x];
      valid[point] = TRUE;
    }

  for (guint bit = 0; bit < FTE3600_BRISK_DESCRIPTOR_BITS; bit++)
    {
      const guint first = descriptor_pairs[bit].first;
      const guint second = descriptor_pairs[bit].second;

      if (valid[first] && valid[second] && samples[second] < samples[first])
        feature->descriptor[bit >> 3] |= 1u << (bit & 7);
    }
}

Fte3600BriskStatus
fte3600_brisk_describe_at (const guint8         *image,
                           gsize                 length,
                           gfloat                x,
                           gfloat                y,
                           Fte3600BriskFeature  *feature)
{
  g_auto(BriskRoundingGuard) rounding_guard = { 0 };
  static const guint initial_boxes[] = { 3, 3, 3, 3 };
  static const guint wide_boxes[] = { 3, 3, 5 };
  g_autofree gfloat *source = NULL;
  g_autofree gfloat *base = NULL;
  g_autofree gfloat *level = NULL;
  g_autofree gfloat *next = NULL;
  gdouble angles[BRISK_ORIENTATION_BINS];
  guint n_angles;

  if (feature != NULL)
    memset (feature, 0, sizeof (*feature));
  if (!brisk_rounding_guard_enter (&rounding_guard))
    return FTE3600_BRISK_INVALID_ARGUMENT;
  if (image == NULL || feature == NULL || length != FTE3600_BRISK_IMAGE_SIZE ||
      !isfinite (x) || !isfinite (y) ||
      x < BRISK_DESCRIPTOR_MARGIN || y < BRISK_DESCRIPTOR_MARGIN ||
      x + BRISK_DESCRIPTOR_MARGIN >= FTE3600_BRISK_WIDTH ||
      y + BRISK_DESCRIPTOR_MARGIN >= FTE3600_BRISK_HEIGHT)
    return FTE3600_BRISK_INVALID_ARGUMENT;
  if (!image_has_contrast (image))
    return FTE3600_BRISK_LOW_CONTRAST;

  source = g_new (gfloat, FTE3600_BRISK_IMAGE_SIZE);
  base = g_new (gfloat, FTE3600_BRISK_IMAGE_SIZE);
  level = g_new (gfloat, FTE3600_BRISK_IMAGE_SIZE);
  next = g_new (gfloat, FTE3600_BRISK_IMAGE_SIZE);
  for (guint i = 0; i < FTE3600_BRISK_IMAGE_SIZE; i++)
    source[i] = image[i];
  apply_box_sequence (source, base, FTE3600_BRISK_WIDTH, FTE3600_BRISK_HEIGHT,
                      initial_boxes, G_N_ELEMENTS (initial_boxes));
  memcpy (level, base, FTE3600_BRISK_IMAGE_SIZE * sizeof (gfloat));
  for (guint i = 1; i <= 4; i++)
    {
      static const guint narrow_boxes[] = { 3, 3, 3 };
      const guint *boxes = i <= 2 ? narrow_boxes : wide_boxes;

      apply_box_sequence (level, next, FTE3600_BRISK_WIDTH, FTE3600_BRISK_HEIGHT,
                          boxes, G_N_ELEMENTS (narrow_boxes));
      {
        gfloat *temporary = level;
        level = next;
        next = temporary;
      }
    }

  n_angles = orientation_peaks (level, FTE3600_BRISK_WIDTH, FTE3600_BRISK_HEIGHT,
                                x, y, 2, angles, G_N_ELEMENTS (angles));
  if (n_angles == 0)
    return FTE3600_BRISK_LOW_CONTRAST;

  feature->x = x;
  feature->y = y;
  feature->orientation = angles[0];
  describe_float_image (level, FTE3600_BRISK_WIDTH, FTE3600_BRISK_HEIGHT,
                        x, y, angles[0], feature);
  return FTE3600_BRISK_OK;
}

Fte3600BriskStatus
fte3600_brisk_extract (const guint8            *image,
                       gsize                    length,
                       Fte3600BriskFeatureSet  *features)
{
  g_auto(BriskRoundingGuard) rounding_guard = { 0 };
  BriskOctave octaves[BRISK_OCTAVES];
  g_autoptr(GArray) keypoints = NULL;
  guint physical_features = 0;

  if (features != NULL)
    {
      memset (features, 0, sizeof (*features));
      features->extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
    }
  if (!brisk_rounding_guard_enter (&rounding_guard))
    return FTE3600_BRISK_INVALID_ARGUMENT;
  if (image == NULL || features == NULL || length != FTE3600_BRISK_IMAGE_SIZE)
    return FTE3600_BRISK_INVALID_ARGUMENT;
  if (!image_has_contrast (image))
    return FTE3600_BRISK_LOW_CONTRAST;

  build_scale_space (image, octaves);
  keypoints = detect_keypoints (octaves);
  for (guint i = 0; i < keypoints->len &&
                    features->n_features < FTE3600_BRISK_MAX_FEATURES; i++)
    {
      const BriskKeypoint keypoint = g_array_index (keypoints, BriskKeypoint, i);
      const BriskOctave *octave = &octaves[keypoint.octave];
      gdouble angles[BRISK_ORIENTATION_BINS];
      const guint n_angles = orientation_peaks (
        octave->gaussian[keypoint.level], octave->width, octave->height,
        keypoint.x, keypoint.y, keypoint.level, angles, G_N_ELEMENTS (angles));

      if (n_angles > 0)
        physical_features++;
      for (guint angle_index = 0; angle_index < n_angles &&
                                  features->n_features < FTE3600_BRISK_MAX_FEATURES;
           angle_index++)
        {
          Fte3600BriskFeature *feature =
            &features->features[features->n_features++];

          feature->x = keypoint.image_x;
          feature->y = keypoint.image_y;
          feature->orientation = angles[angle_index];
          /* Schema v1 intentionally estimates orientation at the detected
           * interval but samples every descriptor from octave Gaussian level
           * 4, matching the observed behavioral contract.  This asymmetry is
           * versioned and covered by the extractor golden vector. */
          describe_float_image (octave->gaussian[4], octave->width, octave->height,
                                keypoint.x, keypoint.y, angles[angle_index], feature);
        }
    }

  for (guint octave = 0; octave < BRISK_OCTAVES; octave++)
    octave_clear (&octaves[octave]);

  if (physical_features < FTE3600_BRISK_MIN_MUTUAL_MATCHES)
    return FTE3600_BRISK_INSUFFICIENT_FEATURES;
  return FTE3600_BRISK_OK;
}

static guint
hamming_distance (const guint8 *first,
                  const guint8 *second)
{
  static const guint8 popcount[16] = {
    0, 1, 1, 2, 1, 2, 2, 3,
    1, 2, 2, 3, 2, 3, 3, 4,
  };
  guint distance = 0;

  for (guint i = 0; i < FTE3600_BRISK_DESCRIPTOR_BYTES; i++)
    {
      const guint value = first[i] ^ second[i];

      distance += popcount[value & 0x0f] + popcount[value >> 4];
    }

  return distance;
}

gboolean
fte3600_brisk_validate_feature_set (const Fte3600BriskFeatureSet *features,
                                     guint                        *physical_count)
{
  g_auto(BriskRoundingGuard) rounding_guard = { 0 };
  gboolean assigned[FTE3600_BRISK_MAX_FEATURES] = { FALSE };
  guint queue[FTE3600_BRISK_MAX_FEATURES];
  guint n_physical = 0;

  if (physical_count != NULL)
    *physical_count = 0;
  if (!brisk_rounding_guard_enter (&rounding_guard))
    return FALSE;
  if (features == NULL ||
      features->extractor_schema_version != FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION ||
      features->n_features > FTE3600_BRISK_MAX_FEATURES)
    return FALSE;

  for (guint i = 0; i < features->n_features; i++)
    {
      const Fte3600BriskFeature *feature = &features->features[i];

      if (!isfinite (feature->x) || !isfinite (feature->y) ||
          !isfinite (feature->orientation) ||
          feature->x < 0.0 || feature->x >= FTE3600_BRISK_WIDTH ||
          feature->y < 0.0 || feature->y >= FTE3600_BRISK_HEIGHT ||
          feature->orientation < -FTE3600_BRISK_ORIENTATION_LIMIT ||
          feature->orientation > FTE3600_BRISK_ORIENTATION_LIMIT)
        return FALSE;
    }

  for (guint first = 0; first < features->n_features; first++)
    {
      guint head = 0;
      guint tail = 0;

      if (assigned[first])
        continue;

      assigned[first] = TRUE;
      queue[tail++] = first;
      n_physical++;
      while (head < tail)
        {
          const Fte3600BriskFeature *current =
            &features->features[queue[head++]];

          for (guint candidate = 0; candidate < features->n_features; candidate++)
            {
              const Fte3600BriskFeature *other;

              if (assigned[candidate])
                continue;
              other = &features->features[candidate];
              const gdouble dx = current->x - other->x;
              const gdouble dy = current->y - other->y;

              if (dx * dx + dy * dy < 2.25)
                {
                  assigned[candidate] = TRUE;
                  queue[tail++] = candidate;
                }
            }
        }
    }

  if (physical_count != NULL)
    *physical_count = n_physical;
  return TRUE;
}

static guint
physical_group_find (guint *parent,
                     guint  feature)
{
  guint root = feature;

  while (parent[root] != root)
    root = parent[root];
  while (parent[feature] != feature)
    {
      const guint next = parent[feature];

      parent[feature] = root;
      feature = next;
    }
  return root;
}

static void
build_physical_groups (const Fte3600BriskFeatureSet *features,
                       PhysicalGroups               *groups)
{
  guint parent[FTE3600_BRISK_MAX_FEATURES];
  guint root_group[FTE3600_BRISK_MAX_FEATURES];

  memset (groups, 0, sizeof (*groups));
  for (guint i = 0; i < features->n_features; i++)
    {
      parent[i] = i;
      root_group[i] = G_MAXUINT;
    }

  /* Multiple orientation peaks from one detector location are variants, not
   * independent evidence.  Connected components make grouping invariant to
   * feature ordering; a chain can only collapse votes, never create them. */
  for (guint i = 0; i < features->n_features; i++)
    for (guint j = i + 1; j < features->n_features; j++)
      {
        const gdouble dx = features->features[i].x - features->features[j].x;
        const gdouble dy = features->features[i].y - features->features[j].y;

        if (dx * dx + dy * dy < 2.25)
          {
            const guint first_root = physical_group_find (parent, i);
            const guint second_root = physical_group_find (parent, j);

            if (first_root < second_root)
              parent[second_root] = first_root;
            else if (second_root < first_root)
              parent[first_root] = second_root;
          }
      }

  for (guint i = 0; i < features->n_features; i++)
    {
      const guint root = physical_group_find (parent, i);

      if (root_group[root] == G_MAXUINT)
        root_group[root] = groups->n_groups++;
      groups->group_for_feature[i] = root_group[root];
    }
}

static guint
collect_mutual_correspondences (const Fte3600BriskFeatureSet *query,
                                const Fte3600BriskFeatureSet *reference,
                                Fte3600BriskCorrespondence   *correspondences)
{
  PhysicalGroups query_groups;
  PhysicalGroups reference_groups;
  GroupPairDistance *distances = NULL;
  guint query_best[FTE3600_BRISK_MAX_FEATURES];
  guint query_second[FTE3600_BRISK_MAX_FEATURES];
  guint query_index[FTE3600_BRISK_MAX_FEATURES];
  guint reference_best[FTE3600_BRISK_MAX_FEATURES];
  guint reference_second[FTE3600_BRISK_MAX_FEATURES];
  guint reference_index[FTE3600_BRISK_MAX_FEATURES];
  guint n_distances;
  guint count = 0;

  build_physical_groups (query, &query_groups);
  build_physical_groups (reference, &reference_groups);
  if (query_groups.n_groups == 0 || reference_groups.n_groups == 0)
    return 0;
  g_assert (query_groups.n_groups <= query->n_features);
  g_assert (reference_groups.n_groups <= reference->n_features);

  n_distances = query_groups.n_groups * reference_groups.n_groups;
  distances = g_new0 (GroupPairDistance, n_distances);
  for (guint i = 0; i < n_distances; i++)
    {
      distances[i].hamming = G_MAXUINT;
      distances[i].query_feature = G_MAXUINT;
      distances[i].reference_feature = G_MAXUINT;
    }

  /* A group-to-group distance is the best orientation variant pair.  Ties are
   * resolved by source indices for a stable schema-independent match order. */
  for (guint i = 0; i < query->n_features; i++)
    for (guint j = 0; j < reference->n_features; j++)
      {
        const guint query_group = query_groups.group_for_feature[i];
        const guint reference_group = reference_groups.group_for_feature[j];
        GroupPairDistance *pair =
          &distances[query_group * reference_groups.n_groups + reference_group];
        const guint distance = hamming_distance (query->features[i].descriptor,
                                                 reference->features[j].descriptor);

        if (distance < pair->hamming ||
            (distance == pair->hamming &&
             (i < pair->query_feature ||
              (i == pair->query_feature && j < pair->reference_feature))))
          {
            pair->hamming = distance;
            pair->query_feature = i;
            pair->reference_feature = j;
          }
      }

  for (guint i = 0; i < query_groups.n_groups; i++)
    {
      query_best[i] = G_MAXUINT;
      query_second[i] = G_MAXUINT;
      query_index[i] = G_MAXUINT;
      for (guint j = 0; j < reference_groups.n_groups; j++)
        {
          const guint distance =
            distances[i * reference_groups.n_groups + j].hamming;

          if (distance < query_best[i])
            {
              query_second[i] = query_best[i];
              query_best[i] = distance;
              query_index[i] = j;
            }
          else if (distance < query_second[i])
            query_second[i] = distance;
        }
    }
  for (guint j = 0; j < reference_groups.n_groups; j++)
    {
      reference_best[j] = G_MAXUINT;
      reference_second[j] = G_MAXUINT;
      reference_index[j] = G_MAXUINT;
      for (guint i = 0; i < query_groups.n_groups; i++)
        {
          const guint distance =
            distances[i * reference_groups.n_groups + j].hamming;

          if (distance < reference_best[j])
            {
              reference_second[j] = reference_best[j];
              reference_best[j] = distance;
              reference_index[j] = i;
            }
          else if (distance < reference_second[j])
            reference_second[j] = distance;
        }
    }

  for (guint i = 0; i < query_groups.n_groups; i++)
    {
      const guint j = query_index[i];
      const GroupPairDistance *pair;

      if (j == G_MAXUINT || reference_index[j] != i ||
          query_best[i] > FTE3600_BRISK_MAX_HAMMING ||
          query_second[i] == G_MAXUINT || reference_second[j] == G_MAXUINT ||
          query_second[i] < query_best[i] ||
          reference_second[j] < reference_best[j] ||
          query_second[i] - query_best[i] < FTE3600_BRISK_MIN_HAMMING_MARGIN ||
          reference_second[j] - reference_best[j] < FTE3600_BRISK_MIN_HAMMING_MARGIN ||
          100u * query_best[i] > FTE3600_BRISK_RATIO_PERCENT * query_second[i] ||
          100u * reference_best[j] > FTE3600_BRISK_RATIO_PERCENT * reference_second[j])
        continue;

      pair = &distances[i * reference_groups.n_groups + j];
      if (pair->query_feature >= query->n_features ||
          pair->reference_feature >= reference->n_features ||
          pair->hamming > G_MAXUINT16)
        continue;
      correspondences[count].query_index = pair->query_feature;
      correspondences[count].reference_index = pair->reference_feature;
      correspondences[count].hamming = pair->hamming;
      count++;
    }

  g_free (distances);
  return count;
}

static RigidModel
model_from_orientation (const Fte3600BriskFeature *query,
                        const Fte3600BriskFeature *reference)
{
  RigidModel model;
  gdouble cosine;
  gdouble sine;

  model.angle = wrap_angle (reference->orientation - query->orientation);
  cosine = cos (model.angle);
  sine = sin (model.angle);
  model.translate_x = reference->x - (cosine * query->x - sine * query->y);
  model.translate_y = reference->y - (sine * query->x + cosine * query->y);
  return model;
}

static gboolean
model_from_pair (const Fte3600BriskFeature *query_a,
                 const Fte3600BriskFeature *query_b,
                 const Fte3600BriskFeature *reference_a,
                 const Fte3600BriskFeature *reference_b,
                 RigidModel                *model)
{
  const gdouble query_dx = query_b->x - query_a->x;
  const gdouble query_dy = query_b->y - query_a->y;
  const gdouble reference_dx = reference_b->x - reference_a->x;
  const gdouble reference_dy = reference_b->y - reference_a->y;
  const gdouble query_distance = hypot (query_dx, query_dy);
  const gdouble reference_distance = hypot (reference_dx, reference_dy);
  gdouble cosine;
  gdouble sine;

  if (query_distance < 4.0 || fabs (query_distance - reference_distance) >= 2.0)
    return FALSE;

  model->angle = wrap_angle (atan2 (reference_dy, reference_dx) -
                             atan2 (query_dy, query_dx));
  if (fabs (wrap_angle ((reference_a->orientation - query_a->orientation) -
                        model->angle)) >= BRISK_ORIENTATION_LIMIT ||
      fabs (wrap_angle ((reference_b->orientation - query_b->orientation) -
                        model->angle)) >= BRISK_ORIENTATION_LIMIT)
    return FALSE;

  cosine = cos (model->angle);
  sine = sin (model->angle);
  model->translate_x = reference_a->x -
                       (cosine * query_a->x - sine * query_a->y);
  model->translate_y = reference_a->y -
                       (sine * query_a->x + cosine * query_a->y);
  return TRUE;
}

static guint
evaluate_model (const Fte3600BriskFeatureSet *query,
                const Fte3600BriskFeatureSet *reference,
                const Fte3600BriskCorrespondence *correspondences,
                guint                       n_correspondences,
                const RigidModel           *model,
                gboolean                   *inlier_mask,
                gdouble                    *square_error)
{
  const gdouble cosine = cos (model->angle);
  const gdouble sine = sin (model->angle);
  guint inliers = 0;
  gdouble error = 0.0;

  for (guint i = 0; i < n_correspondences; i++)
    {
      const Fte3600BriskFeature *a =
        &query->features[correspondences[i].query_index];
      const Fte3600BriskFeature *b =
        &reference->features[correspondences[i].reference_index];
      const gdouble predicted_x = cosine * a->x - sine * a->y + model->translate_x;
      const gdouble predicted_y = sine * a->x + cosine * a->y + model->translate_y;
      const gdouble dx = b->x - predicted_x;
      const gdouble dy = b->y - predicted_y;
      const gdouble orientation_error =
        fabs (wrap_angle ((b->orientation - a->orientation) - model->angle));
      const gboolean inlier = fabs (dx) < BRISK_AXIS_INLIER_LIMIT &&
                              fabs (dy) < BRISK_AXIS_INLIER_LIMIT &&
                              orientation_error < BRISK_ORIENTATION_LIMIT;

      if (inlier_mask != NULL)
        inlier_mask[i] = inlier;
      if (inlier)
        {
          inliers++;
          error += dx * dx + dy * dy;
        }
    }

  if (square_error != NULL)
    *square_error = error;
  return inliers;
}

static gboolean
refit_model (const Fte3600BriskFeatureSet *query,
             const Fte3600BriskFeatureSet *reference,
             const Fte3600BriskCorrespondence *correspondences,
             guint                       n_correspondences,
             const gboolean             *inlier_mask,
             RigidModel                 *model)
{
  gdouble query_center_x = 0.0;
  gdouble query_center_y = 0.0;
  gdouble reference_center_x = 0.0;
  gdouble reference_center_y = 0.0;
  gdouble dot = 0.0;
  gdouble cross = 0.0;
  guint count = 0;

  for (guint i = 0; i < n_correspondences; i++)
    if (inlier_mask[i])
      {
        const Fte3600BriskFeature *a =
          &query->features[correspondences[i].query_index];
        const Fte3600BriskFeature *b =
          &reference->features[correspondences[i].reference_index];

        query_center_x += a->x;
        query_center_y += a->y;
        reference_center_x += b->x;
        reference_center_y += b->y;
        count++;
      }
  if (count < 5)
    return FALSE;

  query_center_x /= count;
  query_center_y /= count;
  reference_center_x /= count;
  reference_center_y /= count;
  for (guint i = 0; i < n_correspondences; i++)
    if (inlier_mask[i])
      {
        const Fte3600BriskFeature *a =
          &query->features[correspondences[i].query_index];
        const Fte3600BriskFeature *b =
          &reference->features[correspondences[i].reference_index];
        const gdouble ax = a->x - query_center_x;
        const gdouble ay = a->y - query_center_y;
        const gdouble bx = b->x - reference_center_x;
        const gdouble by = b->y - reference_center_y;

        dot += ax * bx + ay * by;
        cross += ax * by - ay * bx;
      }
  if (fabs (dot) + fabs (cross) < DBL_EPSILON)
    return FALSE;

  model->angle = atan2 (cross, dot);
  model->translate_x = reference_center_x -
                       (cos (model->angle) * query_center_x -
                        sin (model->angle) * query_center_y);
  model->translate_y = reference_center_y -
                       (sin (model->angle) * query_center_x +
                        cos (model->angle) * query_center_y);
  return TRUE;
}

static gint
compare_double (gconstpointer first,
                gconstpointer second)
{
  const gdouble a = *(const gdouble *) first;
  const gdouble b = *(const gdouble *) second;

  return a < b ? -1 : a > b ? 1 : 0;
}

static gboolean
models_are_competing (const RigidModel *first,
                      const RigidModel *second)
{
  const gdouble center_x = (FTE3600_BRISK_WIDTH - 1) / 2.0;
  const gdouble center_y = (FTE3600_BRISK_HEIGHT - 1) / 2.0;
  const gdouble first_x = cos (first->angle) * center_x -
                          sin (first->angle) * center_y + first->translate_x;
  const gdouble first_y = sin (first->angle) * center_x +
                          cos (first->angle) * center_y + first->translate_y;
  const gdouble second_x = cos (second->angle) * center_x -
                           sin (second->angle) * center_y + second->translate_x;
  const gdouble second_y = sin (second->angle) * center_x +
                           cos (second->angle) * center_y + second->translate_y;

  return fabs (wrap_angle (first->angle - second->angle)) >= 3.0 * BRISK_PI / 180.0 ||
         hypot (first_x - second_x, first_y - second_y) >= 2.4;
}

static void
covariance_geometry (const Fte3600BriskFeatureSet    *features,
                     const Fte3600BriskCorrespondence *correspondences,
                     guint                            n_correspondences,
                     const gboolean                  *inlier_mask,
                     gboolean                         use_query,
                     gdouble                         *minimum_variance,
                     gdouble                         *anisotropy)
{
  gdouble center_x = 0.0;
  gdouble center_y = 0.0;
  gdouble xx = 0.0;
  gdouble xy = 0.0;
  gdouble yy = 0.0;
  guint count = 0;

  *minimum_variance = 0.0;
  *anisotropy = 0.0;
  for (guint i = 0; i < n_correspondences; i++)
    if (inlier_mask[i])
      {
        const guint feature_index = use_query ? correspondences[i].query_index :
                                               correspondences[i].reference_index;
        const Fte3600BriskFeature *feature = &features->features[feature_index];

        center_x += feature->x;
        center_y += feature->y;
        count++;
      }
  if (count < 2)
    return;

  center_x /= count;
  center_y /= count;
  for (guint i = 0; i < n_correspondences; i++)
    if (inlier_mask[i])
      {
        const guint feature_index = use_query ? correspondences[i].query_index :
                                               correspondences[i].reference_index;
        const Fte3600BriskFeature *feature = &features->features[feature_index];
        const gdouble dx = feature->x - center_x;
        const gdouble dy = feature->y - center_y;

        xx += dx * dx;
        xy += dx * dy;
        yy += dy * dy;
      }
  xx /= count;
  xy /= count;
  yy /= count;
  {
    const gdouble trace = xx + yy;
    const gdouble discriminant = sqrt (MAX (0.0,
      (xx - yy) * (xx - yy) + 4.0 * xy * xy));
    const gdouble maximum_variance = 0.5 * (trace + discriminant);

    *minimum_variance = MAX (0.0, 0.5 * (trace - discriminant));
    if (maximum_variance > DBL_EPSILON)
      *anisotropy = *minimum_variance / maximum_variance;
  }
}

static gboolean
match_result_valid (const Fte3600BriskMatchResult *result)
{
  if (result == NULL ||
      result->mutual_matches > FTE3600_BRISK_MAX_FEATURES ||
      result->inliers > result->mutual_matches ||
      result->competing_inliers > result->mutual_matches ||
      result->occupied_cells > 16 || result->occupied_quadrants > 4 ||
      (result->mutual_matches == 0 && result->inlier_ratio != 0.0) ||
      (result->mutual_matches > 0 &&
       fabs (result->inlier_ratio -
             (gdouble) result->inliers / result->mutual_matches) > 1e-9))
    return FALSE;

  return isfinite (result->inlier_ratio) &&
         isfinite (result->mean_hamming) &&
         isfinite (result->rms_error) &&
         isfinite (result->median_error) &&
         isfinite (result->angle) &&
         isfinite (result->translate_x) &&
         isfinite (result->translate_y) &&
         isfinite (result->x_span) && isfinite (result->y_span) &&
         isfinite (result->query_min_variance) &&
         isfinite (result->query_anisotropy) &&
         isfinite (result->reference_min_variance) &&
         isfinite (result->reference_anisotropy) &&
         result->inlier_ratio >= 0.0 && result->inlier_ratio <= 1.0 &&
         result->mean_hamming >= 0.0 && result->mean_hamming <= 256.0 &&
         result->rms_error >= 0.0 && result->median_error >= 0.0 &&
         fabs (result->angle) <= BRISK_PI &&
         result->x_span >= 0.0 && result->x_span < FTE3600_BRISK_WIDTH &&
         result->y_span >= 0.0 && result->y_span < FTE3600_BRISK_HEIGHT &&
         result->query_min_variance >= 0.0 &&
         result->reference_min_variance >= 0.0 &&
         result->query_anisotropy >= 0.0 && result->query_anisotropy <= 1.0 &&
         result->reference_anisotropy >= 0.0 &&
         result->reference_anisotropy <= 1.0;
}

gboolean
fte3600_brisk_result_meets_diagnostic_policy (const Fte3600BriskMatchResult *result)
{
  g_auto(BriskRoundingGuard) rounding_guard = { 0 };

  if (!brisk_rounding_guard_enter (&rounding_guard))
    return FALSE;
  if (!match_result_valid (result) || result->competing_inliers > result->inliers)
    return FALSE;

  return result->mutual_matches >= FTE3600_BRISK_MIN_MUTUAL_MATCHES &&
         result->inliers >= FTE3600_BRISK_MIN_INLIERS &&
         result->inlier_ratio >= 0.20 &&
         result->inliers - result->competing_inliers >= 2 &&
         result->median_error < 1.25 &&
         result->rms_error < 1.40 &&
         result->mean_hamming <= 60.0 &&
         result->occupied_quadrants >= 2 &&
         result->occupied_cells >= 3 &&
         result->x_span >= 8.0 && result->y_span >= 10.0 &&
         result->query_min_variance >= 4.0 &&
         result->reference_min_variance >= 4.0 &&
         result->query_anisotropy >= 0.08 &&
         result->reference_anisotropy >= 0.08;
}

gboolean
fte3600_brisk_result_meets_authentication_policy (const Fte3600BriskMatchResult *result)
{
  g_auto(BriskRoundingGuard) rounding_guard = { 0 };

  if (!brisk_rounding_guard_enter (&rounding_guard))
    return FALSE;
#if FTE3600_ENABLE_PERSONAL_AUTH
  return fte3600_brisk_result_meets_diagnostic_policy (result);
#else
  (void) result;
  return FALSE;
#endif
}

Fte3600BriskStatus
fte3600_brisk_match (const Fte3600BriskFeatureSet *query,
                     const Fte3600BriskFeatureSet *reference,
                     Fte3600BriskMatchResult      *result)
{
  g_auto(BriskRoundingGuard) rounding_guard = { 0 };
  Fte3600BriskCorrespondence correspondences[FTE3600_BRISK_MAX_FEATURES];
  g_autofree RigidModel *hypotheses = NULL;
  gboolean inlier_mask[FTE3600_BRISK_MAX_FEATURES] = { FALSE };
  gboolean trial_mask[FTE3600_BRISK_MAX_FEATURES] = { FALSE };
  gdouble residuals[FTE3600_BRISK_MAX_FEATURES];
  guint n_correspondences;
  guint n_hypotheses = 0;
  guint maximum_hypotheses;
  guint best_inliers = 0;
  gdouble best_error = G_MAXDOUBLE;
  RigidModel best_model = { 0 };

  if (result != NULL)
    memset (result, 0, sizeof (*result));
  if (!brisk_rounding_guard_enter (&rounding_guard))
    return FTE3600_BRISK_INVALID_ARGUMENT;
  if (result == NULL ||
      !fte3600_brisk_validate_feature_set (query, NULL) ||
      !fte3600_brisk_validate_feature_set (reference, NULL))
    return FTE3600_BRISK_INVALID_ARGUMENT;
  if (query->n_features < FTE3600_BRISK_MIN_MUTUAL_MATCHES ||
      reference->n_features < FTE3600_BRISK_MIN_MUTUAL_MATCHES)
    return FTE3600_BRISK_INSUFFICIENT_FEATURES;

  n_correspondences = collect_mutual_correspondences (query, reference,
                                                       correspondences);
  result->mutual_matches = n_correspondences;
  if (n_correspondences < 5)
    return FTE3600_BRISK_NO_CONSENSUS;

  maximum_hypotheses = n_correspondences +
                       n_correspondences * (n_correspondences - 1) / 2;
  hypotheses = g_new (RigidModel, maximum_hypotheses);
  for (guint i = 0; i < n_correspondences; i++)
    {
      const Fte3600BriskFeature *a =
        &query->features[correspondences[i].query_index];
      const Fte3600BriskFeature *b =
        &reference->features[correspondences[i].reference_index];

      hypotheses[n_hypotheses++] = model_from_orientation (a, b);
    }
  for (guint i = 0; i < n_correspondences; i++)
    for (guint j = i + 1; j < n_correspondences; j++)
      {
        const Fte3600BriskFeature *query_a =
          &query->features[correspondences[i].query_index];
        const Fte3600BriskFeature *query_b =
          &query->features[correspondences[j].query_index];
        const Fte3600BriskFeature *reference_a =
          &reference->features[correspondences[i].reference_index];
        const Fte3600BriskFeature *reference_b =
          &reference->features[correspondences[j].reference_index];

        if (model_from_pair (query_a, query_b, reference_a, reference_b,
                             &hypotheses[n_hypotheses]))
          n_hypotheses++;
      }

  for (guint hypothesis = 0; hypothesis < n_hypotheses; hypothesis++)
    {
      gdouble error;
      const guint inliers = evaluate_model (query, reference, correspondences,
                                             n_correspondences,
                                             &hypotheses[hypothesis], NULL, &error);

      if (inliers > best_inliers || (inliers == best_inliers && error < best_error))
        {
          best_inliers = inliers;
          best_error = error;
          best_model = hypotheses[hypothesis];
        }
    }
  if (best_inliers < 5)
    return FTE3600_BRISK_NO_CONSENSUS;

  for (guint iteration = 0; iteration < 2; iteration++)
    {
      evaluate_model (query, reference, correspondences, n_correspondences,
                      &best_model, inlier_mask, NULL);
      if (!refit_model (query, reference, correspondences, n_correspondences,
                        inlier_mask, &best_model))
        return FTE3600_BRISK_NO_CONSENSUS;
    }
  best_inliers = evaluate_model (query, reference, correspondences,
                                  n_correspondences, &best_model,
                                  inlier_mask, &best_error);

  for (guint hypothesis = 0; hypothesis < n_hypotheses; hypothesis++)
    if (models_are_competing (&best_model, &hypotheses[hypothesis]))
      result->competing_inliers = MAX (
        result->competing_inliers,
        evaluate_model (query, reference, correspondences, n_correspondences,
                        &hypotheses[hypothesis], trial_mask, NULL));

  result->inliers = best_inliers;
  result->inlier_ratio = (gdouble) best_inliers / n_correspondences;
  result->angle = best_model.angle;
  result->translate_x = best_model.translate_x;
  result->translate_y = best_model.translate_y;
  if (best_inliers > 0)
    {
      const gdouble cosine = cos (best_model.angle);
      const gdouble sine = sin (best_model.angle);
      gdouble query_center_x = 0.0;
      gdouble query_center_y = 0.0;
      gdouble minimum_x = G_MAXDOUBLE;
      gdouble minimum_y = G_MAXDOUBLE;
      gdouble maximum_x = -G_MAXDOUBLE;
      gdouble maximum_y = -G_MAXDOUBLE;
      gdouble hamming_sum = 0.0;
      guint residual_index = 0;
      guint cell_mask = 0;
      guint quadrant_mask = 0;

      for (guint i = 0; i < n_correspondences; i++)
        if (inlier_mask[i])
          {
            const Fte3600BriskFeature *a =
              &query->features[correspondences[i].query_index];

            query_center_x += a->x;
            query_center_y += a->y;
          }
      query_center_x /= best_inliers;
      query_center_y /= best_inliers;

      for (guint i = 0; i < n_correspondences; i++)
        if (inlier_mask[i])
          {
            const Fte3600BriskFeature *a =
              &query->features[correspondences[i].query_index];
            const Fte3600BriskFeature *b =
              &reference->features[correspondences[i].reference_index];
            const gdouble predicted_x = cosine * a->x - sine * a->y +
                                         best_model.translate_x;
            const gdouble predicted_y = sine * a->x + cosine * a->y +
                                         best_model.translate_y;
            const gdouble dx = b->x - predicted_x;
            const gdouble dy = b->y - predicted_y;
            const guint cell_x = MIN ((guint) (a->x / 16.0), 3);
            const guint cell_y = MIN ((guint) (a->y / 20.0), 3);
            const guint quadrant = (a->x >= query_center_x ? 1 : 0) |
                                   (a->y >= query_center_y ? 2 : 0);

            residuals[residual_index++] = hypot (dx, dy);
            hamming_sum += correspondences[i].hamming;
            minimum_x = MIN (minimum_x, a->x);
            maximum_x = MAX (maximum_x, a->x);
            minimum_y = MIN (minimum_y, a->y);
            maximum_y = MAX (maximum_y, a->y);
            cell_mask |= 1u << (cell_y * 4 + cell_x);
            quadrant_mask |= 1u << quadrant;
          }

      qsort (residuals, best_inliers, sizeof (gdouble), compare_double);
      result->median_error = best_inliers & 1 ? residuals[best_inliers / 2] :
        0.5 * (residuals[best_inliers / 2 - 1] + residuals[best_inliers / 2]);
      result->rms_error = sqrt (best_error / best_inliers);
      result->mean_hamming = hamming_sum / best_inliers;
      result->x_span = maximum_x - minimum_x;
      result->y_span = maximum_y - minimum_y;
      result->occupied_cells = __builtin_popcount (cell_mask);
      result->occupied_quadrants = __builtin_popcount (quadrant_mask);
    }

  covariance_geometry (query, correspondences, n_correspondences, inlier_mask,
                       TRUE, &result->query_min_variance,
                       &result->query_anisotropy);
  covariance_geometry (reference, correspondences, n_correspondences, inlier_mask,
                       FALSE, &result->reference_min_variance,
                       &result->reference_anisotropy);
  result->diagnostic_policy_passed =
    fte3600_brisk_result_meets_diagnostic_policy (result);
  result->authentication_accepted =
    fte3600_brisk_result_meets_authentication_policy (result);
  return FTE3600_BRISK_OK;
}
