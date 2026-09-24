/*
 * Synthetic timing and correctness smoke checks for FTE3600 matcher engines.
 * These generated images are not a population biometric evaluation.
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdio.h>
#include <math.h>
#include <glib.h>
#include "../libfprint/drivers/fte3600-template.h"

#if !FTE3600_ENABLE_PERSONAL_AUTH
#error "The authentication benchmark requires the explicit personal-auth build option"
#endif

#define BENCH_TEST_ROUNDS 200

static void
generate_synthetic_fingerprint (guint8 *image, guint seed)
{
  static const struct {
    gdouble x, y, sigma, amplitude;
  } spots[] = {
    { 17, 18, 1.5,  58 }, { 29, 17, 2.2, -54 },
    { 43, 19, 3.1,  61 }, { 51, 29, 1.8, -48 },
    { 18, 32, 2.7, -61 }, { 32, 31, 1.4,  52 },
    { 43, 39, 2.4, -57 }, { 17, 47, 1.9,  55 },
    { 31, 49, 3.2,  63 }, { 49, 52, 1.5, -53 },
    { 20, 63, 2.3, -58 }, { 36, 63, 1.7,  57 },
    { 48, 66, 2.8,  50 }, { 25, 42, 2.1, -45 },
  };

  gdouble fx = (seed == 0) ? 0.29 : 0.41;
  gdouble fy = (seed == 0) ? 0.17 : 0.23;

  for (guint y = 0; y < FTE3600_BRISK_HEIGHT; y++)
    {
      for (guint x = 0; x < FTE3600_BRISK_WIDTH; x++)
        {
          gdouble v = 126.0 + 18.0 * sin (fx * x + fy * y) + 12.0 * cos (0.13 * x - 0.23 * y);
          if (seed == 0)
            {
              for (guint i = 0; i < G_N_ELEMENTS (spots); i++)
                {
                  gdouble dx = x - spots[i].x;
                  gdouble dy = y - spots[i].y;
                  v += spots[i].amplitude * exp (-(dx * dx + dy * dy) / (2.0 * spots[i].sigma * spots[i].sigma));
                }
            }
          image[y * FTE3600_BRISK_WIDTH + x] = (guint8) CLAMP (floor (v + 0.5), 2, 253);
        }
    }
}

static void
warp_fingerprint (const guint8 *src, guint8 *dst, gdouble angle, gdouble tx, gdouble ty)
{
  const gdouble c = cos (angle), s = sin (angle);
  const gdouble cx = (FTE3600_BRISK_WIDTH - 1) / 2.0;
  const gdouble cy = (FTE3600_BRISK_HEIGHT - 1) / 2.0;

  for (guint y = 0; y < FTE3600_BRISK_HEIGHT; y++)
    {
      for (guint x = 0; x < FTE3600_BRISK_WIDTH; x++)
        {
          gdouble dx = x - cx - tx;
          gdouble dy = y - cy - ty;
          gdouble sx = c * dx + s * dy + cx;
          gdouble sy = -s * dx + c * dy + cy;
          gint x0 = (gint) floor (sx);
          gint y0 = (gint) floor (sy);
          gint val = 126;
          if (x0 >= 0 && y0 >= 0 && x0 + 1 < FTE3600_BRISK_WIDTH && y0 + 1 < FTE3600_BRISK_HEIGHT)
            {
              gdouble fx = sx - x0, fy = sy - y0;
              gdouble top = (1.0 - fx) * src[y0 * FTE3600_BRISK_WIDTH + x0] + fx * src[y0 * FTE3600_BRISK_WIDTH + x0 + 1];
              gdouble bot = (1.0 - fx) * src[(y0 + 1) * FTE3600_BRISK_WIDTH + x0] + fx * src[(y0 + 1) * FTE3600_BRISK_WIDTH + x0 + 1];
              val = (gint) floor ((1.0 - fy) * top + fy * bot + 0.5);
            }
          dst[y * FTE3600_BRISK_WIDTH + x] = (guint8) CLAMP (val, 0, 255);
        }
    }
}

static gboolean
extract_both (const guint8 *image,
              Fte3600BriskFeatureSet *brisk,
              Fte3600IpaFeatureSet *ipa)
{
  if (fpi_fte3600_brisk_extract (image, FTE3600_BRISK_IMAGE_SIZE, brisk) != FTE3600_BRISK_OK ||
      fpi_fte3600_ipa_extract (image, FTE3600_BRISK_IMAGE_SIZE, ipa) != FTE3600_IPA_OK)
    {
      fprintf (stderr, "Synthetic fixture extraction failed\n");
      return FALSE;
    }
  return TRUE;
}

int
main (void)
{
  guint8 images[4][FTE3600_BRISK_IMAGE_SIZE];
  const gchar *conditions[] = { "self", "translation", "rotation", "different synthetic pattern" };
  Fte3600BriskFeatureSet brisk[4];
  Fte3600IpaFeatureSet ipa[4];
  g_autoptr (Fte3600Template) templ = fpi_fte3600_template_new ();
  g_autoptr (Fte3600Template) decoded = NULL;
  g_autoptr (GBytes) wire = NULL;
  const Fte3600EngineMode modes[] = {
    FTE3600_ENGINE_MODE_BRISK_ONLY,
#if FTE3600_ENABLE_IPA_AUTH
    FTE3600_ENGINE_MODE_IPA_ONLY,
    FTE3600_ENGINE_MODE_DUAL_FUSION,
#endif
  };
  const gchar *mode_names[] = {
    "BRISK",
#if FTE3600_ENABLE_IPA_AUTH
    "IPA", "DUAL",
#endif
  };
  guint checks = 0, failures = 0, decisions = 0, accepted = 0;
  gint64 started;

  generate_synthetic_fingerprint (images[0], 0);
  warp_fingerprint (images[0], images[1], 0.0, 2.5, -2.0);
  warp_fingerprint (images[0], images[2], 0.20, 0.0, 0.0);
  generate_synthetic_fingerprint (images[3], 1);
  for (guint i = 0; i < G_N_ELEMENTS (images); i++)
    if (!extract_both (images[i], &brisk[i], &ipa[i]))
      return 1;

  puts ("Synthetic FTE3600 benchmark; not FAR/FRR or a real enrollment evaluation.");
  printf ("Authentication options: personal=%d, IPA=%d\n",
          FTE3600_ENABLE_PERSONAL_AUTH, FTE3600_ENABLE_IPA_AUTH);
  started = g_get_monotonic_time ();
  for (guint i = 0; i < BENCH_TEST_ROUNDS; i++)
    if (fpi_fte3600_brisk_extract (images[0], sizeof (images[0]), &brisk[0]) != FTE3600_BRISK_OK)
      return 1;
  printf ("BRISK extract: %.2f us/call (%u iterations)\n",
          (gdouble) (g_get_monotonic_time () - started) / BENCH_TEST_ROUNDS, BENCH_TEST_ROUNDS);
  started = g_get_monotonic_time ();
  for (guint i = 0; i < BENCH_TEST_ROUNDS; i++)
    if (fpi_fte3600_ipa_extract (images[0], sizeof (images[0]), &ipa[0]) != FTE3600_IPA_OK)
      return 1;
  printf ("IPA extract (diagnostic processing): %.2f us/call (%u iterations)\n",
          (gdouble) (g_get_monotonic_time () - started) / BENCH_TEST_ROUNDS, BENCH_TEST_ROUNDS);

  for (guint sample = 0; sample < FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES; sample++)
    {
      guint8 image[FTE3600_BRISK_IMAGE_SIZE];
      Fte3600BriskFeatureSet b;
      Fte3600IpaFeatureSet p;
      const gint offset = (gint) sample - 3;
      const Fte3600TemplateStatus expected =
        sample + 1 == FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES ?
        FTE3600_TEMPLATE_OK : FTE3600_TEMPLATE_NEED_MORE_SAMPLES;
      Fte3600TemplateStatus status;

      warp_fingerprint (images[0], image, 0.015 * offset, 0.4 * offset, 0.3 * offset);
      if (!extract_both (image, &b, &p))
        return 1;
      status = fpi_fte3600_template_add_dual_features (templ, &b, &p, NULL);
      if (status != expected)
        {
          fprintf (stderr, "Synthetic gallery stage %u: status %d, expected %d\n",
                   sample + 1, status, expected);
          return 1;
        }
    }
  if (fpi_fte3600_template_encode (templ, &wire) != FTE3600_TEMPLATE_OK ||
      wire == NULL ||
      fpi_fte3600_template_decode (wire, FTE3600_TEMPLATE_LOAD_AUTHENTICATION,
                                  &decoded) != FTE3600_TEMPLATE_OK ||
      decoded == NULL)
    {
      fprintf (stderr, "Synthetic gallery encode/decode failed\n");
      return 1;
    }
  printf ("Encoded gallery: %" G_GSIZE_FORMAT " bytes\n", g_bytes_get_size (wire));

  for (guint mode = 0; mode < G_N_ELEMENTS (modes); mode++)
    {
      Fte3600TemplateCompareResult result = { 0 };

      started = g_get_monotonic_time ();
      for (guint i = 0; i < BENCH_TEST_ROUNDS; i++)
        if (fpi_fte3600_template_compare_with_mode (
              decoded, &brisk[1], &ipa[1], FTE3600_TEMPLATE_LOAD_AUTHENTICATION,
              modes[mode], &result) != FTE3600_TEMPLATE_OK)
          {
            fprintf (stderr, "%s gallery comparison failed\n", mode_names[mode]);
            return 1;
          }
      printf ("%s gallery: %.2f us/call (%u iterations); no latency limit asserted\n",
              mode_names[mode],
              (gdouble) (g_get_monotonic_time () - started) / BENCH_TEST_ROUNDS,
              BENCH_TEST_ROUNDS);
      for (guint scenario = 0; scenario < G_N_ELEMENTS (images); scenario++)
        {
          Fte3600TemplateStatus status = fpi_fte3600_template_compare_with_mode (
            decoded, &brisk[scenario], &ipa[scenario],
            FTE3600_TEMPLATE_LOAD_AUTHENTICATION, modes[mode], &result);
          if (status != FTE3600_TEMPLATE_OK)
            {
              fprintf (stderr, "%s/%s failed with status %d\n",
                       mode_names[mode], conditions[scenario], status);
              return 1;
            }
          decisions++;
          accepted += result.authentication_accepted;
          printf ("%s/%s: %s\n", mode_names[mode], conditions[scenario],
                  result.authentication_accepted ? "accepted" : "rejected");
          /* Self-match and this one distinct fixture are correctness smoke
           * expectations; transformed probes are descriptive observations. */
          if (scenario == 0 || scenario == 3)
            {
              checks++;
              if (result.authentication_accepted != (scenario == 0))
                failures++;
            }
        }
    }

  printf ("Observed synthetic decisions: %u accepted of %u; "
          "smoke expectations: %u checked, %u failed.\n",
          accepted, decisions, checks, failures);
  puts ("No population FAR/FRR, rotation guarantee, or allocation count was measured.");
  return failures == 0 ? 0 : 1;
}
