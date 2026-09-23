/*
 * 2D Invariant Point Attention (2D-IPA) Matcher for FocalTech FT9361 / FTE3600
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define FTE3600_IPA_WIDTH 64
#define FTE3600_IPA_HEIGHT 80
#define FTE3600_IPA_IMAGE_SIZE (FTE3600_IPA_WIDTH * FTE3600_IPA_HEIGHT)

#define FTE3600_IPA_MAX_MINUTIAE 32
#define FTE3600_IPA_DESC_DIM 32
#define FTE3600_IPA_NUM_PROBES 4

#define FTE3600_IPA_POLICY_MIN_INLIERS 4
#define FTE3600_IPA_POLICY_MIN_SCORE   0.50f

typedef enum {
  FTE3600_IPA_OK = 0,
  FTE3600_IPA_ERR_PARAM = -1,
  FTE3600_IPA_ERR_TOO_FEW_POINTS = -2,
} Fte3600IpaStatus;

typedef struct {
  gfloat x;
  gfloat y;
  gfloat theta;
  gfloat desc[FTE3600_IPA_DESC_DIM];
} Fte3600IpaMinutia;

typedef struct {
  guint n_minutiae;
  Fte3600IpaMinutia minutiae[FTE3600_IPA_MAX_MINUTIAE];
} Fte3600IpaFeatureSet;

typedef struct {
  guint n_matched_pairs;
  guint n_supported_inliers;
  gfloat consensus_score;
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
 * Match two minutiae sets using 2D Invariant Point Attention and
 * SE(2) Topological Invariant Consensus without RANSAC.
 */
Fte3600IpaStatus fpi_fte3600_ipa_match (const Fte3600IpaFeatureSet *query,
                                        const Fte3600IpaFeatureSet *reference,
                                        Fte3600IpaMatchResult      *result);

/*
 * Decision policy: returns TRUE if match result satisfies the authentication gate.
 */
gboolean fpi_fte3600_ipa_result_meets_policy (const Fte3600IpaMatchResult *result);

G_END_DECLS
