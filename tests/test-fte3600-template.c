/*
 * Unit tests for the FTE3600 BRISK wire-template container
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <fenv.h>
#include <glib.h>
#include <math.h>
#include <string.h>

#include "../libfprint/drivers/fte3600-template.h"

#define TEST_FEATURES 12
#define TEST_RECORD_SIZE (8 + TEST_FEATURES * FTE3600_TEMPLATE_FEATURE_RECORD_SIZE)
#define TEST_WIRE_SIZE \
  (FTE3600_TEMPLATE_WIRE_HEADER_SIZE + \
   FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES * \
   TEST_RECORD_SIZE)

static guint16
read_u16 (const guint8 *data)
{
  return (guint16) data[0] | (guint16) data[1] << 8;
}

static guint32
read_u32 (const guint8 *data)
{
  return (guint32) data[0] |
         (guint32) data[1] << 8 |
         (guint32) data[2] << 16 |
         (guint32) data[3] << 24;
}

static void
write_u16 (guint8 *data,
           guint16 value)
{
  data[0] = value & 0xff;
  data[1] = value >> 8;
}

static void
write_u32 (guint8 *data,
           guint32 value)
{
  data[0] = value & 0xff;
  data[1] = (value >> 8) & 0xff;
  data[2] = (value >> 16) & 0xff;
  data[3] = value >> 24;
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
fill_descriptor (guint8 *descriptor,
                 guint   feature,
                 guint   sample)
{
  guint32 state = 0x9e3779b9u ^ (feature + 1) * 0x45d9f3bu;

  for (guint i = 0; i < FTE3600_BRISK_DESCRIPTOR_BYTES; i++)
    descriptor[i] = xorshift32 (&state) >> 24;
  descriptor[0] ^= sample;
}

static void
make_feature_set_n (Fte3600BriskFeatureSet *features,
                    guint                   sample,
                    guint                   n_features,
                    gboolean                reverse)
{
  memset (features, 0, sizeof (*features));
  features->extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
  features->n_features = n_features;
  for (guint output = 0; output < n_features; output++)
    {
      const guint logical = reverse ? n_features - output - 1 : output;
      Fte3600BriskFeature *feature = &features->features[output];

      if (n_features == TEST_FEATURES)
        {
          feature->x = 8.0f + 12.0f * (logical % 4);
          feature->y = 10.0f + 25.0f * (logical / 4);
        }
      else
        {
          feature->x = 2.0f + 3.0f * (logical % 20);
          feature->y = 2.0f + 4.0f * (logical / 20);
        }
      feature->orientation = 0.0f;
      fill_descriptor (feature->descriptor, logical, sample);
    }
}

static void
make_feature_set (Fte3600BriskFeatureSet *features,
                  guint                   sample,
                  gboolean                reverse)
{
  make_feature_set_n (features, sample, TEST_FEATURES, reverse);
}

static Fte3600Template *
make_ready_template (gboolean reverse)
{
  Fte3600Template *templ = fpi_fte3600_template_new ();

  for (guint sample = 0;
       sample < FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES; sample++)
    {
      Fte3600BriskFeatureSet features;
      const Fte3600TemplateStatus expected =
        sample + 1 == FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES ?
        FTE3600_TEMPLATE_OK : FTE3600_TEMPLATE_NEED_MORE_SAMPLES;

      make_feature_set (&features, sample, reverse && (sample & 1));
      g_assert_cmpint (fpi_fte3600_template_add_features (templ, &features, NULL),
                       ==, expected);
    }
  g_assert_true (fpi_fte3600_template_is_ready (templ));
  return templ;
}

static GBytes *
mutable_copy (GBytes  *wire,
              guint8 **data_out,
              gsize   *size_out)
{
  gsize size;
  const guint8 *data = g_bytes_get_data (wire, &size);
  guint8 *copy = g_memdup2 (data, size);

  if (data_out != NULL)
    *data_out = copy;
  if (size_out != NULL)
    *size_out = size;
  return g_bytes_new_take (copy, size);
}

static Fte3600TemplateStatus
decode_status (GBytes *wire)
{
  Fte3600Template *decoded = (Fte3600Template *) 0x1;
  const Fte3600TemplateStatus status =
    fpi_fte3600_template_decode (wire, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC,
                                 &decoded);

  if (status == FTE3600_TEMPLATE_OK)
    fpi_fte3600_template_free (decoded);
  else
    g_assert_null (decoded);
  return status;
}

static void
test_validate_feature_set (void)
{
  Fte3600BriskFeatureSet features = { 0 };
  guint physical_count = 99;

  features.extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
  features.n_features = 4;
  features.features[0].x = 1.0f;
  features.features[1].x = 2.4f;
  features.features[2].x = 3.8f;
  features.features[3].x = 10.0f;
  for (guint i = 0; i < features.n_features; i++)
    features.features[i].y = 5.0f;

  g_assert_true (fpi_fte3600_brisk_validate_feature_set (&features,
                                                         &physical_count));
  g_assert_cmpuint (physical_count, ==, 2);
  features.features[0].orientation = (gfloat) G_PI;
  g_assert_true (fpi_fte3600_brisk_validate_feature_set (&features, NULL));
  features.features[0].orientation = -(gfloat) G_PI;
  g_assert_true (fpi_fte3600_brisk_validate_feature_set (&features, NULL));
  features.features[0].x = NAN;
  g_assert_false (fpi_fte3600_brisk_validate_feature_set (&features,
                                                          &physical_count));
  g_assert_cmpuint (physical_count, ==, 0);
}

static void
test_roundtrip_and_header (void)
{
  g_autoptr(Fte3600Template) original = make_ready_template (TRUE);
  g_autoptr(Fte3600Template) reverse_samples = fpi_fte3600_template_new ();
  g_autoptr(Fte3600Template) decoded = NULL;
  g_autoptr(Fte3600Template) copy = NULL;
  g_autoptr(GBytes) wire = NULL;
  g_autoptr(GBytes) roundtrip = NULL;
  g_autoptr(GBytes) copied_wire = NULL;
  g_autoptr(GBytes) reverse_wire = NULL;
  const guint8 *data;
  gsize size;

  g_assert_cmpint (fpi_fte3600_template_encode (original, &wire), ==,
                   FTE3600_TEMPLATE_OK);
  data = g_bytes_get_data (wire, &size);
  g_assert_cmpuint (size, ==, TEST_WIRE_SIZE);
  g_assert_cmpmem (data, 8, "FT36BRK\0", 8);
  g_assert_cmpuint (read_u16 (&data[8]), ==, 1);
  g_assert_cmpuint (read_u16 (&data[10]), ==, 40);
  g_assert_cmpuint (read_u32 (&data[12]), ==, size);
  g_assert_cmphex (read_u16 (&data[16]), ==, 0x9361);
  g_assert_cmpuint (read_u16 (&data[18]), ==, 64);
  g_assert_cmpuint (read_u16 (&data[20]), ==, 80);
  g_assert_cmpuint (read_u16 (&data[22]), ==, 44);
  g_assert_cmpuint (read_u16 (&data[24]), ==,
                    FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION);
  g_assert_cmpuint (read_u16 (&data[26]), ==,
                    FTE3600_BRISK_DIAGNOSTIC_POLICY_VERSION);
  g_assert_cmpuint (read_u16 (&data[28]), ==,
                    FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION);
  g_assert_cmpuint (read_u16 (&data[30]), ==, 8);
  {
    /* Freeze the wire policy identity independently of the header macros. */
    const guint8 version_fields[8] = {
      3, 0, 5, 0, FTE3600_ENABLE_PERSONAL_AUTH ? 6 : 0, 0, 8, 0
    };

    g_assert_cmpmem (&data[24], sizeof (version_fields),
                     version_fields, sizeof (version_fields));
  }
  g_assert_cmpuint (read_u32 (&data[32]), ==, 0);
  g_assert_cmpuint (read_u32 (&data[36]), ==, 0);
  g_assert_cmpuint (read_u32 (&data[40]), ==, TEST_RECORD_SIZE);
  g_assert_cmpuint (read_u16 (&data[44]), ==, TEST_FEATURES);
  g_assert_cmpuint (read_u16 (&data[46]), ==, TEST_FEATURES);
  /* First canonical x coordinate is 8.0f: IEEE-754 0x41000000, little endian. */
  g_assert_cmphex (read_u32 (&data[48]), ==, 0x41000000u);

  g_assert_cmpint (fpi_fte3600_template_decode (wire,
                                                FTE3600_TEMPLATE_LOAD_DIAGNOSTIC,
                                                &decoded), ==,
                   FTE3600_TEMPLATE_OK);
  g_assert_true (fpi_fte3600_template_is_ready (decoded));
  g_assert_cmpint (fpi_fte3600_template_encode (decoded, &roundtrip), ==,
                   FTE3600_TEMPLATE_OK);
  g_assert_true (g_bytes_equal (wire, roundtrip));

  copy = fpi_fte3600_template_copy (decoded);
  g_assert_nonnull (copy);
  g_assert_cmpint (fpi_fte3600_template_encode (copy, &copied_wire), ==,
                   FTE3600_TEMPLATE_OK);
  g_assert_true (g_bytes_equal (wire, copied_wire));
  g_assert_null (fpi_fte3600_template_copy (NULL));

  for (guint added = 0;
       added < FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES; added++)
    {
      Fte3600BriskFeatureSet features;
      const guint sample = FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES - added - 1;

      make_feature_set (&features, sample, added & 1);
      g_assert_cmpint (fpi_fte3600_template_add_features (
                         reverse_samples, &features, NULL), ==,
                       added + 1 == FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES ?
                       FTE3600_TEMPLATE_OK :
                       FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
    }
  g_assert_cmpint (fpi_fte3600_template_encode (reverse_samples, &reverse_wire),
                   ==, FTE3600_TEMPLATE_OK);
  g_assert_true (g_bytes_equal (wire, reverse_wire));
}

static void
test_enrollment_validation (void)
{
  g_autoptr(Fte3600Template) templ = fpi_fte3600_template_new ();
  Fte3600BriskFeatureSet first;
  Fte3600BriskFeatureSet related;
  Fte3600BriskFeatureSet reordered;
  Fte3600BriskFeatureSet unrelated;
  Fte3600BriskFeatureSet insufficient;
  Fte3600BriskMatchResult nearest;

  make_feature_set (&first, 0, FALSE);
  make_feature_set (&reordered, 0, TRUE);
  g_assert_cmpint (fpi_fte3600_template_add_features (templ, &first, &nearest), ==,
                   FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
  g_assert_cmpuint (nearest.mutual_matches, ==, 0);
  g_assert_cmpint (fpi_fte3600_template_add_features (templ, &reordered, &nearest),
                   ==, FTE3600_TEMPLATE_RETRY_DUPLICATE);

  make_feature_set (&related, 1, FALSE);
  g_assert_cmpint (fpi_fte3600_template_add_features (templ, &related, &nearest),
                   ==, FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
#if FTE3600_ENABLE_PERSONAL_AUTH
  g_assert_true (nearest.authentication_accepted);
#endif

  /* Personal authentication builds must not turn an unrelated sample into an
   * independent identity in the any-of-eight gallery.  Default builds retain
   * the diagnostic-only container behavior. */
  make_feature_set (&unrelated, 29, FALSE);
  for (guint i = 0; i < unrelated.n_features; i++)
    for (guint j = 0; j < FTE3600_BRISK_DESCRIPTOR_BYTES; j++)
      unrelated.features[i].descriptor[j] ^= 0xa5u;
  g_assert_cmpint (fpi_fte3600_template_add_features (templ, &unrelated, &nearest),
#if FTE3600_ENABLE_PERSONAL_AUTH
                   ==, FTE3600_TEMPLATE_RETRY_INCONSISTENT);
#else
                   ==, FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
#endif

#if FTE3600_ENABLE_PERSONAL_AUTH
  /* A rejected sample must not consume an enrollment stage or mutate the
   * template: the six remaining related samples still finish exactly at 8. */
  for (guint sample = 2; sample < FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES;
       sample++)
    {
      make_feature_set (&related, sample, sample & 1);
      g_assert_cmpint (fpi_fte3600_template_add_features (templ, &related, NULL),
                       ==, sample + 1 ==
                       FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES ?
                       FTE3600_TEMPLATE_OK :
                       FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
    }
  g_assert_true (fpi_fte3600_template_is_ready (templ));
#endif

  make_feature_set (&insufficient, 3, FALSE);
  insufficient.n_features = 10;
  g_assert_cmpint (fpi_fte3600_template_add_features (templ, &insufficient, NULL),
                   ==, FTE3600_TEMPLATE_RETRY_INSUFFICIENT_FEATURES);
  insufficient = first;
  insufficient.extractor_schema_version++;
  g_assert_cmpint (fpi_fte3600_template_add_features (templ, &insufficient, NULL),
                   ==, FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR);
}

static void
test_location_canonical_rules (void)
{
  g_autoptr(Fte3600Template) templ = fpi_fte3600_template_new ();
  Fte3600BriskFeatureSet features;

  make_feature_set (&features, 0, FALSE);
  features.features[1].x = features.features[0].x + 1.0f;
  features.features[1].y = features.features[0].y;
  g_assert_cmpint (fpi_fte3600_template_add_features (templ, &features, NULL), ==,
                   FTE3600_TEMPLATE_INVALID_WIRE);

  make_feature_set (&features, 0, FALSE);
  features.features[1].x = features.features[0].x;
  features.features[1].y = features.features[0].y;
  features.features[1].orientation = features.features[0].orientation;
  g_assert_cmpint (fpi_fte3600_template_add_features (templ, &features, NULL), ==,
                   FTE3600_TEMPLATE_INVALID_WIRE);

  make_feature_set (&features, 1, FALSE);
  features.n_features = TEST_FEATURES + 35;
  for (guint i = TEST_FEATURES; i < features.n_features; i++)
    {
      features.features[i] = features.features[0];
      features.features[i].orientation = -3.0f +
                                         0.16f * (i - TEST_FEATURES);
      fill_descriptor (features.features[i].descriptor, i, 1);
    }
  g_assert_cmpint (fpi_fte3600_template_add_features (templ, &features, NULL), ==,
                   FTE3600_TEMPLATE_NEED_MORE_SAMPLES);

  features.features[features.n_features] = features.features[0];
  features.features[features.n_features].orientation = 2.75f;
  features.n_features++;
  g_assert_cmpint (fpi_fte3600_template_add_features (templ, &features, NULL), ==,
                   FTE3600_TEMPLATE_INVALID_WIRE);
}

static void
test_authentication_policy (void)
{
  g_autoptr(Fte3600Template) templ = make_ready_template (FALSE);
  g_autoptr(GBytes) wire = NULL;
  g_autoptr(Fte3600Template) decoded = NULL;
  Fte3600BriskFeatureSet query;
  Fte3600TemplateCompareResult result;

  make_feature_set (&query, 0, FALSE);
  g_assert_cmpint (fpi_fte3600_template_compare_features (
                     templ, &query, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC,
                     &result), ==, FTE3600_TEMPLATE_OK);
  g_assert_cmpuint (result.n_compared, ==,
                    FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES + 1);
  g_assert_cmpuint (result.diagnostic_passes, >=, 1);
  g_assert_cmpuint (result.best_subtemplate, <,
                    FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES);
  g_assert_false (result.authentication_accepted);

  memset (&result, 0xa5, sizeof (result));
#if FTE3600_ENABLE_PERSONAL_AUTH
  g_assert_cmpint (fpi_fte3600_template_compare_features (
                     templ, &query, FTE3600_TEMPLATE_LOAD_AUTHENTICATION,
                     &result), ==, FTE3600_TEMPLATE_OK);
  g_assert_true (result.authentication_accepted);
  g_assert_cmpuint (result.n_compared, ==,
                    FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES + 1);
#else
  g_assert_cmpint (fpi_fte3600_template_compare_features (
                     templ, &query, FTE3600_TEMPLATE_LOAD_AUTHENTICATION,
                     &result), ==, FTE3600_TEMPLATE_NOT_CALIBRATED);
  g_assert_false (result.authentication_accepted);
  g_assert_cmpuint (result.n_compared, ==, 0);
#endif

  g_assert_cmpint (fpi_fte3600_template_encode (templ, &wire), ==,
                   FTE3600_TEMPLATE_OK);
#if FTE3600_ENABLE_PERSONAL_AUTH
  g_assert_cmpint (fpi_fte3600_template_decode (
                     wire, FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &decoded),
                   ==, FTE3600_TEMPLATE_OK);
  g_assert_nonnull (decoded);
#else
  g_assert_cmpint (fpi_fte3600_template_decode (
                     wire, FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &decoded),
                   ==, FTE3600_TEMPLATE_NOT_CALIBRATED);
  g_assert_null (decoded);
#endif
}

static void
assert_header_mutation (GBytes               *original,
                        gsize                 offset,
                        guint16               value,
                        Fte3600TemplateStatus expected)
{
  guint8 *data;
  gsize size;

  g_autoptr(Fte3600Template) decoded = NULL;

  g_autoptr(GBytes) changed = mutable_copy (original, &data, &size);

  g_assert_cmpuint (offset + 2, <=, size);
  write_u16 (&data[offset], value);
  g_assert_cmpint (decode_status (changed), ==, expected);
  g_assert_cmpint (fpi_fte3600_template_decode (
                     changed, FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &decoded),
                   ==, expected);
  g_assert_null (decoded);
}

static void
test_malformed_headers_and_lengths (void)
{
  g_autoptr(Fte3600Template) templ = make_ready_template (FALSE);
  g_autoptr(GBytes) wire = NULL;
  const guint8 *original;
  gsize original_size;

  g_assert_cmpint (fpi_fte3600_template_encode (templ, &wire), ==,
                   FTE3600_TEMPLATE_OK);
  original = g_bytes_get_data (wire, &original_size);

  assert_header_mutation (wire, 8, 3, FTE3600_TEMPLATE_INVALID_WIRE);
  assert_header_mutation (wire, 8, 4, FTE3600_TEMPLATE_INVALID_WIRE);
  assert_header_mutation (wire, 8, 5, FTE3600_TEMPLATE_UNSUPPORTED_SCHEMA);
  assert_header_mutation (wire, 10, 38, FTE3600_TEMPLATE_INVALID_WIRE);
  /* Both old raw and potentially double-normalized templates must be rejected. */
  assert_header_mutation (wire, 24, 1,
                          FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR);
  assert_header_mutation (wire, 24, 2,
                          FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR);
  assert_header_mutation (wire, 24, 4,
                          FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR);
  /* Older gallery policies and Pocket's distinct policy cannot be reused. */
  for (guint version = 1; version < 5; version++)
    assert_header_mutation (wire, 26, version,
                            FTE3600_TEMPLATE_UNSUPPORTED_POLICY);
  assert_header_mutation (wire, 28,
                          !FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION,
                          FTE3600_TEMPLATE_UNSUPPORTED_POLICY);
  for (guint version = 1; version < 6; version++)
    assert_header_mutation (wire, 28, version,
                            FTE3600_TEMPLATE_UNSUPPORTED_POLICY);
  assert_header_mutation (wire, 30, 7, FTE3600_TEMPLATE_INVALID_WIRE);

  {
    guint8 *data;
    gsize size;
    g_autoptr(GBytes) changed = mutable_copy (wire, &data, &size);

    data[0] ^= 1;
    g_assert_cmpint (decode_status (changed), ==,
                     FTE3600_TEMPLATE_INVALID_WIRE);
  }
  {
    guint8 *data;
    gsize size;
    g_autoptr(GBytes) changed = mutable_copy (wire, &data, &size);

    write_u32 (&data[12], size - 1);
    g_assert_cmpint (decode_status (changed), ==,
                     FTE3600_TEMPLATE_INVALID_WIRE);
  }
  {
    g_autoptr(GBytes) short_wire = g_bytes_new (original, original_size - 1);

    g_assert_cmpint (decode_status (short_wire), ==,
                     FTE3600_TEMPLATE_INVALID_WIRE);
  }
  {
    guint8 *data = g_malloc (original_size + 1);
    g_autoptr(GBytes) trailing = NULL;

    memcpy (data, original, original_size);
    data[original_size] = 0;
    write_u32 (&data[12], original_size + 1);
    trailing = g_bytes_new_take (data, original_size + 1);
    g_assert_cmpint (decode_status (trailing), ==,
                     FTE3600_TEMPLATE_INVALID_WIRE);
  }
  {
    g_autofree guint8 *oversize =
      g_malloc0 (FTE3600_TEMPLATE_MAX_WIRE_SIZE + 1);
    g_autoptr(GBytes) bytes = g_bytes_new (oversize,
                                           FTE3600_TEMPLATE_MAX_WIRE_SIZE + 1);

    g_assert_cmpint (decode_status (bytes), ==,
                     FTE3600_TEMPLATE_INVALID_WIRE);
  }
}

static GBytes *
copy_and_mutate_u16 (GBytes *wire,
                     gsize   offset,
                     guint16 value)
{
  guint8 *data;
  GBytes *copy = mutable_copy (wire, &data, NULL);

  write_u16 (&data[offset], value);
  return copy;
}

static GBytes *
copy_and_mutate_u32 (GBytes *wire,
                     gsize   offset,
                     guint32 value)
{
  guint8 *data;
  GBytes *copy = mutable_copy (wire, &data, NULL);

  write_u32 (&data[offset], value);
  return copy;
}

static void
test_malformed_records (void)
{
  g_autoptr(Fte3600Template) templ = make_ready_template (FALSE);
  g_autoptr(GBytes) wire = NULL;

  g_assert_cmpint (fpi_fte3600_template_encode (templ, &wire), ==,
                   FTE3600_TEMPLATE_OK);
  {
    g_autoptr(GBytes) changed = copy_and_mutate_u32 (wire, 40,
                                                     TEST_RECORD_SIZE + 1);
    g_assert_cmpint (decode_status (changed), ==,
                     FTE3600_TEMPLATE_INVALID_WIRE);
  }
  {
    g_autoptr(GBytes) changed = copy_and_mutate_u16 (wire, 44, 10);
    g_assert_cmpint (decode_status (changed), ==,
                     FTE3600_TEMPLATE_INVALID_WIRE);
  }
  {
    g_autoptr(GBytes) changed = copy_and_mutate_u16 (wire, 46,
                                                     TEST_FEATURES - 1);
    g_assert_cmpint (decode_status (changed), ==,
                     FTE3600_TEMPLATE_INVALID_WIRE);
  }
  {
    g_autoptr(GBytes) changed = copy_and_mutate_u32 (wire, 48, 0x7fc00000u);
    g_assert_cmpint (decode_status (changed), ==,
                     FTE3600_TEMPLATE_INVALID_WIRE);
  }
  {
    g_autoptr(GBytes) changed = copy_and_mutate_u32 (wire, 56, 0x80000000u);
    g_assert_cmpint (decode_status (changed), ==,
                     FTE3600_TEMPLATE_INVALID_WIRE);
  }
  {
    guint8 *data;
    gsize size;
    guint8 temporary[FTE3600_TEMPLATE_FEATURE_RECORD_SIZE];
    g_autoptr(GBytes) changed = mutable_copy (wire, &data, &size);

    memcpy (temporary, &data[48], sizeof (temporary));
    memcpy (&data[48], &data[48 + FTE3600_TEMPLATE_FEATURE_RECORD_SIZE],
            sizeof (temporary));
    memcpy (&data[48 + FTE3600_TEMPLATE_FEATURE_RECORD_SIZE], temporary,
            sizeof (temporary));
    g_assert_cmpint (decode_status (changed), ==,
                     FTE3600_TEMPLATE_INVALID_WIRE);
  }
  {
    guint8 *data;
    gsize size;
    g_autoptr(GBytes) changed = mutable_copy (wire, &data, &size);

    /* Make the second feature a nearby, non-identical detector location. */
    write_u32 (&data[48 + FTE3600_TEMPLATE_FEATURE_RECORD_SIZE], 0x41100000u);
    write_u32 (&data[52 + FTE3600_TEMPLATE_FEATURE_RECORD_SIZE], 0x41200000u);
    g_assert_cmpint (decode_status (changed), ==,
                     FTE3600_TEMPLATE_INVALID_WIRE);
  }
  {
    guint8 *data;
    gsize size;
    g_autoptr(GBytes) changed = mutable_copy (wire, &data, &size);

    /* Duplicate one complete canonical subtemplate. */
    memcpy (&data[40 + TEST_RECORD_SIZE], &data[40], TEST_RECORD_SIZE);
    g_assert_cmpint (decode_status (changed), ==,
                     FTE3600_TEMPLATE_INVALID_WIRE);
  }
  {
    guint8 *data;
    gsize size;
    guint8 temporary[TEST_RECORD_SIZE];
    g_autoptr(GBytes) changed = mutable_copy (wire, &data, &size);
    const gsize last = 40 +
                       (FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES - 1) *
                       TEST_RECORD_SIZE;

    memcpy (temporary, &data[40], sizeof (temporary));
    memcpy (&data[40], &data[last], sizeof (temporary));
    memcpy (&data[last], temporary, sizeof (temporary));
    g_assert_cmpint (decode_status (changed), ==,
                     FTE3600_TEMPLATE_INVALID_WIRE);
  }
}

static void
test_maximum_size (void)
{
  g_autoptr(Fte3600Template) templ = fpi_fte3600_template_new ();
  g_autoptr(Fte3600Template) decoded = NULL;
  g_autoptr(GBytes) wire = NULL;
  gsize size;

  for (guint sample = 0;
       sample < FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES; sample++)
    {
      Fte3600BriskFeatureSet features;

      make_feature_set_n (&features, sample, FTE3600_BRISK_MAX_FEATURES,
                          sample & 1);
      g_assert_cmpint (fpi_fte3600_template_add_features (templ, &features, NULL),
                       ==, sample + 1 ==
                       FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES ?
                       FTE3600_TEMPLATE_OK :
                       FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
    }
  g_assert_cmpint (fpi_fte3600_template_encode (templ, &wire), ==,
                   FTE3600_TEMPLATE_OK);
  g_bytes_get_data (wire, &size);
  g_assert_cmpuint (size, ==, FTE3600_TEMPLATE_CURRENT_MAX_WIRE_SIZE);
  g_assert_cmpint (fpi_fte3600_template_decode (wire,
                                                FTE3600_TEMPLATE_LOAD_DIAGNOSTIC,
                                                &decoded), ==,
                   FTE3600_TEMPLATE_OK);
}

static void
test_incomplete_and_arguments (void)
{
  g_autoptr(Fte3600Template) templ = fpi_fte3600_template_new ();
  GBytes *wire = (GBytes *) 0x1;
  Fte3600Template *decoded = (Fte3600Template *) 0x1;

  g_assert_false (fpi_fte3600_template_is_ready (NULL));
  g_assert_cmpint (fpi_fte3600_template_encode (templ, &wire), ==,
                   FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
  g_assert_null (wire);
  g_assert_cmpint (fpi_fte3600_template_decode (
                     NULL, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &decoded), ==,
                   FTE3600_TEMPLATE_INVALID_WIRE);
  g_assert_null (decoded);
  g_assert_cmpint (fpi_fte3600_template_add_features (NULL, NULL, NULL), ==,
                   FTE3600_TEMPLATE_INVALID_WIRE);
}

static void
test_rounding_mode_isolation (void)
{
  static const gint modes[] = { FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO };
  const gint caller_mode = fegetround ();

  g_autoptr(Fte3600Template) templ = NULL;
  g_autoptr(Fte3600Template) baseline_decoded = NULL;
  g_autoptr(GBytes) baseline_wire = NULL;
  Fte3600BriskFeatureSet query;
  Fte3600TemplateCompareResult baseline_result;

  g_assert_cmpint (caller_mode, !=, -1);
  g_assert_cmpint (fesetround (FE_TONEAREST), ==, 0);
  templ = make_ready_template (TRUE);
  make_feature_set (&query, 0, TRUE);
  g_assert_cmpint (fpi_fte3600_template_encode (templ, &baseline_wire), ==,
                   FTE3600_TEMPLATE_OK);
  g_assert_cmpint (fpi_fte3600_template_decode (
                     baseline_wire, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC,
                     &baseline_decoded), ==, FTE3600_TEMPLATE_OK);
  g_assert_cmpint (fpi_fte3600_template_compare_features (
                     baseline_decoded, &query,
                     FTE3600_TEMPLATE_LOAD_DIAGNOSTIC,
                     &baseline_result), ==, FTE3600_TEMPLATE_OK);

  for (guint i = 0; i < G_N_ELEMENTS (modes); i++)
    {
      g_autoptr(Fte3600Template) decoded = NULL;
      g_autoptr(Fte3600Template) partial = fpi_fte3600_template_new ();
      g_autoptr(GBytes) wire = NULL;
      Fte3600TemplateCompareResult result;

      g_assert_cmpint (fesetround (modes[i]), ==, 0);
      g_assert_cmpint (fpi_fte3600_template_encode (templ, &wire), ==,
                       FTE3600_TEMPLATE_OK);
      g_assert_cmpint (fegetround (), ==, modes[i]);
      g_assert_true (g_bytes_equal (wire, baseline_wire));

      g_assert_cmpint (fpi_fte3600_template_decode (
                         wire, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &decoded),
                       ==, FTE3600_TEMPLATE_OK);
      g_assert_cmpint (fegetround (), ==, modes[i]);
      g_assert_cmpint (fpi_fte3600_template_compare_features (
                         decoded, &query,
                         FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &result), ==,
                       FTE3600_TEMPLATE_OK);
      g_assert_cmpint (fegetround (), ==, modes[i]);
      g_assert_cmpmem (&result, sizeof (result),
                       &baseline_result, sizeof (baseline_result));

      g_assert_cmpint (fpi_fte3600_template_add_features (partial, &query, NULL),
                       ==, FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
      g_assert_cmpint (fegetround (), ==, modes[i]);
      g_assert_cmpint (fpi_fte3600_template_compare_features (
                         decoded, &query,
                         FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &result), ==,
#if FTE3600_ENABLE_PERSONAL_AUTH
                       FTE3600_TEMPLATE_OK);
      g_assert_true (result.authentication_accepted);
#else
                       FTE3600_TEMPLATE_NOT_CALIBRATED);
                       g_assert_false (result.authentication_accepted);
#endif
      g_assert_cmpint (fegetround (), ==, modes[i]);
    }

  g_assert_cmpint (fesetround (caller_mode), ==, 0);
}

static void
make_fingerprint_pattern (guint8 *image)
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

  for (guint y = 0; y < FTE3600_IPA_HEIGHT; y++)
    {
      for (guint x = 0; x < FTE3600_IPA_WIDTH; x++)
        {
          gdouble value = 126.0 + 15.0 * sin (0.29 * x + 0.17 * y) +
                          10.0 * cos (0.13 * x - 0.23 * y);

          for (guint i = 0; i < G_N_ELEMENTS (spots); i++)
            {
              gdouble dx = x - spots[i].x;
              gdouble dy = y - spots[i].y;
              value += spots[i].amplitude *
                       exp (-(dx * dx + dy * dy) /
                            (2.0 * spots[i].sigma * spots[i].sigma));
            }
          value = CLAMP (value, 0.0, 255.0);
          image[y * FTE3600_IPA_WIDTH + x] = (guint8) floor (value + 0.5);
        }
    }
}

static void
test_dual_enrollment_tied_peaks (void)
{
  guint8 image[FTE3600_IPA_IMAGE_SIZE];
  Fte3600BriskFeatureSet brisk;
  Fte3600IpaFeatureSet ipa;
  guint physical_count;

  g_autoptr(Fte3600Template) templ = fpi_fte3600_template_new ();

  make_fingerprint_pattern (image);
  for (guint y = 0; y < 20; y++)
    for (guint x = 0; x < FTE3600_IPA_WIDTH; x++)
      image[y * FTE3600_IPA_WIDTH + x] = (guint8) floor (
        128.0 + 70.0 * cos ((x - 31.5) * G_PI / 3.0) *
        cos ((y - 39.5) * G_PI / 2.0) + 0.5);

  g_assert_cmpint (fpi_fte3600_brisk_extract (image, sizeof (image), &brisk),
                   ==, FTE3600_BRISK_OK);
  g_assert_true (fpi_fte3600_brisk_validate_feature_set (&brisk, &physical_count));
  g_assert_cmpuint (physical_count, >=, FTE3600_TEMPLATE_MIN_PHYSICAL_FEATURES);
  g_assert_cmpint (fpi_fte3600_ipa_extract (image, sizeof (image), &ipa),
                   ==, FTE3600_IPA_OK);
  g_assert_cmpint (fpi_fte3600_template_add_dual_features (templ, &brisk, &ipa, NULL),
                   ==, FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
}

static void
test_dual_engine_fusion (void)
{
  g_autoptr(Fte3600Template) templ = fpi_fte3600_template_new ();
  g_autoptr(GBytes) wire = NULL;
  g_autoptr(Fte3600Template) decoded = NULL;
  guint8 image[FTE3600_IPA_IMAGE_SIZE];
  Fte3600IpaFeatureSet ipa_ref = { 0 };
  Fte3600BriskFeatureSet brisk_match = { 0 };
  const guint8 *wire_data;
  gsize wire_size = 0;

  make_fingerprint_pattern (image);
  g_assert_cmpint (fpi_fte3600_ipa_extract (image, sizeof (image), &ipa_ref),
                   ==, FTE3600_IPA_OK);
  g_assert_cmpuint (ipa_ref.n_minutiae, >=, FTE3600_IPA_POLICY_MIN_INLIERS);

  for (guint sample = 0;
       sample < FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES; sample++)
    {
      Fte3600BriskFeatureSet features;
      make_feature_set (&features, sample, FALSE);
      if (sample == 0)
        make_feature_set (&brisk_match, sample, FALSE);
      g_assert_cmpint (
        fpi_fte3600_template_add_dual_features (templ, &features, &ipa_ref, NULL),
        ==,
        sample + 1 == FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES ?
        FTE3600_TEMPLATE_OK : FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
    }

  g_assert_true (fpi_fte3600_template_is_ready (templ));
  g_assert_cmpint (fpi_fte3600_template_encode (templ, &wire), ==, FTE3600_TEMPLATE_OK);

  wire_data = g_bytes_get_data (wire, &wire_size);
  g_assert_cmpuint (read_u16 (&wire_data[8]), ==, FTE3600_TEMPLATE_WIRE_VERSION_V3);
  g_assert_cmpuint (read_u32 (&wire_data[32]), ==, 0x01); /* Dual engine flag */
  g_assert_cmpuint (read_u16 (&wire_data[46]), ==, FTE3600_TEMPLATE_FUSION_POLICY_VERSION);

#if FTE3600_ENABLE_IPA_AUTH
  Fte3600IpaFeatureSet ipa_probe = ipa_ref;
  Fte3600BriskFeatureSet brisk_nomatch = { 0 };
  Fte3600TemplateCompareResult result;

  g_assert_cmpint (fpi_fte3600_template_decode (wire, FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &decoded),
                   ==, FTE3600_TEMPLATE_OK);
  g_assert_true (fpi_fte3600_template_is_ready (decoded));

  brisk_nomatch.extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
  brisk_nomatch.n_features = TEST_FEATURES;
  for (guint i = 0; i < TEST_FEATURES; i++)
    {
      brisk_nomatch.features[i].x = 5.0f + (gfloat) (i % 3) * 15.0f;
      brisk_nomatch.features[i].y = 5.0f + (gfloat) (i / 3) * 18.0f;
      brisk_nomatch.features[i].orientation = 0.0f;
      memset (brisk_nomatch.features[i].descriptor, 0xaa ^ (guint8) i,
              FTE3600_BRISK_DESCRIPTOR_BYTES);
    }

  /* 1. Dual Match: BRISK passes, IPA passes -> Auth Accepted */
  g_assert_cmpint (fpi_fte3600_template_compare_dual_features (
                     decoded, &brisk_match, &ipa_probe,
                     FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &result), ==, FTE3600_TEMPLATE_OK);
  g_assert_true (result.brisk_accepted);
  g_assert_true (result.ipa_accepted);
  g_assert_true (result.authentication_accepted);

  /* 2. BRISK only (IPA NULL): BRISK passes -> Auth Accepted */
  g_assert_cmpint (fpi_fte3600_template_compare_dual_features (
                     decoded, &brisk_match, NULL,
                     FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &result), ==, FTE3600_TEMPLATE_OK);
  g_assert_true (result.brisk_accepted);
  g_assert_false (result.ipa_accepted);
  g_assert_true (result.authentication_accepted);

  /* 3. IPA only (BRISK fails): IPA passes -> Auth Accepted (OR strategy) */
  g_assert_cmpint (fpi_fte3600_template_compare_dual_features (
                     decoded, &brisk_nomatch, &ipa_probe,
                     FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &result), ==, FTE3600_TEMPLATE_OK);
  g_assert_false (result.brisk_accepted);
  g_assert_true (result.ipa_accepted);
  g_assert_true (result.authentication_accepted);

  /* 4. Neither passes: BRISK fails, IPA NULL -> Auth Rejected */
  g_assert_cmpint (fpi_fte3600_template_compare_dual_features (
                     decoded, &brisk_nomatch, NULL,
                     FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &result), ==, FTE3600_TEMPLATE_OK);
  g_assert_false (result.brisk_accepted);
  g_assert_false (result.ipa_accepted);
  g_assert_false (result.authentication_accepted);
#else
#if !FTE3600_ENABLE_PERSONAL_AUTH
  g_assert_cmpint (fpi_fte3600_template_decode (wire, FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &decoded),
                   ==, FTE3600_TEMPLATE_NOT_CALIBRATED);
#endif
  g_assert_cmpint (fpi_fte3600_template_decode (wire, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &decoded),
                   ==, FTE3600_TEMPLATE_OK);
  g_assert_true (fpi_fte3600_template_is_ready (decoded));
#endif
}

#if FTE3600_ENABLE_IPA_AUTH
static void
test_mono_engine_modes (void)
{
  g_autoptr(Fte3600Template) templ = fpi_fte3600_template_new ();
  g_autoptr(GBytes) wire = NULL;
  g_autoptr(Fte3600Template) decoded = NULL;
  guint8 image[FTE3600_IPA_IMAGE_SIZE];
  Fte3600IpaFeatureSet ipa_ref = { 0 };
  Fte3600BriskFeatureSet brisk_match = { 0 };
  Fte3600BriskFeatureSet brisk_nomatch = { 0 };
  Fte3600TemplateCompareResult result;

  make_fingerprint_pattern (image);
  g_assert_cmpint (fpi_fte3600_ipa_extract (image, sizeof (image), &ipa_ref),
                   ==, FTE3600_IPA_OK);

  for (guint sample = 0;
       sample < FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES; sample++)
    {
      Fte3600BriskFeatureSet features;
      make_feature_set (&features, sample, FALSE);
      if (sample == 0)
        make_feature_set (&brisk_match, sample, FALSE);
      (void) fpi_fte3600_template_add_dual_features (templ, &features, &ipa_ref, NULL);
    }

  g_assert_cmpint (fpi_fte3600_template_encode (templ, &wire), ==, FTE3600_TEMPLATE_OK);
  g_assert_cmpint (fpi_fte3600_template_decode (wire, FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &decoded),
                   ==, FTE3600_TEMPLATE_OK);

  brisk_nomatch.extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
  brisk_nomatch.n_features = TEST_FEATURES;
  for (guint i = 0; i < TEST_FEATURES; i++)
    {
      brisk_nomatch.features[i].x = 5.0f + (gfloat) (i % 3) * 15.0f;
      brisk_nomatch.features[i].y = 5.0f + (gfloat) (i / 3) * 18.0f;
      brisk_nomatch.features[i].orientation = 0.0f;
      memset (brisk_nomatch.features[i].descriptor, 0xaa ^ (guint8) i,
              FTE3600_BRISK_DESCRIPTOR_BYTES);
    }

  /* 1. Test Mono-Engine BRISK-ONLY Mode */
  /* Case A: BRISK match -> Accepted */
  g_assert_cmpint (fpi_fte3600_template_compare_features (
                     decoded, &brisk_match,
                     FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &result), ==, FTE3600_TEMPLATE_OK);
  g_assert_cmpint (result.engine_mode, ==, FTE3600_ENGINE_MODE_BRISK_ONLY);
  g_assert_true (result.brisk_accepted);
  g_assert_true (result.authentication_accepted);

  /* Case B: IPA matches, but BRISK fails -> REJECTED in BRISK-only mode */
  g_assert_cmpint (fpi_fte3600_template_compare_with_mode (
                     decoded, &brisk_nomatch, &ipa_ref,
                     FTE3600_TEMPLATE_LOAD_AUTHENTICATION,
                     FTE3600_ENGINE_MODE_BRISK_ONLY, &result), ==, FTE3600_TEMPLATE_OK);
  g_assert_false (result.brisk_accepted);
  g_assert_false (result.ipa_accepted);
  g_assert_false (result.authentication_accepted);

  /* 2. Test Mono-Engine 2D-IPA-ONLY Mode */
  /* Case A: IPA match -> Accepted */
  g_assert_cmpint (fpi_fte3600_template_compare_ipa_features (
                     decoded, &ipa_ref,
                     FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &result), ==, FTE3600_TEMPLATE_OK);
  g_assert_cmpint (result.engine_mode, ==, FTE3600_ENGINE_MODE_IPA_ONLY);
  g_assert_true (result.ipa_accepted);
  g_assert_true (result.authentication_accepted);

  /* Case B: BRISK passes, but IPA fails/nomatch -> REJECTED in IPA-only mode */
  Fte3600IpaFeatureSet ipa_nomatch = { 0 };
  ipa_nomatch.extractor_schema_version = FTE3600_IPA_EXTRACTOR_SCHEMA_VERSION;
  ipa_nomatch.n_minutiae = 5;
  for (guint i = 0; i < 5; i++)
    {
      ipa_nomatch.minutiae[i].x = 10.0f + (gfloat) i * 8.0f;
      ipa_nomatch.minutiae[i].y = 15.0f;
      ipa_nomatch.minutiae[i].theta = 0.0f;
      ipa_nomatch.minutiae[i].desc[0] = 1.0f;
    }
  g_assert_cmpint (fpi_fte3600_template_compare_with_mode (
                     decoded, &brisk_match, &ipa_nomatch,
                     FTE3600_TEMPLATE_LOAD_AUTHENTICATION,
                     FTE3600_ENGINE_MODE_IPA_ONLY, &result), ==, FTE3600_TEMPLATE_OK);
  g_assert_false (result.ipa_accepted);
  g_assert_false (result.authentication_accepted);
}
#endif

static Fte3600Template *
make_ipa_gallery_for_profile (const Fte3600MatchProfile *profile,
                              guint                      ipa_mask,
                              gboolean                   maximum)
{
  Fte3600Template *templ = profile ? fpi_fte3600_template_new_for_profile (profile) :
                           fpi_fte3600_template_new ();
  Fte3600IpaFeatureSet ipa = { 0 };
  guint8 image[FTE3600_IPA_IMAGE_SIZE];

  if (maximum)
    {
      ipa.extractor_schema_version = FTE3600_IPA_EXTRACTOR_SCHEMA_VERSION;
      ipa.n_minutiae = FTE3600_IPA_MAX_MINUTIAE;
      for (guint i = 0; i < ipa.n_minutiae; i++)
        {
          ipa.minutiae[i].x = 3.0f + 7.0f * (i % 8);
          ipa.minutiae[i].y = 4.0f + 12.0f * (i / 8);
          ipa.minutiae[i].desc[i % FTE3600_IPA_DESC_DIM] = 1.0f;
        }
    }
  else
    {
      make_fingerprint_pattern (image);
      g_assert_cmpint (fpi_fte3600_ipa_extract (image, sizeof (image), &ipa), ==, FTE3600_IPA_OK);
    }
  for (guint sample = 0; sample < FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES; sample++)
    {
      Fte3600BriskFeatureSet brisk;
      make_feature_set_n (&brisk, sample,
                          maximum ? FTE3600_BRISK_MAX_FEATURES : TEST_FEATURES, FALSE);
      g_assert_cmpint (fpi_fte3600_template_add_dual_features (
                         templ, &brisk, (ipa_mask & (1u << sample)) ? &ipa : NULL, NULL),
                       ==, sample + 1 == FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES ?
                       FTE3600_TEMPLATE_OK : FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
    }
  return templ;
}

static Fte3600Template *
make_ipa_gallery (guint ipa_mask, gboolean maximum)
{
  return make_ipa_gallery_for_profile (NULL, ipa_mask, maximum);
}

static void
test_ipa_profile_isolation (void)
{
  g_autoptr(Fte3600Template) original = make_ipa_gallery (0xff, FALSE);
  g_autoptr(GBytes) wire = NULL;
  Fte3600BriskFeatureSet brisk;
  Fte3600IpaFeatureSet ipa = { 0 };

  g_assert_cmpint (fpi_fte3600_template_encode (original, &wire), ==,
                   FTE3600_TEMPLATE_OK);
  make_feature_set (&brisk, 0, FALSE);
  for (guint i = 0; i < brisk.n_features; i++)
    brisk.features[i].x *= 0.5f;
  ipa.extractor_schema_version = FTE3600_IPA_EXTRACTOR_SCHEMA_VERSION;
  ipa.n_minutiae = 3;
  for (guint i = 0; i < ipa.n_minutiae; i++)
    {
      ipa.minutiae[i].x = 10.0f + 8.0f * i;
      ipa.minutiae[i].y = 12.0f + 10.0f * i;
      ipa.minutiae[i].desc[i] = 1.0f;
    }

  for (guint sensor = FTE3600_SENSOR_FT9338; sensor < FTE3600_SENSOR_COUNT; sensor++)
    {
      const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (sensor);
      g_autoptr(Fte3600Template) foreign = NULL;

      if (sensor == FTE3600_SENSOR_FT9361)
        continue;
      if (sensor != FTE3600_SENSOR_FT9369)
        {
          foreign = fpi_fte3600_template_new_for_profile (profile);
          g_assert_cmpint (fpi_fte3600_template_add_dual_features (foreign, &brisk, &ipa, NULL),
                           ==, FTE3600_TEMPLATE_INVALID_WIRE);
          g_assert_cmpint (fpi_fte3600_template_add_features (foreign, &brisk, NULL),
                           ==, FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
        }

      /* In particular, another 64x80 model must never decode as FT9361.
       * Cover both processing metadata forms accepted by FT9361 V3. */
      for (guint processing = 0; processing <= 1; processing++)
        {
          guint8 *data;
          g_autoptr(GBytes) changed = mutable_copy (wire, &data, NULL);

          write_u16 (&data[16], profile->model);
          write_u16 (&data[18], profile->width);
          write_u16 (&data[20], profile->height);
          write_u32 (&data[36], processing ? profile->processing_version : 0);
          for (guint purpose = FTE3600_TEMPLATE_LOAD_DIAGNOSTIC;
               purpose <= FTE3600_TEMPLATE_LOAD_AUTHENTICATION; purpose++)
            {
              g_autoptr(Fte3600Template) decoded = NULL;

              g_assert_cmpint (fpi_fte3600_template_decode (changed, purpose, &decoded),
                               ==, FTE3600_TEMPLATE_INVALID_WIRE);
              g_assert_null (decoded);
            }
        }
    }
}

static void
assert_comparison_equal (const Fte3600TemplateCompareResult *first,
                         const Fte3600TemplateCompareResult *second)
{
  g_assert_cmpuint (first->n_compared, ==, second->n_compared);
  g_assert_cmpuint (first->diagnostic_passes, ==, second->diagnostic_passes);
  g_assert_cmpuint (first->best_subtemplate, ==, second->best_subtemplate);
  g_assert_cmpint (first->brisk_accepted, ==, second->brisk_accepted);
  g_assert_cmpint (first->ipa_accepted, ==, second->ipa_accepted);
  g_assert_cmpint (first->authentication_accepted, ==, second->authentication_accepted);
  g_assert_cmpuint (first->best.inliers, ==, second->best.inliers);
  g_assert_cmpfloat (first->best.normalized_query_anisotropy, ==,
                     second->best.normalized_query_anisotropy);
  g_assert_cmpfloat (first->best.normalized_reference_anisotropy, ==,
                     second->best.normalized_reference_anisotropy);
  g_assert_cmpfloat (first->best.query_max_variance, ==, second->best.query_max_variance);
  g_assert_cmpfloat (first->best.reference_max_variance, ==, second->best.reference_max_variance);
  g_assert_cmpfloat (first->best.translate_x, ==, second->best.translate_x);
  g_assert_cmpfloat (first->best.translate_y, ==, second->best.translate_y);
  g_assert_cmpfloat (first->best.angle, ==, second->best.angle);
  g_assert_cmpfloat (first->best_ipa.consensus_score, ==, second->best_ipa.consensus_score);
}

static void
assert_gallery_behavior_equal (const Fte3600Template *first,
                               const Fte3600Template *second)
{
  Fte3600BriskFeatureSet brisk;
  Fte3600IpaFeatureSet ipa;
  guint8 image[FTE3600_IPA_IMAGE_SIZE];
  const Fte3600BriskFeatureSet *first_mosaic = fpi_fte3600_template_get_mosaic (first);
  const Fte3600BriskFeatureSet *second_mosaic = fpi_fte3600_template_get_mosaic (second);

  g_assert_nonnull (first_mosaic);
  g_assert_nonnull (second_mosaic);
  g_assert_cmpuint (first_mosaic->n_features, ==, second_mosaic->n_features);
  g_assert_cmpmem (first_mosaic->features,
                   first_mosaic->n_features * sizeof (first_mosaic->features[0]),
                   second_mosaic->features,
                   second_mosaic->n_features * sizeof (second_mosaic->features[0]));
  make_fingerprint_pattern (image);
  g_assert_cmpint (fpi_fte3600_ipa_extract (image, sizeof (image), &ipa), ==, FTE3600_IPA_OK);
  for (guint sample = 0; sample < 2; sample++)
    {
      make_feature_set (&brisk, sample ? 0xff : 0, FALSE);
      for (guint mode = FTE3600_ENGINE_MODE_BRISK_ONLY;
           mode <= FTE3600_ENGINE_MODE_DUAL_FUSION; mode++)
        for (guint purpose = FTE3600_TEMPLATE_LOAD_DIAGNOSTIC;
             purpose <= FTE3600_TEMPLATE_LOAD_AUTHENTICATION; purpose++)
          {
            Fte3600TemplateCompareResult first_result;
            Fte3600TemplateCompareResult second_result;
            Fte3600TemplateStatus first_status;
            Fte3600TemplateStatus second_status;

            first_status = fpi_fte3600_template_compare_with_mode (
              first, &brisk, &ipa, purpose, mode, &first_result);
            second_status = fpi_fte3600_template_compare_with_mode (
              second, &brisk, &ipa, purpose, mode, &second_result);
            g_assert_cmpint (first_status, ==, second_status);
            assert_comparison_equal (&first_result, &second_result);
            if (first_status == FTE3600_TEMPLATE_OK && mode != FTE3600_ENGINE_MODE_IPA_ONLY)
              g_assert_cmpuint (first_result.n_compared, ==,
                                FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES + 1);
          }
    }
}

static void
test_ipa_processing_metadata_roundtrip (void)
{
  const Fte3600MatchProfile *profile =
    fpi_fte3600_match_profile_get (FTE3600_SENSOR_FT9361);

  for (guint modern = 0; modern <= 1; modern++)
    {
      g_autoptr(Fte3600Template) templ =
        make_ipa_gallery_for_profile (modern ? profile : NULL, 0xff, FALSE);
      g_autoptr(Fte3600Template) decoded = NULL;
      g_autoptr(GBytes) wire = NULL;
      g_autoptr(GBytes) roundtrip = NULL;
      const guint8 *data;

      g_assert_cmpint (fpi_fte3600_template_encode (templ, &wire), ==,
                       FTE3600_TEMPLATE_OK);
      data = g_bytes_get_data (wire, NULL);
      g_assert_cmpuint (read_u16 (&data[8]), ==, FTE3600_TEMPLATE_WIRE_VERSION_V3);
      g_assert_cmpuint (read_u32 (&data[36]), ==, modern ? profile->processing_version : 0);
      const guint8 policy[] = { 3, 0, 5, 0, FTE3600_ENABLE_PERSONAL_AUTH ? 6 : 0, 0 };
      g_assert_cmpmem (&data[24], sizeof (policy), policy, sizeof (policy));
      g_assert_cmpint (fpi_fte3600_template_decode (wire, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC,
                                                    &decoded), ==, FTE3600_TEMPLATE_OK);
      g_assert_true (fpi_fte3600_template_get_profile (decoded) == profile);
      g_assert_cmpint (fpi_fte3600_template_encode (decoded, &roundtrip), ==,
                       FTE3600_TEMPLATE_OK);
      g_assert_true (g_bytes_equal (wire, roundtrip));
      assert_gallery_behavior_equal (templ, decoded);
    }
}

static void
test_mixed_ipa_roundtrip (void)
{
  static const guint masks[] = { 0, 1, 0x80, 0x55, 0xff };
  const Fte3600MatchProfile *fw9369 =
    fpi_fte3600_match_profile_get (FTE3600_SENSOR_FT9369);

  for (guint profiled = 0; profiled <= 1; profiled++)
    {
      const Fte3600MatchProfile *profile = profiled ? fw9369 : NULL;

      for (guint i = 0; i < G_N_ELEMENTS (masks); i++)
        {
          g_autoptr(Fte3600Template) templ = make_ipa_gallery_for_profile (profile, masks[i], FALSE);
          g_autoptr(Fte3600Template) decoded = NULL;
          g_autoptr(GBytes) wire = NULL;
          g_autoptr(GBytes) roundtrip = NULL;
          const guint version = profiled ?
                                (masks[i] ? FTE3600_TEMPLATE_PROFILE_DUAL_WIRE_VERSION :
                                 FTE3600_TEMPLATE_PROFILE_WIRE_VERSION) :
                                (masks[i] ? FTE3600_TEMPLATE_WIRE_VERSION_V3 :
                                 FTE3600_TEMPLATE_WIRE_VERSION);

          g_assert_cmpint (fpi_fte3600_template_encode (templ, &wire), ==, FTE3600_TEMPLATE_OK);
          const guint8 *data = g_bytes_get_data (wire, NULL);
          g_assert_cmpuint (read_u16 (&data[8]), ==, version);
          g_assert_cmpint (fpi_fte3600_template_decode (wire, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &decoded),
                           ==, FTE3600_TEMPLATE_OK);
          g_assert_cmpint (fpi_fte3600_template_encode (decoded, &roundtrip), ==, FTE3600_TEMPLATE_OK);
          g_assert_true (g_bytes_equal (wire, roundtrip));
          assert_gallery_behavior_equal (templ, decoded);
        }
      g_autoptr(Fte3600Template) maximum = make_ipa_gallery_for_profile (profile, 0xff, TRUE);
      g_autoptr(Fte3600Template) decoded = NULL;
      g_autoptr(GBytes) wire = NULL;
      g_assert_cmpint (fpi_fte3600_template_encode (maximum, &wire), ==, FTE3600_TEMPLATE_OK);
      g_assert_cmpuint (g_bytes_get_size (wire), ==, FTE3600_TEMPLATE_V3_CURRENT_MAX_WIRE_SIZE);
      g_assert_cmpint (fpi_fte3600_template_decode (wire, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &decoded),
                       ==, FTE3600_TEMPLATE_OK);
    }
}

static void
test_fw9369_dual_profile_policy (void)
{
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (FTE3600_SENSOR_FT9369);
  g_autoptr(Fte3600Template) dual = make_ipa_gallery_for_profile (profile, 0x80, FALSE);
  g_autoptr(Fte3600Template) brisk = make_ipa_gallery_for_profile (profile, 0, FALSE);
  g_autoptr(Fte3600Template) decoded = NULL;
  g_autoptr(GBytes) wire = NULL;
  Fte3600BriskFeatureSet query;
  Fte3600TemplateCompareResult dual_result;
  Fte3600TemplateCompareResult brisk_result;

  g_assert_cmpint (fpi_fte3600_template_encode (dual, &wire), ==, FTE3600_TEMPLATE_OK);
  const guint8 *data = g_bytes_get_data (wire, NULL);
  const guint8 identity[] = { 0x69, 0x93, 64, 0, 80, 0, 44, 0,
                             3, 0, 7, 0, FTE3600_ENABLE_PERSONAL_AUTH ? 8 : 0, 0, 8, 0,
                             1, 0, 0, 0, 1, 0, 0, 0 };
  g_assert_cmpuint (read_u16 (&data[8]), ==, 4);
  g_assert_cmpuint (read_u16 (&data[10]), ==, 48);
  g_assert_cmpmem (&data[16], sizeof (identity), identity, sizeof (identity));
  g_assert_cmpint (fpi_fte3600_template_decode (wire, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC,
                                                &decoded), ==, FTE3600_TEMPLATE_OK);
  g_assert_true (fpi_fte3600_template_get_profile (decoded) == profile);

  for (guint purpose = FTE3600_TEMPLATE_LOAD_DIAGNOSTIC;
       purpose <= FTE3600_TEMPLATE_LOAD_AUTHENTICATION; purpose++)
    {
      make_feature_set (&query, 0, FALSE);
      g_assert_cmpint (fpi_fte3600_template_compare_features (
                         dual, &query, purpose, &dual_result), ==,
                       fpi_fte3600_template_compare_features (
                         brisk, &query, purpose, &brisk_result));
      assert_comparison_equal (&dual_result, &brisk_result);
    }
  for (guint sensor = FTE3600_SENSOR_FT9338; sensor < FTE3600_SENSOR_COUNT; sensor++)
    {
      const Fte3600MatchProfile *foreign = fpi_fte3600_match_profile_get (sensor);

      if (foreign == profile)
        continue;
      g_assert_cmpint (fpi_fte3600_template_compare_features_for_profile (
                         decoded, foreign, &query, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC,
                         &dual_result), ==, FTE3600_TEMPLATE_INVALID_WIRE);
      assert_header_mutation (wire, 16, foreign->model, FTE3600_TEMPLATE_INVALID_WIRE);
    }
  assert_header_mutation (wire, 8, 3, FTE3600_TEMPLATE_INVALID_WIRE);
  assert_header_mutation (wire, 18, 80, FTE3600_TEMPLATE_INVALID_WIRE);
  assert_header_mutation (wire, 20, 64, FTE3600_TEMPLATE_INVALID_WIRE);
  assert_header_mutation (wire, 36, 0, FTE3600_TEMPLATE_INVALID_WIRE);
  assert_header_mutation (wire, 36, 2, FTE3600_TEMPLATE_INVALID_WIRE);
  assert_header_mutation (wire, 38, 1, FTE3600_TEMPLATE_INVALID_WIRE);
  assert_header_mutation (wire, 26, FTE3600_BRISK_DIAGNOSTIC_POLICY_VERSION,
                          FTE3600_TEMPLATE_UNSUPPORTED_POLICY);
  assert_header_mutation (wire, 46, FTE3600_TEMPLATE_FUSION_POLICY_VERSION + 1,
                          FTE3600_TEMPLATE_UNSUPPORTED_POLICY);
}

static void
test_rejected_ipa_preserves_brisk_schema (void)
{
  static const Fte3600Sensor sensors[] = { FTE3600_SENSOR_FT9361, FTE3600_SENSOR_FT9369 };

  for (guint i = 0; i < G_N_ELEMENTS (sensors); i++)
    {
      const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (sensors[i]);
      g_autoptr(Fte3600Template) templ = fpi_fte3600_template_new_for_profile (profile);
      g_autoptr(Fte3600Template) expected = make_ipa_gallery_for_profile (profile, 0, FALSE);
      g_autoptr(GBytes) wire = NULL;
      g_autoptr(GBytes) expected_wire = NULL;
      Fte3600BriskFeatureSet features;
      Fte3600IpaFeatureSet ipa;
      guint8 image[FTE3600_IPA_IMAGE_SIZE];

      make_fingerprint_pattern (image);
      g_assert_cmpint (fpi_fte3600_ipa_extract (image, sizeof (image), &ipa), ==, FTE3600_IPA_OK);
      for (guint sample = 0; sample < FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES; sample++)
        {
          make_feature_set (&features, sample, FALSE);
          if (sample == 4)
            {
              ipa.extractor_schema_version++;
              g_assert_cmpint (fpi_fte3600_template_add_dual_features (
                                 templ, &features, &ipa, NULL), ==, FTE3600_TEMPLATE_INVALID_WIRE);
              ipa.extractor_schema_version--;
            }
          g_assert_cmpint (fpi_fte3600_template_add_features (templ, &features, NULL), ==,
                           sample + 1 == FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES ?
                           FTE3600_TEMPLATE_OK : FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
        }
      /* A complete gallery also cannot silently acquire a different policy. */
      g_assert_cmpint (fpi_fte3600_template_add_dual_features (templ, &features, &ipa, NULL),
                       ==, FTE3600_TEMPLATE_INVALID_WIRE);
      g_assert_cmpint (fpi_fte3600_template_encode (templ, &wire), ==, FTE3600_TEMPLATE_OK);
      g_assert_cmpint (fpi_fte3600_template_encode (expected, &expected_wire), ==, FTE3600_TEMPLATE_OK);
      g_assert_true (g_bytes_equal (wire, expected_wire));
    }
}

static void
test_ipa_version_isolation (void)
{
  g_autoptr(Fte3600Template) templ = make_ipa_gallery (0xff, FALSE);
  g_autoptr(GBytes) wire = NULL;

  g_assert_cmpint (fpi_fte3600_template_encode (templ, &wire), ==, FTE3600_TEMPLATE_OK);
  assert_header_mutation (wire, 8, 4, FTE3600_TEMPLATE_INVALID_WIRE);
  assert_header_mutation (wire, 8, 5,
                          FTE3600_TEMPLATE_UNSUPPORTED_SCHEMA);
  assert_header_mutation (wire, 8, 2,
                          FTE3600_TEMPLATE_INVALID_WIRE);
  assert_header_mutation (wire, 40, 2,
                          FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR);
  assert_header_mutation (wire, 40, FTE3600_IPA_EXTRACTOR_SCHEMA_VERSION + 1,
                          FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR);
  assert_header_mutation (wire, 40, 1, FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR);
  assert_header_mutation (wire, 42, FTE3600_IPA_DIAGNOSTIC_POLICY_VERSION + 1,
                          FTE3600_TEMPLATE_UNSUPPORTED_POLICY);
  assert_header_mutation (wire, 42, 1, FTE3600_TEMPLATE_UNSUPPORTED_POLICY);
  assert_header_mutation (wire, 44, !FTE3600_IPA_AUTHENTICATION_POLICY_VERSION,
                          FTE3600_TEMPLATE_UNSUPPORTED_POLICY);
  assert_header_mutation (wire, 44, 1, FTE3600_TEMPLATE_UNSUPPORTED_POLICY);
  assert_header_mutation (wire, 46, FTE3600_TEMPLATE_FUSION_POLICY_VERSION + 1,
                          FTE3600_TEMPLATE_UNSUPPORTED_POLICY);
  assert_header_mutation (wire, 46, 1, FTE3600_TEMPLATE_UNSUPPORTED_POLICY);

  /* A complete pre-mosaic compatibility tuple is rejected for both load
   * purposes; rewriting a header is not an allowed migration. */
  guint8 *data;
  g_autoptr(GBytes) legacy = mutable_copy (wire, &data, NULL);
  write_u16 (&data[26], 2);
  write_u16 (&data[28], FTE3600_ENABLE_PERSONAL_AUTH ? 3 : 0);
  write_u16 (&data[46], 1);
  for (guint purpose = FTE3600_TEMPLATE_LOAD_DIAGNOSTIC;
       purpose <= FTE3600_TEMPLATE_LOAD_AUTHENTICATION; purpose++)
    {
      g_autoptr(Fte3600Template) decoded = NULL;
      g_assert_cmpint (fpi_fte3600_template_decode (legacy, purpose, &decoded),
                       ==, FTE3600_TEMPLATE_UNSUPPORTED_POLICY);
      g_assert_null (decoded);
    }
}

static void
test_mode_gates_and_query_fallback (void)
{
  g_autoptr(Fte3600Template) templ = make_ipa_gallery (0xff, FALSE);
  Fte3600TemplateCompareResult result;
  Fte3600IpaFeatureSet ipa;
  Fte3600BriskFeatureSet brisk;
  Fte3600EngineMode mode;
  guint8 image[FTE3600_IPA_IMAGE_SIZE];

  make_fingerprint_pattern (image);
  make_feature_set (&brisk, 0, FALSE);
  g_assert_cmpint (fpi_fte3600_ipa_extract (image, sizeof (image), &ipa), ==, FTE3600_IPA_OK);
  g_assert_true (fpi_fte3600_engine_mode_parse (NULL, &mode));
  g_assert_cmpint (mode, ==, FTE3600_ENABLE_IPA_AUTH ?
                   FTE3600_ENGINE_MODE_DUAL_FUSION : FTE3600_ENGINE_MODE_BRISK_ONLY);
  g_assert_false (fpi_fte3600_engine_mode_parse ("unknown", &mode));

  /* A valid IPA extraction can be used even when BRISK returned no query. */
  g_assert_cmpint (fpi_fte3600_template_compare_dual_features (
                     templ, NULL, &ipa, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &result),
                   ==, FTE3600_TEMPLATE_OK);
  g_assert_true (result.best_ipa.diagnostic_policy_passed);
  g_assert_false (result.authentication_accepted);
  g_assert_cmpuint (result.n_compared, ==, FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES);
  g_assert_cmpuint (result.best_subtemplate, ==, FTE3600_TEMPLATE_SUBTEMPLATE_NONE);
  g_assert_cmpint (fpi_fte3600_template_compare_dual_features (
                     templ, NULL, &ipa, FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &result),
                   ==, FTE3600_ENABLE_IPA_AUTH ? FTE3600_TEMPLATE_OK : FTE3600_TEMPLATE_NOT_CALIBRATED);
  g_assert_cmpint (result.authentication_accepted, ==, FTE3600_ENABLE_IPA_AUTH);

  brisk.n_features = 10; /* Valid but insufficient for the BRISK template gate. */
  g_assert_cmpint (fpi_fte3600_template_compare_dual_features (
                     templ, &brisk, &ipa, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &result),
                   ==, FTE3600_TEMPLATE_OK);
  g_assert_true (result.best_ipa.diagnostic_policy_passed);
  g_assert_cmpint (fpi_fte3600_template_compare_with_mode (
                     templ, NULL, &ipa, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, (Fte3600EngineMode) 99, &result),
                   ==, FTE3600_TEMPLATE_INVALID_WIRE);
}

static void
test_template_mosaicking (void)
{
  g_autoptr(Fte3600Template) templ = make_ready_template (FALSE);
  g_autoptr(Fte3600Template) decoded = NULL;
  g_autoptr(GBytes) wire = NULL;
  const Fte3600BriskFeatureSet *mosaic;

  g_assert_true (fpi_fte3600_template_is_ready (templ));
  mosaic = fpi_fte3600_template_get_mosaic (templ);
  g_assert_nonnull (mosaic);
  g_assert_cmpuint (mosaic->n_features, >=, FTE3600_TEMPLATE_MIN_PHYSICAL_FEATURES);

  /* Verify mosaic persistence across wire encode & decode */
  g_assert_cmpint (fpi_fte3600_template_encode (templ, &wire), ==, FTE3600_TEMPLATE_OK);
  g_assert_cmpint (fpi_fte3600_template_decode (wire, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &decoded),
                   ==, FTE3600_TEMPLATE_OK);
  g_assert_nonnull (decoded);
  mosaic = fpi_fte3600_template_get_mosaic (decoded);
  g_assert_nonnull (mosaic);
  g_assert_cmpuint (mosaic->n_features, >=, FTE3600_TEMPLATE_MIN_PHYSICAL_FEATURES);
}

static void
test_boundary_straddling_probe (void)
{
  g_autoptr(Fte3600Template) templ = fpi_fte3600_template_new ();
  Fte3600BriskFeatureSet query;
  Fte3600TemplateCompareResult result;
  Fte3600BriskFeatureSet s0_feats;
  Fte3600BriskFeatureSet s1_feats;

  /* Build 8 consistent samples.
   * Common overlap between Sample 0 and Sample 1..7 is points 4..8 (5 inliers >= 5).
   * Sample 0 has points 0..8 + 3 padding points (100..102).
   * Sample 1..7 have points 4..12 + 3 distinct padding points (200 + s * 10..). */
  for (guint s = 0; s < FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES; s++)
    {
      Fte3600BriskFeatureSet sample_features;
      memset (&sample_features, 0, sizeof (sample_features));
      sample_features.extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
      sample_features.n_features = 12;

      for (guint i = 0; i < 9; i++)
        {
          const guint pt = (s == 0) ? i : (4 + i);
          Fte3600BriskFeature *feat = &sample_features.features[i];
          feat->x = 10.0f + 3.5f * pt;
          feat->y = 15.0f + 25.0f * (pt % 3);
          feat->orientation = 0.0f;
          fill_descriptor (feat->descriptor, pt, 0);
        }
      for (guint i = 0; i < 3; i++)
        {
          const guint pt = (s == 0) ? (100 + i) : (200 + s * 10 + i);
          Fte3600BriskFeature *feat = &sample_features.features[9 + i];
          feat->x = 56.0f + 2.0f * (i % 2);
          feat->y = 15.0f + 25.0f * (i % 3);
          feat->orientation = 0.0f;
          fill_descriptor (feat->descriptor, pt, 0);
        }

      if (s == 0)
        s0_feats = sample_features;
      if (s == 1)
        s1_feats = sample_features;

      const Fte3600TemplateStatus expected =
        (s + 1 == FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES) ?
        FTE3600_TEMPLATE_OK : FTE3600_TEMPLATE_NEED_MORE_SAMPLES;
      const Fte3600TemplateStatus status =
        fpi_fte3600_template_add_features (templ, &sample_features, NULL);
      g_assert_cmpint (status, ==, expected);
    }

  g_assert_true (fpi_fte3600_template_is_ready (templ));

  /* Create query probe that has:
   * Points 0, 1, 2 (which only exist in Sample 0, NOT in Sample 1..7)
   * Points 10, 11, 12 (which only exist in Sample 1..7, NOT in Sample 0)
   * 6 non-matching padding points so total features = 12 (>= FTE3600_TEMPLATE_MIN_PHYSICAL_FEATURES). */
  memset (&query, 0, sizeof (query));
  query.extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
  query.n_features = 12;
  const guint probe_pts[6] = { 0, 1, 2, 10, 11, 12 };
  for (guint i = 0; i < 6; i++)
    {
      const guint pt = probe_pts[i];
      Fte3600BriskFeature *feat = &query.features[i];
      feat->x = 10.0f + 3.5f * pt;
      feat->y = 15.0f + 25.0f * (pt % 3);
      feat->orientation = 0.0f;
      fill_descriptor (feat->descriptor, pt, 0);
    }
  for (guint i = 0; i < 6; i++)
    {
      Fte3600BriskFeature *feat = &query.features[6 + i];
      feat->x = 2.0f + 3.0f * (i % 2);
      feat->y = 15.0f + 25.0f * (i % 3);
      feat->orientation = 0.0f;
      fill_descriptor (feat->descriptor, 500 + i, 0);
    }

  /* 1. Directly test matching against Sample 0 alone:
   * Only points 0, 1, 2 match (3 inliers < 5 minimum). Must NOT reach consensus! */
  Fte3600BriskMatchResult s0_match;
  Fte3600BriskStatus s0_status = fpi_fte3600_brisk_match (&query,
                                                          &s0_feats,
                                                          &s0_match);
  g_assert_cmpint (s0_status, ==, FTE3600_BRISK_NO_CONSENSUS);

  /* 2. Directly test matching against Sample 1 alone:
   * Only points 10, 11, 12 match (3 inliers < 5 minimum). Must NOT reach consensus! */
  Fte3600BriskMatchResult s1_match;
  Fte3600BriskStatus s1_status = fpi_fte3600_brisk_match (&query,
                                                          &s1_feats,
                                                          &s1_match);
  g_assert_cmpint (s1_status, ==, FTE3600_BRISK_NO_CONSENSUS);

  /* 3. Test comparing against the template container:
   * Because individual subtemplates each have only 3 inliers, none passes alone.
   * The stitched mosaic contains points 0..12 and matches all 6 probe points,
   * satisfying the five-inlier gate. */
#if FTE3600_ENABLE_PERSONAL_AUTH
  g_assert_cmpint (fpi_fte3600_template_compare_features (
                     templ, &query, FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &result),
                   ==, FTE3600_TEMPLATE_OK);
  g_assert_true (result.authentication_accepted);
  g_assert_cmpuint (result.best.inliers, ==, 6);
#else
  g_assert_cmpint (fpi_fte3600_template_compare_features (
                     templ, &query, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &result),
                   ==, FTE3600_TEMPLATE_OK);
  g_assert_cmpuint (result.diagnostic_passes, >=, 1);
  g_assert_cmpuint (result.best.inliers, ==, 6);
#endif
  g_assert_cmpuint (result.n_compared, ==, 9);
  g_assert_cmpuint (result.best_subtemplate, ==,
                    FTE3600_TEMPLATE_SUBTEMPLATE_MOSAIC);
}

static void
make_noisy_boundary_point (Fte3600BriskFeature *feature,
                           guint                point,
                           gfloat               noise)
{
  feature->x = 10.0f + 3.5f * point + noise * ((point % 3) - 1.0f);
  feature->y = 15.0f + 25.0f * (point % 3) + noise * ((point % 4) - 1.5f);
  fill_descriptor (feature->descriptor, point, 0);
}

static void
test_mosaic_roundtrip_decision (void)
{
  const guint jitter_order[] = { 0, 7, 1, 6, 2, 5, 3, 4 };
  const guint probe_points[] = { 0, 1, 2, 10, 11, 12 };
  const gfloat probe_noise[] = { 0.0f, -0.27f * 3.0f };
  Fte3600BriskFeatureSet samples[FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES] = { 0 };

  g_autoptr(Fte3600Template) forward = fpi_fte3600_template_new ();
  g_autoptr(Fte3600Template) reverse = fpi_fte3600_template_new ();
  g_autoptr(Fte3600Template) decoded = NULL;
  g_autoptr(GBytes) wire = NULL;
  g_autoptr(GBytes) reverse_wire = NULL;

  /* Generated partial-overlap samples with small, non-monotonic coordinate
   * jitter. Capture-order fusion used to accept the marginal probe below,
   * while the same template rejected it after saving and loading. */
  for (guint s = 0; s < G_N_ELEMENTS (samples); s++)
    {
      Fte3600BriskFeatureSet *sample = &samples[s];

      sample->extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
      sample->n_features = 12;
      for (guint i = 0; i < 9; i++)
        make_noisy_boundary_point (&sample->features[i], s == 0 ? i : 4 + i,
                                   0.015f * jitter_order[s]);
      for (guint i = 0; i < 3; i++)
        {
          Fte3600BriskFeature *feature = &sample->features[9 + i];

          feature->x = 56.0f + 2.0f * (i % 2);
          feature->y = 15.0f + 25.0f * (i % 3);
          fill_descriptor (feature->descriptor, s == 0 ? 100 + i : 200 + s * 10 + i, 0);
        }
    }
  for (guint s = 0; s < G_N_ELEMENTS (samples); s++)
    {
      const Fte3600TemplateStatus expected = s + 1 == G_N_ELEMENTS (samples) ?
                                             FTE3600_TEMPLATE_OK : FTE3600_TEMPLATE_NEED_MORE_SAMPLES;

      g_assert_cmpint (fpi_fte3600_template_add_features (forward, &samples[s], NULL),
                       ==, expected);
      g_assert_cmpint (fpi_fte3600_template_add_features (
                         reverse, &samples[G_N_ELEMENTS (samples) - 1 - s], NULL),
                       ==, expected);
    }
  g_assert_cmpint (fpi_fte3600_template_encode (forward, &wire), ==, FTE3600_TEMPLATE_OK);
  g_assert_cmpint (fpi_fte3600_template_encode (reverse, &reverse_wire), ==, FTE3600_TEMPLATE_OK);
  g_assert_true (g_bytes_equal (wire, reverse_wire));
  g_assert_cmpint (fpi_fte3600_template_decode (
                     wire, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &decoded), ==, FTE3600_TEMPLATE_OK);

  for (guint p = 0; p < G_N_ELEMENTS (probe_noise); p++)
    {
      Fte3600BriskFeatureSet query = { 0 };
      Fte3600Template *templates[] = { forward, reverse, decoded };

      query.extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
      query.n_features = 12;
      for (guint i = 0; i < G_N_ELEMENTS (probe_points); i++)
        make_noisy_boundary_point (&query.features[i], probe_points[i], probe_noise[p]);
      for (guint i = 0; i < 6; i++)
        {
          query.features[6 + i].x = 2.0f + 3.0f * (i % 2);
          query.features[6 + i].y = 15.0f + 25.0f * (i % 3);
          fill_descriptor (query.features[6 + i].descriptor, 500 + i, 0);
        }
      for (guint s = 0; s < G_N_ELEMENTS (samples); s++)
        {
          Fte3600BriskMatchResult single;

          g_assert_cmpint (fpi_fte3600_brisk_match (&query, &samples[s], &single),
                           ==, FTE3600_BRISK_NO_CONSENSUS);
        }
      for (guint t = 0; t < G_N_ELEMENTS (templates); t++)
        for (guint purpose = FTE3600_TEMPLATE_LOAD_DIAGNOSTIC;
             purpose <= FTE3600_TEMPLATE_LOAD_AUTHENTICATION; purpose++)
          {
            Fte3600TemplateCompareResult result;

#if !FTE3600_ENABLE_PERSONAL_AUTH
            if (purpose == FTE3600_TEMPLATE_LOAD_AUTHENTICATION)
              continue;
#endif
            g_assert_cmpint (fpi_fte3600_template_compare_features (
                               templates[t], &query, purpose, &result), ==, FTE3600_TEMPLATE_OK);
            g_assert_cmpuint (result.diagnostic_passes, ==, p == 0 ? 1 : 0);
            g_assert_cmpint (result.authentication_accepted, ==,
                             p == 0 && purpose == FTE3600_TEMPLATE_LOAD_AUTHENTICATION);
          }
    }
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/fte3600-template/validate-feature-set",
                   test_validate_feature_set);
  g_test_add_func ("/fte3600-template/roundtrip-and-header",
                   test_roundtrip_and_header);
  g_test_add_func ("/fte3600-template/enrollment-validation",
                   test_enrollment_validation);
  g_test_add_func ("/fte3600-template/location-canonical-rules",
                   test_location_canonical_rules);
  g_test_add_func ("/fte3600-template/authentication-policy",
                   test_authentication_policy);
  g_test_add_func ("/fte3600-template/malformed-headers-and-lengths",
                   test_malformed_headers_and_lengths);
  g_test_add_func ("/fte3600-template/malformed-records",
                   test_malformed_records);
  g_test_add_func ("/fte3600-template/maximum-size", test_maximum_size);
  g_test_add_func ("/fte3600-template/incomplete-and-arguments",
                   test_incomplete_and_arguments);
  g_test_add_func ("/fte3600-template/rounding-mode-isolation",
                   test_rounding_mode_isolation);
  g_test_add_func ("/fte3600-template/dual-enrollment-tied-peaks",
                   test_dual_enrollment_tied_peaks);
  g_test_add_func ("/fte3600-template/dual-engine-fusion",
                   test_dual_engine_fusion);
#if FTE3600_ENABLE_IPA_AUTH
  g_test_add_func ("/fte3600-template/mono-engine-modes",
                   test_mono_engine_modes);
#endif
  g_test_add_func ("/fte3600-template/mixed-ipa-roundtrip",
                   test_mixed_ipa_roundtrip);
  g_test_add_func ("/fte3600-template/fw9369-dual-profile-policy",
                   test_fw9369_dual_profile_policy);
  g_test_add_func ("/fte3600-template/rejected-ipa-preserves-brisk-schema",
                   test_rejected_ipa_preserves_brisk_schema);
  g_test_add_func ("/fte3600-template/ipa-profile-isolation",
                   test_ipa_profile_isolation);
  g_test_add_func ("/fte3600-template/ipa-processing-metadata-roundtrip",
                   test_ipa_processing_metadata_roundtrip);
  g_test_add_func ("/fte3600-template/ipa-version-isolation",
                   test_ipa_version_isolation);
  g_test_add_func ("/fte3600-template/mode-gates-and-query-fallback",
                   test_mode_gates_and_query_fallback);
  g_test_add_func ("/fte3600-template/mosaicking",
                   test_template_mosaicking);
  g_test_add_func ("/fte3600-template/boundary-straddling-probe",
                   test_boundary_straddling_probe);
  g_test_add_func ("/fte3600-template/mosaic-roundtrip-decision",
                   test_mosaic_roundtrip_decision);
  return g_test_run ();
}
