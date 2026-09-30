/*
 * Versioned clean-room BRISK template container for FocalTech FT9361
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "fte3600-template.h"

#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TEMPLATE_MODEL_ID 0x9361
#define TEMPLATE_SUBTEMPLATE_HEADER_SIZE 8

static const guint8 template_magic[8] = {
  'F', 'T', '3', '6', 'B', 'R', 'K', '\0'
};

typedef enum {
  FEATURE_SET_VALID,
  FEATURE_SET_INVALID,
  FEATURE_SET_UNSUPPORTED_EXTRACTOR,
  FEATURE_SET_INSUFFICIENT,
} FeatureSetValidation;

typedef struct
{
  Fte3600BriskFeatureSet features;
  guint                  physical_count;
} CanonicalSubtemplate;

typedef struct
{
  gint     previous_mode;
  gboolean changed;
} TemplateRoundingGuard;

typedef struct
{
  gdouble angle;
  gdouble translate_x;
  gdouble translate_y;
} TemplatePose;

struct _Fte3600Template
{
  guint                  n_subtemplates;
  CanonicalSubtemplate   subtemplates[FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES];
  gboolean               has_mosaic;
  Fte3600BriskFeatureSet mosaic;
  TemplatePose           poses[FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES];
};

static void
template_secure_clear (gpointer data,
                       gsize    size)
{
  volatile guint8 *bytes = data;

  while (size-- > 0)
    *bytes++ = 0;
}

G_STATIC_ASSERT (sizeof (gfloat) == 4);
G_STATIC_ASSERT (FLT_RADIX == 2);
G_STATIC_ASSERT (FLT_MANT_DIG == 24);
G_STATIC_ASSERT (FLT_MAX_EXP == 128);
G_STATIC_ASSERT (FTE3600_TEMPLATE_FEATURE_RECORD_SIZE ==
                 3 * sizeof (gfloat) + FTE3600_BRISK_DESCRIPTOR_BYTES);
G_STATIC_ASSERT (FTE3600_TEMPLATE_CURRENT_MAX_WIRE_SIZE ==
                 FTE3600_TEMPLATE_WIRE_HEADER_SIZE +
                 FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES *
                 (TEMPLATE_SUBTEMPLATE_HEADER_SIZE +
                  FTE3600_BRISK_MAX_FEATURES *
                  FTE3600_TEMPLATE_FEATURE_RECORD_SIZE));
G_STATIC_ASSERT (FTE3600_TEMPLATE_MAX_WIRE_SIZE ==
                 FTE3600_TEMPLATE_WIRE_HEADER_SIZE + 12 *
                 (TEMPLATE_SUBTEMPLATE_HEADER_SIZE +
                  FTE3600_BRISK_MAX_FEATURES *
                  FTE3600_TEMPLATE_FEATURE_RECORD_SIZE));

static gboolean
template_rounding_guard_enter (TemplateRoundingGuard *guard)
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
template_rounding_guard_clear (TemplateRoundingGuard *guard)
{
  if (guard->changed && fesetround (guard->previous_mode) != 0)
    g_warning ("Failed to restore caller floating-point rounding mode");
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (TemplateRoundingGuard,
                                  template_rounding_guard_clear)

static void
put_uint16_le (guint8 *destination,
               guint16 value)
{
  destination[0] = value & 0xff;
  destination[1] = value >> 8;
}

static void
put_uint32_le (guint8 *destination,
               guint32 value)
{
  destination[0] = value & 0xff;
  destination[1] = (value >> 8) & 0xff;
  destination[2] = (value >> 16) & 0xff;
  destination[3] = value >> 24;
}

static guint16
get_uint16_le (const guint8 *source)
{
  return (guint16) source[0] | (guint16) source[1] << 8;
}

static guint32
get_uint32_le (const guint8 *source)
{
  return (guint32) source[0] |
         (guint32) source[1] << 8 |
         (guint32) source[2] << 16 |
         (guint32) source[3] << 24;
}

static guint32
float_bits (gfloat value)
{
  guint32 bits;

  memcpy (&bits, &value, sizeof (bits));
  return bits;
}

static gfloat
float_from_bits (guint32 bits)
{
  gfloat value;

  memcpy (&value, &bits, sizeof (value));
  return value;
}

static gint
feature_compare (const void *first,
                 const void *second)
{
  const Fte3600BriskFeature *a = first;
  const Fte3600BriskFeature *b = second;
  gint descriptor_order;

  if (a->x < b->x)
    return -1;
  if (a->x > b->x)
    return 1;
  if (a->y < b->y)
    return -1;
  if (a->y > b->y)
    return 1;
  if (a->orientation < b->orientation)
    return -1;
  if (a->orientation > b->orientation)
    return 1;
  descriptor_order = memcmp (a->descriptor, b->descriptor,
                             sizeof (a->descriptor));
  return (descriptor_order > 0) - (descriptor_order < 0);
}

static gint
subtemplate_compare (const void *first,
                     const void *second)
{
  const CanonicalSubtemplate *a = first;
  const CanonicalSubtemplate *b = second;

  if (a->features.n_features < b->features.n_features)
    return -1;
  if (a->features.n_features > b->features.n_features)
    return 1;
  for (guint i = 0; i < a->features.n_features; i++)
    {
      const gint order = feature_compare (&a->features.features[i],
                                          &b->features.features[i]);

      if (order != 0)
        return order;
    }
  return 0;
}

static gboolean
same_location (const Fte3600BriskFeature *first,
               const Fte3600BriskFeature *second)
{
  return first->x == second->x && first->y == second->y;
}

static FeatureSetValidation
canonicalize_feature_set (const Fte3600BriskFeatureSet *source,
                          CanonicalSubtemplate         *canonical)
{
  guint public_physical_count;
  guint exact_physical_count = 0;

  memset (canonical, 0, sizeof (*canonical));
  if (source == NULL)
    return FEATURE_SET_INVALID;
  if (source->extractor_schema_version !=
      FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION)
    return FEATURE_SET_UNSUPPORTED_EXTRACTOR;
  if (!fpi_fte3600_brisk_validate_feature_set (source, &public_physical_count))
    return FEATURE_SET_INVALID;

  canonical->features.extractor_schema_version =
    source->extractor_schema_version;
  canonical->features.n_features = source->n_features;
  memcpy (canonical->features.features, source->features,
          source->n_features * sizeof (source->features[0]));
  for (guint i = 0; i < canonical->features.n_features; i++)
    {
      Fte3600BriskFeature *feature = &canonical->features.features[i];

      /* IEEE -0 has the same mathematical value as +0.  Persist only +0 so
       * one semantic template has exactly one byte representation. */
      if (feature->x == 0.0f)
        feature->x = 0.0f;
      if (feature->y == 0.0f)
        feature->y = 0.0f;
      if (feature->orientation == 0.0f)
        feature->orientation = 0.0f;
      if (feature->orientation == FTE3600_BRISK_ORIENTATION_LIMIT)
        feature->orientation = -FTE3600_BRISK_ORIENTATION_LIMIT;
    }
  qsort (canonical->features.features, canonical->features.n_features,
         sizeof (canonical->features.features[0]), feature_compare);

  for (guint i = 0; i < canonical->features.n_features; i++)
    {
      const Fte3600BriskFeature *feature =
        &canonical->features.features[i];
      guint variants_at_location = 1;

      if (i == 0 ||
          !same_location (feature, &canonical->features.features[i - 1]))
        exact_physical_count++;
      for (guint j = i + 1; j < canonical->features.n_features; j++)
        {
          const Fte3600BriskFeature *other =
            &canonical->features.features[j];
          const gdouble dx = feature->x - other->x;
          const gdouble dy = feature->y - other->y;

          if (same_location (feature, other))
            {
              variants_at_location++;
              if (feature->orientation == other->orientation)
                return FEATURE_SET_INVALID;
            }
          else if (dx * dx + dy * dy < 2.25)
            {
              /* Schema v1 permits orientation variants only at the exact
               * detector location.  Nearby-but-distinct points would make
               * physical grouping dependent on transitive chains. */
              return FEATURE_SET_INVALID;
            }
        }
      if (variants_at_location >
          FTE3600_TEMPLATE_MAX_ORIENTATIONS_PER_LOCATION)
        return FEATURE_SET_INVALID;
    }

  if (exact_physical_count != public_physical_count)
    return FEATURE_SET_INVALID;
  canonical->physical_count = exact_physical_count;
  if (canonical->physical_count < FTE3600_TEMPLATE_MIN_PHYSICAL_FEATURES)
    return FEATURE_SET_INSUFFICIENT;
  return FEATURE_SET_VALID;
}

static gboolean
match_is_better (const Fte3600BriskMatchResult *candidate,
                 const Fte3600BriskMatchResult *current)
{
  if (candidate->diagnostic_policy_passed != current->diagnostic_policy_passed)
    return candidate->diagnostic_policy_passed;
  if (candidate->inliers != current->inliers)
    return candidate->inliers > current->inliers;
  if (candidate->mutual_matches != current->mutual_matches)
    return candidate->mutual_matches > current->mutual_matches;
  if (candidate->inlier_ratio != current->inlier_ratio)
    return candidate->inlier_ratio > current->inlier_ratio;
  if (candidate->mean_hamming != current->mean_hamming)
    return candidate->mean_hamming < current->mean_hamming;
  return candidate->rms_error < current->rms_error;
}

static Fte3600TemplateStatus
validation_to_status (FeatureSetValidation validation)
{
  switch (validation)
    {
    case FEATURE_SET_VALID:
      return FTE3600_TEMPLATE_OK;

    case FEATURE_SET_UNSUPPORTED_EXTRACTOR:
      return FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR;

    case FEATURE_SET_INSUFFICIENT:
      return FTE3600_TEMPLATE_RETRY_INSUFFICIENT_FEATURES;

    case FEATURE_SET_INVALID:
    default:
      return FTE3600_TEMPLATE_INVALID_WIRE;
    }
}

Fte3600Template *
fpi_fte3600_template_new (void)
{
  return g_new0 (Fte3600Template, 1);
}

Fte3600Template *
fpi_fte3600_template_copy (const Fte3600Template *templ)
{
  Fte3600Template *copy;

  if (templ == NULL)
    return NULL;
  copy = g_new (Fte3600Template, 1);
  *copy = *templ;
  return copy;
}

void
fpi_fte3600_template_free (Fte3600Template *templ)
{
  if (templ == NULL)
    return;

  template_secure_clear (templ, sizeof (*templ));
  g_free (templ);
}

static guint
template_hamming_distance (const guint8 *a,
                           const guint8 *b,
                           gsize         len)
{
  guint dist = 0;

  for (gsize i = 0; i < len; i++)
    dist += __builtin_popcount (a[i] ^ b[i]);
  return dist;
}

static gdouble
template_wrap_angle (gdouble angle)
{
  while (angle <= -G_PI)
    angle += 2.0 * G_PI;
  while (angle > G_PI)
    angle -= 2.0 * G_PI;
  return angle;
}

static void
template_stitch_sample (Fte3600Template              *templ,
                        const Fte3600BriskFeatureSet *sample,
                        gdouble                       pose_angle,
                        gdouble                       pose_tx,
                        gdouble                       pose_ty)
{
  const gdouble cosine = cos (pose_angle);
  const gdouble sine = sin (pose_angle);

  for (guint i = 0; i < sample->n_features; i++)
    {
      const Fte3600BriskFeature *feat = &sample->features[i];
      const gdouble xm = cosine * feat->x - sine * feat->y + pose_tx;
      const gdouble ym = sine * feat->x + cosine * feat->y + pose_ty;
      gdouble orient_m = template_wrap_angle (feat->orientation + pose_angle);
      gboolean fused = FALSE;

      if (orient_m > FTE3600_BRISK_ORIENTATION_LIMIT)
        orient_m = -FTE3600_BRISK_ORIENTATION_LIMIT;

      if (xm < 0.0 || xm >= FTE3600_BRISK_MOSAIC_WIDTH ||
          ym < 0.0 || ym >= FTE3600_BRISK_MOSAIC_HEIGHT)
        continue;

      /* Check for duplicates in existing mosaic (within 2.5px & Hamming <= 40) */
      for (guint j = 0; j < templ->mosaic.n_features; j++)
        {
          Fte3600BriskFeature *existing = &templ->mosaic.features[j];
          const gdouble dx = xm - existing->x;
          const gdouble dy = ym - existing->y;

          if (dx * dx + dy * dy < 2.5 * 2.5)
            {
              const guint h = template_hamming_distance (feat->descriptor,
                                                         existing->descriptor,
                                                         FTE3600_BRISK_DESCRIPTOR_BYTES);
              if (h <= 40)
                {
                  /* Fuse: average coordinates to refine subpixel location */
                  existing->x = 0.5f * (existing->x + (gfloat) xm);
                  existing->y = 0.5f * (existing->y + (gfloat) ym);
                  fused = TRUE;
                  break;
                }
            }
        }

      if (!fused && templ->mosaic.n_features < FTE3600_BRISK_MAX_FEATURES)
        {
          Fte3600BriskFeature *new_f = &templ->mosaic.features[templ->mosaic.n_features++];
          new_f->x = (gfloat) xm;
          new_f->y = (gfloat) ym;
          new_f->orientation = (gfloat) orient_m;
          memcpy (new_f->descriptor, feat->descriptor, sizeof (new_f->descriptor));
        }
    }
}

static void
template_reconstruct_mosaic (Fte3600Template *templ)
{
  gboolean aligned[FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES] = { FALSE };

  if (templ == NULL || templ->n_subtemplates == 0)
    return;

  memset (&templ->mosaic, 0, sizeof (templ->mosaic));
  templ->mosaic.extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
  templ->has_mosaic = FALSE;

  /* Sample 0 is anchor */
  templ->poses[0].angle = 0.0;
  templ->poses[0].translate_x = FTE3600_BRISK_MOSAIC_ANCHOR_X;
  templ->poses[0].translate_y = FTE3600_BRISK_MOSAIC_ANCHOR_Y;
  template_stitch_sample (templ, &templ->subtemplates[0].features,
                          templ->poses[0].angle,
                          templ->poses[0].translate_x,
                          templ->poses[0].translate_y);
  aligned[0] = TRUE;

  /* Align remaining subtemplates by matching against already aligned subtemplates */
  for (guint step = 1; step < templ->n_subtemplates; step++)
    {
      guint best_unaligned = G_MAXUINT;
      guint best_aligned = G_MAXUINT;
      Fte3600BriskMatchResult best_match = { 0 };
      gboolean found_match = FALSE;

      for (guint u = 0; u < templ->n_subtemplates; u++)
        {
          if (aligned[u])
            continue;

          for (guint a = 0; a < templ->n_subtemplates; a++)
            {
              if (!aligned[a])
                continue;

              Fte3600BriskMatchResult match;
              Fte3600BriskStatus status;

              status = fpi_fte3600_brisk_match (&templ->subtemplates[u].features,
                                                &templ->subtemplates[a].features,
                                                &match);
              if (status == FTE3600_BRISK_OK && match.inliers >= FTE3600_BRISK_MIN_INLIERS)
                {
                  if (!found_match || match_is_better (&match, &best_match))
                    {
                      best_match = match;
                      best_unaligned = u;
                      best_aligned = a;
                      found_match = TRUE;
                    }
                }
            }
        }

      if (!found_match)
        break;

      /* u is matched against a: pose_u = pose_a o pose_{u -> a} */
      const TemplatePose *pose_a = &templ->poses[best_aligned];
      const gdouble cos_a = cos (pose_a->angle);
      const gdouble sin_a = sin (pose_a->angle);
      TemplatePose *pose_u = &templ->poses[best_unaligned];

      pose_u->angle = template_wrap_angle (pose_a->angle + best_match.angle);
      pose_u->translate_x = cos_a * best_match.translate_x - sin_a * best_match.translate_y + pose_a->translate_x;
      pose_u->translate_y = sin_a * best_match.translate_x + cos_a * best_match.translate_y + pose_a->translate_y;

      template_stitch_sample (templ, &templ->subtemplates[best_unaligned].features,
                              pose_u->angle, pose_u->translate_x, pose_u->translate_y);
      aligned[best_unaligned] = TRUE;
    }

  qsort (templ->mosaic.features, templ->mosaic.n_features,
         sizeof (templ->mosaic.features[0]), feature_compare);

  if (templ->mosaic.n_features >= FTE3600_TEMPLATE_MIN_PHYSICAL_FEATURES)
    templ->has_mosaic = TRUE;
}

const Fte3600BriskFeatureSet *
fpi_fte3600_template_get_mosaic (const Fte3600Template *templ)
{
  if (templ == NULL || !templ->has_mosaic)
    return NULL;
  return &templ->mosaic;
}

Fte3600TemplateStatus
fpi_fte3600_template_add_features (Fte3600Template              *templ,
                                   const Fte3600BriskFeatureSet *features,
                                   Fte3600BriskMatchResult      *nearest_match)
{
  g_auto(TemplateRoundingGuard) rounding_guard = { 0 };
  CanonicalSubtemplate candidate;
  FeatureSetValidation validation;
  gboolean have_nearest = FALSE;
  gboolean duplicate = FALSE;
  guint best_ref_idx = 0;
  Fte3600BriskMatchResult best_ref_match = { 0 };
  gboolean have_best_ref = FALSE;
#if FTE3600_ENABLE_PERSONAL_AUTH
  gboolean consistent = FALSE;
#endif

  if (nearest_match != NULL)
    memset (nearest_match, 0, sizeof (*nearest_match));
  if (!template_rounding_guard_enter (&rounding_guard))
    return FTE3600_TEMPLATE_INVALID_WIRE;
  validation = canonicalize_feature_set (features, &candidate);
  if (templ == NULL)
    return FTE3600_TEMPLATE_INVALID_WIRE;
  if (validation != FEATURE_SET_VALID)
    return validation_to_status (validation);
  if (templ->n_subtemplates > FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES)
    return FTE3600_TEMPLATE_INVALID_WIRE;

  for (guint i = 0; i < templ->n_subtemplates; i++)
    {
      Fte3600BriskMatchResult match;

      duplicate |=
        subtemplate_compare (&candidate, &templ->subtemplates[i]) == 0;
      (void) fpi_fte3600_brisk_match (&candidate.features,
                                      &templ->subtemplates[i].features, &match);
#if FTE3600_ENABLE_PERSONAL_AUTH
      consistent |= match.authentication_accepted;
#endif
      if (!have_best_ref || match_is_better (&match, &best_ref_match))
        {
          best_ref_match = match;
          best_ref_idx = i;
          have_best_ref = TRUE;
        }
      if (!have_nearest || (nearest_match != NULL &&
                            match_is_better (&match, nearest_match)))
        {
          if (nearest_match != NULL)
            *nearest_match = match;
          have_nearest = TRUE;
        }
    }

  if (duplicate)
    return FTE3600_TEMPLATE_RETRY_DUPLICATE;

  if (templ->n_subtemplates == FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES)
    return FTE3600_TEMPLATE_INVALID_WIRE;

#if FTE3600_ENABLE_PERSONAL_AUTH
  /* A sample which cannot authenticate against any already accepted sample
   * must not become an independent accepted identity in the any-of-eight
   * gallery.  The first sample establishes the enrollment anchor. */
  if (templ->n_subtemplates > 0 && !consistent)
    return FTE3600_TEMPLATE_RETRY_INCONSISTENT;
#endif

  /* Incremental stitching into mosaic */
  const guint cur_idx = templ->n_subtemplates;

  if (cur_idx == 0)
    {
      memset (&templ->mosaic, 0, sizeof (templ->mosaic));
      templ->mosaic.extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
      templ->poses[0].angle = 0.0;
      templ->poses[0].translate_x = FTE3600_BRISK_MOSAIC_ANCHOR_X;
      templ->poses[0].translate_y = FTE3600_BRISK_MOSAIC_ANCHOR_Y;
      template_stitch_sample (templ, &candidate.features,
                              templ->poses[0].angle,
                              templ->poses[0].translate_x,
                              templ->poses[0].translate_y);
      templ->has_mosaic = TRUE;
    }
  else if (have_best_ref && best_ref_match.inliers >= FTE3600_BRISK_MIN_INLIERS)
    {
      const TemplatePose *ref_pose = &templ->poses[best_ref_idx];
      const gdouble cos_ref = cos (ref_pose->angle);
      const gdouble sin_ref = sin (ref_pose->angle);
      TemplatePose *cur_pose = &templ->poses[cur_idx];

      cur_pose->angle = template_wrap_angle (ref_pose->angle + best_ref_match.angle);
      cur_pose->translate_x = cos_ref * best_ref_match.translate_x -
                              sin_ref * best_ref_match.translate_y + ref_pose->translate_x;
      cur_pose->translate_y = sin_ref * best_ref_match.translate_x +
                              cos_ref * best_ref_match.translate_y + ref_pose->translate_y;
      template_stitch_sample (templ, &candidate.features,
                              cur_pose->angle,
                              cur_pose->translate_x,
                              cur_pose->translate_y);
    }

  templ->subtemplates[templ->n_subtemplates++] = candidate;
  if (templ->n_subtemplates == FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES)
    {
      qsort (templ->mosaic.features, templ->mosaic.n_features,
             sizeof (templ->mosaic.features[0]), feature_compare);
      if (templ->mosaic.n_features >= FTE3600_TEMPLATE_MIN_PHYSICAL_FEATURES)
        templ->has_mosaic = TRUE;
      return FTE3600_TEMPLATE_OK;
    }
  return FTE3600_TEMPLATE_NEED_MORE_SAMPLES;
}

gboolean
fpi_fte3600_template_is_ready (const Fte3600Template *templ)
{
  return templ != NULL &&
         templ->n_subtemplates == FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES;
}

Fte3600TemplateStatus
fpi_fte3600_template_encode (const Fte3600Template *templ,
                             GBytes               **wire)
{
  g_auto(TemplateRoundingGuard) rounding_guard = { 0 };
  CanonicalSubtemplate sorted[FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES];
  gsize total_size = FTE3600_TEMPLATE_WIRE_HEADER_SIZE;
  guint8 *data;
  gsize offset;

  if (wire != NULL)
    *wire = NULL;
  if (!template_rounding_guard_enter (&rounding_guard))
    return FTE3600_TEMPLATE_INVALID_WIRE;
  if (wire == NULL || !fpi_fte3600_template_is_ready (templ))
    return templ == NULL || wire == NULL ? FTE3600_TEMPLATE_INVALID_WIRE :
           FTE3600_TEMPLATE_NEED_MORE_SAMPLES;

  for (guint i = 0; i < templ->n_subtemplates; i++)
    {
      const FeatureSetValidation validation =
        canonicalize_feature_set (&templ->subtemplates[i].features, &sorted[i]);

      if (validation != FEATURE_SET_VALID)
        return validation_to_status (validation);
      total_size += TEMPLATE_SUBTEMPLATE_HEADER_SIZE +
                    sorted[i].features.n_features *
                    FTE3600_TEMPLATE_FEATURE_RECORD_SIZE;
    }
  qsort (sorted, G_N_ELEMENTS (sorted), sizeof (sorted[0]),
         subtemplate_compare);
  for (guint i = 1; i < G_N_ELEMENTS (sorted); i++)
    if (subtemplate_compare (&sorted[i - 1], &sorted[i]) == 0)
      return FTE3600_TEMPLATE_RETRY_DUPLICATE;
  if (total_size > FTE3600_TEMPLATE_CURRENT_MAX_WIRE_SIZE ||
      total_size > G_MAXUINT32)
    return FTE3600_TEMPLATE_INVALID_WIRE;

  data = g_malloc0 (total_size);
  memcpy (data, template_magic, sizeof (template_magic));
  put_uint16_le (&data[8], FTE3600_TEMPLATE_WIRE_VERSION);
  put_uint16_le (&data[10], FTE3600_TEMPLATE_WIRE_HEADER_SIZE);
  put_uint32_le (&data[12], total_size);
  put_uint16_le (&data[16], TEMPLATE_MODEL_ID);
  put_uint16_le (&data[18], FTE3600_BRISK_WIDTH);
  put_uint16_le (&data[20], FTE3600_BRISK_HEIGHT);
  put_uint16_le (&data[22], FTE3600_TEMPLATE_FEATURE_RECORD_SIZE);
  put_uint16_le (&data[24], FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION);
  put_uint16_le (&data[26], FTE3600_BRISK_DIAGNOSTIC_POLICY_VERSION);
  put_uint16_le (&data[28], FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION);
  put_uint16_le (&data[30], FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES);
  /* flags and reserved are already the canonical all-zero representation. */

  offset = FTE3600_TEMPLATE_WIRE_HEADER_SIZE;
  for (guint sample = 0; sample < G_N_ELEMENTS (sorted); sample++)
    {
      const Fte3600BriskFeatureSet *features = &sorted[sample].features;
      const guint32 record_size = TEMPLATE_SUBTEMPLATE_HEADER_SIZE +
                                  features->n_features *
                                  FTE3600_TEMPLATE_FEATURE_RECORD_SIZE;

      put_uint32_le (&data[offset], record_size);
      put_uint16_le (&data[offset + 4], features->n_features);
      put_uint16_le (&data[offset + 6], sorted[sample].physical_count);
      offset += TEMPLATE_SUBTEMPLATE_HEADER_SIZE;
      for (guint i = 0; i < features->n_features; i++)
        {
          const Fte3600BriskFeature *feature = &features->features[i];

          put_uint32_le (&data[offset], float_bits (feature->x));
          put_uint32_le (&data[offset + 4], float_bits (feature->y));
          put_uint32_le (&data[offset + 8], float_bits (feature->orientation));
          memcpy (&data[offset + 12], feature->descriptor,
                  sizeof (feature->descriptor));
          offset += FTE3600_TEMPLATE_FEATURE_RECORD_SIZE;
        }
    }
  g_assert (offset == total_size);
  *wire = g_bytes_new_take (data, total_size);
  return FTE3600_TEMPLATE_OK;
}

static Fte3600TemplateStatus
validate_header (const guint8 *data,
                 gsize         size)
{
  if (size < FTE3600_TEMPLATE_WIRE_HEADER_SIZE ||
      size > FTE3600_TEMPLATE_MAX_WIRE_SIZE ||
      memcmp (data, template_magic, sizeof (template_magic)) != 0)
    return FTE3600_TEMPLATE_INVALID_WIRE;
  if (get_uint16_le (&data[8]) != FTE3600_TEMPLATE_WIRE_VERSION)
    return FTE3600_TEMPLATE_UNSUPPORTED_SCHEMA;
  if (get_uint16_le (&data[10]) != FTE3600_TEMPLATE_WIRE_HEADER_SIZE ||
      get_uint32_le (&data[12]) != size ||
      get_uint16_le (&data[16]) != TEMPLATE_MODEL_ID ||
      get_uint16_le (&data[18]) != FTE3600_BRISK_WIDTH ||
      get_uint16_le (&data[20]) != FTE3600_BRISK_HEIGHT ||
      get_uint16_le (&data[22]) != FTE3600_TEMPLATE_FEATURE_RECORD_SIZE ||
      get_uint16_le (&data[30]) != FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES ||
      get_uint32_le (&data[32]) != 0 || get_uint32_le (&data[36]) != 0)
    return FTE3600_TEMPLATE_INVALID_WIRE;
  if (get_uint16_le (&data[24]) !=
      FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION)
    return FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR;
  if (get_uint16_le (&data[26]) !=
      FTE3600_BRISK_DIAGNOSTIC_POLICY_VERSION ||
      get_uint16_le (&data[28]) !=
      FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION)
    return FTE3600_TEMPLATE_UNSUPPORTED_POLICY;
  if (size > FTE3600_TEMPLATE_CURRENT_MAX_WIRE_SIZE)
    return FTE3600_TEMPLATE_INVALID_WIRE;
  return FTE3600_TEMPLATE_OK;
}

Fte3600TemplateStatus
fpi_fte3600_template_decode (GBytes                    *wire,
                             Fte3600TemplateLoadPurpose purpose,
                             Fte3600Template          **templ)
{
  g_auto(TemplateRoundingGuard) rounding_guard = { 0 };
  const guint8 *data;
  gsize size;
  gsize offset = FTE3600_TEMPLATE_WIRE_HEADER_SIZE;
  g_autoptr(Fte3600Template) decoded = NULL;
  Fte3600TemplateStatus status;

  if (templ != NULL)
    *templ = NULL;
  if (!template_rounding_guard_enter (&rounding_guard))
    return FTE3600_TEMPLATE_INVALID_WIRE;
  if (wire == NULL || templ == NULL ||
      (purpose != FTE3600_TEMPLATE_LOAD_DIAGNOSTIC &&
       purpose != FTE3600_TEMPLATE_LOAD_AUTHENTICATION))
    return FTE3600_TEMPLATE_INVALID_WIRE;
  data = g_bytes_get_data (wire, &size);
  status = validate_header (data, size);
  if (status != FTE3600_TEMPLATE_OK)
    return status;

  decoded = fpi_fte3600_template_new ();
  for (guint sample = 0;
       sample < FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES; sample++)
    {
      CanonicalSubtemplate parsed;
      CanonicalSubtemplate canonical;
      guint32 record_size;
      guint feature_count;
      guint stored_physical_count;
      gsize expected_record_size;

      memset (&parsed, 0, sizeof (parsed));
      if (offset > size || size - offset < TEMPLATE_SUBTEMPLATE_HEADER_SIZE)
        return FTE3600_TEMPLATE_INVALID_WIRE;
      record_size = get_uint32_le (&data[offset]);
      feature_count = get_uint16_le (&data[offset + 4]);
      stored_physical_count = get_uint16_le (&data[offset + 6]);
      if (feature_count < FTE3600_TEMPLATE_MIN_PHYSICAL_FEATURES ||
          feature_count > FTE3600_BRISK_MAX_FEATURES)
        return FTE3600_TEMPLATE_INVALID_WIRE;
      expected_record_size = TEMPLATE_SUBTEMPLATE_HEADER_SIZE +
                             feature_count *
                             FTE3600_TEMPLATE_FEATURE_RECORD_SIZE;
      if (record_size != expected_record_size || record_size > size - offset)
        return FTE3600_TEMPLATE_INVALID_WIRE;
      offset += TEMPLATE_SUBTEMPLATE_HEADER_SIZE;

      parsed.features.extractor_schema_version =
        FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
      parsed.features.n_features = feature_count;
      for (guint i = 0; i < feature_count; i++)
        {
          Fte3600BriskFeature *feature = &parsed.features.features[i];
          const guint32 x_bits = get_uint32_le (&data[offset]);
          const guint32 y_bits = get_uint32_le (&data[offset + 4]);
          const guint32 orientation_bits = get_uint32_le (&data[offset + 8]);

          /* Reject negative zero rather than silently normalizing a
           * non-canonical wire representation. */
          if (x_bits == 0x80000000u || y_bits == 0x80000000u ||
              orientation_bits == 0x80000000u)
            return FTE3600_TEMPLATE_INVALID_WIRE;
          feature->x = float_from_bits (x_bits);
          feature->y = float_from_bits (y_bits);
          feature->orientation = float_from_bits (orientation_bits);
          memcpy (feature->descriptor, &data[offset + 12],
                  sizeof (feature->descriptor));
          offset += FTE3600_TEMPLATE_FEATURE_RECORD_SIZE;
        }

      if (canonicalize_feature_set (&parsed.features, &canonical) !=
          FEATURE_SET_VALID || canonical.physical_count != stored_physical_count)
        return FTE3600_TEMPLATE_INVALID_WIRE;
      for (guint i = 0; i < feature_count; i++)
        if (feature_compare (&parsed.features.features[i],
                             &canonical.features.features[i]) != 0)
          return FTE3600_TEMPLATE_INVALID_WIRE;
      if (sample > 0 &&
          subtemplate_compare (&decoded->subtemplates[sample - 1],
                               &canonical) >= 0)
        return FTE3600_TEMPLATE_INVALID_WIRE;
      decoded->subtemplates[sample] = canonical;
      decoded->n_subtemplates++;
    }
  if (offset != size)
    return FTE3600_TEMPLATE_INVALID_WIRE;

  template_reconstruct_mosaic (decoded);

  if (purpose == FTE3600_TEMPLATE_LOAD_AUTHENTICATION &&
      FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION == 0)
    return FTE3600_TEMPLATE_NOT_CALIBRATED;
  *templ = g_steal_pointer (&decoded);
  return FTE3600_TEMPLATE_OK;
}

Fte3600TemplateStatus
fpi_fte3600_template_compare_features (const Fte3600Template        *templ,
                                       const Fte3600BriskFeatureSet *query,
                                       Fte3600TemplateLoadPurpose    purpose,
                                       Fte3600TemplateCompareResult *result)
{
  g_auto(TemplateRoundingGuard) rounding_guard = { 0 };
  CanonicalSubtemplate canonical_query;
  FeatureSetValidation validation;
  gboolean have_best = FALSE;

  if (result != NULL)
    {
      memset (result, 0, sizeof (*result));
      result->best_subtemplate = G_MAXUINT;
    }
  if (!template_rounding_guard_enter (&rounding_guard))
    return FTE3600_TEMPLATE_INVALID_WIRE;
  validation = canonicalize_feature_set (query, &canonical_query);
  if (templ == NULL || result == NULL || !fpi_fte3600_template_is_ready (templ) ||
      (purpose != FTE3600_TEMPLATE_LOAD_DIAGNOSTIC &&
       purpose != FTE3600_TEMPLATE_LOAD_AUTHENTICATION))
    return FTE3600_TEMPLATE_INVALID_WIRE;
  if (purpose == FTE3600_TEMPLATE_LOAD_AUTHENTICATION &&
      FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION == 0)
    return FTE3600_TEMPLATE_NOT_CALIBRATED;
  if (validation != FEATURE_SET_VALID)
    return validation_to_status (validation);

  for (guint i = 0; i < templ->n_subtemplates; i++)
    {
      Fte3600BriskMatchResult match;

      (void) fpi_fte3600_brisk_match (&canonical_query.features,
                                      &templ->subtemplates[i].features, &match);
      result->n_compared++;
      if (match.diagnostic_policy_passed)
        result->diagnostic_passes++;
      if (purpose == FTE3600_TEMPLATE_LOAD_AUTHENTICATION &&
          match.authentication_accepted)
        result->authentication_accepted = TRUE;
      if (!have_best || match_is_better (&match, &result->best))
        {
          result->best = match;
          result->best_subtemplate = i;
          have_best = TRUE;
        }
    }

  if (templ->has_mosaic)
    {
      Fte3600BriskMatchResult mosaic_match;

      if (fpi_fte3600_brisk_match_mosaic (&canonical_query.features,
                                          &templ->mosaic,
                                          &mosaic_match) == FTE3600_BRISK_OK)
        {
          if (mosaic_match.diagnostic_policy_passed)
            result->diagnostic_passes++;
          if (purpose == FTE3600_TEMPLATE_LOAD_AUTHENTICATION &&
              mosaic_match.authentication_accepted)
            result->authentication_accepted = TRUE;
          if (!have_best || match_is_better (&mosaic_match, &result->best))
            {
              result->best = mosaic_match;
              if (result->best_subtemplate == G_MAXUINT)
                result->best_subtemplate = 0;
              have_best = TRUE;
            }
        }
    }

  /* Diagnostic loads never authenticate.  Authentication loads reach this
   * point only for a non-zero, explicitly selected policy version, and use an
   * any-of-eight gallery decision matching the frozen evaluator semantics. */
  if (purpose == FTE3600_TEMPLATE_LOAD_DIAGNOSTIC)
    result->authentication_accepted = FALSE;
  return FTE3600_TEMPLATE_OK;
}
