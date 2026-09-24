/*
 * Unit tests for the FocalTech FT9361 / FTE3600 2D Invariant Point Attention (2D-IPA) matcher.
 * All fixtures are generated mathematical patterns, not real biometric data.
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <math.h>
#include <string.h>
#include <glib.h>

#include "../libfprint/drivers/fte3600-ipa.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

static void
assert_policy_result (const Fte3600IpaMatchResult *result, gboolean expected)
{
  g_assert_cmpint (result->diagnostic_policy_passed, ==, expected);
  g_assert_cmpint (fpi_fte3600_ipa_result_meets_policy (result), ==, expected);
  g_assert_cmpint (result->authentication_accepted, ==,
                   expected && FTE3600_ENABLE_IPA_AUTH);
}

static void
make_fingerprint_pattern (guint8 *image, guint seed_variant)
{
  static const struct
  {
    gdouble x;
    gdouble y;
    gdouble sigma;
    gdouble amplitude;
  } spots_a[] = {
    { 17, 18, 1.5,  58 }, { 29, 17, 2.2, -54 },
    { 43, 19, 3.1,  61 }, { 51, 29, 1.8, -48 },
    { 18, 32, 2.7, -61 }, { 32, 31, 1.4,  52 },
    { 43, 39, 2.4, -57 }, { 17, 47, 1.9,  55 },
    { 31, 49, 3.2,  63 }, { 49, 52, 1.5, -53 },
    { 20, 63, 2.3, -58 }, { 36, 63, 1.7,  57 },
    { 48, 66, 2.8,  50 },
  };

  static const struct
  {
    gdouble x;
    gdouble y;
    gdouble sigma;
    gdouble amplitude;
  } spots_b[] = {
    { 22, 22, 2.0, -60 }, { 38, 24, 1.8,  55 },
    { 25, 38, 2.5,  62 }, { 45, 42, 2.2, -50 },
    { 19, 54, 1.6, -56 }, { 35, 56, 3.0,  58 },
    { 46, 60, 2.1, -52 }, { 28, 68, 1.9,  60 },
  };

  gdouble freq_x = (seed_variant == 0) ? 0.29 : 0.41;
  gdouble freq_y = (seed_variant == 0) ? 0.17 : 0.23;

  for (guint y = 0; y < FTE3600_IPA_HEIGHT; y++)
    {
      for (guint x = 0; x < FTE3600_IPA_WIDTH; x++)
        {
          gdouble value = 126.0 + 15.0 * sin (freq_x * x + freq_y * y) +
                          10.0 * cos (0.13 * x - 0.23 * y);

          if (seed_variant == 0)
            {
              for (guint i = 0; i < G_N_ELEMENTS (spots_a); i++)
                {
                  const gdouble dx = x - spots_a[i].x;
                  const gdouble dy = y - spots_a[i].y;
                  value += spots_a[i].amplitude *
                           exp (-(dx * dx + dy * dy) / (2.0 * spots_a[i].sigma * spots_a[i].sigma));
                }
            }
          else
            {
              for (guint i = 0; i < G_N_ELEMENTS (spots_b); i++)
                {
                  const gdouble dx = x - spots_b[i].x;
                  const gdouble dy = y - spots_b[i].y;
                  value += spots_b[i].amplitude *
                           exp (-(dx * dx + dy * dy) / (2.0 * spots_b[i].sigma * spots_b[i].sigma));
                }
            }

          image[y * FTE3600_IPA_WIDTH + x] = (guint8) CLAMP (floor (value + 0.5), 2, 253);
        }
    }
}

static void
warp_image (const guint8 *source,
            guint8       *destination,
            gdouble       angle,
            gdouble       translate_x,
            gdouble       translate_y)
{
  const gdouble cosine = cos (angle);
  const gdouble sine = sin (angle);
  const gdouble center_x = (FTE3600_IPA_WIDTH - 1) / 2.0;
  const gdouble center_y = (FTE3600_IPA_HEIGHT - 1) / 2.0;

  for (guint y = 0; y < FTE3600_IPA_HEIGHT; y++)
    {
      for (guint x = 0; x < FTE3600_IPA_WIDTH; x++)
        {
          const gdouble destination_x = x - center_x - translate_x;
          const gdouble destination_y = y - center_y - translate_y;
          const gdouble source_x = cosine * destination_x + sine * destination_y + center_x;
          const gdouble source_y = -sine * destination_x + cosine * destination_y + center_y;
          const gint x0 = (gint) floor (source_x);
          const gint y0 = (gint) floor (source_y);
          gint value = 126;

          if (x0 >= 0 && y0 >= 0 && x0 + 1 < FTE3600_IPA_WIDTH && y0 + 1 < FTE3600_IPA_HEIGHT)
            {
              const gdouble fx = source_x - x0;
              const gdouble fy = source_y - y0;
              const gdouble top = (1.0 - fx) * source[y0 * FTE3600_IPA_WIDTH + x0] +
                                  fx * source[y0 * FTE3600_IPA_WIDTH + x0 + 1];
              const gdouble bottom = (1.0 - fx) * source[(y0 + 1) * FTE3600_IPA_WIDTH + x0] +
                                     fx * source[(y0 + 1) * FTE3600_IPA_WIDTH + x0 + 1];
              value = (gint) floor ((1.0 - fy) * top + fy * bottom + 0.5);
            }
          destination[y * FTE3600_IPA_WIDTH + x] = (guint8) CLAMP (value, 0, 255);
        }
    }
}

static void
test_ipa_parameter_validation (void)
{
  guint8 image[FTE3600_IPA_IMAGE_SIZE];
  Fte3600IpaFeatureSet features = { 0 };
  Fte3600IpaMatchResult result;

  memset (image, 128, sizeof (image));

  /* Null pointer checks */
  g_assert_cmpint (fpi_fte3600_ipa_extract (NULL, sizeof (image), &features),
                   ==, FTE3600_IPA_ERR_PARAM);
  g_assert_cmpint (fpi_fte3600_ipa_extract (image, sizeof (image), NULL),
                   ==, FTE3600_IPA_ERR_PARAM);
  g_assert_cmpint (fpi_fte3600_ipa_extract (image, sizeof (image) - 1, &features),
                   ==, FTE3600_IPA_ERR_PARAM);

  g_assert_cmpint (fpi_fte3600_ipa_match (NULL, &features, &result),
                   ==, FTE3600_IPA_ERR_PARAM);
  g_assert_cmpint (fpi_fte3600_ipa_match (&features, NULL, &result),
                   ==, FTE3600_IPA_ERR_PARAM);
  g_assert_cmpint (fpi_fte3600_ipa_match (&features, &features, NULL),
                   ==, FTE3600_IPA_ERR_PARAM);

  g_assert_false (fpi_fte3600_ipa_result_meets_policy (NULL));
}

static void
test_ipa_extract_deterministic (void)
{
  guint8 image[FTE3600_IPA_IMAGE_SIZE];
  Fte3600IpaFeatureSet feat1, feat2;

  make_fingerprint_pattern (image, 0);

  g_assert_cmpint (fpi_fte3600_ipa_extract (image, sizeof (image), &feat1),
                   ==, FTE3600_IPA_OK);
  g_assert_cmpuint (feat1.n_minutiae, >=, 5);
  g_assert_cmpuint (feat1.n_minutiae, <=, FTE3600_IPA_MAX_MINUTIAE);

  g_assert_cmpint (fpi_fte3600_ipa_extract (image, sizeof (image), &feat2),
                   ==, FTE3600_IPA_OK);
  g_assert_cmpuint (feat1.n_minutiae, ==, feat2.n_minutiae);

  /* Bit-exact determinism */
  for (guint i = 0; i < feat1.n_minutiae; i++)
    {
      g_assert_cmpfloat_with_epsilon (feat1.minutiae[i].x, feat2.minutiae[i].x, 1e-6);
      g_assert_cmpfloat_with_epsilon (feat1.minutiae[i].y, feat2.minutiae[i].y, 1e-6);
      g_assert_cmpfloat_with_epsilon (feat1.minutiae[i].theta, feat2.minutiae[i].theta, 1e-6);
      for (guint d = 0; d < FTE3600_IPA_DESC_DIM; d++)
        g_assert_cmpfloat_with_epsilon (feat1.minutiae[i].desc[d],
                                        feat2.minutiae[i].desc[d], 1e-6);
    }
}

static void
test_ipa_self_match (void)
{
  guint8 image[FTE3600_IPA_IMAGE_SIZE];
  Fte3600IpaFeatureSet feat;
  Fte3600IpaMatchResult result;

  make_fingerprint_pattern (image, 0);

  g_assert_cmpint (fpi_fte3600_ipa_extract (image, sizeof (image), &feat),
                   ==, FTE3600_IPA_OK);
  g_assert_cmpint (fpi_fte3600_ipa_match (&feat, &feat, &result),
                   ==, FTE3600_IPA_OK);

  g_test_message ("Self-match: pairs=%u inliers=%u score=%.4f accepted=%d",
                  result.n_matched_pairs, result.n_supported_inliers,
                  result.consensus_score, result.authentication_accepted);

  g_assert_cmpuint (result.n_matched_pairs, ==, feat.n_minutiae);
  g_assert_cmpuint (result.n_supported_inliers, >=, FTE3600_IPA_POLICY_MIN_INLIERS);
  g_assert_cmpfloat (result.consensus_score, >=, FTE3600_IPA_POLICY_MIN_SCORE);
  assert_policy_result (&result, TRUE);
  g_assert_true (fpi_fte3600_ipa_result_meets_policy (&result));
}

static void
test_ipa_translation_invariance (void)
{
  guint8 source[FTE3600_IPA_IMAGE_SIZE];
  guint8 translated[FTE3600_IPA_IMAGE_SIZE];
  Fte3600IpaFeatureSet query, ref;
  Fte3600IpaMatchResult result;

  make_fingerprint_pattern (source, 0);
  warp_image (source, translated, 0.0, 2.0, -2.0);

  g_assert_cmpint (fpi_fte3600_ipa_extract (source, sizeof (source), &query),
                   ==, FTE3600_IPA_OK);
  g_assert_cmpint (fpi_fte3600_ipa_extract (translated, sizeof (translated), &ref),
                   ==, FTE3600_IPA_OK);

  g_assert_cmpint (fpi_fte3600_ipa_match (&query, &ref, &result),
                   ==, FTE3600_IPA_OK);

  g_test_message ("Translation (+2,-2): pairs=%u inliers=%u score=%.4f accepted=%d",
                  result.n_matched_pairs, result.n_supported_inliers,
                  result.consensus_score, result.authentication_accepted);

  g_assert_cmpuint (result.n_supported_inliers, >=, FTE3600_IPA_POLICY_MIN_INLIERS);
  g_assert_cmpfloat (result.consensus_score, >=, FTE3600_IPA_POLICY_MIN_SCORE);
  assert_policy_result (&result, TRUE);
}

static void
test_ipa_rotation_invariance (void)
{
  guint8 source[FTE3600_IPA_IMAGE_SIZE];
  Fte3600IpaFeatureSet query;

  make_fingerprint_pattern (source, 0);
  g_assert_cmpint (fpi_fte3600_ipa_extract (source, sizeof (source), &query),
                   ==, FTE3600_IPA_OK);

  static const gdouble test_angles[] = {
    0.15,  /* ~8.6 deg */
    0.35,  /* ~20.0 deg */
    -0.25, /* ~-14.3 deg */
  };

  for (guint a = 0; a < G_N_ELEMENTS (test_angles); a++)
    {
      guint8 rotated[FTE3600_IPA_IMAGE_SIZE];
      Fte3600IpaFeatureSet ref;
      Fte3600IpaMatchResult result;
      gdouble angle = test_angles[a];

      warp_image (source, rotated, angle, 0.0, 0.0);
      g_assert_cmpint (fpi_fte3600_ipa_extract (rotated, sizeof (rotated), &ref),
                       ==, FTE3600_IPA_OK);

      g_assert_cmpint (fpi_fte3600_ipa_match (&query, &ref, &result),
                       ==, FTE3600_IPA_OK);

      g_test_message ("Rotation (angle=%.2f rad): pairs=%u inliers=%u score=%.4f accepted=%d",
                      angle, result.n_matched_pairs, result.n_supported_inliers,
                      result.consensus_score, result.authentication_accepted);

      g_assert_cmpuint (result.n_supported_inliers, >=, FTE3600_IPA_POLICY_MIN_INLIERS);
      g_assert_cmpfloat (result.consensus_score, >=, FTE3600_IPA_POLICY_MIN_SCORE);
      assert_policy_result (&result, TRUE);
    }
}

static void
test_ipa_impostor_rejection (void)
{
  guint8 image_a[FTE3600_IPA_IMAGE_SIZE];
  guint8 image_b[FTE3600_IPA_IMAGE_SIZE];
  Fte3600IpaFeatureSet feat_a, feat_b;
  Fte3600IpaMatchResult result;

  make_fingerprint_pattern (image_a, 0);
  make_fingerprint_pattern (image_b, 1);

  g_assert_cmpint (fpi_fte3600_ipa_extract (image_a, sizeof (image_a), &feat_a),
                   ==, FTE3600_IPA_OK);
  g_assert_cmpint (fpi_fte3600_ipa_extract (image_b, sizeof (image_b), &feat_b),
                   ==, FTE3600_IPA_OK);

  g_assert_cmpint (fpi_fte3600_ipa_match (&feat_a, &feat_b, &result),
                   ==, FTE3600_IPA_OK);

  g_test_message ("Impostor (A vs B): pairs=%u inliers=%u score=%.4f accepted=%d",
                  result.n_matched_pairs, result.n_supported_inliers,
                  result.consensus_score, result.authentication_accepted);

  /* One synthetic nonmatching pair is a regression check, not a FAR study. */
  assert_policy_result (&result, FALSE);
  g_assert_false (fpi_fte3600_ipa_result_meets_policy (&result));
}

static void
test_ipa_execution_speed (void)
{
  guint8 image[FTE3600_IPA_IMAGE_SIZE];
  Fte3600IpaFeatureSet feat1, feat2;
  Fte3600IpaMatchResult result;
  const guint iterations = 50;

  make_fingerprint_pattern (image, 0);

  gint64 t0 = g_get_monotonic_time ();
  for (guint i = 0; i < iterations; i++)
    fpi_fte3600_ipa_extract (image, sizeof (image), &feat1);
  gint64 t1 = g_get_monotonic_time ();

  fpi_fte3600_ipa_extract (image, sizeof (image), &feat2);

  gint64 t2 = g_get_monotonic_time ();
  for (guint i = 0; i < iterations; i++)
    fpi_fte3600_ipa_match (&feat1, &feat2, &result);
  gint64 t3 = g_get_monotonic_time ();

  gdouble extract_us = (gdouble) (t1 - t0) / iterations;
  gdouble match_us = (gdouble) (t3 - t2) / iterations;

  g_test_message ("Performance: 2D-IPA extract = %.2f us (%.3f ms), match = %.2f us (%.3f ms)",
                  extract_us, extract_us / 1000.0, match_us, match_us / 1000.0);

  /* Timing is informational: slow, instrumented and loaded systems must
   * still be able to run correctness tests. */
}

static void
test_projection_orthonormal (void)
{
  for (guint row = 0; row < FTE3600_IPA_DESC_DIM; row++)
    for (guint other = 0; other < FTE3600_IPA_DESC_DIM; other++)
      {
        gdouble dot = 0.0;
        for (guint col = 0; col < FTE3600_IPA_DESC_DIM; col++)
          dot += fpi_fte3600_ipa_projection_coefficient (row, col) *
                 fpi_fte3600_ipa_projection_coefficient (other, col);
        g_assert_cmpfloat_with_epsilon (dot, row == other ? 1.0 : 0.0, 0.00001);
      }
}

static void
test_rigid_feature_rotation (void)
{
  static const gfloat coordinates[6][2] = {
    { 21, 30 }, { 32, 27 }, { 42, 33 }, { 22, 46 }, { 32, 52 }, { 43, 45 }
  };
  static const gfloat angles[] = { 0.6f, -0.75f, 2.0f, -2.2f };
  Fte3600IpaFeatureSet query = { 0 };

  query.extractor_schema_version = FTE3600_IPA_EXTRACTOR_SCHEMA_VERSION;
  query.n_minutiae = G_N_ELEMENTS (coordinates);
  for (guint i = 0; i < query.n_minutiae; i++)
    {
      query.minutiae[i].x = coordinates[i][0];
      query.minutiae[i].y = coordinates[i][1];
      query.minutiae[i].theta = 0.1f * i;
      query.minutiae[i].desc[i] = 1.0f;
    }
  g_assert_true (fpi_fte3600_ipa_validate_feature_set (&query));
  for (guint a = 0; a < G_N_ELEMENTS (angles); a++)
    {
      Fte3600IpaFeatureSet reference = query;
      const gfloat angle = angles[a];
      Fte3600IpaMatchResult result;

      for (guint i = 0; i < query.n_minutiae; i++)
        {
          const gfloat x = query.minutiae[i].x - 32.0f;
          const gfloat y = query.minutiae[i].y - 40.0f;
          reference.minutiae[i].x = cosf (angle) * x - sinf (angle) * y + 32.0f;
          reference.minutiae[i].y = sinf (angle) * x + cosf (angle) * y + 40.0f;
          reference.minutiae[i].theta = query.minutiae[i].theta + angle;
          if (i & 1)
            reference.minutiae[i].theta += (gfloat) M_PI;
          reference.minutiae[i].theta = atan2f (sinf (reference.minutiae[i].theta),
                                                cosf (reference.minutiae[i].theta));
        }
      g_assert_cmpint (fpi_fte3600_ipa_match (&query, &reference, &result), ==, FTE3600_IPA_OK);
      g_assert_cmpuint (result.n_supported_inliers, ==, query.n_minutiae);
      assert_policy_result (&result, TRUE);
      g_assert_cmpint (fpi_fte3600_ipa_match (&reference, &query, &result), ==, FTE3600_IPA_OK);
      assert_policy_result (&result, TRUE);
    }
}

typedef struct
{
  guint8               image[FTE3600_IPA_IMAGE_SIZE];
  Fte3600IpaFeatureSet expected;
} ExtractThreadData;

static gpointer
extract_thread (gpointer user_data)
{
  const ExtractThreadData *data = user_data;

  for (guint i = 0; i < 30; i++)
    {
      Fte3600IpaFeatureSet actual;
      g_assert_cmpint (fpi_fte3600_ipa_extract (data->image, sizeof (data->image), &actual),
                       ==, FTE3600_IPA_OK);
      g_assert_cmpmem (&actual, sizeof (actual), &data->expected, sizeof (data->expected));
    }
  return NULL;
}

static void
test_parallel_extract (void)
{
  ExtractThreadData first, second;

  make_fingerprint_pattern (first.image, 0);
  make_fingerprint_pattern (second.image, 1);
  g_assert_cmpint (fpi_fte3600_ipa_extract (first.image, sizeof (first.image), &first.expected),
                   ==, FTE3600_IPA_OK);
  g_assert_cmpint (fpi_fte3600_ipa_extract (second.image, sizeof (second.image), &second.expected),
                   ==, FTE3600_IPA_OK);
  GThread *a = g_thread_new ("ipa-first", extract_thread, &first);
  GThread *b = g_thread_new ("ipa-second", extract_thread, &second);
  g_thread_join (a);
  g_thread_join (b);
}

static void
test_feature_validation (void)
{
  guint8 image[FTE3600_IPA_IMAGE_SIZE];
  Fte3600IpaFeatureSet features;
  Fte3600IpaMatchResult result;

  make_fingerprint_pattern (image, 0);
  g_assert_cmpint (fpi_fte3600_ipa_extract (image, sizeof (image), &features), ==, FTE3600_IPA_OK);
  g_assert_true (fpi_fte3600_ipa_validate_feature_set (&features));
  Fte3600IpaFeatureSet invalid = features;
  invalid.n_minutiae = FTE3600_IPA_MAX_MINUTIAE + 1;
  g_assert_false (fpi_fte3600_ipa_validate_feature_set (&invalid));
  g_assert_cmpint (fpi_fte3600_ipa_match (&invalid, &features, &result), ==, FTE3600_IPA_ERR_PARAM);
  g_assert_false (result.authentication_accepted);
  invalid = features;
  invalid.minutiae[0].x = -1.0f;
  g_assert_false (fpi_fte3600_ipa_validate_feature_set (&invalid));
  invalid = features;
  invalid.extractor_schema_version++;
  g_assert_false (fpi_fte3600_ipa_validate_feature_set (&invalid));
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/fte3600-ipa/parameter-validation", test_ipa_parameter_validation);
  g_test_add_func ("/fte3600-ipa/extract-deterministic", test_ipa_extract_deterministic);
  g_test_add_func ("/fte3600-ipa/self-match", test_ipa_self_match);
  g_test_add_func ("/fte3600-ipa/translation-invariance", test_ipa_translation_invariance);
  g_test_add_func ("/fte3600-ipa/rotation-invariance", test_ipa_rotation_invariance);
  g_test_add_func ("/fte3600-ipa/impostor-rejection", test_ipa_impostor_rejection);
  g_test_add_func ("/fte3600-ipa/execution-speed", test_ipa_execution_speed);
  g_test_add_func ("/fte3600-ipa/projection-orthonormal", test_projection_orthonormal);
  g_test_add_func ("/fte3600-ipa/rigid-feature-rotation", test_rigid_feature_rotation);
  g_test_add_func ("/fte3600-ipa/parallel-extract", test_parallel_extract);
  g_test_add_func ("/fte3600-ipa/feature-validation", test_feature_validation);

  return g_test_run ();
}
