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

#define TEST_FEATURES       12
#define TEST_RECORD_SIZE    (8 + TEST_FEATURES * FTE3600_TEMPLATE_FEATURE_RECORD_SIZE)
#define TEST_WIRE_SIZE      (FTE3600_TEMPLATE_WIRE_HEADER_SIZE + \
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
write_u16 (guint8  *data,
           guint16  value)
{
  data[0] = value & 0xff;
  data[1] = value >> 8;
}

static void
write_u32 (guint8  *data,
           guint32  value)
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
  Fte3600Template *templ = fte3600_template_new ();

  for (guint sample = 0;
       sample < FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES; sample++)
    {
      Fte3600BriskFeatureSet features;
      const Fte3600TemplateStatus expected =
        sample + 1 == FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES ?
          FTE3600_TEMPLATE_OK : FTE3600_TEMPLATE_NEED_MORE_SAMPLES;

      make_feature_set (&features, sample, reverse && (sample & 1));
      g_assert_cmpint (fte3600_template_add_features (templ, &features, NULL),
                       ==, expected);
    }
  g_assert_true (fte3600_template_is_ready (templ));
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
    fte3600_template_decode (wire, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC,
                             &decoded);

  if (status == FTE3600_TEMPLATE_OK)
    fte3600_template_free (decoded);
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

  g_assert_true (fte3600_brisk_validate_feature_set (&features,
                                                      &physical_count));
  g_assert_cmpuint (physical_count, ==, 2);
  features.features[0].orientation = (gfloat) G_PI;
  g_assert_true (fte3600_brisk_validate_feature_set (&features, NULL));
  features.features[0].orientation = -(gfloat) G_PI;
  g_assert_true (fte3600_brisk_validate_feature_set (&features, NULL));
  features.features[0].x = NAN;
  g_assert_false (fte3600_brisk_validate_feature_set (&features,
                                                       &physical_count));
  g_assert_cmpuint (physical_count, ==, 0);
}

static void
test_roundtrip_and_header (void)
{
  g_autoptr(Fte3600Template) original = make_ready_template (TRUE);
  g_autoptr(Fte3600Template) reverse_samples = fte3600_template_new ();
  g_autoptr(Fte3600Template) decoded = NULL;
  g_autoptr(Fte3600Template) copy = NULL;
  g_autoptr(GBytes) wire = NULL;
  g_autoptr(GBytes) roundtrip = NULL;
  g_autoptr(GBytes) copied_wire = NULL;
  g_autoptr(GBytes) reverse_wire = NULL;
  const guint8 *data;
  gsize size;

  g_assert_cmpint (fte3600_template_encode (original, &wire), ==,
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
  g_assert_cmpuint (read_u16 (&data[24]), ==, 1);
  g_assert_cmpuint (read_u16 (&data[26]), ==, 1);
  g_assert_cmpuint (read_u16 (&data[28]), ==,
                    FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION);
  g_assert_cmpuint (read_u16 (&data[30]), ==, 8);
  g_assert_cmpuint (read_u32 (&data[32]), ==, 0);
  g_assert_cmpuint (read_u32 (&data[36]), ==, 0);
  g_assert_cmpuint (read_u32 (&data[40]), ==, TEST_RECORD_SIZE);
  g_assert_cmpuint (read_u16 (&data[44]), ==, TEST_FEATURES);
  g_assert_cmpuint (read_u16 (&data[46]), ==, TEST_FEATURES);
  /* First canonical x coordinate is 8.0f: IEEE-754 0x41000000, little endian. */
  g_assert_cmphex (read_u32 (&data[48]), ==, 0x41000000u);

  g_assert_cmpint (fte3600_template_decode (wire,
                                            FTE3600_TEMPLATE_LOAD_DIAGNOSTIC,
                                            &decoded), ==,
                   FTE3600_TEMPLATE_OK);
  g_assert_true (fte3600_template_is_ready (decoded));
  g_assert_cmpint (fte3600_template_encode (decoded, &roundtrip), ==,
                   FTE3600_TEMPLATE_OK);
  g_assert_true (g_bytes_equal (wire, roundtrip));

  copy = fte3600_template_copy (decoded);
  g_assert_nonnull (copy);
  g_assert_cmpint (fte3600_template_encode (copy, &copied_wire), ==,
                   FTE3600_TEMPLATE_OK);
  g_assert_true (g_bytes_equal (wire, copied_wire));
  g_assert_null (fte3600_template_copy (NULL));

  for (guint added = 0;
       added < FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES; added++)
    {
      Fte3600BriskFeatureSet features;
      const guint sample = FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES - added - 1;

      make_feature_set (&features, sample, added & 1);
      g_assert_cmpint (fte3600_template_add_features (
                           reverse_samples, &features, NULL), ==,
                       added + 1 == FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES ?
                         FTE3600_TEMPLATE_OK :
                         FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
    }
  g_assert_cmpint (fte3600_template_encode (reverse_samples, &reverse_wire),
                   ==, FTE3600_TEMPLATE_OK);
  g_assert_true (g_bytes_equal (wire, reverse_wire));
}

static void
test_enrollment_validation (void)
{
  g_autoptr(Fte3600Template) templ = fte3600_template_new ();
  Fte3600BriskFeatureSet first;
  Fte3600BriskFeatureSet related;
  Fte3600BriskFeatureSet reordered;
  Fte3600BriskFeatureSet unrelated;
  Fte3600BriskFeatureSet insufficient;
  Fte3600BriskMatchResult nearest;

  make_feature_set (&first, 0, FALSE);
  make_feature_set (&reordered, 0, TRUE);
  g_assert_cmpint (fte3600_template_add_features (templ, &first, &nearest), ==,
                   FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
  g_assert_cmpuint (nearest.mutual_matches, ==, 0);
  g_assert_cmpint (fte3600_template_add_features (templ, &reordered, &nearest),
                   ==, FTE3600_TEMPLATE_RETRY_DUPLICATE);

  make_feature_set (&related, 1, FALSE);
  g_assert_cmpint (fte3600_template_add_features (templ, &related, &nearest),
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
  g_assert_cmpint (fte3600_template_add_features (templ, &unrelated, &nearest),
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
      g_assert_cmpint (fte3600_template_add_features (templ, &related, NULL),
                       ==, sample + 1 ==
                           FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES ?
                             FTE3600_TEMPLATE_OK :
                             FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
    }
  g_assert_true (fte3600_template_is_ready (templ));
#endif

  make_feature_set (&insufficient, 3, FALSE);
  insufficient.n_features = 10;
  g_assert_cmpint (fte3600_template_add_features (templ, &insufficient, NULL),
                   ==, FTE3600_TEMPLATE_RETRY_INSUFFICIENT_FEATURES);
  insufficient = first;
  insufficient.extractor_schema_version++;
  g_assert_cmpint (fte3600_template_add_features (templ, &insufficient, NULL),
                   ==, FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR);
}

static void
test_location_canonical_rules (void)
{
  g_autoptr(Fte3600Template) templ = fte3600_template_new ();
  Fte3600BriskFeatureSet features;

  make_feature_set (&features, 0, FALSE);
  features.features[1].x = features.features[0].x + 1.0f;
  features.features[1].y = features.features[0].y;
  g_assert_cmpint (fte3600_template_add_features (templ, &features, NULL), ==,
                   FTE3600_TEMPLATE_INVALID_WIRE);

  make_feature_set (&features, 0, FALSE);
  features.features[1].x = features.features[0].x;
  features.features[1].y = features.features[0].y;
  features.features[1].orientation = features.features[0].orientation;
  g_assert_cmpint (fte3600_template_add_features (templ, &features, NULL), ==,
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
  g_assert_cmpint (fte3600_template_add_features (templ, &features, NULL), ==,
                   FTE3600_TEMPLATE_NEED_MORE_SAMPLES);

  features.features[features.n_features] = features.features[0];
  features.features[features.n_features].orientation = 2.75f;
  features.n_features++;
  g_assert_cmpint (fte3600_template_add_features (templ, &features, NULL), ==,
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
  g_assert_cmpint (fte3600_template_compare_features (
                       templ, &query, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC,
                       &result), ==, FTE3600_TEMPLATE_OK);
  g_assert_cmpuint (result.n_compared, ==,
                    FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES);
  g_assert_cmpuint (result.diagnostic_passes, >=, 1);
  g_assert_cmpuint (result.best_subtemplate, <,
                    FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES);
  g_assert_false (result.authentication_accepted);

  memset (&result, 0xa5, sizeof (result));
#if FTE3600_ENABLE_PERSONAL_AUTH
  g_assert_cmpint (fte3600_template_compare_features (
                       templ, &query, FTE3600_TEMPLATE_LOAD_AUTHENTICATION,
                       &result), ==, FTE3600_TEMPLATE_OK);
  g_assert_true (result.authentication_accepted);
  g_assert_cmpuint (result.n_compared, ==,
                    FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES);
#else
  g_assert_cmpint (fte3600_template_compare_features (
                       templ, &query, FTE3600_TEMPLATE_LOAD_AUTHENTICATION,
                       &result), ==, FTE3600_TEMPLATE_NOT_CALIBRATED);
  g_assert_false (result.authentication_accepted);
  g_assert_cmpuint (result.n_compared, ==, 0);
#endif

  g_assert_cmpint (fte3600_template_encode (templ, &wire), ==,
                   FTE3600_TEMPLATE_OK);
#if FTE3600_ENABLE_PERSONAL_AUTH
  g_assert_cmpint (fte3600_template_decode (
                       wire, FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &decoded),
                   ==, FTE3600_TEMPLATE_OK);
  g_assert_nonnull (decoded);
#else
  g_assert_cmpint (fte3600_template_decode (
                       wire, FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &decoded),
                   ==, FTE3600_TEMPLATE_NOT_CALIBRATED);
  g_assert_null (decoded);
#endif
}

static void
assert_header_mutation (GBytes                    *original,
                        gsize                      offset,
                        guint16                    value,
                        Fte3600TemplateStatus      expected)
{
  guint8 *data;
  gsize size;
  g_autoptr(GBytes) changed = mutable_copy (original, &data, &size);

  g_assert_cmpuint (offset + 2, <=, size);
  write_u16 (&data[offset], value);
  g_assert_cmpint (decode_status (changed), ==, expected);
}

static void
test_malformed_headers_and_lengths (void)
{
  g_autoptr(Fte3600Template) templ = make_ready_template (FALSE);
  g_autoptr(GBytes) wire = NULL;
  const guint8 *original;
  gsize original_size;

  g_assert_cmpint (fte3600_template_encode (templ, &wire), ==,
                   FTE3600_TEMPLATE_OK);
  original = g_bytes_get_data (wire, &original_size);

  assert_header_mutation (wire, 8, 2, FTE3600_TEMPLATE_UNSUPPORTED_SCHEMA);
  assert_header_mutation (wire, 10, 38, FTE3600_TEMPLATE_INVALID_WIRE);
  assert_header_mutation (wire, 16, 0x9360, FTE3600_TEMPLATE_INVALID_WIRE);
  assert_header_mutation (wire, 24, 2,
                          FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR);
  assert_header_mutation (wire, 26, 2,
                          FTE3600_TEMPLATE_UNSUPPORTED_POLICY);
  assert_header_mutation (wire, 28,
                          !FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION,
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
copy_and_mutate_u16 (GBytes  *wire,
                     gsize    offset,
                     guint16  value)
{
  guint8 *data;
  GBytes *copy = mutable_copy (wire, &data, NULL);

  write_u16 (&data[offset], value);
  return copy;
}

static GBytes *
copy_and_mutate_u32 (GBytes  *wire,
                     gsize    offset,
                     guint32  value)
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

  g_assert_cmpint (fte3600_template_encode (templ, &wire), ==,
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
  g_autoptr(Fte3600Template) templ = fte3600_template_new ();
  g_autoptr(Fte3600Template) decoded = NULL;
  g_autoptr(GBytes) wire = NULL;
  gsize size;

  for (guint sample = 0;
       sample < FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES; sample++)
    {
      Fte3600BriskFeatureSet features;

      make_feature_set_n (&features, sample, FTE3600_BRISK_MAX_FEATURES,
                          sample & 1);
      g_assert_cmpint (fte3600_template_add_features (templ, &features, NULL),
                       ==, sample + 1 ==
                           FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES ?
                             FTE3600_TEMPLATE_OK :
                             FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
    }
  g_assert_cmpint (fte3600_template_encode (templ, &wire), ==,
                   FTE3600_TEMPLATE_OK);
  g_bytes_get_data (wire, &size);
  g_assert_cmpuint (size, ==, FTE3600_TEMPLATE_CURRENT_MAX_WIRE_SIZE);
  g_assert_cmpint (fte3600_template_decode (wire,
                                            FTE3600_TEMPLATE_LOAD_DIAGNOSTIC,
                                            &decoded), ==,
                   FTE3600_TEMPLATE_OK);
}

static void
test_incomplete_and_arguments (void)
{
  g_autoptr(Fte3600Template) templ = fte3600_template_new ();
  GBytes *wire = (GBytes *) 0x1;
  Fte3600Template *decoded = (Fte3600Template *) 0x1;

  g_assert_false (fte3600_template_is_ready (NULL));
  g_assert_cmpint (fte3600_template_encode (templ, &wire), ==,
                   FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
  g_assert_null (wire);
  g_assert_cmpint (fte3600_template_decode (
                       NULL, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &decoded), ==,
                   FTE3600_TEMPLATE_INVALID_WIRE);
  g_assert_null (decoded);
  g_assert_cmpint (fte3600_template_add_features (NULL, NULL, NULL), ==,
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
  g_assert_cmpint (fte3600_template_encode (templ, &baseline_wire), ==,
                   FTE3600_TEMPLATE_OK);
  g_assert_cmpint (fte3600_template_decode (
                       baseline_wire, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC,
                       &baseline_decoded), ==, FTE3600_TEMPLATE_OK);
  g_assert_cmpint (fte3600_template_compare_features (
                       baseline_decoded, &query,
                       FTE3600_TEMPLATE_LOAD_DIAGNOSTIC,
                       &baseline_result), ==, FTE3600_TEMPLATE_OK);

  for (guint i = 0; i < G_N_ELEMENTS (modes); i++)
    {
      g_autoptr(Fte3600Template) decoded = NULL;
      g_autoptr(Fte3600Template) partial = fte3600_template_new ();
      g_autoptr(GBytes) wire = NULL;
      Fte3600TemplateCompareResult result;

      g_assert_cmpint (fesetround (modes[i]), ==, 0);
      g_assert_cmpint (fte3600_template_encode (templ, &wire), ==,
                       FTE3600_TEMPLATE_OK);
      g_assert_cmpint (fegetround (), ==, modes[i]);
      g_assert_true (g_bytes_equal (wire, baseline_wire));

      g_assert_cmpint (fte3600_template_decode (
                           wire, FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &decoded),
                       ==, FTE3600_TEMPLATE_OK);
      g_assert_cmpint (fegetround (), ==, modes[i]);
      g_assert_cmpint (fte3600_template_compare_features (
                           decoded, &query,
                           FTE3600_TEMPLATE_LOAD_DIAGNOSTIC, &result), ==,
                       FTE3600_TEMPLATE_OK);
      g_assert_cmpint (fegetround (), ==, modes[i]);
      g_assert_cmpmem (&result, sizeof (result),
                       &baseline_result, sizeof (baseline_result));

      g_assert_cmpint (fte3600_template_add_features (partial, &query, NULL),
                       ==, FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
      g_assert_cmpint (fegetround (), ==, modes[i]);
      g_assert_cmpint (fte3600_template_compare_features (
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
  return g_test_run ();
}
