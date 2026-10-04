/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 FTE3600 Linux contributors
 *
 * 2D Invariant Point Attention (2D-IPA) biometric matcher core.
 * Independent mathematical feature extraction and rigid point-set matching.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define FPI_IPA_WIDTH 64
#define FPI_IPA_HEIGHT 80
#define FPI_IPA_IMAGE_SIZE (FPI_IPA_WIDTH * FPI_IPA_HEIGHT)

#define FPI_IPA_MAX_MINUTIAE 40
#define FPI_IPA_DESC_DIM 32
#define FPI_IPA_NUM_PROBES 4

#define FPI_IPA_EXTRACTOR_SCHEMA_VERSION 3
#define FPI_IPA_DIAGNOSTIC_POLICY_VERSION 2

#define FPI_IPA_ORIENTATION_LIMIT ((gfloat) 3.14159265358979323846)

#define FPI_IPA_POLICY_MIN_INLIERS 4
#define FPI_IPA_POLICY_MIN_SCORE 0.41f
#define FPI_IPA_POLICY_MIN_SPAN_X 6.0f
#define FPI_IPA_POLICY_MIN_SPAN_Y 8.0f
#define FPI_IPA_POLICY_RELAXED_INLIERS 5
#define FPI_IPA_POLICY_RELAXED_SCORE 0.35f

typedef enum {
  FPI_IPA_OK = 0,
  FPI_IPA_ERR_PARAM = -1,
  FPI_IPA_ERR_TOO_FEW_POINTS = -2,
} FpiIpaStatus;

typedef struct
{
  gfloat x;
  gfloat y;
  gfloat theta;
  gfloat desc[FPI_IPA_DESC_DIM];
} FpiIpaMinutia;

typedef struct
{
  guint         extractor_schema_version;
  guint         n_minutiae;
  FpiIpaMinutia minutiae[FPI_IPA_MAX_MINUTIAE];
} FpiIpaFeatureSet;

typedef struct
{
  guint    n_matched_pairs;
  guint    n_supported_inliers;
  gfloat   consensus_score;
  gfloat   x_span;
  gfloat   y_span;
  gboolean diagnostic_policy_passed;
} FpiIpaMatchEvidence;

/*
 * Extract salient minutiae points and rotation-aligned patch descriptors
 * from a raw 64x80 grayscale image using Structure Tensor and Harris NMS.
 */
FpiIpaStatus fpi_ipa_extract (const guint8     *image,
                              gsize             length,
                              FpiIpaFeatureSet *features);

/*
 * Match two minutiae sets using 2D Invariant Point Attention,
 * SE(2) Vector Bearing Consistency, and Global Rigid Cluster Verification.
 */
FpiIpaStatus fpi_ipa_match (const FpiIpaFeatureSet *query,
                            const FpiIpaFeatureSet *reference,
                            FpiIpaMatchEvidence    *result);

gboolean fpi_ipa_result_meets_policy (const FpiIpaMatchEvidence *result);

gboolean fpi_ipa_validate_feature_set (const FpiIpaFeatureSet *features);

/* Projection used by schemas v1/v2/v3: normalized Sylvester Hadamard H32.
 * H[row,column] = (-1)^popcount(row & column) / sqrt(32). */
gfloat fpi_ipa_projection_coefficient (guint row,
                                       guint column);

G_END_DECLS
