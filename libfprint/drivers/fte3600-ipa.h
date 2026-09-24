/*
 * 2D Invariant Point Attention (2D-IPA) Matcher for FocalTech FT9361 / FTE3600
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>
#include "fte3600-brisk.h"

G_BEGIN_DECLS

#define FTE3600_IPA_WIDTH 64
#define FTE3600_IPA_HEIGHT 80
#define FTE3600_IPA_IMAGE_SIZE (FTE3600_IPA_WIDTH * FTE3600_IPA_HEIGHT)

#define FTE3600_IPA_MAX_MINUTIAE 40
#define FTE3600_IPA_DESC_DIM 32
#define FTE3600_IPA_NUM_PROBES 4

/* Versioned experimental schema. These gates have no population FAR/FRR
 * calibration. Authentication needs a separate opt-in in addition to the
 * BRISK personal-authentication build; diagnostic matching is always available. */
#define FTE3600_IPA_EXTRACTOR_SCHEMA_VERSION 2
#define FTE3600_IPA_DIAGNOSTIC_POLICY_VERSION 2
#ifndef FTE3600_ENABLE_IPA_AUTH
#define FTE3600_ENABLE_IPA_AUTH 0
#endif
#if FTE3600_ENABLE_IPA_AUTH != 0 && FTE3600_ENABLE_IPA_AUTH != 1
#error "FTE3600_ENABLE_IPA_AUTH must be zero or one"
#endif
#if FTE3600_ENABLE_IPA_AUTH && !FTE3600_ENABLE_PERSONAL_AUTH
#error "IPA authentication requires personal authentication as well"
#endif
#define FTE3600_IPA_AUTHENTICATION_POLICY_VERSION (FTE3600_ENABLE_IPA_AUTH ? 2 : 0)
#define FTE3600_IPA_ORIENTATION_LIMIT ((gfloat) 3.14159265358979323846)

#define FTE3600_IPA_POLICY_MIN_INLIERS 4
#define FTE3600_IPA_POLICY_MIN_SCORE 0.41f
#define FTE3600_IPA_POLICY_MIN_SPAN_X 6.0f
#define FTE3600_IPA_POLICY_MIN_SPAN_Y 8.0f
#define FTE3600_IPA_POLICY_RELAXED_INLIERS 5
#define FTE3600_IPA_POLICY_RELAXED_SCORE 0.35f

typedef enum {
  FTE3600_IPA_OK = 0,
  FTE3600_IPA_ERR_PARAM = -1,
  FTE3600_IPA_ERR_TOO_FEW_POINTS = -2,
} Fte3600IpaStatus;

typedef struct
{
  gfloat x;
  gfloat y;
  gfloat theta;
  gfloat desc[FTE3600_IPA_DESC_DIM];
} Fte3600IpaMinutia;

typedef struct
{
  guint             extractor_schema_version;
  guint             n_minutiae;
  Fte3600IpaMinutia minutiae[FTE3600_IPA_MAX_MINUTIAE];
} Fte3600IpaFeatureSet;

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

/*
 * Extract salient minutiae points and rotation-aligned patch descriptors
 * from a raw 64x80 grayscale image using Structure Tensor and Harris NMS.
 */
Fte3600IpaStatus fpi_fte3600_ipa_extract (const guint8         *image,
                                          gsize                 length,
                                          Fte3600IpaFeatureSet *features);

/*
 * Match two minutiae sets using 2D Invariant Point Attention,
 * SE(2) Vector Bearing Consistency, and Global Rigid Cluster Verification.
 */
Fte3600IpaStatus fpi_fte3600_ipa_match (const Fte3600IpaFeatureSet *query,
                                        const Fte3600IpaFeatureSet *reference,
                                        Fte3600IpaMatchResult      *result);

/*
 * Diagnostic gate only; match.authentication_accepted additionally applies
 * both explicit build-time authentication opt-ins.
 */
gboolean fpi_fte3600_ipa_result_meets_policy (const Fte3600IpaMatchResult *result);

gboolean fpi_fte3600_ipa_validate_feature_set (const Fte3600IpaFeatureSet *features);

/* Projection used by schemas v1/v2: normalized Sylvester Hadamard H32.
 * H[row,column] = (-1)^popcount(row & column) / sqrt(32). */
gfloat fpi_fte3600_ipa_projection_coefficient (guint row,
                                               guint column);

G_END_DECLS
