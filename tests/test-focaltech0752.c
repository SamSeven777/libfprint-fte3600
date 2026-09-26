/*
 * Unit tests for FocalTech FT9362 (2808:0752) driver components
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "../libfprint/drivers/focaltech0752-img.h"
#include "../libfprint/drivers/fte3600-brisk.h"
#include "../libfprint/drivers/fte3600-template.h"

static void
generate_synthetic_raw (uint8_t *raw_buffer, double angle_offset)
{
  int16_t *pixels;

  memset (raw_buffer, 0, RAW_IMAGE_SIZE);
  raw_buffer[0] = 0x02; /* header */

  pixels = (int16_t *) (raw_buffer + FT_RAW_HEADER);

  /* Synthesize fingerprint-like sinusoids in active area */
  for (int y = 0; y < FT9362_ACTIVE_HEIGHT; y++)
    {
      for (int x = 0; x < FT9362_ACTIVE_WIDTH; x++)
        {
          double u = x * cos (0.3 + angle_offset) + y * sin (0.3 + angle_offset);
          double wave = sin (u * 0.7) * 400.0;
          double noise = ((x ^ y) & 7) * 10.0;
          int16_t val = (int16_t) (2048 + wave + noise);

          pixels[3040 + y * FT9362_ACTIVE_WIDTH + x] = val;
        }
    }
}

static void
test_focaltech0752_raw_processing (void)
{
  uint8_t raw_buffer[RAW_IMAGE_SIZE];
  guint8 brisk_image[FTE3600_BRISK_IMAGE_SIZE];

  generate_synthetic_raw (raw_buffer, 0.0);
  focaltech0752_process_raw_to_brisk (raw_buffer, brisk_image);

  /* Check border padding: top 2 rows, bottom 2 rows, left 12 cols, right 12 cols must be 128 */
  g_assert_cmpint (brisk_image[0], ==, 128);
  g_assert_cmpint (brisk_image[FTE3600_BRISK_WIDTH - 1], ==, 128);

  /* Check that active area contains stretched dynamic range */
  gboolean found_dark = FALSE;
  gboolean found_bright = FALSE;

  for (int i = 0; i < FTE3600_BRISK_IMAGE_SIZE; i++)
    {
      if (brisk_image[i] < 60)
        found_dark = TRUE;
      if (brisk_image[i] > 190)
        found_bright = TRUE;
    }

  g_assert_true (found_dark);
  g_assert_true (found_bright);
}

static void
test_focaltech0752_brisk_extraction (void)
{
  uint8_t raw_buffer[RAW_IMAGE_SIZE];
  guint8 brisk_image[FTE3600_BRISK_IMAGE_SIZE];
  Fte3600BriskFeatureSet features;
  Fte3600BriskStatus bstatus;

  generate_synthetic_raw (raw_buffer, 0.0);
  focaltech0752_process_raw_to_brisk (raw_buffer, brisk_image);

  bstatus = fpi_fte3600_brisk_extract (brisk_image, sizeof (brisk_image), &features);
  g_assert_cmpint (bstatus, ==, FTE3600_BRISK_OK);
  g_assert_cmpuint (features.n_features, >, 0);
  g_assert_cmpuint (features.n_features, <=, FTE3600_BRISK_MAX_FEATURES);
}

static guint32
xorshift32 (guint32 *state)
{
  guint32 value = *state;
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  *state = value;
  return value;
}

static void
fill_descriptor (guint8 *descriptor, guint feature, guint sample)
{
  guint32 state = 0x9e3779b9u ^ (feature + 1) * 0x45d9f3bu;
  for (guint i = 0; i < FTE3600_BRISK_DESCRIPTOR_BYTES; i++)
    descriptor[i] = xorshift32 (&state) >> 24;
  descriptor[0] ^= sample;
}

static void
make_synthetic_feature_set (Fte3600BriskFeatureSet *features, guint sample)
{
  memset (features, 0, sizeof (*features));
  features->extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
  features->n_features = 24;
  for (guint i = 0; i < 24; i++)
    {
      Fte3600BriskFeature *f = &features->features[i];
      f->x = 8.0f + 12.0f * (i % 4);
      f->y = 10.0f + 10.0f * (i / 4);
      f->orientation = 0.0f;
      fill_descriptor (f->descriptor, i, sample);
    }
}

static void
test_focaltech0752_template_roundtrip (void)
{
  g_autoptr(Fte3600Template) templ = fpi_fte3600_template_new ();
  Fte3600TemplateStatus tstatus;

  for (int stage = 0; stage < FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES; stage++)
    {
      Fte3600BriskFeatureSet features;
      Fte3600BriskMatchResult match_res;

      make_synthetic_feature_set (&features, stage);
      tstatus = fpi_fte3600_template_add_features (templ, &features, &match_res);
      g_assert_true (tstatus == FTE3600_TEMPLATE_OK || tstatus == FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
    }

  g_assert_true (fpi_fte3600_template_is_ready (templ));

  g_autoptr(GBytes) wire = NULL;
  tstatus = fpi_fte3600_template_encode (templ, &wire);
  g_assert_cmpint (tstatus, ==, FTE3600_TEMPLATE_OK);
  g_assert_nonnull (wire);

  g_autoptr(Fte3600Template) decoded = NULL;
  tstatus = fpi_fte3600_template_decode (wire, FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &decoded);
  g_assert_cmpint (tstatus, ==, FTE3600_TEMPLATE_OK);
  g_assert_nonnull (decoded);
  g_assert_true (fpi_fte3600_template_is_ready (decoded));
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/focaltech0752/raw_processing", test_focaltech0752_raw_processing);
  g_test_add_func ("/focaltech0752/brisk_extraction", test_focaltech0752_brisk_extraction);
  g_test_add_func ("/focaltech0752/template_roundtrip", test_focaltech0752_template_roundtrip);

  return g_test_run ();
}
