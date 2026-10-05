/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 FTE3600 Linux contributors
 *
 * Sensor adapter and decision policy for 2D-IPA. Reusable algorithm core lives
 * in matchers/ipa.
 */

#include "fte3600-ipa.h"
#include <string.h>

gboolean
fpi_fte3600_ipa_supports_profile (const Fte3600MatchProfile *profile)
{
  profile = fpi_fte3600_match_profile_resolve (profile);
  return profile &&
         (profile->sensor == FTE3600_SENSOR_FT9361 ||
          profile->sensor == FTE3600_SENSOR_FT9369) &&
         profile->width == FPI_IPA_WIDTH && profile->height == FPI_IPA_HEIGHT;
}

Fte3600IpaStatus
fpi_fte3600_ipa_extract_for_profile (const Fte3600MatchProfile *profile,
                                     const guint8             *image,
                                     gsize                     length,
                                     Fte3600IpaFeatureSet     *features)
{
  if (!fpi_fte3600_ipa_supports_profile (profile))
    {
      if (features)
        memset (features, 0, sizeof (*features));
      return FTE3600_IPA_ERR_PARAM;
    }
  return fpi_ipa_extract (image, length, features);
}

Fte3600IpaStatus
fpi_fte3600_ipa_extract (const guint8         *image,
                         gsize                 length,
                         Fte3600IpaFeatureSet *features)
{
  return fpi_ipa_extract (image, length, (FpiIpaFeatureSet *) features);
}

Fte3600IpaStatus
fpi_fte3600_ipa_match (const Fte3600IpaFeatureSet *query,
                       const Fte3600IpaFeatureSet *reference,
                       Fte3600IpaMatchResult      *result)
{
  FpiIpaMatchEvidence evidence;
  FpiIpaStatus status;

  if (result == NULL)
    return FTE3600_IPA_ERR_PARAM;

  memset (result, 0, sizeof (*result));
  status = fpi_ipa_match ((const FpiIpaFeatureSet *) query,
                          (const FpiIpaFeatureSet *) reference,
                          &evidence);
  if (status != FPI_IPA_OK)
    return status;

  result->n_matched_pairs = evidence.n_matched_pairs;
  result->n_supported_inliers = evidence.n_supported_inliers;
  result->consensus_score = evidence.consensus_score;
  result->x_span = evidence.x_span;
  result->y_span = evidence.y_span;
  result->diagnostic_policy_passed = evidence.diagnostic_policy_passed;
#if FTE3600_ENABLE_IPA_AUTH
  result->authentication_accepted = evidence.diagnostic_policy_passed;
#else
  result->authentication_accepted = FALSE;
#endif

  return FTE3600_IPA_OK;
}

gboolean
fpi_fte3600_ipa_result_meets_policy (const Fte3600IpaMatchResult *result)
{
  if (result == NULL)
    return FALSE;
  return fpi_ipa_result_meets_policy ((const FpiIpaMatchEvidence *) result);
}

gboolean
fpi_fte3600_ipa_validate_feature_set (const Fte3600IpaFeatureSet *features)
{
  return fpi_ipa_validate_feature_set ((const FpiIpaFeatureSet *) features);
}

gfloat
fpi_fte3600_ipa_projection_coefficient (guint row, guint column)
{
  return fpi_ipa_projection_coefficient (row, column);
}
