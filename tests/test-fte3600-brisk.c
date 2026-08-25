/*
 * Unit tests for the FT9361 clean-room BRISK-style matcher prototype.
 * All fixtures are generated mathematical patterns, not biometric data.
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <fenv.h>
#include <math.h>
#include <string.h>

#include <glib.h>

#include "../libfprint/drivers/fte3600-brisk.h"

#define TEST_PI 3.14159265358979323846

static guint32
test_xorshift32 (guint32 *state)
{
  guint32 value = *state;

  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  *state = value;
  return value;
}

static void
assert_authentication_policy_result (const Fte3600BriskMatchResult *result)
{
#if FTE3600_ENABLE_PERSONAL_AUTH
  g_assert_cmpint (result->authentication_accepted, ==,
                   result->diagnostic_policy_passed);
  g_assert_cmpint (
      fte3600_brisk_result_meets_authentication_policy (result), ==,
      result->diagnostic_policy_passed);
#else
  g_assert_false (result->authentication_accepted);
  g_assert_false (
      fte3600_brisk_result_meets_authentication_policy (result));
#endif
}

static void
make_visual_pattern (guint8 *image)
{
  static const struct {
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
fill_descriptor (guint8  descriptor[FTE3600_BRISK_DESCRIPTOR_BYTES],
                 guint32 seed)
{
  guint32 state = seed;

  for (guint i = 0; i < FTE3600_BRISK_DESCRIPTOR_BYTES; i++)
    descriptor[i] = test_xorshift32 (&state) >> 24;
}

static gchar *
feature_set_checksum (const Fte3600BriskFeatureSet *features)
{
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
  guint32 value = GUINT32_TO_LE (features->extractor_schema_version);

  g_checksum_update (checksum, (const guchar *) &value, sizeof (value));
  value = GUINT32_TO_LE (features->n_features);
  g_checksum_update (checksum, (const guchar *) &value, sizeof (value));
  for (guint i = 0; i < features->n_features; i++)
    {
      const Fte3600BriskFeature *feature = &features->features[i];
      guint32 bits;

      memcpy (&bits, &feature->x, sizeof (bits));
      bits = GUINT32_TO_LE (bits);
      g_checksum_update (checksum, (const guchar *) &bits, sizeof (bits));
      memcpy (&bits, &feature->y, sizeof (bits));
      bits = GUINT32_TO_LE (bits);
      g_checksum_update (checksum, (const guchar *) &bits, sizeof (bits));
      memcpy (&bits, &feature->orientation, sizeof (bits));
      bits = GUINT32_TO_LE (bits);
      g_checksum_update (checksum, (const guchar *) &bits, sizeof (bits));
      g_checksum_update (checksum, feature->descriptor,
                         sizeof (feature->descriptor));
    }

  return g_strdup (g_checksum_get_string (checksum));
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

static void
warp_pattern (const guint8 *source,
              guint8       *destination,
              gdouble       angle,
              gdouble       translate_x,
              gdouble       translate_y)
{
  const gdouble cosine = cos (angle);
  const gdouble sine = sin (angle);
  const gdouble center_x = (FTE3600_BRISK_WIDTH - 1) / 2.0;
  const gdouble center_y = (FTE3600_BRISK_HEIGHT - 1) / 2.0;

  for (guint y = 0; y < FTE3600_BRISK_HEIGHT; y++)
    for (guint x = 0; x < FTE3600_BRISK_WIDTH; x++)
      {
        const gdouble destination_x = x - center_x - translate_x;
        const gdouble destination_y = y - center_y - translate_y;
        const gdouble source_x = cosine * destination_x + sine * destination_y +
                                 center_x;
        const gdouble source_y = -sine * destination_x + cosine * destination_y +
                                 center_y;
        const gint x0 = floor (source_x);
        const gint y0 = floor (source_y);
        gint value = 126;

        if (x0 >= 0 && y0 >= 0 && x0 + 1 < FTE3600_BRISK_WIDTH &&
            y0 + 1 < FTE3600_BRISK_HEIGHT)
          {
            const gdouble fx = source_x - x0;
            const gdouble fy = source_y - y0;
            const gdouble top = (1.0 - fx) * source[y0 * FTE3600_BRISK_WIDTH + x0] +
                                fx * source[y0 * FTE3600_BRISK_WIDTH + x0 + 1];
            const gdouble bottom =
              (1.0 - fx) * source[(y0 + 1) * FTE3600_BRISK_WIDTH + x0] +
              fx * source[(y0 + 1) * FTE3600_BRISK_WIDTH + x0 + 1];

            value = floor ((1.0 - fy) * top + fy * bottom + 0.5);
          }
        value += ((13 * x + 29 * y) % 3) - 1;
        destination[y * FTE3600_BRISK_WIDTH + x] = CLAMP (value, 0, 255);
      }
}

static void
initialize_rigid_fixture (Fte3600BriskFeatureSet *query,
                          Fte3600BriskFeatureSet *reference,
                          gboolean                 clustered)
{
  const gdouble angle = 6.0 * TEST_PI / 180.0;
  const gdouble cosine = cos (angle);
  const gdouble sine = sin (angle);

  memset (query, 0, sizeof (*query));
  memset (reference, 0, sizeof (*reference));
  query->extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
  reference->extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
  query->n_features = 16;
  reference->n_features = 20;

  for (guint i = 0; i < query->n_features; i++)
    {
      Fte3600BriskFeature *a = &query->features[i];
      Fte3600BriskFeature *b = &reference->features[i];
      const guint column = i % 4;
      const guint row = i / 4;

      if (clustered)
        {
          a->x = 27.0 + 2.0 * column;
          a->y = 36.0 + 2.0 * row;
        }
      else
        {
          a->x = 12.0 + 12.0 * column;
          a->y = 14.0 + 15.0 * row;
        }
      a->orientation = -0.55 + 0.061 * i;
      fill_descriptor (a->descriptor, 0x6f123bb5u + 0x9e3779b9u * i);

      b->x = cosine * a->x - sine * a->y + 3.0 + 0.08 * ((gint) (i % 3) - 1);
      b->y = sine * a->x + cosine * a->y - 1.0 + 0.07 * ((gint) (i % 5) - 2);
      b->orientation = a->orientation + angle + 0.002 * ((gint) (i % 3) - 1);
      memcpy (b->descriptor, a->descriptor, sizeof (b->descriptor));
      for (guint bit = 0; bit < 4; bit++)
        {
          const guint index = (17 * i + 47 * bit) % FTE3600_BRISK_DESCRIPTOR_BITS;

          b->descriptor[index >> 3] ^= 1u << (index & 7);
        }
    }

  for (guint i = query->n_features; i < reference->n_features; i++)
    {
      Fte3600BriskFeature *feature = &reference->features[i];

      feature->x = 8.0 + 11.0 * (i - query->n_features);
      feature->y = 72.0 - 9.0 * (i - query->n_features);
      feature->orientation = -0.4 + 0.1 * i;
      fill_descriptor (feature->descriptor, 0x18a74f21u + 0x85ebca6bu * i);
    }
}

static void
test_pattern_and_pairs (void)
{
  gboolean used[FTE3600_BRISK_PATTERN_POINTS][FTE3600_BRISK_PATTERN_POINTS] = { { FALSE } };
  guint point_usage[FTE3600_BRISK_PATTERN_POINTS] = { 0 };
  guint8 serialized_pairs[FTE3600_BRISK_DESCRIPTOR_BITS * 2];
  guint cross_ring = 0;
  guint minimum_usage = G_MAXUINT;
  guint maximum_usage = 0;
  gfloat x;
  gfloat y;

  g_assert_cmpuint (sizeof (Fte3600BriskFeature), ==, 44);
  g_assert_cmpuint (FTE3600_BRISK_DESCRIPTOR_VERSION, ==, 1);
  g_assert_cmpuint (FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION, ==, 1);
  g_assert_cmpuint (FTE3600_BRISK_DIAGNOSTIC_POLICY_VERSION, ==, 1);
  g_assert_cmpuint (FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION, ==,
                    FTE3600_ENABLE_PERSONAL_AUTH ? 2 : 0);
  g_assert_cmpuint (FTE3600_BRISK_PAIR_SEED, ==, 0x46544231u);
  g_assert_cmpuint (FTE3600_BRISK_THRESHOLDS_CALIBRATED, ==, 0);

  g_assert_true (fte3600_brisk_pattern_point (0, &x, &y));
  g_assert_cmpfloat_with_epsilon (x, 4.0, 1e-6);
  g_assert_cmpfloat_with_epsilon (y, 0.0, 1e-6);
  g_assert_true (fte3600_brisk_pattern_point (10, &x, &y));
  g_assert_cmpfloat_with_epsilon (x, 8.0, 1e-6);
  g_assert_true (fte3600_brisk_pattern_point (25, &x, &y));
  g_assert_cmpfloat_with_epsilon (x, 13.0, 1e-6);
  g_assert_false (fte3600_brisk_pattern_point (45, &x, &y));

  for (guint bit = 0; bit < FTE3600_BRISK_DESCRIPTOR_BITS; bit++)
    {
      guint first;
      guint second;
      guint repeat_first;
      guint repeat_second;

      g_assert_true (fte3600_brisk_descriptor_pair (bit, &first, &second));
      g_assert_true (fte3600_brisk_descriptor_pair (bit, &repeat_first, &repeat_second));
      g_assert_cmpuint (first, <, FTE3600_BRISK_PATTERN_POINTS);
      g_assert_cmpuint (second, <, FTE3600_BRISK_PATTERN_POINTS);
      g_assert_cmpuint (first, !=, second);
      g_assert_cmpuint (repeat_first, ==, first);
      g_assert_cmpuint (repeat_second, ==, second);
      serialized_pairs[2 * bit] = first;
      serialized_pairs[2 * bit + 1] = second;
      g_assert_false (used[MIN (first, second)][MAX (first, second)]);
      used[MIN (first, second)][MAX (first, second)] = TRUE;
      point_usage[first]++;
      point_usage[second]++;
      if ((first < 10 ? 0 : first < 25 ? 1 : 2) !=
          (second < 10 ? 0 : second < 25 ? 1 : 2))
        cross_ring++;
    }
  for (guint point = 0; point < FTE3600_BRISK_PATTERN_POINTS; point++)
    {
      minimum_usage = MIN (minimum_usage, point_usage[point]);
      maximum_usage = MAX (maximum_usage, point_usage[point]);
    }
  g_assert_cmpuint (maximum_usage - minimum_usage, <=, 2);
  g_assert_cmpuint (cross_ring, >, FTE3600_BRISK_DESCRIPTOR_BITS / 2);
  {
    g_autofree gchar *hash =
      g_compute_checksum_for_data (G_CHECKSUM_SHA256, serialized_pairs,
                                   sizeof (serialized_pairs));

    g_assert_cmpstr (hash, ==, FTE3600_BRISK_PAIR_TABLE_SHA256);
  }
  {
    static const struct {
      guint bit;
      guint first;
      guint second;
    } golden_pairs[] = {
      { 0, 22, 40 }, { 63, 25, 42 }, { 127, 13, 3 },
      { 191, 17, 7 }, { 255, 5, 16 },
    };

    for (guint i = 0; i < G_N_ELEMENTS (golden_pairs); i++)
      {
        guint first;
        guint second;

        g_assert_true (fte3600_brisk_descriptor_pair (golden_pairs[i].bit,
                                                       &first, &second));
        g_assert_cmpuint (first, ==, golden_pairs[i].first);
        g_assert_cmpuint (second, ==, golden_pairs[i].second);
      }
  }
}

static void
test_descriptor_deterministic (void)
{
  Fte3600BriskFeature first;
  Fte3600BriskFeature second;
  guint8 image[FTE3600_BRISK_IMAGE_SIZE];
  guint8 flat[FTE3600_BRISK_IMAGE_SIZE];

  make_visual_pattern (image);
  memset (flat, 128, sizeof (flat));
  g_assert_cmpint (fte3600_brisk_describe_at (image, sizeof (image), 32.0, 40.0,
                                              &first),
                   ==, FTE3600_BRISK_OK);
  g_assert_cmpint (fte3600_brisk_describe_at (image, sizeof (image), 32.0, 40.0,
                                              &second),
                   ==, FTE3600_BRISK_OK);
  g_assert_cmpmem (&first, sizeof (first), &second, sizeof (second));
  {
    g_autoptr(GString) vector = g_string_new (NULL);
    guint32 orientation_bits;

    for (guint i = 0; i < FTE3600_BRISK_DESCRIPTOR_BYTES; i++)
      g_string_append_printf (vector, "%02x", first.descriptor[i]);
    memcpy (&orientation_bits, &first.orientation, sizeof (orientation_bits));
    g_assert_cmpstr (vector->str, ==,
                     "8575d733ce5840295f8b0610af361936"
                     "991663bf880090f4ac2f0e0a25df8363");
    g_assert_cmphex (orientation_bits, ==, 0x40079158u);
  }
  g_assert_cmpint (fte3600_brisk_describe_at (flat, sizeof (flat), 32.0, 40.0,
                                              &second),
                   ==, FTE3600_BRISK_LOW_CONTRAST);
  g_assert_cmpint (fte3600_brisk_describe_at (image, sizeof (image) - 1, 32.0, 40.0,
                                              &second),
                   ==, FTE3600_BRISK_INVALID_ARGUMENT);
  g_assert_cmpint (fte3600_brisk_describe_at (image, sizeof (image), 4.0, 40.0,
                                              &second),
                   ==, FTE3600_BRISK_INVALID_ARGUMENT);
}

static void
test_extract_deterministic (void)
{
  Fte3600BriskFeatureSet first;
  Fte3600BriskFeatureSet second;
  Fte3600BriskStatus first_status;
  Fte3600BriskStatus second_status;
  guint8 image[FTE3600_BRISK_IMAGE_SIZE];

  make_visual_pattern (image);
  first_status = fte3600_brisk_extract (image, sizeof (image), &first);
  second_status = fte3600_brisk_extract (image, sizeof (image), &second);
  g_assert_cmpint (first_status, ==, second_status);
  g_assert_cmpint (first_status, ==, FTE3600_BRISK_OK);
  g_test_message ("synthetic detector produced %u oriented features", first.n_features);
  g_assert_cmpuint (first.n_features, >=, FTE3600_BRISK_MIN_MUTUAL_MATCHES);
  g_assert_cmpuint (first.n_features, ==, second.n_features);
  g_assert_cmpmem (&first, sizeof (first), &second, sizeof (second));
  {
    g_autofree gchar *hash = feature_set_checksum (&first);

    g_assert_cmpstr (hash, ==,
                     "440e4403a4f22206516f366c615b793e"
                     "c2d1b2dc46501f9c6d8dc1f4aa7f7b94");
  }
}

static void
test_rounding_mode_isolation (void)
{
  static const gint modes[] = { FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO };
  const gint caller_mode = fegetround ();
  guint8 image[FTE3600_BRISK_IMAGE_SIZE];
  Fte3600BriskFeature described_nearest;
  Fte3600BriskFeatureSet extracted_nearest;
  Fte3600BriskMatchResult match_nearest;
  Fte3600BriskStatus match_status_nearest;
  gfloat point_x_nearest;
  gfloat point_y_nearest;
  gboolean diagnostic_nearest;

  g_assert_cmpint (caller_mode, !=, -1);
  g_assert_cmpint (fesetround (FE_TONEAREST), ==, 0);
  make_visual_pattern (image);
  g_assert_true (fte3600_brisk_pattern_point (17, &point_x_nearest,
                                              &point_y_nearest));
  g_assert_cmpint (fte3600_brisk_describe_at (
                       image, sizeof (image), 32.0f, 40.0f,
                       &described_nearest), ==, FTE3600_BRISK_OK);
  g_assert_cmpint (fte3600_brisk_extract (image, sizeof (image),
                                          &extracted_nearest), ==,
                   FTE3600_BRISK_OK);
  g_assert_true (fte3600_brisk_validate_feature_set (&extracted_nearest,
                                                      NULL));
  match_status_nearest = fte3600_brisk_match (&extracted_nearest,
                                               &extracted_nearest,
                                               &match_nearest);
  diagnostic_nearest =
    fte3600_brisk_result_meets_diagnostic_policy (&match_nearest);

  for (guint i = 0; i < G_N_ELEMENTS (modes); i++)
    {
      Fte3600BriskFeature described;
      Fte3600BriskFeatureSet extracted;
      Fte3600BriskMatchResult match;
      Fte3600BriskStatus match_status;
      Fte3600BriskFeatureSet boundary = extracted_nearest;
      gfloat point_x;
      gfloat point_y;

      g_assert_cmpint (fesetround (modes[i]), ==, 0);
      g_assert_true (fte3600_brisk_pattern_point (17, &point_x, &point_y));
      g_assert_cmpint (fegetround (), ==, modes[i]);
      g_assert_cmpmem (&point_x, sizeof (point_x),
                       &point_x_nearest, sizeof (point_x_nearest));
      g_assert_cmpmem (&point_y, sizeof (point_y),
                       &point_y_nearest, sizeof (point_y_nearest));

      g_assert_cmpint (fte3600_brisk_describe_at (
                           image, sizeof (image), 32.0f, 40.0f, &described),
                       ==, FTE3600_BRISK_OK);
      g_assert_cmpint (fegetround (), ==, modes[i]);
      g_assert_cmpmem (&described, sizeof (described),
                       &described_nearest, sizeof (described_nearest));

      g_assert_cmpint (fte3600_brisk_extract (image, sizeof (image),
                                              &extracted), ==,
                       FTE3600_BRISK_OK);
      g_assert_cmpint (fegetround (), ==, modes[i]);
      g_assert_cmpmem (&extracted, sizeof (extracted),
                       &extracted_nearest, sizeof (extracted_nearest));
      g_assert_true (fte3600_brisk_validate_feature_set (&extracted, NULL));
      g_assert_cmpint (fegetround (), ==, modes[i]);

      match_status = fte3600_brisk_match (&extracted, &extracted, &match);
      g_assert_cmpint (fegetround (), ==, modes[i]);
      g_assert_cmpint (match_status, ==, match_status_nearest);
      g_assert_cmpmem (&match, sizeof (match),
                       &match_nearest, sizeof (match_nearest));
      g_assert_cmpint (fte3600_brisk_result_meets_diagnostic_policy (&match),
                       ==, diagnostic_nearest);
      g_assert_cmpint (fegetround (), ==, modes[i]);
      assert_authentication_policy_result (&match);
      g_assert_cmpint (fegetround (), ==, modes[i]);

      boundary.features[0].orientation =
        FTE3600_BRISK_ORIENTATION_LIMIT;
      g_assert_true (fte3600_brisk_validate_feature_set (&boundary, NULL));
      g_assert_cmpint (fegetround (), ==, modes[i]);
      boundary.features[0].orientation =
        -FTE3600_BRISK_ORIENTATION_LIMIT;
      g_assert_true (fte3600_brisk_validate_feature_set (&boundary, NULL));
      boundary.features[0].orientation =
        nextafterf (FTE3600_BRISK_ORIENTATION_LIMIT, INFINITY);
      g_assert_false (fte3600_brisk_validate_feature_set (&boundary, NULL));
      boundary.features[0].orientation =
        nextafterf (-FTE3600_BRISK_ORIENTATION_LIMIT, -INFINITY);
      g_assert_false (fte3600_brisk_validate_feature_set (&boundary, NULL));
      boundary.features[0].orientation = NAN;
      g_assert_false (fte3600_brisk_validate_feature_set (&boundary, NULL));
      g_assert_cmpint (fegetround (), ==, modes[i]);
    }

  g_assert_cmpint (fesetround (caller_mode), ==, 0);
}

static void
test_rigid_consensus (void)
{
  Fte3600BriskFeatureSet query;
  Fte3600BriskFeatureSet reference;
  Fte3600BriskMatchResult result;

  initialize_rigid_fixture (&query, &reference, FALSE);
  g_assert_cmpint (fte3600_brisk_match (&query, &reference, &result),
                   ==, FTE3600_BRISK_OK);
  g_assert_cmpuint (result.mutual_matches, ==, query.n_features);
  g_assert_cmpuint (result.inliers, ==, query.n_features);
  g_assert_cmpfloat_with_epsilon (result.angle, 6.0 * TEST_PI / 180.0, 0.01);
  g_assert_cmpfloat (result.median_error, <, 0.25);
  g_assert_cmpuint (result.occupied_quadrants, ==, 4);
  g_assert_true (result.diagnostic_policy_passed);
  assert_authentication_policy_result (&result);
  g_assert_true (fte3600_brisk_result_meets_diagnostic_policy (&result));
}

static void
test_orientation_variants_are_one_vote (void)
{
  static const gfloat coordinates[4][2] = {
    { 12.0, 14.0 }, { 49.0, 14.0 }, { 12.0, 62.0 }, { 49.0, 62.0 },
  };
  Fte3600BriskFeatureSet query = { 0 };
  Fte3600BriskFeatureSet reference = { 0 };
  Fte3600BriskMatchResult result;

  query.extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
  reference.extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
  query.n_features = 12;
  reference.n_features = 12;
  for (guint point = 0; point < 4; point++)
    for (guint variant = 0; variant < 3; variant++)
      {
        const guint index = 3 * point + variant;
        Fte3600BriskFeature *a = &query.features[index];
        Fte3600BriskFeature *b = &reference.features[index];

        a->x = coordinates[point][0] + 0.3 * ((gint) variant - 1);
        a->y = coordinates[point][1] +
               0.2 * ((gint) ((variant + 1) % 3) - 1);
        a->orientation = -0.7 + 0.5 * variant;
        fill_descriptor (a->descriptor, 0x9137c621u + 0x9e3779b9u * index);
        b->x = a->x + 2.0;
        b->y = a->y + 1.0;
        b->orientation = a->orientation;
        memcpy (b->descriptor, a->descriptor, sizeof (b->descriptor));
        b->descriptor[(11 * index) % FTE3600_BRISK_DESCRIPTOR_BYTES] ^= 0x03;
      }

  g_assert_cmpint (fte3600_brisk_match (&query, &reference, &result),
                   ==, FTE3600_BRISK_NO_CONSENSUS);
  g_assert_cmpuint (result.mutual_matches, ==, 4);
  g_assert_false (result.diagnostic_policy_passed);
  assert_authentication_policy_result (&result);
}

static void
test_ambiguous_descriptors_rejected (void)
{
  Fte3600BriskFeatureSet query;
  Fte3600BriskFeatureSet reference;
  Fte3600BriskMatchResult result;

  initialize_rigid_fixture (&query, &reference, FALSE);
  for (guint i = 0; i < query.n_features; i++)
    memset (query.features[i].descriptor, 0, FTE3600_BRISK_DESCRIPTOR_BYTES);
  for (guint i = 0; i < reference.n_features; i++)
    memset (reference.features[i].descriptor, 0, FTE3600_BRISK_DESCRIPTOR_BYTES);

  g_assert_cmpint (fte3600_brisk_match (&query, &reference, &result),
                   ==, FTE3600_BRISK_NO_CONSENSUS);
  g_assert_cmpuint (result.mutual_matches, ==, 0);
  g_assert_false (result.diagnostic_policy_passed);
  assert_authentication_policy_result (&result);
}

static void
test_spatial_coverage_required (void)
{
  Fte3600BriskFeatureSet query;
  Fte3600BriskFeatureSet reference;
  Fte3600BriskMatchResult result;

  initialize_rigid_fixture (&query, &reference, TRUE);
  g_assert_cmpint (fte3600_brisk_match (&query, &reference, &result),
                   ==, FTE3600_BRISK_OK);
  g_assert_cmpuint (result.inliers, ==, query.n_features);
  g_assert_false (result.diagnostic_policy_passed);
  assert_authentication_policy_result (&result);
  g_assert_cmpfloat (result.x_span, <, 8.0);
  g_assert_cmpfloat (result.y_span, <, 10.0);
}

static void
test_degenerate_geometry_rejected (void)
{
  const gdouble angle = 6.0 * TEST_PI / 180.0;
  const gdouble cosine = cos (angle);
  const gdouble sine = sin (angle);
  Fte3600BriskFeatureSet query;
  Fte3600BriskFeatureSet reference;
  Fte3600BriskMatchResult result;

  initialize_rigid_fixture (&query, &reference, FALSE);
  for (guint i = 0; i < query.n_features; i++)
    {
      Fte3600BriskFeature *a = &query.features[i];
      Fte3600BriskFeature *b = &reference.features[i];

      a->x = 8.0 + 3.0 * i;
      a->y = 30.0 + 0.12 * (i % 2);
      b->x = cosine * a->x - sine * a->y + 3.0;
      b->y = sine * a->x + cosine * a->y - 1.0;
      b->orientation = a->orientation + angle;
    }

  g_assert_cmpint (fte3600_brisk_match (&query, &reference, &result),
                   ==, FTE3600_BRISK_OK);
  g_assert_cmpuint (result.inliers, ==, query.n_features);
  g_assert_cmpfloat (result.query_min_variance, <, 4.0);
  g_assert_cmpfloat (result.query_anisotropy, <, 0.08);
  g_assert_cmpfloat (result.reference_min_variance, <, 4.0);
  g_assert_cmpfloat (result.reference_anisotropy, <, 0.08);
  g_assert_false (result.diagnostic_policy_passed);
  assert_authentication_policy_result (&result);
}

static void
test_end_to_end_translation (void)
{
  Fte3600BriskFeatureSet query;
  Fte3600BriskFeatureSet reference;
  Fte3600BriskMatchResult result;
  guint8 source[FTE3600_BRISK_IMAGE_SIZE];
  guint8 translated[FTE3600_BRISK_IMAGE_SIZE];

  make_visual_pattern (source);
  translate_pattern (source, translated, 2, 3, TRUE);
  g_assert_cmpint (fte3600_brisk_extract (source, sizeof (source), &query),
                   ==, FTE3600_BRISK_OK);
  g_assert_cmpint (fte3600_brisk_extract (translated, sizeof (translated), &reference),
                   ==, FTE3600_BRISK_OK);
  {
    const Fte3600BriskStatus status =
      fte3600_brisk_match (&query, &reference, &result);

    g_test_message ("e2e query=%u reference=%u mutual=%u inliers=%u status=%u",
                    query.n_features, reference.n_features, result.mutual_matches,
                    result.inliers, status);
    g_assert_cmpint (status, ==, FTE3600_BRISK_OK);
  }
  g_assert_cmpuint (result.mutual_matches, >=, FTE3600_BRISK_MIN_MUTUAL_MATCHES);
  g_assert_cmpuint (result.inliers, >=, FTE3600_BRISK_MIN_INLIERS);
  g_assert_cmpfloat_with_epsilon (result.angle, 0.0, 0.03);
  g_assert_cmpfloat_with_epsilon (result.translate_x, 2.0, 0.5);
  g_assert_cmpfloat_with_epsilon (result.translate_y, 3.0, 0.5);
  assert_authentication_policy_result (&result);
}

static void
test_end_to_end_rotation (void)
{
  const gdouble expected_angle = 2.0 * TEST_PI / 180.0;
  const gdouble center_x = (FTE3600_BRISK_WIDTH - 1) / 2.0;
  const gdouble center_y = (FTE3600_BRISK_HEIGHT - 1) / 2.0;
  const gdouble expected_x = center_x + 1.0 -
                             cos (expected_angle) * center_x +
                             sin (expected_angle) * center_y;
  const gdouble expected_y = center_y + 2.0 -
                             sin (expected_angle) * center_x -
                             cos (expected_angle) * center_y;
  Fte3600BriskFeatureSet query;
  Fte3600BriskFeatureSet reference;
  Fte3600BriskMatchResult result;
  guint8 source[FTE3600_BRISK_IMAGE_SIZE];
  guint8 transformed[FTE3600_BRISK_IMAGE_SIZE];

  make_visual_pattern (source);
  warp_pattern (source, transformed, expected_angle, 1.0, 2.0);
  g_assert_cmpint (fte3600_brisk_extract (source, sizeof (source), &query),
                   ==, FTE3600_BRISK_OK);
  g_assert_cmpint (fte3600_brisk_extract (transformed, sizeof (transformed),
                                          &reference),
                   ==, FTE3600_BRISK_OK);
  g_assert_cmpint (fte3600_brisk_match (&query, &reference, &result),
                   ==, FTE3600_BRISK_OK);
  g_test_message ("rotation e2e mutual=%u inliers=%u angle=%.4f",
                  result.mutual_matches, result.inliers, result.angle);
  g_assert_cmpuint (result.inliers, >=, FTE3600_BRISK_MIN_INLIERS);
  g_assert_cmpfloat_with_epsilon (result.angle, expected_angle, 0.025);
  g_assert_cmpfloat_with_epsilon (result.translate_x, expected_x, 0.8);
  g_assert_cmpfloat_with_epsilon (result.translate_y, expected_y, 0.8);
  assert_authentication_policy_result (&result);
}

static void
test_incoherent_geometry_rejected (void)
{
  static const guint permutation[16] = {
    7, 12, 2, 14, 5, 9, 0, 11, 15, 3, 13, 6, 10, 1, 8, 4,
  };
  Fte3600BriskFeatureSet query;
  Fte3600BriskFeatureSet reference;
  Fte3600BriskMatchResult result;
  Fte3600BriskFeature positions[16];
  Fte3600BriskStatus status;

  initialize_rigid_fixture (&query, &reference, FALSE);
  memcpy (positions, reference.features, sizeof (positions));
  for (guint i = 0; i < query.n_features; i++)
    {
      reference.features[i].x = positions[permutation[i]].x;
      reference.features[i].y = positions[permutation[i]].y;
      reference.features[i].orientation = positions[permutation[i]].orientation;
    }

  status = fte3600_brisk_match (&query, &reference, &result);
  g_assert_true (status == FTE3600_BRISK_NO_CONSENSUS || status == FTE3600_BRISK_OK);
  g_assert_false (result.diagnostic_policy_passed);
  assert_authentication_policy_result (&result);
}

static void
test_malformed_sets_rejected (void)
{
  Fte3600BriskFeatureSet query;
  Fte3600BriskFeatureSet reference;
  Fte3600BriskMatchResult result;

  initialize_rigid_fixture (&query, &reference, FALSE);
  query.extractor_schema_version++;
  g_assert_cmpint (fte3600_brisk_match (&query, &reference, &result),
                   ==, FTE3600_BRISK_INVALID_ARGUMENT);

  initialize_rigid_fixture (&query, &reference, FALSE);
  query.features[0].x = NAN;
  g_assert_cmpint (fte3600_brisk_match (&query, &reference, &result),
                   ==, FTE3600_BRISK_INVALID_ARGUMENT);

  initialize_rigid_fixture (&query, &reference, FALSE);
  query.n_features = FTE3600_BRISK_MAX_FEATURES + 1;
  g_assert_cmpint (fte3600_brisk_match (&query, &reference, &result),
                   ==, FTE3600_BRISK_INVALID_ARGUMENT);

  memset (&result, 0, sizeof (result));
  result.mutual_matches = FTE3600_BRISK_MAX_FEATURES;
  result.inliers = FTE3600_BRISK_MAX_FEATURES;
  result.competing_inliers = G_MAXUINT;
  g_assert_false (fte3600_brisk_result_meets_diagnostic_policy (&result));
  assert_authentication_policy_result (&result);
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/fte3600-brisk/pattern-and-pairs", test_pattern_and_pairs);
  g_test_add_func ("/fte3600-brisk/descriptor-deterministic",
                   test_descriptor_deterministic);
  g_test_add_func ("/fte3600-brisk/extract-deterministic",
                   test_extract_deterministic);
  g_test_add_func ("/fte3600-brisk/rounding-mode-isolation",
                   test_rounding_mode_isolation);
  g_test_add_func ("/fte3600-brisk/rigid-consensus", test_rigid_consensus);
  g_test_add_func ("/fte3600-brisk/orientation-variants-one-vote",
                   test_orientation_variants_are_one_vote);
  g_test_add_func ("/fte3600-brisk/ambiguous-descriptors",
                   test_ambiguous_descriptors_rejected);
  g_test_add_func ("/fte3600-brisk/spatial-coverage",
                   test_spatial_coverage_required);
  g_test_add_func ("/fte3600-brisk/degenerate-geometry",
                   test_degenerate_geometry_rejected);
  g_test_add_func ("/fte3600-brisk/end-to-end-translation",
                   test_end_to_end_translation);
  g_test_add_func ("/fte3600-brisk/end-to-end-rotation",
                   test_end_to_end_rotation);
  g_test_add_func ("/fte3600-brisk/incoherent-geometry",
                   test_incoherent_geometry_rejected);
  g_test_add_func ("/fte3600-brisk/malformed-sets", test_malformed_sets_rejected);

  return g_test_run ();
}
