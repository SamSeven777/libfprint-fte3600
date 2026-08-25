/*
 * Clean-room BRISK-style feature matcher prototype for the FocalTech FT9361
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define FTE3600_BRISK_WIDTH                 64
#define FTE3600_BRISK_HEIGHT                80
#define FTE3600_BRISK_IMAGE_SIZE            (FTE3600_BRISK_WIDTH * FTE3600_BRISK_HEIGHT)

#define FTE3600_BRISK_PATTERN_POINTS        45
#define FTE3600_BRISK_DESCRIPTOR_BITS       256
#define FTE3600_BRISK_DESCRIPTOR_BYTES      (FTE3600_BRISK_DESCRIPTOR_BITS / 8)
#define FTE3600_BRISK_MAX_FEATURES          160
/* Inclusive representable orientation domain for schema v1.  Extractor
 * output is a gfloat, so validation uses this binary32 boundary rather than a
 * narrower binary64 approximation of pi. */
#define FTE3600_BRISK_ORIENTATION_LIMIT      ((gfloat) 3.14159265358979323846)

/*
 * This version covers the complete extractor schema, including image scaling,
 * blur/DoG construction and units, keypoint refinement/order, orientation,
 * point layout, fixed pair table, sampling/rounding, descriptor bit order, and
 * feature field meaning.  Any change to those rules requires a new version.
 *
 * Version 1's clean-room pair table was generated offline using xorshift32,
 * this public seed, and the balancing algorithm recorded beside the frozen
 * table.  It was not copied from a vendor binary.
 *
 * Fte3600BriskFeatureSet is an in-memory type, not a wire format.  The
 * fte3600-template codec defines a separate magic/version, fixed endianness,
 * lengths, and IEEE binary32 encoding; this structure must never be memcpy'd
 * to persistent storage.
 */
#define FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION 1
#define FTE3600_BRISK_DESCRIPTOR_VERSION       FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION
#define FTE3600_BRISK_PAIR_SEED             ((guint32) 0x46544231u)
#define FTE3600_BRISK_PAIR_TABLE_SHA256      \
  "89a0eb6d633305294aeb095acaef2b87"     \
  "2237ebd4499ea1bab68f403f791f21d8"

/* No population FAR/FRR calibration exists for this prototype.  The optional
 * personal policy is an explicit local opt-in which reuses the frozen strict
 * diagnostic gates; it must not be presented as generally authentication-safe.
 * Policy version 2 also requires every enrollment sample after the first to
 * pass the authentication gate against an already accepted sample.  Extractor
 * compatibility and decision-policy revisions are intentionally versioned
 * separately.  A default build retains policy version zero and cannot
 * authenticate. */
#ifndef FTE3600_ENABLE_PERSONAL_AUTH
#define FTE3600_ENABLE_PERSONAL_AUTH 0
#endif
#if FTE3600_ENABLE_PERSONAL_AUTH != 0 && FTE3600_ENABLE_PERSONAL_AUTH != 1
#error "FTE3600_ENABLE_PERSONAL_AUTH must be zero or one"
#endif
#define FTE3600_BRISK_DIAGNOSTIC_POLICY_VERSION 1
#if FTE3600_ENABLE_PERSONAL_AUTH
#define FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION 2
#else
#define FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION 0
#endif
#define FTE3600_BRISK_THRESHOLDS_CALIBRATED 0
G_STATIC_ASSERT (FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION ==
                 (FTE3600_ENABLE_PERSONAL_AUTH ? 2 : 0));
#define FTE3600_BRISK_MAX_HAMMING            64
#define FTE3600_BRISK_RATIO_PERCENT          80
#define FTE3600_BRISK_MIN_HAMMING_MARGIN       8
#define FTE3600_BRISK_MIN_MUTUAL_MATCHES      11
#define FTE3600_BRISK_MIN_INLIERS             11

typedef enum {
  FTE3600_BRISK_OK,
  FTE3600_BRISK_INVALID_ARGUMENT,
  FTE3600_BRISK_LOW_CONTRAST,
  FTE3600_BRISK_INSUFFICIENT_FEATURES,
  FTE3600_BRISK_NO_CONSENSUS,
} Fte3600BriskStatus;

/* Deliberately kept at the vendor-observed conceptual size without copying
 * the vendor representation or serialized format. */
typedef struct {
  gfloat x;
  gfloat y;
  gfloat orientation;
  guint8 descriptor[FTE3600_BRISK_DESCRIPTOR_BYTES];
} Fte3600BriskFeature;

G_STATIC_ASSERT (sizeof (Fte3600BriskFeature) == 44);

typedef struct {
  guint                extractor_schema_version;
  guint                n_features;
  Fte3600BriskFeature  features[FTE3600_BRISK_MAX_FEATURES];
} Fte3600BriskFeatureSet;

typedef struct {
  guint   query_index;
  guint   reference_index;
  guint16 hamming;
} Fte3600BriskCorrespondence;

typedef struct {
  guint    mutual_matches;
  guint    inliers;
  guint    competing_inliers;
  guint    occupied_cells;
  guint    occupied_quadrants;
  gdouble  inlier_ratio;
  gdouble  mean_hamming;
  gdouble  rms_error;
  gdouble  median_error;
  gdouble  angle;
  gdouble  translate_x;
  gdouble  translate_y;
  gdouble  x_span;
  gdouble  y_span;
  gdouble  query_min_variance;
  gdouble  query_anisotropy;
  gdouble  reference_min_variance;
  gdouble  reference_anisotropy;
  gboolean diagnostic_policy_passed;
  gboolean authentication_accepted;
} Fte3600BriskMatchResult;

/* Introspection helpers make the clean-room construction reproducible and
 * testable.  Angles are radians; the three rings contain 10, 15, and 20 points
 * at radii 4, 8, and 13 pixels respectively.  Public operations that perform
 * floating-point work temporarily use FE_TONEAREST and restore the caller's
 * rounding mode before returning; this is part of extractor schema v1. */
gboolean fte3600_brisk_pattern_point (guint   index,
                                      gfloat *x,
                                      gfloat *y);

gboolean fte3600_brisk_descriptor_pair (guint  bit,
                                        guint *first,
                                        guint *second);

Fte3600BriskStatus fte3600_brisk_describe_at (const guint8          *image,
                                               gsize                  length,
                                               gfloat                 x,
                                               gfloat                 y,
                                               Fte3600BriskFeature  *feature);

Fte3600BriskStatus fte3600_brisk_extract (const guint8             *image,
                                          gsize                     length,
                                          Fte3600BriskFeatureSet   *features);

/* Validate the extractor schema and every feature's finite coordinate and
 * orientation range.  If requested, @physical_count receives the number of
 * connected components formed by feature locations less than 1.5 pixels
 * apart.  Multiple orientation variants at one detector location therefore
 * count as one piece of physical evidence. */
gboolean fte3600_brisk_validate_feature_set (const Fte3600BriskFeatureSet *features,
                                              guint                        *physical_count);

Fte3600BriskStatus fte3600_brisk_match (const Fte3600BriskFeatureSet *query,
                                        const Fte3600BriskFeatureSet *reference,
                                        Fte3600BriskMatchResult      *result);

/* Diagnostic policy only: useful for calibration experiments, never an
 * authentication decision. */
gboolean fte3600_brisk_result_meets_diagnostic_policy (const Fte3600BriskMatchResult *result);

/* Authentication gate.  A default build always returns FALSE.  An explicitly
 * opted-in personal build applies the same frozen strict gates as diagnostic
 * policy version 1 without claiming population calibration. */
gboolean fte3600_brisk_result_meets_authentication_policy (const Fte3600BriskMatchResult *result);

G_END_DECLS
