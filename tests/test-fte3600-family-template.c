/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 FTE3600 Linux contributors
 * Mathematical fixtures only: no fingerprint images or recorded descriptors.
 */
#include <fenv.h>
#include <math.h>
#include <string.h>

#include "../libfprint/drivers/fte3600-template.h"
#include "../libfprint/drivers/fte3600-match-profile.h"

typedef struct
{
  Fte3600Sensor sensor;
  guint16       model;
  guint16       width;
  guint16       height;
} ExpectedProfile;

static const ExpectedProfile expected_profiles[] = {
  { FTE3600_SENSOR_FT9338, 0x9338, 88, 88 },
  { FTE3600_SENSOR_FT9348, 0x9348, 96, 96 },
  { FTE3600_SENSOR_FT9361, 0x9361, 64, 80 },
  { FTE3600_SENSOR_FT9536, 0x9536, 64, 128 },
  { FTE3600_SENSOR_FT9365, 0x9365, 64, 80 },
  { FTE3600_SENSOR_FT9368, 0x9368, 64, 80 },
  { FTE3600_SENSOR_FT9369, 0x9369, 64, 80 },
  { FTE3600_SENSOR_FT9769, 0x9769, 40, 196 },
};

static guint16
read_u16 (const guint8 *bytes)
{
  return bytes[0] | (guint16) bytes[1] << 8;
}

static guint32
read_u32 (const guint8 *bytes)
{
  return bytes[0] | (guint32) bytes[1] << 8 |
         (guint32) bytes[2] << 16 | (guint32) bytes[3] << 24;
}

static void
write_u16 (guint8 *bytes, guint16 value)
{
  bytes[0] = value;
  bytes[1] = value >> 8;
}

static void
write_u32 (guint8 *bytes, guint32 value)
{
  for (guint i = 0; i < 4; i++)
    bytes[i] = value >> (8 * i);
}

static guint32
random_step (guint32 *state)
{
  *state ^= *state << 13;
  *state ^= *state >> 17;
  *state ^= *state << 5;
  return *state;
}

static void
make_features (const Fte3600MatchProfile *profile,
               Fte3600BriskFeatureSet *features, guint sample)
{
  memset (features, 0, sizeof *features);
  features->extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
  features->n_features = 12;
  for (guint i = 0; i < features->n_features; i++)
    {
      Fte3600BriskFeature *feature = &features->features[i];
      guint32 state = 0x85647319u ^ (i + 1) * 0x9e3779b9u;

      /* A rectangular constellation exercises each sensor's actual extent,
       * including coordinates beyond the old 64x80 image domain. */
      feature->x = (profile->width - 1) * (0.08f + 0.28f * (i % 4));
      feature->y = (profile->height - 1) * (0.20f + 0.30f * (i / 4));
      for (guint j = 0; j < sizeof feature->descriptor; j++)
        feature->descriptor[j] = random_step (&state) >> 24;
      /* Distinct, nearby descriptors model eight consistent acquisitions. */
      feature->descriptor[0] ^= sample;
    }
}

static void
make_unrelated (Fte3600BriskFeatureSet *features)
{
  for (guint i = 0; i < features->n_features; i++)
    for (guint j = 0; j < sizeof features->features[i].descriptor; j++)
      features->features[i].descriptor[j] ^= 0xa5;
}

static void
make_pattern (guint8 *pixels, guint width, guint height, gsize stride)
{
  const guint columns = MAX (2, (width - 20) / 14);
  const guint rows = MAX (3, (height - 20) / 14);

  /* Gaussian spots and incommensurate waves create reproducible texture.
   * The narrow sensor has two columns and more rows, not a resized 64x80
   * image, so its source sampling and long-axis bounds are exercised. */
  for (guint y = 0; y < height; y++)
    for (guint x = 0; x < width; x++)
      {
        gdouble value = 125 + 11 * sin (0.29 * x + 0.17 * y) +
                        9 * cos (0.13 * x - 0.23 * y);

        for (guint row = 0; row < rows; row++)
          for (guint column = 0; column < columns; column++)
            {
              const gdouble cx = 14 + (width - 28.0) * column / (columns - 1);
              const gdouble cy = 14 + (height - 28.0) * row / (rows - 1);
              const gdouble sigma = 1.4 + 0.27 * ((column + 3 * row) % 7);
              const gdouble amplitude = (column + row) % 2 ? 65 : -62;
              const gdouble dx = x - cx, dy = y - cy;

              value += amplitude * exp (-(dx * dx + dy * dy) / (2 * sigma * sigma));
            }
        pixels[y * stride + x] = CLAMP (floor (value + 0.5), 1, 254);
      }
}

static Fte3600Template *
ready_template (const Fte3600MatchProfile *profile, gboolean legacy)
{
  Fte3600Template *templ = legacy ? fpi_fte3600_template_new () :
                           fpi_fte3600_template_new_for_profile (profile);

  g_assert_nonnull (templ);
  for (guint sample = 0; sample < 8; sample++)
    {
      Fte3600BriskFeatureSet features;

      make_features (profile, &features, sample);
      g_assert_cmpint (fpi_fte3600_template_add_features (templ, &features, NULL), ==,
                       sample == 7 ? FTE3600_TEMPLATE_OK : FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
      g_assert_cmpint (fpi_fte3600_template_is_ready (templ), ==, sample == 7);
    }
  return templ;
}

static void
assert_profile (const Fte3600MatchProfile *profile, const ExpectedProfile *expected)
{
  g_assert_nonnull (profile);
  g_assert_cmpint (profile->sensor, ==, expected->sensor);
  g_assert_cmphex (profile->model, ==, expected->model);
  g_assert_cmpuint (profile->width, ==, expected->width);
  g_assert_cmpuint (profile->height, ==, expected->height);
  g_assert_cmpuint (profile->processing_version, ==, 1);
}

static void
test_profile_registry (void)
{
  g_assert_null (fpi_fte3600_match_profile_get (FTE3600_SENSOR_UNKNOWN));
  g_assert_null (fpi_fte3600_match_profile_get (FTE3600_SENSOR_COUNT));
  g_assert_null (fpi_fte3600_match_profile_find (0));
  g_assert_null (fpi_fte3600_match_profile_find (0x9362));
  g_assert_null (fpi_fte3600_match_profile_find (0x9391));
  g_assert_null (fpi_fte3600_match_profile_find (0x9395));
  g_assert_null (fpi_fte3600_match_profile_resolve (NULL));
  g_assert_null (fpi_fte3600_template_new_for_profile (NULL));
  for (guint i = 0; i < G_N_ELEMENTS (expected_profiles); i++)
    {
      const ExpectedProfile *expected = &expected_profiles[i];
      const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (expected->sensor);
      const Fte3600SensorDescriptor *sensor = fpi_fte3600_sensor_get (expected->sensor);
      Fte3600MatchProfile copy;

      assert_profile (profile, expected);
      g_assert_nonnull (sensor);
      g_assert_cmpuint (profile->width, ==, sensor->width);
      g_assert_cmpuint (profile->height, ==, sensor->height);
      g_assert_true (profile == fpi_fte3600_match_profile_find (expected->model));
      copy = *profile;
      g_assert_true (profile == fpi_fte3600_match_profile_resolve (&copy));
      copy.processing_version++;
      g_assert_null (fpi_fte3600_match_profile_resolve (&copy));
      g_assert_null (fpi_fte3600_template_new_for_profile (&copy));
      copy = *profile;
      copy.width++;
      g_assert_null (fpi_fte3600_match_profile_resolve (&copy));
      copy = *profile;
      copy.height++;
      g_assert_null (fpi_fte3600_match_profile_resolve (&copy));
      copy = *profile;
      copy.model = 0xffff;
      g_assert_null (fpi_fte3600_match_profile_resolve (&copy));
      copy = *profile;
      copy.sensor = FTE3600_SENSOR_UNKNOWN;
      g_assert_null (fpi_fte3600_match_profile_resolve (&copy));
    }
}

static void
test_extract (gconstpointer user_data)
{
  const ExpectedProfile *expected = user_data;
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (expected->sensor);
  const gsize size = (gsize) expected->width * expected->height;
  const gsize stride = expected->width + 13;
  const gsize padded_size = (expected->height - 1) * stride + expected->width;
  g_autofree guint8 *pixels = g_malloc (size);
  g_autofree guint8 *padded = g_malloc (padded_size);
  FpiBriskImage image = { pixels, size, expected->width, expected->height, expected->width };
  FpiBriskImage strided = { padded, padded_size, expected->width, expected->height, stride };
  Fte3600BriskFeatureSet packed_features, strided_features;
  Fte3600TemplateCompareResult result;
  Fte3600MatchProfile bad_profile = *profile;

  g_autoptr(Fte3600Template) templ = NULL;
  g_autoptr(Fte3600Template) decoded = NULL;
  g_autoptr(GBytes) wire = NULL;

  memset (padded, 0xa5, padded_size);
  make_pattern (pixels, image.width, image.height, image.stride);
  make_pattern (padded, strided.width, strided.height, strided.stride);
  g_assert_cmpint (fpi_fte3600_brisk_extract_for_profile (profile, &image, &packed_features),
                   ==, FTE3600_BRISK_OK);
  g_assert_cmpint (fpi_fte3600_brisk_extract_for_profile (profile, &strided, &strided_features),
                   ==, FTE3600_BRISK_OK);
  g_assert_cmpmem (&packed_features, sizeof packed_features, &strided_features, sizeof strided_features);
  g_assert_true (fpi_fte3600_brisk_validate_feature_set_for_profile (profile, &packed_features, NULL));
  for (guint y = 0; y < expected->height - 1; y++)
    for (guint x = expected->width; x < stride; x++)
      g_assert_cmphex (padded[y * stride + x], ==, 0xa5);

  /* Exercise extraction through enrollment/serialization independently of
   * the direct descriptor fixtures. This mathematical texture must supply
   * enough physical points at every geometry. */
  templ = fpi_fte3600_template_new_for_profile (profile);
  for (guint sample = 0; sample < 8; sample++)
    {
      Fte3600BriskFeatureSet acquired = packed_features;

      for (guint i = 0; i < acquired.n_features; i++)
        acquired.features[i].descriptor[0] ^= sample;
      g_assert_cmpint (fpi_fte3600_template_add_features (templ, &acquired, NULL), ==,
                       sample == 7 ? FTE3600_TEMPLATE_OK : FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
    }
  g_assert_cmpint (fpi_fte3600_template_encode (templ, &wire), ==, FTE3600_TEMPLATE_OK);
  g_assert_cmpint (fpi_fte3600_template_decode (wire, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &decoded),
                   ==, FTE3600_TEMPLATE_OK);
  g_assert_cmpint (fpi_fte3600_template_compare_features_for_profile (
                     decoded, profile, &packed_features, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &result),
                   ==, FTE3600_TEMPLATE_OK);
  g_test_message ("%04x texture: features=%u inliers=%u competing=%u cells=%u spans=%.2f,%.2f variances=%.4f,%.4f anisotropy=%.4f,%.4f",
                  expected->model, packed_features.n_features, result.best.inliers,
                  result.best.competing_inliers, result.best.occupied_cells,
                  result.best.x_span, result.best.y_span,
                  result.best.query_min_variance, result.best.reference_min_variance,
                  result.best.query_anisotropy, result.best.reference_anisotropy);
  g_assert_cmpuint (result.diagnostic_passes, >, 0);
  g_assert_false (result.authentication_accepted);

  {
    guint32 noise_state = 0x14539fe7;

    for (gsize i = 0; i < size; i++)
      pixels[i] = random_step (&noise_state) >> 24;
    g_assert_cmpint (fpi_fte3600_brisk_extract_for_profile (profile, &image, &strided_features),
                     ==, FTE3600_BRISK_OK);
    g_assert_cmpint (fpi_fte3600_template_compare_features_for_profile (
                       decoded, profile, &strided_features, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &result),
                     ==, FTE3600_TEMPLATE_OK);
    g_assert_cmpuint (result.diagnostic_passes, ==, 0);
    g_assert_false (result.authentication_accepted);
  }

  image.length--;
  g_assert_cmpint (fpi_fte3600_brisk_extract_for_profile (profile, &image, &strided_features),
                   ==, FTE3600_BRISK_INVALID_ARGUMENT);
  g_assert_cmpuint (strided_features.n_features, ==, 0);
  image.length++;
  image.width--;
  g_assert_cmpint (fpi_fte3600_brisk_extract_for_profile (profile, &image, &strided_features),
                   ==, FTE3600_BRISK_INVALID_ARGUMENT);
  image.width++;
  image.stride--;
  g_assert_cmpint (fpi_fte3600_brisk_extract_for_profile (profile, &image, &strided_features),
                   ==, FTE3600_BRISK_INVALID_ARGUMENT);
  image.stride++;
  bad_profile.processing_version++;
  g_assert_cmpint (fpi_fte3600_brisk_extract_for_profile (&bad_profile, &image, &strided_features),
                   ==, FTE3600_BRISK_INVALID_ARGUMENT);
  memset (pixels, 127, size);
  g_assert_cmpint (fpi_fte3600_brisk_extract_for_profile (profile, &image, &strided_features),
                   ==, FTE3600_BRISK_LOW_CONTRAST);
  g_assert_cmpuint (strided_features.n_features, ==, 0);
}

static void
test_roundtrip_and_policy (gconstpointer user_data)
{
  const ExpectedProfile *expected = user_data;
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (expected->sensor);

  g_autoptr(Fte3600Template) templ = ready_template (profile, FALSE);
  g_autoptr(Fte3600Template) decoded = NULL;
  g_autoptr(Fte3600Template) copy = fpi_fte3600_template_copy (templ);
  g_autoptr(Fte3600Template) authenticated = NULL;
  g_autoptr(GBytes) wire = NULL;
  g_autoptr(GBytes) roundtrip = NULL;
  g_autoptr(GBytes) copied_wire = NULL;
  Fte3600BriskFeatureSet features;
  Fte3600TemplateCompareResult result;
  const guint8 *data;
  gsize size;
  const guint diagnostic = 7;
  const guint authentication = FTE3600_ENABLE_PERSONAL_AUTH ? 8 : 0;

  assert_profile (fpi_fte3600_template_get_profile (templ), expected);
  assert_profile (fpi_fte3600_template_get_profile (copy), expected);
  g_assert_cmpint (fpi_fte3600_template_encode (templ, &wire), ==, FTE3600_TEMPLATE_OK);
  data = g_bytes_get_data (wire, &size);
  g_assert_cmpmem (data, 8, "FT36BRK\0", 8);
  g_assert_cmpuint (read_u16 (data + 8), ==, 2);
  g_assert_cmpuint (read_u16 (data + 10), ==, 40);
  g_assert_cmpuint (read_u32 (data + 12), ==, size);
  g_assert_cmphex (read_u16 (data + 16), ==, expected->model);
  g_assert_cmpuint (read_u16 (data + 18), ==, expected->width);
  g_assert_cmpuint (read_u16 (data + 20), ==, expected->height);
  g_assert_cmpuint (read_u16 (data + 22), ==, 44);
  g_assert_cmpuint (read_u16 (data + 24), ==, 3);
  g_assert_cmpuint (read_u16 (data + 26), ==, diagnostic);
  g_assert_cmpuint (read_u16 (data + 28), ==, authentication);
  g_assert_cmpuint (read_u16 (data + 30), ==, 8);
  g_assert_cmpuint (read_u32 (data + 32), ==, 0);
  g_assert_cmpuint (read_u32 (data + 36), ==, 1);
  {
    /* Frozen extractor/policy/count bytes; a policy change requires a new
     * golden value rather than reinterpreting existing persisted features. */
    const guint8 versions[] = { 3, 0, 7, 0, FTE3600_ENABLE_PERSONAL_AUTH ? 8 : 0, 0, 8, 0 };

    g_assert_cmpmem (data + 24, sizeof versions, versions, sizeof versions);
  }
  g_assert_cmpint (fpi_fte3600_template_decode (wire, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &decoded),
                   ==, FTE3600_TEMPLATE_OK);
  assert_profile (fpi_fte3600_template_get_profile (decoded), expected);
  g_assert_cmpint (fpi_fte3600_template_encode (decoded, &roundtrip), ==, FTE3600_TEMPLATE_OK);
  g_assert_cmpint (fpi_fte3600_template_encode (copy, &copied_wire), ==, FTE3600_TEMPLATE_OK);
  g_assert_true (g_bytes_equal (wire, roundtrip));
  g_assert_true (g_bytes_equal (wire, copied_wire));

  make_features (profile, &features, 0);
  g_assert_cmpint (fpi_fte3600_template_compare_features_for_profile (
                     decoded, profile, &features, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &result),
                   ==, FTE3600_TEMPLATE_OK);
  g_assert_cmpuint (result.n_compared, ==, 9);
  g_assert_cmpuint (result.diagnostic_passes, ==, 9);
  g_assert_false (result.authentication_accepted);
  memset (&result, 0xa5, sizeof result);
  g_assert_cmpint (fpi_fte3600_template_compare_features_for_profile (
                     decoded, profile, &features, FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &result),
                   ==, FTE3600_ENABLE_PERSONAL_AUTH ? FTE3600_TEMPLATE_OK : FTE3600_TEMPLATE_NOT_CALIBRATED);
  g_assert_cmpint (result.authentication_accepted, ==, FTE3600_ENABLE_PERSONAL_AUTH);
  if (!FTE3600_ENABLE_PERSONAL_AUTH)
    g_assert_cmpuint (result.n_compared, ==, 0);
  g_assert_cmpint (fpi_fte3600_template_decode (wire, FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &authenticated),
                   ==, FTE3600_ENABLE_PERSONAL_AUTH ? FTE3600_TEMPLATE_OK : FTE3600_TEMPLATE_NOT_CALIBRATED);
  if (FTE3600_ENABLE_PERSONAL_AUTH)
    assert_profile (fpi_fte3600_template_get_profile (authenticated), expected);
  else
    g_assert_null (authenticated);

  make_unrelated (&features);
  g_assert_cmpint (fpi_fte3600_template_compare_features_for_profile (
                     decoded, profile, &features, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &result),
                   ==, FTE3600_TEMPLATE_OK);
  g_assert_cmpuint (result.diagnostic_passes, ==, 0);
  g_assert_false (result.authentication_accepted);
  g_assert_cmpint (fpi_fte3600_template_compare_features_for_profile (
                     decoded, profile, &features, FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &result),
                   ==, FTE3600_ENABLE_PERSONAL_AUTH ? FTE3600_TEMPLATE_OK : FTE3600_TEMPLATE_NOT_CALIBRATED);
  g_assert_false (result.authentication_accepted);
}

static void
test_enrollment (gconstpointer user_data)
{
  const ExpectedProfile *expected = user_data;
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (expected->sensor);

  g_autoptr(Fte3600Template) templ = fpi_fte3600_template_new_for_profile (profile);
  Fte3600BriskFeatureSet features;
  Fte3600BriskMatchResult nearest;

  make_features (profile, &features, 0);
  g_assert_cmpint (fpi_fte3600_template_add_features (templ, &features, NULL),
                   ==, FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
  g_assert_cmpint (fpi_fte3600_template_add_features (templ, &features, NULL),
                   ==, FTE3600_TEMPLATE_RETRY_DUPLICATE);
  features.n_features = 10;
  g_assert_cmpint (fpi_fte3600_template_add_features (templ, &features, NULL),
                   ==, FTE3600_TEMPLATE_RETRY_INSUFFICIENT_FEATURES);
  make_features (profile, &features, 1);
  features.features[0].x = profile->width;
  g_assert_cmpint (fpi_fte3600_template_add_features (templ, &features, NULL),
                   ==, FTE3600_TEMPLATE_INVALID_WIRE);
  make_features (profile, &features, 1);
  features.features[0].y = profile->height;
  g_assert_cmpint (fpi_fte3600_template_add_features (templ, &features, NULL),
                   ==, FTE3600_TEMPLATE_INVALID_WIRE);
  make_features (profile, &features, 1);
  features.features[0].x = NAN;
  g_assert_cmpint (fpi_fte3600_template_add_features (templ, &features, NULL),
                   ==, FTE3600_TEMPLATE_INVALID_WIRE);
  make_features (profile, &features, 1);
  features.extractor_schema_version++;
  g_assert_cmpint (fpi_fte3600_template_add_features (templ, &features, NULL),
                   ==, FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR);
#if FTE3600_ENABLE_PERSONAL_AUTH
  make_features (profile, &features, 1);
  make_unrelated (&features);
  g_assert_cmpint (fpi_fte3600_template_add_features (templ, &features, &nearest),
                   ==, FTE3600_TEMPLATE_RETRY_INCONSISTENT);
  g_assert_false (nearest.authentication_accepted);
#endif
  /* Rejections above must not consume stages or spoil the accepted sample. */
  for (guint sample = 1; sample < 8; sample++)
    {
      make_features (profile, &features, sample);
      g_assert_cmpint (fpi_fte3600_template_add_features (templ, &features, &nearest), ==,
                       sample == 7 ? FTE3600_TEMPLATE_OK : FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
      g_assert_cmpint (nearest.authentication_accepted, ==, FTE3600_ENABLE_PERSONAL_AUTH);
      g_assert_cmpint (fpi_fte3600_template_is_ready (templ), ==, sample == 7);
    }
}

static void
assert_mutation_rejected (GBytes *wire, guint offset, guint32 value, gboolean wide)
{
  gsize size;
  const guint8 *data = g_bytes_get_data (wire, &size);
  guint8 *copy = g_memdup2 (data, size);

  g_autoptr(GBytes) changed = g_bytes_new_take (copy, size);
  Fte3600Template *decoded = (Fte3600Template *) 0x1;

  if (wide)
    write_u32 (copy + offset, value);
  else
    write_u16 (copy + offset, value);
  g_assert_cmpint (fpi_fte3600_template_decode (changed, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &decoded),
                   !=, FTE3600_TEMPLATE_OK);
  g_assert_null (decoded);
}

static void
test_invalid_wire (gconstpointer user_data)
{
  const ExpectedProfile *expected = user_data;
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (expected->sensor);

  g_autoptr(Fte3600Template) templ = ready_template (profile, FALSE);
  g_autoptr(GBytes) wire = NULL;
  guint32 float_bits;
  gfloat coordinate;

  g_assert_cmpint (fpi_fte3600_template_encode (templ, &wire), ==, FTE3600_TEMPLATE_OK);
  assert_mutation_rejected (wire, 8, 3, FALSE);
  assert_mutation_rejected (wire, 8, 1, FALSE);
  assert_mutation_rejected (wire, 16, 0xffff, FALSE);
  assert_mutation_rejected (wire, 18, expected->width + 1, FALSE);
  assert_mutation_rejected (wire, 20, expected->height + 1, FALSE);
  assert_mutation_rejected (wire, 22, 43, FALSE);
  assert_mutation_rejected (wire, 24, 2, FALSE);
  assert_mutation_rejected (wire, 26, 0xffff, FALSE);
  assert_mutation_rejected (wire, 26, 5, FALSE);
  assert_mutation_rejected (wire, 28, 0xffff, FALSE);
  assert_mutation_rejected (wire, 28, 6, FALSE);
  assert_mutation_rejected (wire, 30, 7, FALSE);
  assert_mutation_rejected (wire, 30, 9, FALSE);
  assert_mutation_rejected (wire, 32, 1, TRUE);
  assert_mutation_rejected (wire, 36, 0, TRUE);
  assert_mutation_rejected (wire, 36, 2, TRUE);
  assert_mutation_rejected (wire, 36, 0x00010001, TRUE);
  /* First feature starts after the 40-byte header and 8-byte record header. */
  coordinate = expected->width;
  memcpy (&float_bits, &coordinate, sizeof float_bits);
  assert_mutation_rejected (wire, 48, float_bits, TRUE);
  coordinate = expected->height;
  memcpy (&float_bits, &coordinate, sizeof float_bits);
  assert_mutation_rejected (wire, 52, float_bits, TRUE);
}

static void
test_identity_isolation (void)
{
  for (guint i = 0; i < G_N_ELEMENTS (expected_profiles); i++)
    {
      const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (expected_profiles[i].sensor);
      g_autoptr(Fte3600Template) templ = ready_template (profile, FALSE);
      Fte3600BriskFeatureSet features;
      Fte3600TemplateCompareResult result;

      make_features (profile, &features, 0);
      for (guint j = 0; j < G_N_ELEMENTS (expected_profiles); j++)
        {
          const Fte3600MatchProfile *other = fpi_fte3600_match_profile_get (expected_profiles[j].sensor);

          if (i == j)
            continue;
          memset (&result, 0xa5, sizeof result);
          g_assert_cmpint (fpi_fte3600_template_compare_features_for_profile (
                             templ, other, &features, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &result),
                           ==, FTE3600_TEMPLATE_INVALID_WIRE);
          g_assert_cmpuint (result.n_compared, ==, 0);
          g_assert_false (result.authentication_accepted);
          g_assert_cmpint (fpi_fte3600_template_compare_features_for_profile (
                             templ, other, &features, FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &result),
                           ==, FTE3600_TEMPLATE_INVALID_WIRE);
          g_assert_false (result.authentication_accepted);
        }
    }
}

static void
test_legacy_compatibility (void)
{
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (FTE3600_SENSOR_FT9361);

  g_autoptr(Fte3600Template) legacy = ready_template (profile, TRUE);
  g_autoptr(Fte3600Template) decoded = NULL;
  g_autoptr(Fte3600Template) modern = ready_template (profile, FALSE);
  g_autoptr(GBytes) old_wire = NULL;
  g_autoptr(GBytes) roundtrip = NULL;
  g_autoptr(GBytes) new_wire = NULL;
  Fte3600BriskFeatureSet query;
  Fte3600TemplateCompareResult result;
  const guint8 *data;

  g_assert_cmpint (fpi_fte3600_template_encode (legacy, &old_wire), ==, FTE3600_TEMPLATE_OK);
  data = g_bytes_get_data (old_wire, NULL);
  g_assert_cmpuint (read_u16 (data + 8), ==, 1);
  g_assert_cmphex (read_u16 (data + 16), ==, 0x9361);
  g_assert_cmpuint (read_u16 (data + 26), ==, 5);
  g_assert_cmpuint (read_u16 (data + 28), ==, FTE3600_ENABLE_PERSONAL_AUTH ? 6 : 0);
  g_assert_cmpuint (read_u32 (data + 36), ==, 0);
  g_assert_cmpint (fpi_fte3600_template_decode (old_wire, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &decoded),
                   ==, FTE3600_TEMPLATE_OK);
  g_assert_cmpint (fpi_fte3600_template_get_profile (decoded)->sensor, ==, FTE3600_SENSOR_FT9361);
  g_assert_cmpint (fpi_fte3600_template_encode (decoded, &roundtrip), ==, FTE3600_TEMPLATE_OK);
  g_assert_true (g_bytes_equal (old_wire, roundtrip));
  g_assert_cmpint (fpi_fte3600_template_encode (modern, &new_wire), ==, FTE3600_TEMPLATE_OK);
  g_assert_false (g_bytes_equal (old_wire, new_wire));
  make_features (profile, &query, 0);
  g_assert_cmpint (fpi_fte3600_template_compare_features_for_profile (
                     decoded, profile, &query, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &result),
                   ==, FTE3600_TEMPLATE_OK);
  g_assert_cmpuint (result.diagnostic_passes, >, 0);
  assert_mutation_rejected (old_wire, 16, 0x9365, FALSE);
  assert_mutation_rejected (old_wire, 8, 2, FALSE);
  assert_mutation_rejected (old_wire, 36, 1, TRUE);
}

static void
test_narrow_mosaic (void)
{
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (FTE3600_SENSOR_FT9769);

  g_autoptr(Fte3600Template) templ = ready_template (profile, FALSE);
  const Fte3600BriskFeatureSet *mosaic = fpi_fte3600_template_get_mosaic (templ);
  Fte3600BriskFeatureSet edge, query;
  Fte3600BriskMatchResult match;
  gboolean beyond_legacy_height = FALSE;

  g_assert_nonnull (mosaic);
  g_assert_true (fpi_fte3600_brisk_validate_mosaic_feature_set_for_profile (profile, mosaic, NULL));
  for (guint i = 0; i < mosaic->n_features; i++)
    {
      g_assert_cmpfloat (mosaic->features[i].x, >=, 0);
      g_assert_cmpfloat (mosaic->features[i].x, <, 120);
      g_assert_cmpfloat (mosaic->features[i].y, >=, 0);
      g_assert_cmpfloat (mosaic->features[i].y, <, 588);
      beyond_legacy_height |= mosaic->features[i].y >= 240;
    }
  g_assert_true (beyond_legacy_height);
  make_features (profile, &query, 0);
  g_assert_cmpint (fpi_fte3600_brisk_match_mosaic_for_profile (profile, &query, mosaic, &match),
                   ==, FTE3600_BRISK_OK);
  g_assert_true (match.diagnostic_policy_passed);

  /* A single feature can exercise exact canvas limits without the fixture's
   * physical-point grouping interfering with the boundary being tested. */
  memset (&edge, 0, sizeof edge);
  edge.extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
  edge.n_features = 1;
  edge.features[0].x = 119.5f;
  edge.features[0].y = 587.5f;
  g_assert_true (fpi_fte3600_brisk_validate_mosaic_feature_set_for_profile (profile, &edge, NULL));
  edge.features[0].x = 120;
  g_assert_false (fpi_fte3600_brisk_validate_mosaic_feature_set_for_profile (profile, &edge, NULL));
  edge.features[0].x = 119.5f;
  edge.features[0].y = 588;
  g_assert_false (fpi_fte3600_brisk_validate_mosaic_feature_set_for_profile (profile, &edge, NULL));
}

static void
test_degenerate_evidence (void)
{
  for (guint p = 0; p < G_N_ELEMENTS (expected_profiles); p++)
    for (guint shape = 0; shape < 3; shape++)
      {
        const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (expected_profiles[p].sensor);
        Fte3600BriskFeatureSet features;
        Fte3600BriskMatchResult match;
        Fte3600BriskStatus status;
        g_autoptr(Fte3600Template) templ = fpi_fte3600_template_new_for_profile (profile);

        make_features (profile, &features, 0);
        /* Exact diagonal, a narrow perturbed diagonal, and a line with one
         * outlier. Every point is distinct; descriptor consensus must not
         * turn deficient spatial evidence into an authentication decision. */
        for (guint i = 0; i < features.n_features; i++)
          {
            features.features[i].x = 3 + 2.5f * i;
            features.features[i].y = 10 + 3.0f * i;
            if (shape == 1)
              features.features[i].y += (i % 2) * 0.02f;
            if (shape == 2)
              features.features[i].y = 20 + (i == 11 ? 1.0f : 0);
          }
        g_assert_true (fpi_fte3600_brisk_validate_feature_set_for_profile (profile, &features, NULL));
        status = fpi_fte3600_brisk_match_for_profile (profile, &features, &features, &match);
        g_assert_true (status == FTE3600_BRISK_OK || status == FTE3600_BRISK_NO_CONSENSUS);
        if (status == FTE3600_BRISK_OK && shape == 2)
          g_assert_cmpfloat (match.query_min_variance, <, 3);
        g_assert_false (match.diagnostic_policy_passed);
        g_assert_false (match.authentication_accepted);
        g_assert_cmpint (fpi_fte3600_template_add_features (templ, &features, NULL),
                         ==, FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
        for (guint i = 0; i < features.n_features; i++)
          features.features[i].descriptor[0] ^= 1;
        g_assert_cmpint (fpi_fte3600_template_add_features (templ, &features, NULL), ==,
                         FTE3600_ENABLE_PERSONAL_AUTH ? FTE3600_TEMPLATE_RETRY_INCONSISTENT :
                         FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
        g_assert_false (fpi_fte3600_template_is_ready (templ));
      }
}

static void
test_covariance_geometry (gconstpointer user_data)
{
  const ExpectedProfile *expected = user_data;
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (expected->sensor);
  Fte3600BriskFeatureSet features;
  FpiBriskMatchEvidence evidence;
  Fte3600BriskMatchResult result;
  const gdouble dx = (expected->width - 1) * 0.28;
  const gdouble dy = (expected->height - 1) * 0.30;
  /* Closed-form population moments of four equally spaced x positions and
  * three equally spaced y positions; every Cartesian pair occurs once. */
  const gdouble variance_x = 1.25 * dx * dx;
  const gdouble variance_y = (2.0 / 3) * dy * dy;
  const gdouble aspect = (gdouble) MAX (expected->width, expected->height) /
                         MIN (expected->width, expected->height);
  const gdouble expected_ratio = MIN (1.0, MIN (variance_x, variance_y) /
                                      MAX (variance_x, variance_y) * aspect * aspect);

  make_features (profile, &features, 0);
  g_assert_cmpint (fpi_brisk_match (&features, expected->width, expected->height,
                                    &features, expected->width, expected->height, &evidence),
                   ==, FPI_BRISK_OK);
  g_assert_cmpuint (evidence.inliers, ==, 12);
  g_assert_cmpfloat_with_epsilon (evidence.query_covariance_xx, variance_x, 0.001);
  g_assert_cmpfloat_with_epsilon (evidence.query_covariance_yy, variance_y, 0.001);
  g_assert_cmpfloat_with_epsilon (evidence.query_covariance_xy, 0, 0.001);
  g_assert_cmpfloat_with_epsilon (evidence.reference_covariance_xx, variance_x, 0.001);
  g_assert_cmpfloat_with_epsilon (evidence.reference_covariance_yy, variance_y, 0.001);
  g_assert_cmpfloat_with_epsilon (evidence.reference_covariance_xy, 0, 0.001);
  g_assert_cmpint (fpi_fte3600_brisk_match_for_profile (profile, &features, &features, &result),
                   ==, FTE3600_BRISK_OK);
  g_assert_cmpfloat_with_epsilon (result.normalized_query_anisotropy, expected_ratio, 0.00001);
  g_assert_cmpfloat_with_epsilon (result.normalized_reference_anisotropy, expected_ratio, 0.00001);
  g_assert_cmpfloat_with_epsilon (result.query_max_variance, MAX (variance_x, variance_y), 0.001);
  g_assert_cmpfloat_with_epsilon (result.reference_max_variance, MAX (variance_x, variance_y), 0.001);
  /* Geometry normalization supplies only distribution shape. Raw variance
   * and geometric evidence remain in their original physical pixel units. */
  g_assert_cmpfloat (result.query_min_variance, ==, evidence.query_min_variance);
  g_assert_cmpfloat (result.reference_min_variance, ==, evidence.reference_min_variance);
  g_assert_cmpfloat (result.query_anisotropy, ==, evidence.query_anisotropy);
  g_assert_true (result.diagnostic_policy_passed);
}

/* These Cartesian products have exactly var(x)=5*a*a and var(y)=2*b*b/3.
 * Rotation changes neither eigenvalue; unique synthetic descriptors remove
 * ambiguity about which correspondences should supply the evidence. */
static void
make_principal_grid (const Fte3600MatchProfile *profile,
                     Fte3600BriskFeatureSet *features,
                     gdouble minor_variance, gdouble major_variance)
{
  const gdouble a = sqrt (minor_variance / 5.0);
  const gdouble b = sqrt (major_variance * 1.5);

  make_features (profile, features, 0);
  for (guint i = 0; i < features->n_features; i++)
    {
      features->features[i].x = profile->width / 2.0 + ((gint) (i % 4) * 2 - 3) * a;
      features->features[i].y = profile->height / 2.0 + ((gint) (i / 4) - 1) * b;
    }
}

static void
rotate_features (const Fte3600MatchProfile *profile,
                 const Fte3600BriskFeatureSet *source,
                 Fte3600BriskFeatureSet *destination,
                 gdouble angle, gboolean mosaic)
{
  const gdouble cx = profile->width / 2.0;
  const gdouble cy = profile->height / 2.0;

  *destination = *source;
  for (guint i = 0; i < source->n_features; i++)
    {
      const gdouble x = source->features[i].x - cx;
      const gdouble y = source->features[i].y - cy;

      destination->features[i].x = cx + cos (angle) * x - sin (angle) * y +
                                   (mosaic ? profile->width : 0);
      destination->features[i].y = cy + sin (angle) * x + cos (angle) * y +
                                   (mosaic ? profile->height : 0);
      destination->features[i].orientation = angle;
    }
}

static void
test_rotation_policy (gconstpointer user_data)
{
  const ExpectedProfile *expected = user_data;
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (expected->sensor);
  const gdouble aspect = (gdouble) MAX (profile->width, profile->height) /
                         MIN (profile->width, profile->height);

  const struct
  {
    gdouble  minor;
    gdouble  major;
    gboolean passes;
  } shapes[] = {
    { 7.2, 32.0, TRUE },              /* All angles fit every native image. */
    { 16.0, 400.0, TRUE },            /* Original FT9769 30-degree failure. */
    { 2.9, 12.0, FALSE },             /* Below minimum minor variance. */
    { 3.1, 5.2, FALSE },              /* Below major equivalent spread. */
    { 3.1, 5.5, TRUE },               /* Above both spread boundaries. */
    { 3.2, 3.2 * aspect * aspect / 0.029, FALSE },
    { 3.2, 3.2 * aspect * aspect / 0.031, TRUE },
  };

  for (guint shape = 0; shape < G_N_ELEMENTS (shapes); shape++)
    {
      Fte3600BriskFeatureSet query;
      guint direct_count = 0;
      guint mosaic_count = 0;
      const gdouble expected_ratio = MIN (1.0, shapes[shape].minor /
                                          shapes[shape].major * aspect * aspect);

      make_principal_grid (profile, &query, shapes[shape].minor, shapes[shape].major);
      g_assert_true (fpi_fte3600_brisk_validate_feature_set_for_profile (profile, &query, NULL));
      for (gint degrees = -175; degrees <= 180; degrees += 5)
        for (guint canvas = 0; canvas < 2; canvas++)
          {
            Fte3600BriskFeatureSet rotated;
            Fte3600BriskMatchResult result;
            const gboolean mosaic = canvas != 0;
            const gdouble angle = degrees * G_PI / 180.0;
            gboolean valid;

            rotate_features (profile, &query, &rotated, angle, mosaic);
            valid = mosaic ? fpi_fte3600_brisk_validate_mosaic_feature_set_for_profile (profile, &rotated, NULL) :
                    fpi_fte3600_brisk_validate_feature_set_for_profile (profile, &rotated, NULL);
            /* A long native image cannot contain its horizontal rotation.
             * Do not clip points or silently change the feature population. */
            if (!valid)
              continue;
            g_assert_cmpint (mosaic ? fpi_fte3600_brisk_match_mosaic_for_profile (profile, &query, &rotated, &result) :
                             fpi_fte3600_brisk_match_for_profile (profile, &query, &rotated, &result),
                             ==, FTE3600_BRISK_OK);
            g_assert_cmpuint (result.inliers, ==, 12);
            g_assert_cmpuint (result.competing_inliers, ==, 0);
            g_assert_cmpfloat (result.rms_error, <, 0.0001);
            g_assert_cmpfloat_with_epsilon (result.normalized_query_anisotropy, expected_ratio, 0.00001);
            g_assert_cmpfloat_with_epsilon (result.normalized_reference_anisotropy, expected_ratio, 0.00001);
            g_assert_cmpint (result.diagnostic_policy_passed, ==, shapes[shape].passes);
            g_assert_cmpint (result.authentication_accepted, ==,
                             shapes[shape].passes && FTE3600_ENABLE_PERSONAL_AUTH);
            if (mosaic)
              {
                mosaic_count++;
              }
            else
              {
                direct_count++;
                /* The query-side grid/spans used to make the policy
                 * direction-dependent as well as rotation-dependent. */
                g_assert_cmpint (fpi_fte3600_brisk_match_for_profile (profile, &rotated, &query, &result),
                                 ==, FTE3600_BRISK_OK);
                g_assert_cmpint (result.diagnostic_policy_passed, ==, shapes[shape].passes);
              }
          }
      g_assert_cmpuint (direct_count, >, 0);
      g_assert_cmpuint (mosaic_count, >, 0);
      if (shape == 0)
        {
          g_assert_cmpuint (direct_count, ==, 72);
          g_assert_cmpuint (mosaic_count, ==, 72);
        }
    }
}

static void
test_retired_family_policy (void)
{
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (FTE3600_SENSOR_FT9361);

  g_autoptr(Fte3600Template) templ = ready_template (profile, FALSE);
  g_autoptr(GBytes) wire = NULL;
  gsize size;
  const guint8 *data;

  g_assert_cmpint (fpi_fte3600_template_encode (templ, &wire), ==, FTE3600_TEMPLATE_OK);
  data = g_bytes_get_data (wire, &size);
  for (guint purpose = FTE3600_TEMPLATE_LOAD_DIAGNOSTIC;
       purpose <= FTE3600_TEMPLATE_LOAD_AUTHENTICATION; purpose++)
    {
      guint8 *copy = g_memdup2 (data, size);
      g_autoptr(GBytes) retired = g_bytes_new_take (copy, size);
      Fte3600Template *decoded = (Fte3600Template *) 0x1;

      write_u16 (copy + 26, 6);
      write_u16 (copy + 28, FTE3600_ENABLE_PERSONAL_AUTH ? 7 : 0);
      g_assert_cmpint (fpi_fte3600_template_decode (retired, purpose, &decoded),
                       ==, FTE3600_TEMPLATE_UNSUPPORTED_POLICY);
      g_assert_null (decoded);
    }
}

/* An independently encoded mathematical fixture exercises reconstruction on
 * decode in both policy builds, including templates whose components cannot
 * be joined under the current full quality gate. Features are emitted in
 * x/y order and samples in increasing feature count and x offset. */
static GBytes *
make_weak_connection_wire (const Fte3600MatchProfile    *profile,
                           const Fte3600BriskFeatureSet *anchor,
                           const Fte3600BriskFeatureSet *weak)
{
  const gsize total = 40 + 8 + 12 * 44 + 7 * (8 + 13 * 44);
  guint8 *data = g_malloc0 (total);
  gsize offset = 40;

  memcpy (data, "FT36BRK\0", 8);
  write_u16 (data + 8, 2);
  write_u16 (data + 10, 40);
  write_u32 (data + 12, total);
  write_u16 (data + 16, profile->model);
  write_u16 (data + 18, profile->width);
  write_u16 (data + 20, profile->height);
  write_u16 (data + 22, 44);
  write_u16 (data + 24, 3);
  write_u16 (data + 26, 7);
  write_u16 (data + 28, FTE3600_ENABLE_PERSONAL_AUTH ? 8 : 0);
  write_u16 (data + 30, 8);
  write_u32 (data + 36, 1);
  for (guint sample = 0; sample < 8; sample++)
    {
      const Fte3600BriskFeatureSet *features = sample ? weak : anchor;

      write_u32 (data + offset, 8 + features->n_features * 44);
      write_u16 (data + offset + 4, features->n_features);
      write_u16 (data + offset + 6, features->n_features);
      offset += 8;
      /* make_features uses row order; visit columns first for canonical x/y. */
      for (guint record = 0; record < features->n_features; record++)
        {
          const guint i = record < 12 ? (record % 3) * 4 + record / 3 : 12;
          const Fte3600BriskFeature *feature = &features->features[i];
          const gfloat fields[] = { feature->x + sample * 0.125f, feature->y, 0 };

          for (guint f = 0; f < 3; f++)
            {
              guint32 bits;

              memcpy (&bits, &fields[f], sizeof bits);
              write_u32 (data + offset + f * 4, bits);
            }
          memcpy (data + offset + 12, feature->descriptor, 32);
          offset += 44;
        }
    }
  g_assert_cmpuint (offset, ==, total);
  return g_bytes_new_take (data, total);
}

static void
test_mosaic_requires_quality (void)
{
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (FTE3600_SENSOR_FT9361);
  Fte3600BriskFeatureSet anchor, weak;
  Fte3600BriskMatchResult result;

  g_autoptr(Fte3600Template) decoded = NULL;
  g_autoptr(Fte3600Template) enrollment = fpi_fte3600_template_new_for_profile (profile);
  g_autoptr(GBytes) wire = NULL;
  g_autoptr(GBytes) roundtrip = NULL;
  const Fte3600BriskFeatureSet *mosaic;

  make_features (profile, &anchor, 0);
  weak = anchor;
  for (guint i = 0; i < weak.n_features; i++)
    {
      /* 63 bits pass the correspondence limit (64) but fail the complete
      * decision's mean-Hamming limit (60), independently of geometry. */
      for (guint byte = 0; byte < 7; byte++)
        weak.features[i].descriptor[byte] ^= 0xff;
      weak.features[i].descriptor[7] ^= 0x7f;
    }
  weak.n_features = 13;
  weak.features[12].x = profile->width - 2;
  weak.features[12].y = profile->height / 2;
  memset (weak.features[12].descriptor, 0x96, 32);
  g_assert_cmpint (fpi_fte3600_brisk_match_for_profile (profile, &weak, &anchor, &result),
                   ==, FTE3600_BRISK_OK);
  g_assert_cmpuint (result.inliers, ==, 12);
  g_assert_cmpfloat (result.mean_hamming, ==, 63);
  g_assert_false (result.diagnostic_policy_passed);
  g_assert_false (result.authentication_accepted);
  g_assert_cmpint (fpi_fte3600_template_add_features (enrollment, &anchor, NULL),
                   ==, FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
  g_assert_cmpint (fpi_fte3600_template_add_features (enrollment, &weak, NULL), ==,
                   FTE3600_ENABLE_PERSONAL_AUTH ? FTE3600_TEMPLATE_RETRY_INCONSISTENT :
                   FTE3600_TEMPLATE_NEED_MORE_SAMPLES);

  wire = make_weak_connection_wire (profile, &anchor, &weak);
  g_assert_cmpint (fpi_fte3600_template_decode (wire, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &decoded),
                   ==, FTE3600_TEMPLATE_OK);
  mosaic = fpi_fte3600_template_get_mosaic (decoded);
  g_assert_nonnull (mosaic);
  g_assert_cmpuint (mosaic->n_features, ==, anchor.n_features);
  for (guint i = 0; i < mosaic->n_features; i++)
    g_assert_cmpint (memcmp (mosaic->features[i].descriptor, weak.features[12].descriptor, 32), !=, 0);
  g_assert_cmpint (fpi_fte3600_template_encode (decoded, &roundtrip), ==, FTE3600_TEMPLATE_OK);
  g_assert_true (g_bytes_equal (wire, roundtrip));
}

static void
test_modern_rounding (gconstpointer user_data)
{
  const ExpectedProfile *expected = user_data;
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (expected->sensor);
  const gint modes[] = { FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
  const gint original_mode = fegetround ();
  Fte3600BriskFeatureSet features;
  Fte3600BriskMatchResult reference, reference_mosaic, actual;
  Fte3600TemplateCompareResult template_reference, template_actual;

  g_autoptr(Fte3600Template) templ = NULL;
  const Fte3600BriskFeatureSet *mosaic;

  g_assert_cmpint (original_mode, !=, -1);
  g_assert_cmpint (fesetround (FE_TONEAREST), ==, 0);
  make_features (profile, &features, 0);
  templ = ready_template (profile, FALSE);
  mosaic = fpi_fte3600_template_get_mosaic (templ);
  g_assert_nonnull (mosaic);
  g_assert_cmpint (fpi_fte3600_brisk_match_for_profile (profile, &features, &features, &reference),
                   ==, FTE3600_BRISK_OK);
  g_assert_cmpint (fpi_fte3600_brisk_match_mosaic_for_profile (profile, &features, mosaic, &reference_mosaic),
                   ==, FTE3600_BRISK_OK);
  g_assert_cmpint (fpi_fte3600_template_compare_features_for_profile (
                     templ, profile, &features, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &template_reference),
                   ==, FTE3600_TEMPLATE_OK);
  g_assert_cmpfloat (reference.normalized_query_anisotropy, !=, reference.query_anisotropy);
  for (guint i = 0; i < G_N_ELEMENTS (modes); i++)
    {
      g_assert_cmpint (fesetround (modes[i]), ==, 0);
      g_assert_cmpint (fpi_fte3600_brisk_match_for_profile (profile, &features, &features, &actual),
                       ==, FTE3600_BRISK_OK);
      g_assert_cmpint (fegetround (), ==, modes[i]);
      g_assert_cmpmem (&actual, sizeof actual, &reference, sizeof reference);
      g_assert_cmpint (fpi_fte3600_brisk_match_mosaic_for_profile (profile, &features, mosaic, &actual),
                       ==, FTE3600_BRISK_OK);
      g_assert_cmpint (fegetround (), ==, modes[i]);
      g_assert_cmpmem (&actual, sizeof actual, &reference_mosaic, sizeof reference_mosaic);
      g_assert_cmpint (fpi_fte3600_template_compare_features_for_profile (
                         templ, profile, &features, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &template_actual),
                       ==, FTE3600_TEMPLATE_OK);
      g_assert_cmpint (fegetround (), ==, modes[i]);
      g_assert_cmpmem (&template_actual, sizeof template_actual,
                       &template_reference, sizeof template_reference);
    }
  g_assert_cmpint (fesetround (original_mode), ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/fte3600/family-template/profiles", test_profile_registry);
  g_test_add_func ("/fte3600/family-template/identity-isolation", test_identity_isolation);
  g_test_add_func ("/fte3600/family-template/legacy-v1", test_legacy_compatibility);
  g_test_add_func ("/fte3600/family-template/ft9769-mosaic", test_narrow_mosaic);
  g_test_add_func ("/fte3600/family-template/degenerate-evidence", test_degenerate_evidence);
  g_test_add_func ("/fte3600/family-template/retired-policy", test_retired_family_policy);
  g_test_add_func ("/fte3600/family-template/mosaic-quality", test_mosaic_requires_quality);
  for (guint i = 0; i < G_N_ELEMENTS (expected_profiles); i++)
    {
      const ExpectedProfile *profile = &expected_profiles[i];
      g_autofree gchar *extract = g_strdup_printf ("/fte3600/family-template/%04x/extract", profile->model);
      g_autofree gchar *codec = g_strdup_printf ("/fte3600/family-template/%04x/roundtrip-policy", profile->model);
      g_autofree gchar *enroll = g_strdup_printf ("/fte3600/family-template/%04x/enrollment", profile->model);
      g_autofree gchar *invalid = g_strdup_printf ("/fte3600/family-template/%04x/invalid-wire", profile->model);
      g_autofree gchar *covariance = g_strdup_printf ("/fte3600/family-template/%04x/covariance", profile->model);
      g_autofree gchar *rotation = g_strdup_printf ("/fte3600/family-template/%04x/rotation", profile->model);

      g_test_add_data_func (extract, profile, test_extract);
      g_test_add_data_func (codec, profile, test_roundtrip_and_policy);
      g_test_add_data_func (enroll, profile, test_enrollment);
      g_test_add_data_func (invalid, profile, test_invalid_wire);
      g_test_add_data_func (covariance, profile, test_covariance_geometry);
      g_test_add_data_func (rotation, profile, test_rotation_policy);
      if (profile->sensor == FTE3600_SENSOR_FT9361 || profile->sensor == FTE3600_SENSOR_FT9769)
        {
          g_autofree gchar *rounding = g_strdup_printf ("/fte3600/family-template/%04x/rounding", profile->model);

          g_test_add_data_func (rounding, profile, test_modern_rounding);
        }
    }
  return g_test_run ();
}
