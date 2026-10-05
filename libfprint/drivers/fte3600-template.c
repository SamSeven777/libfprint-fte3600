/*
 * Versioned BRISK template container for the FocalTech FTE3600 family
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
  const Fte3600MatchProfile *profile;
  guint16                    wire_version;
  /* V3 accepts both legacy (0) and profiled processing metadata. Keep the
   * stored value independently of its matching-policy version. */
  guint32                wire_processing_version;
  guint                  n_subtemplates;
  CanonicalSubtemplate   subtemplates[FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES];
  gboolean               has_mosaic;
  Fte3600BriskFeatureSet mosaic;
};

static guint16
template_authentication_policy (const Fte3600Template *templ)
{
  return templ->wire_version == FTE3600_TEMPLATE_PROFILE_WIRE_VERSION ?
         fpi_fte3600_brisk_authentication_policy_version (templ->profile) :
         FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION;
}

static Fte3600BriskStatus
template_match (const Fte3600Template        *templ,
                const Fte3600BriskFeatureSet *query,
                const Fte3600BriskFeatureSet *reference,
                gboolean                      mosaic,
                Fte3600BriskMatchResult      *result)
{
  /* Wire v1 and v3 preserve the historical FT9361 raw-coordinate policy. Every
   * modern profile follows the same principal-axis geometry policy. */
  if (templ->wire_version != FTE3600_TEMPLATE_PROFILE_WIRE_VERSION)
    return mosaic ? fpi_fte3600_brisk_match_mosaic (query, reference, result) :
           fpi_fte3600_brisk_match (query, reference, result);
  return mosaic ? fpi_fte3600_brisk_match_mosaic_for_profile (templ->profile, query, reference, result) :
         fpi_fte3600_brisk_match_for_profile (templ->profile, query, reference, result);
}

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
ipa_point_compare (const void *first,
                   const void *second)
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
  if (a->has_ipa != b->has_ipa)
    return a->has_ipa ? 1 : -1;
  if (a->has_ipa)
    {
      if (a->ipa_features.n_minutiae < b->ipa_features.n_minutiae)
        return -1;
      if (a->ipa_features.n_minutiae > b->ipa_features.n_minutiae)
        return 1;
      for (guint i = 0; i < a->ipa_features.n_minutiae; i++)
        {
          const gint res = ipa_point_compare (&a->ipa_features.minutiae[i],
                                              &b->ipa_features.minutiae[i]);
          if (res != 0)
            return res;
        }
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
canonicalize_feature_set (const Fte3600MatchProfile    *profile,
                          const Fte3600BriskFeatureSet *source,
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
  if (!fpi_fte3600_brisk_validate_feature_set_for_profile (profile, source, &public_physical_count))
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

static inline gboolean
fte3600_match_practical_brisk (const Fte3600BriskMatchResult *res)
{
  if (res == NULL)
    return FALSE;
  if (res->inliers >= 7 || fpi_fte3600_brisk_result_meets_diagnostic_policy (res))
    return TRUE;
  if (res->inliers >= 6 && res->rms_error <= 1.80)
    return TRUE;
  if (res->inliers >= 5 && res->rms_error <= 1.40 && res->inlier_ratio >= 0.30)
    return TRUE;
  if (res->inliers >= 4 && res->rms_error <= 1.00 && res->inlier_ratio >= 0.45 &&
      (res->x_span >= 12.0 || res->y_span >= 15.0) && res->competing_inliers == 0)
    return TRUE;
  return FALSE;
}

static inline gboolean
fte3600_match_constrained_ipa (const Fte3600IpaMatchResult *res)
{
  if (res == NULL ||
      res->n_matched_pairs > FTE3600_IPA_MAX_MINUTIAE ||
      res->n_supported_inliers > res->n_matched_pairs ||
      !isfinite (res->consensus_score) ||
      res->consensus_score < 0.0f || res->consensus_score > 1.0f ||
      !isfinite (res->x_span) || res->x_span < 0.0f ||
      res->x_span >= FTE3600_IPA_WIDTH ||
      !isfinite (res->y_span) || res->y_span < 0.0f ||
      res->y_span >= FTE3600_IPA_HEIGHT)
    return FALSE;

  if (res->n_supported_inliers >= 5 && res->consensus_score >= 0.40f &&
      res->x_span >= 6.0f && res->y_span >= 8.0f)
    return TRUE;
  if (res->n_supported_inliers == 4 && res->consensus_score >= 0.59f &&
      res->x_span >= 8.0f && res->y_span >= 10.0f)
    return TRUE;
  return FALSE;
}

static inline gboolean
fte3600_match_coactive_synergy (const Fte3600BriskMatchResult *b_res,
                                const Fte3600IpaMatchResult   *i_res)
{
  if (b_res == NULL || i_res == NULL)
    return FALSE;
  if (b_res->inliers >= 4 && b_res->rms_error <= 1.25 && b_res->competing_inliers == 0 &&
      i_res->n_supported_inliers >= 3 && i_res->consensus_score >= 0.35f)
    return TRUE;
  return FALSE;
}

gboolean
fpi_fte3600_engine_mode_parse (const gchar       *value,
                               Fte3600EngineMode *mode)
{
  if (mode == NULL)
    return FALSE;
  if (value == NULL || *value == '\0')
    {
      *mode = FTE3600_ENABLE_IPA_AUTH ?
              FTE3600_ENGINE_MODE_DUAL_FUSION : FTE3600_ENGINE_MODE_BRISK_ONLY;
      return TRUE;
    }
  if (g_ascii_strcasecmp (value, "brisk") == 0 ||
      g_ascii_strcasecmp (value, "brisk-only") == 0)
    {
      *mode = FTE3600_ENGINE_MODE_BRISK_ONLY;
      return TRUE;
    }
  if (g_ascii_strcasecmp (value, "ipa") == 0 ||
      g_ascii_strcasecmp (value, "ipa-only") == 0 ||
      g_ascii_strcasecmp (value, "2d-ipa") == 0)
    {
      *mode = FTE3600_ENGINE_MODE_IPA_ONLY;
      return TRUE;
    }
  if (g_ascii_strcasecmp (value, "dual") == 0 ||
      g_ascii_strcasecmp (value, "fusion") == 0 ||
      g_ascii_strcasecmp (value, "dual-fusion") == 0)
    {
      *mode = FTE3600_ENGINE_MODE_DUAL_FUSION;
      return TRUE;
    }
  return FALSE;
}

Fte3600Template *
fpi_fte3600_template_new (void)
{
  Fte3600Template *templ = fpi_fte3600_template_new_for_profile (
    fpi_fte3600_match_profile_get (FTE3600_SENSOR_FT9361));

  templ->wire_version = FTE3600_TEMPLATE_WIRE_VERSION;
  templ->wire_processing_version = 0;
  return templ;
}

Fte3600Template *
fpi_fte3600_template_new_for_profile (const Fte3600MatchProfile *profile)
{
  Fte3600Template *templ;

  profile = fpi_fte3600_match_profile_resolve (profile);
  if (!profile)
    return NULL;
  templ = g_new0 (Fte3600Template, 1);
  templ->profile = profile;
  templ->wire_version = FTE3600_TEMPLATE_PROFILE_WIRE_VERSION;
  templ->wire_processing_version = profile->processing_version;
  return templ;
}

const Fte3600MatchProfile *
fpi_fte3600_template_get_profile (const Fte3600Template *templ)
{
  return templ ? templ->profile : NULL;
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

      if (xm < 0.0 || xm >= 3u * templ->profile->width ||
          ym < 0.0 || ym >= 3u * templ->profile->height)
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
  TemplatePose poses[FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES] = { 0 };

  if (templ == NULL || templ->n_subtemplates == 0)
    return;

  memset (&templ->mosaic, 0, sizeof (templ->mosaic));
  templ->mosaic.extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
  templ->has_mosaic = FALSE;

  /* Both enrollment completion and decoding supply the same canonical order.
   * Fusion retains the first descriptor and averages coordinates, so changing
   * this order would change the reconstructed authentication reference. */
  poses[0].translate_x = templ->profile->width;
  poses[0].translate_y = templ->profile->height;
  template_stitch_sample (templ, &templ->subtemplates[0].features,
                          poses[0].angle,
                          poses[0].translate_x,
                          poses[0].translate_y);
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

              status = template_match (templ, &templ->subtemplates[u].features,
                                       &templ->subtemplates[a].features,
                                       FALSE, &match);
              /* Wire v1 keeps its historical reconstruction semantics.
               * Modern mosaics may only inherit coordinates/descriptors
               * through a connection which passes the complete pair gate. */
              if (status == FTE3600_BRISK_OK &&
                  (templ->wire_version == FTE3600_TEMPLATE_WIRE_VERSION ?
                   match.inliers >= FTE3600_BRISK_MIN_INLIERS : match.diagnostic_policy_passed))
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
      const TemplatePose *pose_a = &poses[best_aligned];
      const gdouble cos_a = cos (pose_a->angle);
      const gdouble sin_a = sin (pose_a->angle);
      TemplatePose *pose_u = &poses[best_unaligned];

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
fpi_fte3600_template_add_dual_features (Fte3600Template              *templ,
                                        const Fte3600BriskFeatureSet *brisk_features,
                                        const Fte3600IpaFeatureSet   *ipa_features,
                                        Fte3600BriskMatchResult      *nearest_match)
{
  g_auto(TemplateRoundingGuard) rounding_guard = { 0 };
  CanonicalSubtemplate candidate;
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
  if (templ == NULL)
    return FTE3600_TEMPLATE_INVALID_WIRE;
  /* The current IPA adapter and V3 policy are scoped to FT9361. Equal
   * dimensions do not make another sensor's templates interchangeable. */
  if (ipa_features != NULL && templ->profile->sensor != FTE3600_SENSOR_FT9361)
    return FTE3600_TEMPLATE_INVALID_WIRE;
  validation = canonicalize_feature_set (templ->profile, brisk_features, &candidate);
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
      (void) template_match (templ, &candidate.features,
                             &templ->subtemplates[i].features, FALSE, &match);
#if FTE3600_ENABLE_PERSONAL_AUTH
      if (templ->wire_version == FTE3600_TEMPLATE_PROFILE_WIRE_VERSION)
        consistent |= match.authentication_accepted;
      else
        consistent |= fte3600_match_practical_brisk (&match);

      if (candidate.has_ipa && templ->subtemplates[i].has_ipa)
        {
          Fte3600IpaMatchResult ipa_res = { 0 };
          if (fpi_fte3600_ipa_match (&candidate.ipa_features,
                                     &templ->subtemplates[i].ipa_features,
                                     &ipa_res) == FTE3600_IPA_OK)
            consistent |= fte3600_match_constrained_ipa (&ipa_res) ||
                          fte3600_match_coactive_synergy (&match, &ipa_res);
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
   * must not become an independent accepted identity in the gallery or its
   * mosaic.  The first sample establishes the enrollment identity. */
  if (templ->n_subtemplates > 0 && !consistent)
    return FTE3600_TEMPLATE_RETRY_INCONSISTENT;
#endif

  templ->subtemplates[templ->n_subtemplates++] = candidate;
  if (templ->n_subtemplates == FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES)
    {
      qsort (templ->subtemplates, templ->n_subtemplates,
             sizeof (templ->subtemplates[0]), subtemplate_compare);
      template_reconstruct_mosaic (templ);
      return FTE3600_TEMPLATE_OK;
    }
  return FTE3600_TEMPLATE_NEED_MORE_SAMPLES;
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
  CanonicalSubtemplate sorted[FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES];
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
        canonicalize_feature_set (templ->profile, &templ->subtemplates[i].features, &sorted[i]);

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
    total_size += (FTE3600_TEMPLATE_V3_WIRE_HEADER_SIZE - FTE3600_TEMPLATE_WIRE_HEADER_SIZE) +
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

  data = g_malloc0 (total_size);
  memcpy (data, template_magic, sizeof (template_magic));
  put_uint16_le (&data[8], has_any_ipa ? FTE3600_TEMPLATE_WIRE_VERSION_V3 : templ->wire_version);
  const gsize header_size = has_any_ipa ? FTE3600_TEMPLATE_V3_WIRE_HEADER_SIZE :
                            FTE3600_TEMPLATE_WIRE_HEADER_SIZE;
  put_uint16_le (&data[10], header_size);
  put_uint32_le (&data[12], total_size);
  put_uint16_le (&data[16], templ->profile->model);
  put_uint16_le (&data[18], templ->profile->width);
  put_uint16_le (&data[20], templ->profile->height);
  put_uint16_le (&data[22], FTE3600_TEMPLATE_FEATURE_RECORD_SIZE);
  put_uint16_le (&data[24], FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION);
  put_uint16_le (&data[26], (!has_any_ipa && templ->wire_version == FTE3600_TEMPLATE_PROFILE_WIRE_VERSION) ?
                 fpi_fte3600_brisk_diagnostic_policy_version (templ->profile) :
                 FTE3600_BRISK_DIAGNOSTIC_POLICY_VERSION);
  put_uint16_le (&data[28], (!has_any_ipa && templ->wire_version == FTE3600_TEMPLATE_PROFILE_WIRE_VERSION) ?
                 template_authentication_policy (templ) :
                 FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION);
  put_uint16_le (&data[30], FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES);
  put_uint32_le (&data[36], templ->wire_processing_version);
  if (has_any_ipa)
    {
      put_uint32_le (&data[32], 0x01);
      put_uint16_le (&data[40], FTE3600_IPA_EXTRACTOR_SCHEMA_VERSION);
      put_uint16_le (&data[42], FTE3600_IPA_DIAGNOSTIC_POLICY_VERSION);
      put_uint16_le (&data[44], FTE3600_IPA_AUTHENTICATION_POLICY_VERSION);
      put_uint16_le (&data[46], FTE3600_TEMPLATE_FUSION_POLICY_VERSION);
    }
  else
    {
      put_uint32_le (&data[32], 0);
    }

  offset = header_size;
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

      if (has_any_ipa)
        {
          guint n_pts = sorted[sample].has_ipa ? sorted[sample].ipa_features.n_minutiae : 0;
          guint32 ipa_rec_size = FTE3600_TEMPLATE_IPA_RECORD_HEADER_SIZE +
                                 n_pts * FTE3600_TEMPLATE_IPA_FEATURE_RECORD_SIZE;
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
  g_assert (offset == total_size);
  *wire = g_bytes_new_take (data, total_size);
  return FTE3600_TEMPLATE_OK;
}

static Fte3600TemplateStatus
validate_header (const guint8               *data,
                 gsize                       size,
                 const Fte3600MatchProfile **profile)
{
  guint16 version;
  const Fte3600MatchProfile *identified;

  if (size < FTE3600_TEMPLATE_WIRE_HEADER_SIZE ||
      size > FTE3600_TEMPLATE_V3_MAX_WIRE_SIZE ||
      memcmp (data, template_magic, sizeof (template_magic)) != 0)
    return FTE3600_TEMPLATE_INVALID_WIRE;
  version = get_uint16_le (&data[8]);
  if (version != FTE3600_TEMPLATE_WIRE_VERSION &&
      version != FTE3600_TEMPLATE_PROFILE_WIRE_VERSION &&
      version != FTE3600_TEMPLATE_WIRE_VERSION_V3)
    return FTE3600_TEMPLATE_UNSUPPORTED_SCHEMA;
  identified = fpi_fte3600_match_profile_find (get_uint16_le (&data[16]));
  if (!identified ||
      (version != FTE3600_TEMPLATE_PROFILE_WIRE_VERSION &&
       identified->sensor != FTE3600_SENSOR_FT9361))
    return FTE3600_TEMPLATE_INVALID_WIRE;

  const gboolean has_ipa = (version == FTE3600_TEMPLATE_WIRE_VERSION_V3);
  const gsize header_size = has_ipa ? FTE3600_TEMPLATE_V3_WIRE_HEADER_SIZE :
                            FTE3600_TEMPLATE_WIRE_HEADER_SIZE;
  const gsize max_size = has_ipa ? FTE3600_TEMPLATE_V3_CURRENT_MAX_WIRE_SIZE :
                         FTE3600_TEMPLATE_CURRENT_MAX_WIRE_SIZE;

  if (size < header_size || size > max_size ||
      get_uint16_le (&data[10]) != header_size ||
      get_uint32_le (&data[12]) != size ||
      get_uint16_le (&data[18]) != identified->width ||
      get_uint16_le (&data[20]) != identified->height ||
      get_uint16_le (&data[22]) != FTE3600_TEMPLATE_FEATURE_RECORD_SIZE ||
      get_uint16_le (&data[30]) != FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES)
    return FTE3600_TEMPLATE_INVALID_WIRE;

  if (has_ipa)
    {
      if (get_uint32_le (&data[32]) != 1)
        return FTE3600_TEMPLATE_INVALID_WIRE;
      if (get_uint32_le (&data[36]) != identified->processing_version &&
          get_uint32_le (&data[36]) != 0)
        return FTE3600_TEMPLATE_INVALID_WIRE;
      if (get_uint16_le (&data[40]) != FTE3600_IPA_EXTRACTOR_SCHEMA_VERSION)
        return FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR;
      if (get_uint16_le (&data[42]) != FTE3600_IPA_DIAGNOSTIC_POLICY_VERSION ||
          get_uint16_le (&data[44]) != FTE3600_IPA_AUTHENTICATION_POLICY_VERSION ||
          get_uint16_le (&data[46]) != FTE3600_TEMPLATE_FUSION_POLICY_VERSION)
        return FTE3600_TEMPLATE_UNSUPPORTED_POLICY;
    }
  else
    {
      if (get_uint32_le (&data[32]) != 0 ||
          get_uint32_le (&data[36]) !=
          (version == FTE3600_TEMPLATE_WIRE_VERSION ? 0 : identified->processing_version))
        return FTE3600_TEMPLATE_INVALID_WIRE;
    }

  if (get_uint16_le (&data[24]) !=
      FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION)
    return FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR;
  if (get_uint16_le (&data[26]) !=
      (version == FTE3600_TEMPLATE_PROFILE_WIRE_VERSION ?
       fpi_fte3600_brisk_diagnostic_policy_version (identified) :
       FTE3600_BRISK_DIAGNOSTIC_POLICY_VERSION) ||
      get_uint16_le (&data[28]) !=
      (version == FTE3600_TEMPLATE_PROFILE_WIRE_VERSION ?
       fpi_fte3600_brisk_authentication_policy_version (identified) :
       FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION))
    return FTE3600_TEMPLATE_UNSUPPORTED_POLICY;

  *profile = identified;
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
  const Fte3600MatchProfile *profile = NULL;
  guint16 version;
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
  status = validate_header (data, size, &profile);
  if (status != FTE3600_TEMPLATE_OK)
    return status;

  version = get_uint16_le (&data[8]);
  offset = get_uint16_le (&data[10]);

  decoded = fpi_fte3600_template_new_for_profile (profile);
  decoded->wire_version = version;
  decoded->wire_processing_version = get_uint32_le (&data[36]);

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
      memset (&canonical, 0, sizeof (canonical));
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

      if (canonicalize_feature_set (profile, &parsed.features, &canonical) !=
          FEATURE_SET_VALID || canonical.physical_count != stored_physical_count)
        return FTE3600_TEMPLATE_INVALID_WIRE;
      for (guint i = 0; i < feature_count; i++)
        if (feature_compare (&parsed.features.features[i],
                             &canonical.features.features[i]) != 0)
          return FTE3600_TEMPLATE_INVALID_WIRE;

      if (version == FTE3600_TEMPLATE_WIRE_VERSION_V3)
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

      if (sample > 0 &&
          subtemplate_compare (&decoded->subtemplates[sample - 1],
                               &canonical) >= 0)
        return FTE3600_TEMPLATE_INVALID_WIRE;
      decoded->subtemplates[sample] = canonical;
      decoded->n_subtemplates++;
    }
  if (offset != size ||
      (version == FTE3600_TEMPLATE_WIRE_VERSION_V3 && !has_any_ipa))
    return FTE3600_TEMPLATE_INVALID_WIRE;

  template_reconstruct_mosaic (decoded);

  if (purpose == FTE3600_TEMPLATE_LOAD_AUTHENTICATION &&
      template_authentication_policy (decoded) == 0)
    return FTE3600_TEMPLATE_NOT_CALIBRATED;
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
  CanonicalSubtemplate canonical_query;
  gboolean have_best = FALSE;
  gboolean have_brisk = FALSE;
  gboolean have_ipa = FALSE;

  if (result != NULL)
    {
      memset (result, 0, sizeof (*result));
      result->best_subtemplate = FTE3600_TEMPLATE_SUBTEMPLATE_NONE;
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
      (template_authentication_policy (templ) == 0 ||
       (mode != FTE3600_ENGINE_MODE_BRISK_ONLY && !FTE3600_ENABLE_IPA_AUTH)))
    return FTE3600_TEMPLATE_NOT_CALIBRATED;

  if (mode != FTE3600_ENGINE_MODE_IPA_ONLY && query_brisk != NULL)
    {
      const FeatureSetValidation validation =
        canonicalize_feature_set (templ->profile, query_brisk, &canonical_query);
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
      Fte3600IpaMatchResult ipa_res = { 0 };
      gboolean brisk_ok = FALSE;
      gboolean ipa_ok = FALSE;

      result->n_compared++;

      /* 1. BRISK evaluation */
      if (have_brisk)
        {
          (void) template_match (templ, &canonical_query.features,
                                 &templ->subtemplates[i].features, FALSE, &match);
          if (match.diagnostic_policy_passed)
            result->diagnostic_passes++;
          if (fte3600_match_practical_brisk (&match))
            brisk_ok = TRUE;

          if (!have_best || match_is_better (&match, &result->best))
            {
              result->best = match;
              result->best_subtemplate = i;
              have_best = TRUE;
            }
        }

      /* 2. 2D-IPA evaluation */
      if (have_ipa && templ->subtemplates[i].has_ipa)
        {
          if (fpi_fte3600_ipa_match (query_ipa, &templ->subtemplates[i].ipa_features, &ipa_res) == FTE3600_IPA_OK)
            {
              if (ipa_res.consensus_score > result->best_ipa.consensus_score)
                result->best_ipa = ipa_res;
              if (fte3600_match_constrained_ipa (&ipa_res))
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
              if (brisk_ok || ipa_ok || fte3600_match_coactive_synergy (&match, &ipa_res))
                result->authentication_accepted = TRUE;
              break;

            default:
              g_assert_not_reached ();
            }
        }
    }

  if (have_brisk && templ->has_mosaic)
    {
      Fte3600BriskMatchResult mosaic_match;

      result->n_compared++;
      if (template_match (templ, &canonical_query.features,
                          &templ->mosaic, TRUE,
                          &mosaic_match) == FTE3600_BRISK_OK)
        {
          if (mosaic_match.diagnostic_policy_passed)
            result->diagnostic_passes++;
          if (purpose == FTE3600_TEMPLATE_LOAD_AUTHENTICATION &&
              fte3600_match_practical_brisk (&mosaic_match))
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
fpi_fte3600_template_compare_features_for_profile (const Fte3600Template        *templ,
                                                   const Fte3600MatchProfile    *profile,
                                                   const Fte3600BriskFeatureSet *query,
                                                   Fte3600TemplateLoadPurpose    purpose,
                                                   Fte3600TemplateCompareResult *result)
{
  profile = fpi_fte3600_match_profile_resolve (profile);
  if (!templ || !profile || templ->profile != profile)
    {
      if (result)
        {
          memset (result, 0, sizeof (*result));
          result->best_subtemplate = FTE3600_TEMPLATE_SUBTEMPLATE_NONE;
        }
      return FTE3600_TEMPLATE_INVALID_WIRE;
    }
  return fpi_fte3600_template_compare_features (templ, query, purpose, result);
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
