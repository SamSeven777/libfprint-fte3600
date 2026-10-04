/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Mathematical fixtures only. This test links the matcher core with GLib and
 * libm, without a driver, template codec, platform configuration or policy.
 */
#include <fenv.h>
#include <math.h>
#include <string.h>

#include "../libfprint/matchers/brisk/brisk.h"

static void
make_pattern (guint8 *image, guint width, guint height, gsize stride)
{
  static const struct
  {
    gdouble x, y, sigma, amplitude;
  } spots[] = {
    { 17, 18, 1.5, 58 }, { 29, 17, 2.2, -54 }, { 43, 19, 3.1, 61 },
    { 51, 29, 1.8, -48 }, { 18, 32, 2.7, -61 }, { 32, 31, 1.4, 52 },
    { 43, 39, 2.4, -57 }, { 17, 47, 1.9, 55 }, { 31, 49, 3.2, 63 },
    { 49, 52, 1.5, -53 }, { 20, 63, 2.3, -58 }, { 36, 63, 1.7, 57 },
    { 48, 66, 2.8, 50 },
  };

  for (guint y = 0; y < height; y++)
    for (guint x = 0; x < width; x++)
      {
        gdouble value = 126.0 + 13.0 * sin (0.29 * x + 0.17 * y) +
                        8.0 * cos (0.13 * x - 0.23 * y);

        for (guint i = 0; i < G_N_ELEMENTS (spots); i++)
          {
            const gdouble dx = x - spots[i].x;
            const gdouble dy = y - spots[i].y;

            value += spots[i].amplitude * exp (-(dx * dx + dy * dy) /
                                               (2.0 * spots[i].sigma * spots[i].sigma));
          }
        image[y * stride + x] = CLAMP (floor (value + 0.5), 2, 253);
      }
}

static gchar *
feature_checksum (const FpiBriskFeatureSet *features)
{
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
  guint32 value = GUINT32_TO_LE (features->extractor_schema_version);

  g_checksum_update (checksum, (const guchar *) &value, sizeof (value));
  value = GUINT32_TO_LE (features->n_features);
  g_checksum_update (checksum, (const guchar *) &value, sizeof (value));
  for (guint i = 0; i < features->n_features; i++)
    {
      const FpiBriskFeature *feature = &features->features[i];
      const gfloat values[] = { feature->x, feature->y, feature->orientation };

      for (guint j = 0; j < G_N_ELEMENTS (values); j++)
        {
          guint32 bits;

          memcpy (&bits, &values[j], sizeof (bits));
          bits = GUINT32_TO_LE (bits);
          g_checksum_update (checksum, (const guchar *) &bits, sizeof (bits));
        }
      g_checksum_update (checksum, feature->descriptor, sizeof (feature->descriptor));
    }
  return g_strdup (g_checksum_get_string (checksum));
}

static void
test_legacy_golden_and_stride (void)
{
  guint8 packed[64 * 80];
  guint8 strided[79 * 77 + 64];
  FpiBriskImage image = { packed, sizeof (packed), 64, 80, 64 };
  FpiBriskImage padded = { strided, sizeof (strided), 64, 80, 77 };
  FpiBriskFeatureSet first, second;
  FpiBriskFeature a, b;
  g_autofree gchar *hash = NULL;

  memset (strided, 0xa5, sizeof (strided));
  make_pattern (packed, 64, 80, 64);
  make_pattern (strided, 64, 80, 77);
  g_assert_cmpint (fpi_brisk_extract (&image, &first), ==, FPI_BRISK_OK);
  g_assert_cmpint (fpi_brisk_extract (&padded, &second), ==, FPI_BRISK_OK);
  g_assert_cmpmem (&first, sizeof (first), &second, sizeof (second));
  hash = feature_checksum (&first);
  g_assert_cmpstr (hash, ==,
                   "a94b585062b5187b3b198347df0893f3f8872af10353a4432c6fcdd9d0c6f328");
  g_assert_cmpint (fpi_brisk_describe_at (&image, 32, 40, &a), ==, FPI_BRISK_OK);
  g_assert_cmpint (fpi_brisk_describe_at (&padded, 32, 40, &b), ==, FPI_BRISK_OK);
  g_assert_cmpmem (&a, sizeof (a), &b, sizeof (b));
  for (guint y = 0; y < 79; y++)
    for (guint x = 64; x < 77; x++)
      g_assert_cmpuint (strided[y * 77 + x], ==, 0xa5);
}

static void
test_other_geometries (void)
{
  const guint shapes[][2] = { { 32, 40 }, { 88, 88 }, { 96, 96 }, { 128, 64 } };

  for (guint i = 0; i < G_N_ELEMENTS (shapes); i++)
    {
      guint width = shapes[i][0], height = shapes[i][1];
      g_autofree guint8 *data = g_malloc (width * height);
      FpiBriskImage image = { data, width * height, width, height, width };
      FpiBriskFeatureSet features;
      FpiBriskStatus status;

      make_pattern (data, width, height, width);
      status = fpi_brisk_extract (&image, &features);
      g_assert_true (status == FPI_BRISK_OK || status == FPI_BRISK_INSUFFICIENT_FEATURES);
      g_assert_true (fpi_brisk_validate_feature_set (&features, width, height, NULL));
      if (width >= 64 && height >= 64)
        g_assert_cmpint (status, ==, FPI_BRISK_OK);
    }
}

static void
test_invalid_layout (void)
{
  guint8 data[64 * 80] = { 0 };
  const FpiBriskImage invalid[] = {
    { NULL, sizeof (data), 64, 80, 64 },
    { data, sizeof (data), 0, 80, 64 },
    { data, sizeof (data), 64, 0, 64 },
    { data, G_MAXSIZE, 257, 80, 257 },
    { data, G_MAXSIZE, 64, G_MAXUINT, 64 },
    { data, sizeof (data), 64, 80, 63 },
    { data, sizeof (data) - 1, 64, 80, 64 },
    { data, G_MAXSIZE, 64, 2, G_MAXSIZE },
    { data, G_MAXSIZE, 64, 80, G_MAXSIZE / 2 },
  };
  FpiBriskFeatureSet features;
  FpiBriskFeature feature;

  g_assert_false (fpi_brisk_image_valid (NULL));
  for (guint i = 0; i < G_N_ELEMENTS (invalid); i++)
    {
      g_assert_false (fpi_brisk_image_valid (&invalid[i]));
      memset (&features, 0xa5, sizeof (features));
      g_assert_cmpint (fpi_brisk_extract (&invalid[i], &features), ==,
                       FPI_BRISK_INVALID_ARGUMENT);
      g_assert_cmpuint (features.n_features, ==, 0);
      g_assert_cmpuint (features.extractor_schema_version, ==, 3);
      g_assert_cmpint (fpi_brisk_describe_at (&invalid[i], 32, 40, &feature), ==,
                       FPI_BRISK_INVALID_ARGUMENT);
    }
  {
    FpiBriskImage tiny = { data, sizeof (data), 16, 16, 16 };

    g_assert_true (fpi_brisk_image_valid (&tiny));
    g_assert_cmpint (fpi_brisk_extract (&tiny, &features), ==, FPI_BRISK_INVALID_ARGUMENT);
  }
}

static void
test_normalize_layout_and_rounding (void)
{
  guint8 source[64 * 80], expected[64 * 80], output[80 * 73], alias[64 * 80];
  FpiBriskImage image = { source, sizeof (source), 64, 80, 64 };
  FpiBriskImage inplace = { alias, sizeof (alias), 64, 80, 64 };
  gint caller_mode = fegetround ();

  make_pattern (source, 64, 80, 64);
  memcpy (alias, source, sizeof (source));
  memset (output, 0xa5, sizeof (output));
  g_assert_cmpint (fpi_brisk_normalize (&image, expected, sizeof (expected), 64), ==, FPI_BRISK_OK);
  g_assert_cmpint (fesetround (FE_UPWARD), ==, 0);
  g_assert_cmpint (fpi_brisk_normalize (&image, output, sizeof (output), 73), ==, FPI_BRISK_OK);
  g_assert_cmpint (fegetround (), ==, FE_UPWARD);
  g_assert_cmpint (fesetround (caller_mode), ==, 0);
  for (guint y = 0; y < 80; y++)
    {
      g_assert_cmpmem (expected + y * 64, 64, output + y * 73, 64);
      for (guint x = 64; x < 73; x++)
        g_assert_cmpuint (output[y * 73 + x], ==, 0xa5);
    }
  g_assert_cmpint (fpi_brisk_normalize (&inplace, alias, sizeof (alias), 64), ==, FPI_BRISK_OK);
  g_assert_cmpmem (alias, sizeof (alias), expected, sizeof (expected));
  memset (output, 0xa5, sizeof (output));
  g_assert_cmpint (fpi_brisk_normalize (&image, output, 5120 - 1, 64), ==,
                   FPI_BRISK_INVALID_ARGUMENT);
  g_assert_cmpint (fpi_brisk_normalize (&image, output, G_MAXSIZE, G_MAXSIZE), ==,
                   FPI_BRISK_INVALID_ARGUMENT);
  for (guint i = 0; i < sizeof (output); i++)
    g_assert_cmpuint (output[i], ==, 0xa5);
}

static void
test_normalize_maximum (void)
{
  g_autofree guint8 *source = g_malloc (256 * 256);
  g_autofree guint8 *output = g_malloc (256 * 256);
  FpiBriskImage image = { source, 256 * 256, 256, 256, 256 };

  memset (source, 0, image.length);
  g_assert_cmpint (fpi_brisk_normalize (&image, output, image.length, 256), ==, FPI_BRISK_OK);
  for (guint i = 0; i < image.length; i++)
    g_assert_cmpuint (output[i], ==, 128);
  for (guint i = 0; i < image.length; i++)
    source[i] = i & 1 ? 255 : 0;
  g_assert_cmpint (fpi_brisk_normalize (&image, output, image.length, 256), ==, FPI_BRISK_OK);
  g_assert_cmpuint (output[128 * 256 + 128], <, 128);
  g_assert_cmpuint (output[128 * 256 + 129], >, 128);
}

static void
test_match_geometry_and_evidence (void)
{
  const guint positions[][2] = { { 20, 20 }, { 40, 20 }, { 20, 40 },
                                 { 40, 40 }, { 64, 60 }, { 90, 35 } };
  FpiBriskFeatureSet query = { .extractor_schema_version = 3,
                               .n_features = G_N_ELEMENTS (positions) };
  FpiBriskFeatureSet reference;
  FpiBriskMatchEvidence evidence;

  for (guint i = 0; i < query.n_features; i++)
    {
      FpiBriskFeature *feature = &query.features[i];
      guint32 state = 1234 + 17 * i;

      feature->x = positions[i][0];
      feature->y = positions[i][1];
      for (guint j = 0; j < sizeof (feature->descriptor); j++)
        {
          state ^= state << 13;
          state ^= state >> 17;
          state ^= state << 5;
          feature->descriptor[j] = state >> 24;
        }
    }
  reference = query;
  for (guint i = 0; i < reference.n_features; i++)
    {
      reference.features[i].x += 12;
      reference.features[i].y += 15;
    }
  g_assert_true (fpi_brisk_validate_feature_set (&query, 96, 80, NULL));
  g_assert_false (fpi_brisk_validate_feature_set (&query, 64, 80, NULL));
  g_assert_cmpint (fpi_brisk_match (&query, 96, 80, &reference, 128, 112, &evidence), ==,
                   FPI_BRISK_OK);
  g_assert_cmpuint (evidence.inliers, ==, 6);
  g_assert_cmpfloat_with_epsilon (evidence.translate_x, 12, 1e-9);
  g_assert_cmpfloat_with_epsilon (evidence.translate_y, 15, 1e-9);
  g_assert_cmpfloat_with_epsilon (evidence.rms_error, 0, 1e-9);
  g_assert_cmpint (fpi_brisk_match (&query, 64, 80, &reference, 128, 112, &evidence), ==,
                   FPI_BRISK_INVALID_ARGUMENT);
  g_assert_cmpuint (evidence.inliers, ==, 0);
  g_assert_cmpint (fpi_brisk_match (&query, 96, 80, &reference, G_MAXUINT, 112, &evidence), ==,
                   FPI_BRISK_INVALID_ARGUMENT);
  query.extractor_schema_version = 2;
  g_assert_false (fpi_brisk_validate_feature_set (&query, 96, 80, NULL));
}

typedef struct
{
  const FpiBriskImage      *image;
  const FpiBriskFeatureSet *expected;
  gint                      rounding_mode;
} ConcurrentExtraction;

static gpointer
extract_in_thread (gpointer data)
{
  ConcurrentExtraction *run = data;
  FpiBriskFeatureSet actual;

  g_assert_cmpint (fesetround (run->rounding_mode), ==, 0);
  g_assert_cmpint (fpi_brisk_extract (run->image, &actual), ==, FPI_BRISK_OK);
  g_assert_cmpmem (&actual, sizeof (actual), run->expected, sizeof (*run->expected));
  g_assert_cmpint (fegetround (), ==, run->rounding_mode);
  return NULL;
}

static void
test_concurrent_rounding (void)
{
  const gint modes[] = { FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO };
  const gint caller_mode = fegetround ();
  guint8 pixels[64 * 80];
  FpiBriskImage image = { pixels, sizeof (pixels), 64, 80, 64 };
  FpiBriskFeatureSet expected;
  ConcurrentExtraction runs[G_N_ELEMENTS (modes)];
  GThread *threads[G_N_ELEMENTS (modes)];

  make_pattern (pixels, 64, 80, 64);
  g_assert_cmpint (fpi_brisk_extract (&image, &expected), ==, FPI_BRISK_OK);
  for (guint i = 0; i < G_N_ELEMENTS (modes); i++)
    {
      runs[i] = (ConcurrentExtraction){ &image, &expected, modes[i] };
      threads[i] = g_thread_new ("brisk-rounding", extract_in_thread, &runs[i]);
    }
  for (guint i = 0; i < G_N_ELEMENTS (modes); i++)
    g_thread_join (threads[i]);
  g_assert_cmpint (fegetround (), ==, caller_mode);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/brisk/core/legacy-golden-and-stride", test_legacy_golden_and_stride);
  g_test_add_func ("/brisk/core/other-geometries", test_other_geometries);
  g_test_add_func ("/brisk/core/invalid-layout", test_invalid_layout);
  g_test_add_func ("/brisk/core/normalize-layout-and-rounding", test_normalize_layout_and_rounding);
  g_test_add_func ("/brisk/core/normalize-maximum", test_normalize_maximum);
  g_test_add_func ("/brisk/core/match-geometry-and-evidence", test_match_geometry_and_evidence);
  g_test_add_func ("/brisk/core/concurrent-rounding", test_concurrent_rounding);
  return g_test_run ();
}
