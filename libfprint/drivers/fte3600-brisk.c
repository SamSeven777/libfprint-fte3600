/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 FTE3600 Linux contributors
 * Sensor-parameterized and legacy compatibility decision policies. Image processing
 * and match evidence live in the reusable matchers/brisk core.
 */
#include "fte3600-brisk.h"

#include <fenv.h>
#include <math.h>
#include <string.h>

#define BRISK_PI 3.14159265358979323846

typedef struct
{
  gint     previous_mode;
  gboolean changed;
} BriskRoundingGuard;

static gboolean
brisk_rounding_guard_enter (BriskRoundingGuard *guard)
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
brisk_rounding_guard_clear (BriskRoundingGuard *guard)
{
  if (guard->changed && fesetround (guard->previous_mode) != 0)
    g_warning ("Failed to restore caller floating-point rounding mode");
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (BriskRoundingGuard,
                                  brisk_rounding_guard_clear)

static gboolean
match_result_valid (const Fte3600BriskMatchResult *result, guint width, guint height)
{
  if (result == NULL ||
      result->mutual_matches > FTE3600_BRISK_MAX_FEATURES ||
      result->inliers > result->mutual_matches ||
      result->competing_inliers > result->mutual_matches ||
      result->occupied_cells > 16 || result->occupied_quadrants > 4 ||
      (result->mutual_matches == 0 && result->inlier_ratio != 0.0) ||
      (result->mutual_matches > 0 &&
       fabs (result->inlier_ratio -
             (gdouble) result->inliers / result->mutual_matches) > 1e-9))
    return FALSE;

  return isfinite (result->inlier_ratio) &&
         isfinite (result->mean_hamming) &&
         isfinite (result->rms_error) &&
         isfinite (result->median_error) &&
         isfinite (result->angle) &&
         isfinite (result->translate_x) &&
         isfinite (result->translate_y) &&
         isfinite (result->x_span) && isfinite (result->y_span) &&
         isfinite (result->query_min_variance) &&
         isfinite (result->query_anisotropy) &&
         isfinite (result->reference_min_variance) &&
         isfinite (result->reference_anisotropy) &&
         result->inlier_ratio >= 0.0 && result->inlier_ratio <= 1.0 &&
         result->mean_hamming >= 0.0 && result->mean_hamming <= 256.0 &&
         result->rms_error >= 0.0 && result->median_error >= 0.0 &&
         fabs (result->angle) <= BRISK_PI &&
         result->x_span >= 0.0 && result->x_span < width &&
         result->y_span >= 0.0 && result->y_span < height &&
         result->query_min_variance >= 0.0 &&
         result->reference_min_variance >= 0.0 &&
         result->query_anisotropy >= 0.0 && result->query_anisotropy <= 1.0 &&
         result->reference_anisotropy >= 0.0 &&
         result->reference_anisotropy <= 1.0;
}

static gboolean
result_meets_diagnostic_policy (const Fte3600BriskMatchResult *result,
                                guint width, guint height,
                                gboolean normalized)
{
  g_auto(BriskRoundingGuard) rounding_guard = { 0 };

  if (!brisk_rounding_guard_enter (&rounding_guard))
    return FALSE;
  if (!match_result_valid (result, width, height) || result->competing_inliers > result->inliers)
    return FALSE;
  if (normalized &&
      (!isfinite (result->normalized_query_anisotropy) ||
       !isfinite (result->normalized_reference_anisotropy) ||
       result->normalized_query_anisotropy < 0.0 || result->normalized_query_anisotropy > 1.0 ||
       result->normalized_reference_anisotropy < 0.0 || result->normalized_reference_anisotropy > 1.0 ||
       !isfinite (result->query_max_variance) ||
       !isfinite (result->reference_max_variance) ||
       result->query_max_variance < result->query_min_variance ||
       result->reference_max_variance < result->reference_min_variance))
    return FALSE;

  /* A uniform interval of length d has variance d^2 / 12. Modern policy
   * requires principal equivalent spreads of at least 6 and 8 pixels on
   * both sides. The minor bound is also the existing minimum variance of 3.
   * Axis spans and occupied grid cells remain evidence, but must not decide
   * a rotation-invariant policy. Legacy policy retains its frozen gates. */
  if (normalized)
    {
      if (result->query_max_variance < 64.0 / 12.0 ||
          result->reference_max_variance < 64.0 / 12.0)
        return FALSE;
    }
  else if (result->occupied_quadrants < 1 || result->occupied_cells < 2 ||
           result->x_span < 6.0 || result->y_span < 8.0)
    {
      return FALSE;
    }

  return result->mutual_matches >= FTE3600_BRISK_MIN_MUTUAL_MATCHES &&
         result->inliers >= FTE3600_BRISK_MIN_INLIERS &&
         result->inlier_ratio >= 0.20 &&
         result->inliers - result->competing_inliers >= 1 &&
         result->median_error < 1.25 &&
         result->rms_error < 1.40 &&
         result->mean_hamming <= 60.0 &&
         result->query_min_variance >= 3.0 &&
         result->reference_min_variance >= 3.0 &&
         (normalized ? result->normalized_query_anisotropy : result->query_anisotropy) >= 0.03 &&
         (normalized ? result->normalized_reference_anisotropy : result->reference_anisotropy) >= 0.03;
}

gboolean
fpi_fte3600_brisk_result_meets_diagnostic_policy (const Fte3600BriskMatchResult *result)
{
  return result_meets_diagnostic_policy (result, FTE3600_BRISK_WIDTH, FTE3600_BRISK_HEIGHT, FALSE);
}

gboolean
fpi_fte3600_brisk_result_meets_authentication_policy (const Fte3600BriskMatchResult *result)
{
  g_auto(BriskRoundingGuard) rounding_guard = { 0 };

  if (!brisk_rounding_guard_enter (&rounding_guard))
    return FALSE;
#if FTE3600_ENABLE_PERSONAL_AUTH
  return fpi_fte3600_brisk_result_meets_diagnostic_policy (result);
#else
  (void) result;
  return FALSE;
#endif
}


gboolean
fpi_fte3600_brisk_pattern_point (guint index, gfloat *x, gfloat *y)
{
  return fpi_brisk_pattern_point (index, x, y);
}

gboolean
fpi_fte3600_brisk_descriptor_pair (guint bit, guint *first, guint *second)
{
  return fpi_brisk_descriptor_pair (bit, first, second);
}

Fte3600BriskStatus
fpi_fte3600_brisk_describe_at (const guint8 *data, gsize length,
                               gfloat x, gfloat y, Fte3600BriskFeature *feature)
{
  const FpiBriskImage image = { data, length, FTE3600_BRISK_WIDTH,
                                FTE3600_BRISK_HEIGHT, FTE3600_BRISK_WIDTH };

  if (length != FTE3600_BRISK_IMAGE_SIZE)
    {
      if (feature)
        memset (feature, 0, sizeof (*feature));
      return FTE3600_BRISK_INVALID_ARGUMENT;
    }
  return fpi_brisk_describe_at (&image, x, y, feature);
}

Fte3600BriskStatus
fpi_fte3600_brisk_extract (const guint8 *data, gsize length,
                           Fte3600BriskFeatureSet *features)
{
  const FpiBriskImage image = { data, length, FTE3600_BRISK_WIDTH,
                                FTE3600_BRISK_HEIGHT, FTE3600_BRISK_WIDTH };

  if (length != FTE3600_BRISK_IMAGE_SIZE)
    {
      if (features)
        {
          memset (features, 0, sizeof (*features));
          features->extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
        }
      return FTE3600_BRISK_INVALID_ARGUMENT;
    }
  return fpi_brisk_extract (&image, features);
}

Fte3600BriskStatus
fpi_fte3600_brisk_extract_for_profile (const Fte3600MatchProfile *profile,
                                       const FpiBriskImage       *image,
                                       Fte3600BriskFeatureSet    *features)
{
  profile = fpi_fte3600_match_profile_resolve (profile);
  if (!profile || !image || image->width != profile->width || image->height != profile->height)
    {
      if (features)
        {
          memset (features, 0, sizeof (*features));
          features->extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
        }
      return FTE3600_BRISK_INVALID_ARGUMENT;
    }
  return fpi_brisk_extract (image, features);
}

guint16
fpi_fte3600_brisk_diagnostic_policy_version (const Fte3600MatchProfile *profile)
{
  profile = fpi_fte3600_match_profile_resolve (profile);
  if (!profile)
    return 0;
  return FTE3600_BRISK_FAMILY_DIAGNOSTIC_POLICY_VERSION;
}

guint16
fpi_fte3600_brisk_authentication_policy_version (const Fte3600MatchProfile *profile)
{
  profile = fpi_fte3600_match_profile_resolve (profile);
  if (!profile)
    return 0;
  return FTE3600_BRISK_FAMILY_AUTHENTICATION_POLICY_VERSION;
}

gboolean
fpi_fte3600_brisk_validate_feature_set_for_profile (const Fte3600MatchProfile    *profile,
                                                    const Fte3600BriskFeatureSet *features,
                                                    guint                        *physical_count)
{
  if (physical_count)
    *physical_count = 0;
  profile = fpi_fte3600_match_profile_resolve (profile);
  return profile && fpi_brisk_validate_feature_set (features, profile->width,
                                                    profile->height, physical_count);
}

gboolean
fpi_fte3600_brisk_validate_mosaic_feature_set_for_profile (const Fte3600MatchProfile    *profile,
                                                           const Fte3600BriskFeatureSet *features,
                                                           guint                        *physical_count)
{
  if (physical_count)
    *physical_count = 0;
  profile = fpi_fte3600_match_profile_resolve (profile);
  return profile && fpi_brisk_validate_feature_set (features, 3u * profile->width,
                                                    3u * profile->height, physical_count);
}

void
fpi_fte3600_normalize_image_contrast (const guint8 *source, guint8 *destination,
                                      guint width, guint height)
{
  const FpiBriskImage image = { source, (gsize) width * height, width, height, width };

  fpi_brisk_normalize (&image, destination, image.length, width);
}

gboolean
fpi_fte3600_brisk_validate_feature_set (const Fte3600BriskFeatureSet *features,
                                        guint                        *physical_count)
{
  return fpi_brisk_validate_feature_set (features, FTE3600_BRISK_WIDTH,
                                         FTE3600_BRISK_HEIGHT, physical_count);
}

gboolean
fpi_fte3600_brisk_validate_mosaic_feature_set (const Fte3600BriskFeatureSet *features,
                                               guint                        *physical_count)
{
  return fpi_brisk_validate_feature_set (features, FTE3600_BRISK_MOSAIC_WIDTH,
                                         FTE3600_BRISK_MOSAIC_HEIGHT, physical_count);
}

static gdouble
normalized_principal_anisotropy (const Fte3600MatchProfile *profile,
                                 gdouble                    anisotropy)
{
  const gdouble aspect = (gdouble) MAX (profile->width, profile->height) /
                         MIN (profile->width, profile->height);

  /* Eigenvalues are invariant under rigid rotation. Compare the minor
   * variance per short-side squared to the major variance per long-side
   * squared; clamp rather than swap them when the point cloud is rounder
   * than the sensor. More transverse spread must never reduce this score.
   * The caller holds FE_TONEAREST; images and descriptors are unchanged. */
  return MIN (1.0, anisotropy * aspect * aspect);
}

static Fte3600BriskStatus
match_for_geometry (const Fte3600MatchProfile *profile,
                    const Fte3600BriskFeatureSet *query,
                    const Fte3600BriskFeatureSet *reference,
                    guint reference_width, guint reference_height,
                    gboolean normalized,
                    Fte3600BriskMatchResult *result)
{
  g_auto(BriskRoundingGuard) rounding_guard = { 0 };
  FpiBriskMatchEvidence evidence = { 0 };
  FpiBriskStatus status;

  if (!result)
    return FTE3600_BRISK_INVALID_ARGUMENT;
  memset (result, 0, sizeof (*result));
  if (!brisk_rounding_guard_enter (&rounding_guard))
    return FTE3600_BRISK_INVALID_ARGUMENT;
  profile = fpi_fte3600_match_profile_resolve (profile);
  if (!profile)
    return FTE3600_BRISK_INVALID_ARGUMENT;
  status = fpi_brisk_match (query, profile->width, profile->height,
                            reference, reference_width, reference_height, &evidence);
  result->mutual_matches = evidence.mutual_matches;
  result->inliers = evidence.inliers;
  result->competing_inliers = evidence.competing_inliers;
  result->occupied_cells = evidence.occupied_cells;
  result->occupied_quadrants = evidence.occupied_quadrants;
  result->inlier_ratio = evidence.inlier_ratio;
  result->mean_hamming = evidence.mean_hamming;
  result->rms_error = evidence.rms_error;
  result->median_error = evidence.median_error;
  result->angle = evidence.angle;
  result->translate_x = evidence.translate_x;
  result->translate_y = evidence.translate_y;
  result->x_span = evidence.x_span;
  result->y_span = evidence.y_span;
  result->query_min_variance = evidence.query_min_variance;
  result->query_anisotropy = evidence.query_anisotropy;
  result->reference_min_variance = evidence.reference_min_variance;
  result->reference_anisotropy = evidence.reference_anisotropy;
  if (status == FPI_BRISK_OK)
    {
      if (!normalized)
        {
          result->normalized_query_anisotropy = evidence.query_anisotropy;
          result->normalized_reference_anisotropy = evidence.reference_anisotropy;
        }
      else
        {
          result->normalized_query_anisotropy = normalized_principal_anisotropy (
            profile, evidence.query_anisotropy);
          result->normalized_reference_anisotropy = normalized_principal_anisotropy (
            profile, evidence.reference_anisotropy);
          result->query_max_variance = MAX (0.0, evidence.query_covariance_xx +
                                            evidence.query_covariance_yy - evidence.query_min_variance);
          result->reference_max_variance = MAX (0.0, evidence.reference_covariance_xx +
                                                evidence.reference_covariance_yy - evidence.reference_min_variance);
        }
      result->diagnostic_policy_passed =
        result_meets_diagnostic_policy (result, profile->width, profile->height, normalized);
      result->authentication_accepted =
        (normalized ? FTE3600_BRISK_FAMILY_AUTHENTICATION_POLICY_VERSION :
         FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION) != 0 &&
        result->diagnostic_policy_passed;
    }
  return status;
}

Fte3600BriskStatus
fpi_fte3600_brisk_match (const Fte3600BriskFeatureSet *query,
                         const Fte3600BriskFeatureSet *reference,
                         Fte3600BriskMatchResult      *result)
{
  return match_for_geometry (fpi_fte3600_match_profile_get (FTE3600_SENSOR_FT9361),
                             query, reference, FTE3600_BRISK_WIDTH,
                             FTE3600_BRISK_HEIGHT, FALSE, result);
}

Fte3600BriskStatus
fpi_fte3600_brisk_match_mosaic (const Fte3600BriskFeatureSet *query,
                                const Fte3600BriskFeatureSet *reference,
                                Fte3600BriskMatchResult      *result)
{
  return match_for_geometry (fpi_fte3600_match_profile_get (FTE3600_SENSOR_FT9361),
                             query, reference, FTE3600_BRISK_MOSAIC_WIDTH,
                             FTE3600_BRISK_MOSAIC_HEIGHT, FALSE, result);
}

Fte3600BriskStatus
fpi_fte3600_brisk_match_for_profile (const Fte3600MatchProfile    *profile,
                                     const Fte3600BriskFeatureSet *query,
                                     const Fte3600BriskFeatureSet *reference,
                                     Fte3600BriskMatchResult      *result)
{
  profile = fpi_fte3600_match_profile_resolve (profile);
  return match_for_geometry (profile, query, reference,
                             profile ? profile->width : 0,
                             profile ? profile->height : 0, TRUE, result);
}

Fte3600BriskStatus
fpi_fte3600_brisk_match_mosaic_for_profile (const Fte3600MatchProfile    *profile,
                                            const Fte3600BriskFeatureSet *query,
                                            const Fte3600BriskFeatureSet *mosaic,
                                            Fte3600BriskMatchResult      *result)
{
  profile = fpi_fte3600_match_profile_resolve (profile);
  return match_for_geometry (profile, query, mosaic,
                             profile ? 3u * profile->width : 0,
                             profile ? 3u * profile->height : 0, TRUE, result);
}
