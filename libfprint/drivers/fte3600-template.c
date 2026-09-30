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
  Fte3600IpaFeatureSet   ipa_features;
  gboolean               has_ipa;
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

static void
canonical_subtemplate_clear (CanonicalSubtemplate *sample)
{
  template_secure_clear (sample, sizeof (*sample));
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (CanonicalSubtemplate, canonical_subtemplate_clear)

typedef CanonicalSubtemplate CanonicalGallery[FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES];

static void
canonical_gallery_clear (CanonicalGallery *gallery)
{
  template_secure_clear (gallery, sizeof (*gallery));
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (CanonicalGallery, canonical_gallery_clear)

typedef struct
{
  gsize  size;
  guint8 data[];
} TemplateWireBuffer;

static void
template_wire_buffer_free (gpointer pointer)
{
  TemplateWireBuffer *buffer = pointer;

  if (buffer == NULL)
    return;
  template_secure_clear (buffer->data, buffer->size);
  g_free (buffer);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (TemplateWireBuffer, template_wire_buffer_free)

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
G_STATIC_ASSERT (FTE3600_TEMPLATE_IPA_FEATURE_RECORD_SIZE ==
                 3 * sizeof (gfloat) + FTE3600_IPA_DESC_DIM * sizeof (gfloat));
G_STATIC_ASSERT (FTE3600_TEMPLATE_V2_CURRENT_MAX_WIRE_SIZE ==
                 FTE3600_TEMPLATE_CURRENT_MAX_WIRE_SIZE +
                 FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES *
                 (FTE3600_TEMPLATE_IPA_RECORD_HEADER_SIZE +
                  FTE3600_IPA_MAX_MINUTIAE *
                  FTE3600_TEMPLATE_IPA_FEATURE_RECORD_SIZE));
G_STATIC_ASSERT (FTE3600_TEMPLATE_V2_MAX_WIRE_SIZE ==
                 FTE3600_TEMPLATE_MAX_WIRE_SIZE + 12 *
                 (FTE3600_TEMPLATE_IPA_RECORD_HEADER_SIZE +
                  FTE3600_IPA_MAX_MINUTIAE *
                  FTE3600_TEMPLATE_IPA_FEATURE_RECORD_SIZE));
G_STATIC_ASSERT (FTE3600_TEMPLATE_V3_CURRENT_MAX_WIRE_SIZE ==
                 FTE3600_TEMPLATE_V2_CURRENT_MAX_WIRE_SIZE +
                 FTE3600_TEMPLATE_V3_WIRE_HEADER_SIZE - FTE3600_TEMPLATE_WIRE_HEADER_SIZE);

gboolean
fpi_fte3600_engine_mode_parse (const gchar       *value,
                               Fte3600EngineMode *mode)
{
  if (mode == NULL)
    return FALSE;
  if (value == NULL)
    *mode = FTE3600_ENABLE_IPA_AUTH ? FTE3600_ENGINE_MODE_DUAL_FUSION :
            FTE3600_ENGINE_MODE_BRISK_ONLY;
  else if (g_ascii_strcasecmp (value, "brisk") == 0)
    *mode = FTE3600_ENGINE_MODE_BRISK_ONLY;
  else if (g_ascii_strcasecmp (value, "ipa") == 0)
    *mode = FTE3600_ENGINE_MODE_IPA_ONLY;
  else if (g_ascii_strcasecmp (value, "dual") == 0)
    *mode = FTE3600_ENGINE_MODE_DUAL_FUSION;
  else
    return FALSE;
  return TRUE;
}

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

static gint
ipa_point_compare (const void *first, const void *second)
{
  const Fte3600IpaMinutia *a = first;
  const Fte3600IpaMinutia *b = second;

  if (a->x != b->x)
    return a->x < b->x ? -1 : 1;
  if (a->y != b->y)
    return a->y < b->y ? -1 : 1;
  if (a->theta != b->theta)
    return a->theta < b->theta ? -1 : 1;
  for (guint d = 0; d < FTE3600_IPA_DESC_DIM; d++)
    if (a->desc[d] != b->desc[d])
      return a->desc[d] < b->desc[d] ? -1 : 1;
  return 0;
}

static gboolean
canonicalize_ipa_feature_set (const Fte3600IpaFeatureSet *source,
                              Fte3600IpaFeatureSet       *canonical)
{
  memset (canonical, 0, sizeof (*canonical));
  if (!fpi_fte3600_ipa_validate_feature_set (source) || source->n_minutiae < 3)
    return FALSE;
  *canonical = *source;
  for (guint i = 0; i < canonical->n_minutiae; i++)
    {
      Fte3600IpaMinutia *point = &canonical->minutiae[i];

      if (point->x == 0.0f)
        point->x = 0.0f;
      if (point->y == 0.0f)
        point->y = 0.0f;
      if (point->theta == 0.0f)
        point->theta = 0.0f;
      if (point->theta == FTE3600_IPA_ORIENTATION_LIMIT)
        point->theta = -FTE3600_IPA_ORIENTATION_LIMIT;
      for (guint d = 0; d < FTE3600_IPA_DESC_DIM; d++)
        if (point->desc[d] == 0.0f)
          point->desc[d] = 0.0f;
    }
  qsort (canonical->minutiae, canonical->n_minutiae,
         sizeof (canonical->minutiae[0]), ipa_point_compare);
  return TRUE;
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

static guint
template_hamming_distance (const guint8 *first,
                           const guint8 *second,
                           gsize         length)
{
  guint distance = 0;

  for (gsize i = 0; i < length; i++)
    distance += (guint) __builtin_popcount ((unsigned int) (first[i] ^ second[i]));
  return distance;
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

Fte3600TemplateStatus
fpi_fte3600_template_add_dual_features (Fte3600Template              *templ,
                                        const Fte3600BriskFeatureSet *brisk_features,
                                        const Fte3600IpaFeatureSet   *ipa_features,
                                        Fte3600BriskMatchResult      *nearest_match)
{
  g_auto(TemplateRoundingGuard) rounding_guard = { 0 };
  g_auto(CanonicalSubtemplate) candidate = { 0 };
  FeatureSetValidation validation;
  gboolean have_nearest = FALSE;
  gboolean duplicate = FALSE;
#if FTE3600_ENABLE_PERSONAL_AUTH
  gboolean consistent = FALSE;
#endif

  if (nearest_match != NULL)
    memset (nearest_match, 0, sizeof (*nearest_match));
  if (!template_rounding_guard_enter (&rounding_guard))
    return FTE3600_TEMPLATE_INVALID_WIRE;
  memset (&candidate, 0, sizeof (candidate));
  validation = canonicalize_feature_set (brisk_features, &candidate);
  if (templ == NULL)
    return FTE3600_TEMPLATE_INVALID_WIRE;
  if (validation != FEATURE_SET_VALID)
    return validation_to_status (validation);
  if (templ->n_subtemplates > FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES)
    return FTE3600_TEMPLATE_INVALID_WIRE;

  if (ipa_features != NULL)
    {
      if (!canonicalize_ipa_feature_set (ipa_features, &candidate.ipa_features))
        return FTE3600_TEMPLATE_INVALID_WIRE;
      candidate.has_ipa = TRUE;
    }

  for (guint i = 0; i < templ->n_subtemplates; i++)
    {
      Fte3600BriskMatchResult match;

      duplicate |=
        subtemplate_compare (&candidate, &templ->subtemplates[i]) == 0;
      (void) fpi_fte3600_brisk_match (&candidate.features,
                                      &templ->subtemplates[i].features, &match);
#if FTE3600_ENABLE_PERSONAL_AUTH
      consistent |= match.authentication_accepted;
      if (candidate.has_ipa && templ->subtemplates[i].has_ipa)
        {
          Fte3600IpaMatchResult ipa_res = { 0 };
          if (fpi_fte3600_ipa_match (&candidate.ipa_features,
                                     &templ->subtemplates[i].ipa_features,
                                     &ipa_res) == FTE3600_IPA_OK)
            consistent |= ipa_res.authentication_accepted;
        }
#endif
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

  templ->subtemplates[templ->n_subtemplates++] = candidate;
  if (templ->n_subtemplates < FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES)
    return FTE3600_TEMPLATE_NEED_MORE_SAMPLES;
  template_reconstruct_mosaic (templ);
  return FTE3600_TEMPLATE_OK;
}

Fte3600TemplateStatus
fpi_fte3600_template_add_features (Fte3600Template              *templ,
                                   const Fte3600BriskFeatureSet *features,
                                   Fte3600BriskMatchResult      *nearest_match)
{
  return fpi_fte3600_template_add_dual_features (templ, features, NULL, nearest_match);
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
  g_auto(CanonicalGallery) sorted = { 0 };
  g_autoptr(TemplateWireBuffer) buffer = NULL;
  gsize total_size = FTE3600_TEMPLATE_WIRE_HEADER_SIZE;
  guint8 *data;
  gsize offset;
  gboolean has_any_ipa = FALSE;

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

      sorted[i].has_ipa = templ->subtemplates[i].has_ipa;
      if (sorted[i].has_ipa)
        {
          if (!canonicalize_ipa_feature_set (&templ->subtemplates[i].ipa_features,
                                             &sorted[i].ipa_features))
            return FTE3600_TEMPLATE_INVALID_WIRE;
          has_any_ipa = TRUE;
        }

      total_size += TEMPLATE_SUBTEMPLATE_HEADER_SIZE +
                    sorted[i].features.n_features *
                    FTE3600_TEMPLATE_FEATURE_RECORD_SIZE;
      if (sorted[i].has_ipa)
        total_size += sorted[i].ipa_features.n_minutiae *
                      FTE3600_TEMPLATE_IPA_FEATURE_RECORD_SIZE;
    }
  if (has_any_ipa)
    total_size += FTE3600_TEMPLATE_V3_WIRE_HEADER_SIZE - FTE3600_TEMPLATE_WIRE_HEADER_SIZE +
                  FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES * FTE3600_TEMPLATE_IPA_RECORD_HEADER_SIZE;
  qsort (sorted, G_N_ELEMENTS (sorted), sizeof (sorted[0]),
         subtemplate_compare);
  for (guint i = 1; i < G_N_ELEMENTS (sorted); i++)
    if (subtemplate_compare (&sorted[i - 1], &sorted[i]) == 0)
      return FTE3600_TEMPLATE_RETRY_DUPLICATE;

  gsize max_allowed = has_any_ipa ? FTE3600_TEMPLATE_V3_CURRENT_MAX_WIRE_SIZE :
                      FTE3600_TEMPLATE_CURRENT_MAX_WIRE_SIZE;
  if (total_size > max_allowed || total_size > G_MAXUINT32)
    return FTE3600_TEMPLATE_INVALID_WIRE;

  buffer = g_malloc0 (sizeof (*buffer) + total_size);
  buffer->size = total_size;
  data = buffer->data;
  memcpy (data, template_magic, sizeof (template_magic));
  put_uint16_le (&data[8], has_any_ipa ? FTE3600_TEMPLATE_WIRE_VERSION_V3 :
                 FTE3600_TEMPLATE_WIRE_VERSION_V1);
  const gsize header_size = has_any_ipa ? FTE3600_TEMPLATE_V3_WIRE_HEADER_SIZE :
                            FTE3600_TEMPLATE_WIRE_HEADER_SIZE;
  put_uint16_le (&data[10], header_size);
  put_uint32_le (&data[12], total_size);
  put_uint16_le (&data[16], TEMPLATE_MODEL_ID);
  put_uint16_le (&data[18], FTE3600_BRISK_WIDTH);
  put_uint16_le (&data[20], FTE3600_BRISK_HEIGHT);
  put_uint16_le (&data[22], FTE3600_TEMPLATE_FEATURE_RECORD_SIZE);
  put_uint16_le (&data[24], FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION);
  put_uint16_le (&data[26], FTE3600_BRISK_DIAGNOSTIC_POLICY_VERSION);
  put_uint16_le (&data[28], FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION);
  put_uint16_le (&data[30], FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES);
  if (has_any_ipa)
    {
      put_uint32_le (&data[32], 0x01);
      put_uint16_le (&data[40], FTE3600_IPA_EXTRACTOR_SCHEMA_VERSION);
      put_uint16_le (&data[42], FTE3600_IPA_DIAGNOSTIC_POLICY_VERSION);
      put_uint16_le (&data[44], FTE3600_IPA_AUTHENTICATION_POLICY_VERSION);
      put_uint16_le (&data[46], FTE3600_TEMPLATE_FUSION_POLICY_VERSION);
    }

  offset = header_size;
  for (guint sample = 0; sample < G_N_ELEMENTS (sorted); sample++)
    {
      const Fte3600BriskFeatureSet *features = &sorted[sample].features;
      const guint32 record_size = TEMPLATE_SUBTEMPLATE_HEADER_SIZE +
                                  features->n_features *
                                  FTE3600_TEMPLATE_FEATURE_RECORD_SIZE;

      if (offset > total_size || record_size > total_size - offset)
        return FTE3600_TEMPLATE_INVALID_WIRE;
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

      if (has_any_ipa)
        {
          guint n_pts = sorted[sample].has_ipa ? sorted[sample].ipa_features.n_minutiae : 0;
          guint32 ipa_rec_size = FTE3600_TEMPLATE_IPA_RECORD_HEADER_SIZE +
                                 n_pts * FTE3600_TEMPLATE_IPA_FEATURE_RECORD_SIZE;
          if (offset > total_size || ipa_rec_size > total_size - offset)
            return FTE3600_TEMPLATE_INVALID_WIRE;
          put_uint32_le (&data[offset], ipa_rec_size);
          put_uint16_le (&data[offset + 4], (guint16) n_pts);
          put_uint16_le (&data[offset + 6], 0); /* reserved */
          offset += FTE3600_TEMPLATE_IPA_RECORD_HEADER_SIZE;
          for (guint k = 0; k < n_pts; k++)
            {
              const Fte3600IpaMinutia *m = &sorted[sample].ipa_features.minutiae[k];
              put_uint32_le (&data[offset], float_bits (m->x));
              put_uint32_le (&data[offset + 4], float_bits (m->y));
              put_uint32_le (&data[offset + 8], float_bits (m->theta));
              offset += 12;
              for (guint d = 0; d < FTE3600_IPA_DESC_DIM; d++)
                {
                  put_uint32_le (&data[offset], float_bits (m->desc[d]));
                  offset += 4;
                }
            }
        }
    }
  if (offset != total_size)
    return FTE3600_TEMPLATE_INVALID_WIRE;
  *wire = g_bytes_new_with_free_func (data, total_size,
                                      template_wire_buffer_free,
                                      g_steal_pointer (&buffer));
  return FTE3600_TEMPLATE_OK;
}

static Fte3600TemplateStatus
validate_header (const guint8 *data,
                 gsize         size)
{
  if (size < FTE3600_TEMPLATE_WIRE_HEADER_SIZE ||
      size > FTE3600_TEMPLATE_V3_CURRENT_MAX_WIRE_SIZE ||
      memcmp (data, template_magic, sizeof (template_magic)) != 0)
    return FTE3600_TEMPLATE_INVALID_WIRE;

  const guint16 wire_ver = get_uint16_le (&data[8]);
  if (wire_ver != FTE3600_TEMPLATE_WIRE_VERSION_V1 &&
      wire_ver != FTE3600_TEMPLATE_WIRE_VERSION_V3)
    return FTE3600_TEMPLATE_UNSUPPORTED_SCHEMA;

  const gboolean has_ipa = wire_ver == FTE3600_TEMPLATE_WIRE_VERSION_V3;
  const gsize header_size = has_ipa ? FTE3600_TEMPLATE_V3_WIRE_HEADER_SIZE :
                            FTE3600_TEMPLATE_WIRE_HEADER_SIZE;
  const gsize max_size = has_ipa ? FTE3600_TEMPLATE_V3_CURRENT_MAX_WIRE_SIZE :
                         FTE3600_TEMPLATE_CURRENT_MAX_WIRE_SIZE;
  if (size < header_size || size > max_size ||
      get_uint16_le (&data[10]) != header_size ||
      get_uint32_le (&data[12]) != size ||
      get_uint16_le (&data[16]) != TEMPLATE_MODEL_ID ||
      get_uint16_le (&data[18]) != FTE3600_BRISK_WIDTH ||
      get_uint16_le (&data[20]) != FTE3600_BRISK_HEIGHT ||
      get_uint16_le (&data[22]) != FTE3600_TEMPLATE_FEATURE_RECORD_SIZE ||
      get_uint16_le (&data[30]) != FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES ||
      get_uint32_le (&data[32]) != (has_ipa ? 1u : 0u) ||
      get_uint32_le (&data[36]) != 0)
    return FTE3600_TEMPLATE_INVALID_WIRE;
  if (get_uint16_le (&data[24]) != FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION ||
      (has_ipa && get_uint16_le (&data[40]) != FTE3600_IPA_EXTRACTOR_SCHEMA_VERSION))
    return FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR;
  if (get_uint16_le (&data[26]) != FTE3600_BRISK_DIAGNOSTIC_POLICY_VERSION ||
      get_uint16_le (&data[28]) != FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION ||
      (has_ipa &&
       (get_uint16_le (&data[42]) != FTE3600_IPA_DIAGNOSTIC_POLICY_VERSION ||
        get_uint16_le (&data[44]) != FTE3600_IPA_AUTHENTICATION_POLICY_VERSION ||
        get_uint16_le (&data[46]) != FTE3600_TEMPLATE_FUSION_POLICY_VERSION)))
    return FTE3600_TEMPLATE_UNSUPPORTED_POLICY;
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
  gsize offset;
  g_autoptr(Fte3600Template) decoded = NULL;
  Fte3600TemplateStatus status;
  guint16 wire_ver;
  gboolean has_any_ipa = FALSE;

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
  wire_ver = get_uint16_le (&data[8]);
  offset = get_uint16_le (&data[10]);

  decoded = fpi_fte3600_template_new ();
  for (guint sample = 0;
       sample < FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES; sample++)
    {
      g_auto(CanonicalSubtemplate) parsed = { 0 };
      g_auto(CanonicalSubtemplate) canonical = { 0 };
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

      if (wire_ver == FTE3600_TEMPLATE_WIRE_VERSION_V3)
        {
          if (offset > size || size - offset < FTE3600_TEMPLATE_IPA_RECORD_HEADER_SIZE)
            return FTE3600_TEMPLATE_INVALID_WIRE;
          guint32 ipa_rec_size = get_uint32_le (&data[offset]);
          guint16 ipa_feature_count = get_uint16_le (&data[offset + 4]);
          guint16 ipa_reserved = get_uint16_le (&data[offset + 6]);
          if (ipa_reserved != 0 || ipa_feature_count > FTE3600_IPA_MAX_MINUTIAE ||
              (ipa_feature_count > 0 && ipa_feature_count < 3))
            return FTE3600_TEMPLATE_INVALID_WIRE;
          gsize expected_ipa_size = FTE3600_TEMPLATE_IPA_RECORD_HEADER_SIZE +
                                    (gsize) ipa_feature_count * FTE3600_TEMPLATE_IPA_FEATURE_RECORD_SIZE;
          if (ipa_rec_size != expected_ipa_size || ipa_rec_size > size - offset)
            return FTE3600_TEMPLATE_INVALID_WIRE;
          offset += FTE3600_TEMPLATE_IPA_RECORD_HEADER_SIZE;

          canonical.has_ipa = (ipa_feature_count >= 3);
          parsed.ipa_features.extractor_schema_version = FTE3600_IPA_EXTRACTOR_SCHEMA_VERSION;
          parsed.ipa_features.n_minutiae = ipa_feature_count;
          for (guint k = 0; k < ipa_feature_count; k++)
            {
              Fte3600IpaMinutia *m = &parsed.ipa_features.minutiae[k];
              guint32 x_bits = get_uint32_le (&data[offset]);
              guint32 y_bits = get_uint32_le (&data[offset + 4]);
              guint32 th_bits = get_uint32_le (&data[offset + 8]);
              if (x_bits == 0x80000000u || y_bits == 0x80000000u || th_bits == 0x80000000u)
                return FTE3600_TEMPLATE_INVALID_WIRE;
              m->x = float_from_bits (x_bits);
              m->y = float_from_bits (y_bits);
              m->theta = float_from_bits (th_bits);
              if (!isfinite (m->x) || !isfinite (m->y) || !isfinite (m->theta))
                return FTE3600_TEMPLATE_INVALID_WIRE;
              offset += 12;
              for (guint d = 0; d < FTE3600_IPA_DESC_DIM; d++)
                {
                  guint32 d_bits = get_uint32_le (&data[offset]);
                  if (d_bits == 0x80000000u)
                    return FTE3600_TEMPLATE_INVALID_WIRE;
                  m->desc[d] = float_from_bits (d_bits);
                  if (!isfinite (m->desc[d]))
                    return FTE3600_TEMPLATE_INVALID_WIRE;
                  offset += 4;
                }
            }
          if (canonical.has_ipa)
            {
              if (!canonicalize_ipa_feature_set (&parsed.ipa_features,
                                                 &canonical.ipa_features))
                return FTE3600_TEMPLATE_INVALID_WIRE;
              for (guint k = 0; k < ipa_feature_count; k++)
                if (ipa_point_compare (&parsed.ipa_features.minutiae[k],
                                       &canonical.ipa_features.minutiae[k]) != 0)
                  return FTE3600_TEMPLATE_INVALID_WIRE;
              has_any_ipa = TRUE;
            }
        }

      decoded->subtemplates[sample] = canonical;
      decoded->n_subtemplates++;
    }
  if (offset != size ||
      (wire_ver == FTE3600_TEMPLATE_WIRE_VERSION_V3 && !has_any_ipa))
    return FTE3600_TEMPLATE_INVALID_WIRE;

  if (purpose == FTE3600_TEMPLATE_LOAD_AUTHENTICATION &&
      FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION == 0)
    return FTE3600_TEMPLATE_NOT_CALIBRATED;
  template_reconstruct_mosaic (decoded);
  *templ = g_steal_pointer (&decoded);
  return FTE3600_TEMPLATE_OK;
}

Fte3600TemplateStatus
fpi_fte3600_template_compare_with_mode (const Fte3600Template        *templ,
                                        const Fte3600BriskFeatureSet *query_brisk,
                                        const Fte3600IpaFeatureSet   *query_ipa,
                                        Fte3600TemplateLoadPurpose    purpose,
                                        Fte3600EngineMode             mode,
                                        Fte3600TemplateCompareResult *result)
{
  g_auto(TemplateRoundingGuard) rounding_guard = { 0 };
  g_auto(CanonicalSubtemplate) canonical_query = { 0 };
  gboolean have_best = FALSE;
  gboolean have_brisk = FALSE;
  gboolean have_ipa = FALSE;

  if (result != NULL)
    {
      memset (result, 0, sizeof (*result));
      result->best_subtemplate = G_MAXUINT;
      result->engine_mode = mode;
    }
  if (!template_rounding_guard_enter (&rounding_guard))
    return FTE3600_TEMPLATE_INVALID_WIRE;

  if (templ == NULL || result == NULL || !fpi_fte3600_template_is_ready (templ) ||
      (purpose != FTE3600_TEMPLATE_LOAD_DIAGNOSTIC &&
       purpose != FTE3600_TEMPLATE_LOAD_AUTHENTICATION) ||
      (mode != FTE3600_ENGINE_MODE_BRISK_ONLY &&
       mode != FTE3600_ENGINE_MODE_IPA_ONLY &&
       mode != FTE3600_ENGINE_MODE_DUAL_FUSION))
    return FTE3600_TEMPLATE_INVALID_WIRE;
  if (purpose == FTE3600_TEMPLATE_LOAD_AUTHENTICATION &&
      (FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION == 0 ||
       (mode != FTE3600_ENGINE_MODE_BRISK_ONLY && !FTE3600_ENABLE_IPA_AUTH)))
    return FTE3600_TEMPLATE_NOT_CALIBRATED;

  if (mode != FTE3600_ENGINE_MODE_IPA_ONLY && query_brisk != NULL)
    {
      const FeatureSetValidation validation =
        canonicalize_feature_set (query_brisk, &canonical_query);
      if (validation != FEATURE_SET_VALID &&
          validation != FEATURE_SET_INSUFFICIENT)
        return validation_to_status (validation);
      have_brisk = validation == FEATURE_SET_VALID;
    }

  if (mode != FTE3600_ENGINE_MODE_BRISK_ONLY && query_ipa != NULL)
    {
      if (!fpi_fte3600_ipa_validate_feature_set (query_ipa))
        return FTE3600_TEMPLATE_INVALID_WIRE;
      have_ipa = query_ipa->n_minutiae >= 3;
    }
  if ((mode == FTE3600_ENGINE_MODE_BRISK_ONLY && !have_brisk) ||
      (mode == FTE3600_ENGINE_MODE_IPA_ONLY && !have_ipa) ||
      (mode == FTE3600_ENGINE_MODE_DUAL_FUSION && !have_brisk && !have_ipa))
    return FTE3600_TEMPLATE_RETRY_INSUFFICIENT_FEATURES;

  for (guint i = 0; i < templ->n_subtemplates; i++)
    {
      Fte3600BriskMatchResult match = { 0 };
      gboolean brisk_ok = FALSE;
      gboolean ipa_ok = FALSE;

      result->n_compared++;

      /* 1. BRISK evaluation (run if in BRISK or DUAL mode and query_brisk is present) */
      if (have_brisk)
        {
          (void) fpi_fte3600_brisk_match (&canonical_query.features,
                                          &templ->subtemplates[i].features, &match);
          if (match.diagnostic_policy_passed)
            result->diagnostic_passes++;
          if (match.authentication_accepted)
            brisk_ok = TRUE;

          if (!have_best || match_is_better (&match, &result->best))
            {
              result->best = match;
              result->best_subtemplate = i;
              have_best = TRUE;
            }
        }

      /* 2. 2D-IPA evaluation (run if in IPA or DUAL mode and query_ipa is present) */
      if (have_ipa && templ->subtemplates[i].has_ipa)
        {
          Fte3600IpaMatchResult ipa_res = { 0 };
          if (fpi_fte3600_ipa_match (query_ipa, &templ->subtemplates[i].ipa_features, &ipa_res) == FTE3600_IPA_OK)
            {
              if (ipa_res.consensus_score > result->best_ipa.consensus_score)
                result->best_ipa = ipa_res;
              if (ipa_res.authentication_accepted)
                ipa_ok = TRUE;
            }
        }

      /* 3. Decision arbitration based on engine mode */
      if (purpose == FTE3600_TEMPLATE_LOAD_AUTHENTICATION)
        {
          if (brisk_ok)
            result->brisk_accepted = TRUE;
          if (ipa_ok)
            result->ipa_accepted = TRUE;

          switch (mode)
            {
            case FTE3600_ENGINE_MODE_BRISK_ONLY:
              if (brisk_ok)
                result->authentication_accepted = TRUE;
              break;

            case FTE3600_ENGINE_MODE_IPA_ONLY:
              if (ipa_ok)
                result->authentication_accepted = TRUE;
              break;

            case FTE3600_ENGINE_MODE_DUAL_FUSION:
              if (brisk_ok || ipa_ok)
                result->authentication_accepted = TRUE;
              break;

            default:
              g_assert_not_reached ();
            }
    }

  if (have_brisk && templ->has_mosaic)
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
            {
              result->brisk_accepted = TRUE;
              if (mode == FTE3600_ENGINE_MODE_BRISK_ONLY ||
                  mode == FTE3600_ENGINE_MODE_DUAL_FUSION)
                result->authentication_accepted = TRUE;
            }
          if (!have_best || match_is_better (&mosaic_match, &result->best))
            {
              result->best = mosaic_match;
              result->best_subtemplate = FTE3600_TEMPLATE_SUBTEMPLATE_MOSAIC;
              have_best = TRUE;
            }
        }
    }

  if (purpose == FTE3600_TEMPLATE_LOAD_DIAGNOSTIC)
    result->authentication_accepted = FALSE;
  return FTE3600_TEMPLATE_OK;
}

Fte3600TemplateStatus
fpi_fte3600_template_compare_dual_features (const Fte3600Template        *templ,
                                            const Fte3600BriskFeatureSet *query_brisk,
                                            const Fte3600IpaFeatureSet   *query_ipa,
                                            Fte3600TemplateLoadPurpose    purpose,
                                            Fte3600TemplateCompareResult *result)
{
  return fpi_fte3600_template_compare_with_mode (templ, query_brisk, query_ipa,
                                                 purpose, FTE3600_ENGINE_MODE_DUAL_FUSION,
                                                 result);
}

Fte3600TemplateStatus
fpi_fte3600_template_compare_features (const Fte3600Template        *templ,
                                       const Fte3600BriskFeatureSet *query,
                                       Fte3600TemplateLoadPurpose    purpose,
                                       Fte3600TemplateCompareResult *result)
{
  return fpi_fte3600_template_compare_with_mode (templ, query, NULL,
                                                 purpose, FTE3600_ENGINE_MODE_BRISK_ONLY,
                                                 result);
}

Fte3600TemplateStatus
fpi_fte3600_template_compare_ipa_features (const Fte3600Template        *templ,
                                           const Fte3600IpaFeatureSet   *query_ipa,
                                           Fte3600TemplateLoadPurpose    purpose,
                                           Fte3600TemplateCompareResult *result)
{
  return fpi_fte3600_template_compare_with_mode (templ, NULL, query_ipa,
                                                 purpose, FTE3600_ENGINE_MODE_IPA_ONLY,
                                                 result);
}
