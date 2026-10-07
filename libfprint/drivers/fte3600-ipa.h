/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 FTE3600 Linux contributors
 *
 * Driver adapter for 2D-IPA. Reusable algorithm core lives in matchers/ipa.
 */

#pragma once

#include <glib.h>
#include "../matchers/ipa/ipa.h"
#include "fte3600-build-config.h"
#include "fte3600-match-profile.h"

G_BEGIN_DECLS

#define FTE3600_IPA_WIDTH FPI_IPA_WIDTH
#define FTE3600_IPA_HEIGHT FPI_IPA_HEIGHT
#define FTE3600_IPA_IMAGE_SIZE FPI_IPA_IMAGE_SIZE

#define FTE3600_IPA_MAX_MINUTIAE FPI_IPA_MAX_MINUTIAE
#define FTE3600_IPA_DESC_DIM FPI_IPA_DESC_DIM
#define FTE3600_IPA_EXTRACTOR_SCHEMA_VERSION FPI_IPA_EXTRACTOR_SCHEMA_VERSION
#define FTE3600_IPA_ORIENTATION_LIMIT ((gfloat) 3.14159265358979323846)
#define FTE3600_IPA_DIAGNOSTIC_POLICY_VERSION FPI_IPA_DIAGNOSTIC_POLICY_VERSION
#define FTE3600_IPA_AUTHENTICATION_POLICY_VERSION (FTE3600_ENABLE_IPA_AUTH ? 2 : 0)

#define FTE3600_IPA_POLICY_MIN_INLIERS FPI_IPA_POLICY_MIN_INLIERS
#define FTE3600_IPA_POLICY_MIN_SCORE FPI_IPA_POLICY_MIN_SCORE
#define FTE3600_IPA_POLICY_MIN_SPAN_X FPI_IPA_POLICY_MIN_SPAN_X
#define FTE3600_IPA_POLICY_MIN_SPAN_Y FPI_IPA_POLICY_MIN_SPAN_Y

typedef FpiIpaStatus Fte3600IpaStatus;
#define FTE3600_IPA_OK FPI_IPA_OK
#define FTE3600_IPA_ERR_PARAM FPI_IPA_ERR_PARAM
#define FTE3600_IPA_ERR_TOO_FEW_POINTS FPI_IPA_ERR_TOO_FEW_POINTS

typedef FpiIpaMinutia Fte3600IpaMinutia;
typedef FpiIpaFeatureSet Fte3600IpaFeatureSet;

typedef struct
{
  guint    n_matched_pairs;
  guint    n_supported_inliers;
  gfloat   consensus_score;
  gfloat   x_span;
  gfloat   y_span;
  gboolean diagnostic_policy_passed;
  gboolean authentication_accepted;
} Fte3600IpaMatchResult;

Fte3600IpaStatus fpi_fte3600_ipa_extract (const guint8         *image,
                                          gsize                 length,
                                          Fte3600IpaFeatureSet *features);

/* Experimental adapters for native 64x80 images. Capability does not make
 * templates from distinct sensor profiles interchangeable. */
gboolean fpi_fte3600_ipa_supports_profile (const Fte3600MatchProfile *profile);
Fte3600IpaStatus fpi_fte3600_ipa_extract_for_profile (const Fte3600MatchProfile *profile,
                                                     const guint8             *image,
                                                     gsize                     length,
                                                     Fte3600IpaFeatureSet     *features);

Fte3600IpaStatus fpi_fte3600_ipa_match (const Fte3600IpaFeatureSet *query,
                                        const Fte3600IpaFeatureSet *reference,
                                        Fte3600IpaMatchResult      *result);

gboolean fpi_fte3600_ipa_result_meets_policy (const Fte3600IpaMatchResult *result);

gboolean fpi_fte3600_ipa_validate_feature_set (const Fte3600IpaFeatureSet *features);

gfloat fpi_fte3600_ipa_projection_coefficient (guint row, guint column);

G_END_DECLS
