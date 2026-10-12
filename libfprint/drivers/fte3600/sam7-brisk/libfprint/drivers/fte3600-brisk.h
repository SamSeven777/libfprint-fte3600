/*
 * Sensor-parameterized BRISK adapter for the FocalTech FTE3600 family
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "../matchers/brisk/brisk.h"
#include "fte3600-build-config.h"
#include "fte3600-match-profile.h"

G_BEGIN_DECLS

/* Wire-v1/legacy API compatibility only. Modern profile calls pass their
* native image geometry and never use these as a reference image size. */
#define FTE3600_BRISK_WIDTH 64
#define FTE3600_BRISK_HEIGHT 80
#define FTE3600_BRISK_IMAGE_SIZE (FTE3600_BRISK_WIDTH * FTE3600_BRISK_HEIGHT)

#define FTE3600_BRISK_MOSAIC_WIDTH 192
#define FTE3600_BRISK_MOSAIC_HEIGHT 240
#define FTE3600_BRISK_MOSAIC_ANCHOR_X 64.0f
#define FTE3600_BRISK_MOSAIC_ANCHOR_Y 80.0f

#define FTE3600_BRISK_PATTERN_POINTS FPI_BRISK_PATTERN_POINTS
#define FTE3600_BRISK_DESCRIPTOR_BITS FPI_BRISK_DESCRIPTOR_BITS
#define FTE3600_BRISK_DESCRIPTOR_BYTES FPI_BRISK_DESCRIPTOR_BYTES
#define FTE3600_BRISK_MAX_FEATURES FPI_BRISK_MAX_FEATURES
/* Inclusive representable orientation domain for schema v1.  Extractor
 * output is a gfloat, so validation uses this binary32 boundary rather than a
 * narrower binary64 approximation of pi. */
#define FTE3600_BRISK_ORIENTATION_LIMIT FPI_BRISK_ORIENTATION_LIMIT

/*
 * This version covers the complete extractor schema, including image scaling,
 * blur/DoG construction and units, keypoint refinement/order, orientation,
 * point layout, fixed pair table, sampling/rounding, descriptor bit order, and
 * feature field meaning.  Any change to those rules requires a new version.
 *
 * Version 1: initial baseline clean-room BRISK extractor.
 * Version 2: introduced 13x13 Local Contrast Normalization (LCN). SPI workers
 * also normalized their input, so deployed templates could use two passes.
 * Version 3: the extractor owns the only LCN pass. Reject earlier templates
 * rather than silently compare features from different preprocessing paths.
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
#define FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION FPI_BRISK_EXTRACTOR_SCHEMA_VERSION
#define FTE3600_BRISK_DESCRIPTOR_VERSION FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION
#define FTE3600_BRISK_PAIR_SEED FPI_BRISK_PAIR_SEED
#define FTE3600_BRISK_PAIR_TABLE_SHA256 FPI_BRISK_PAIR_TABLE_SHA256

/* No population FAR/FRR calibration exists for this prototype.  The optional
 * personal policy is an explicit local opt-in which reuses the frozen strict
 * diagnostic gates; it must not be presented as generally authentication-safe.
 * Policy version 2 was the historical 7-inlier prototype policy.
 * Policy version 3 requires at least 5 inliers and 5 mutual matches with
 * spatial variance and residual bounds, and requires every enrollment sample
 * after the first to pass the authentication gate against an already accepted sample.
 * Policy version 4 retains the same gates for contrast-normalized extraction.
 * Policy version 6 accepts any of eight individual subtemplates or their
 * canonically reconstructed mosaic. It retains the five-inlier pair gates.
 * Version 5 belongs to the separate Pocket policy and is not interchangeable.
 * The single-pass Schema v3 correction changes extractor compatibility only.
 * Extractor compatibility and decision-policy revisions are intentionally versioned
 * separately.  A default build retains policy version zero and cannot
 * authenticate.
 * Diagnostic policy version 1 was the historical 7-inlier diagnostic policy;
 * version 2 corresponded to 5-inlier gates under Schema v1;
 * version 3 retains those uncalibrated gates for contrast-normalized input;
 * version 5 adds canonical mosaic comparison to the eight-sample gallery.
 * Diagnostic version 4 belongs to the separate Pocket policy. */
#define FTE3600_BRISK_DIAGNOSTIC_POLICY_VERSION 5
#if FTE3600_ENABLE_PERSONAL_AUTH
#define FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION 6
#else
#define FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION 0
#endif
#define FTE3600_BRISK_THRESHOLDS_CALIBRATED 0
/* Family policy 8/diagnostic 7 uses rotation-invariant principal variances.
 * The minor/major variance ratio is compensated by (long/short sensor side)^2
 * and capped at one. Minimum principal spreads replace axis/grid gates.
 * Policy 7/diagnostic 6 used axis-scaled covariance and is not compatible.
 * Neither policy has population FAR/FRR calibration. */
#define FTE3600_BRISK_FAMILY_DIAGNOSTIC_POLICY_VERSION 7
#if FTE3600_ENABLE_PERSONAL_AUTH
#define FTE3600_BRISK_FAMILY_AUTHENTICATION_POLICY_VERSION 8
#else
#define FTE3600_BRISK_FAMILY_AUTHENTICATION_POLICY_VERSION 0
#endif
G_STATIC_ASSERT (FTE3600_BRISK_AUTHENTICATION_POLICY_VERSION ==
                 (FTE3600_ENABLE_PERSONAL_AUTH ? 6 : 0));
#define FTE3600_BRISK_MAX_HAMMING FPI_BRISK_MAX_HAMMING
#define FTE3600_BRISK_RATIO_PERCENT FPI_BRISK_RATIO_PERCENT
#define FTE3600_BRISK_MIN_HAMMING_MARGIN FPI_BRISK_MIN_HAMMING_MARGIN
#define FTE3600_BRISK_MIN_MUTUAL_MATCHES FPI_BRISK_MIN_MUTUAL_MATCHES
#define FTE3600_BRISK_MIN_INLIERS 5

typedef FpiBriskStatus         Fte3600BriskStatus;
typedef FpiBriskFeature        Fte3600BriskFeature;
typedef FpiBriskFeatureSet     Fte3600BriskFeatureSet;
typedef FpiBriskCorrespondence Fte3600BriskCorrespondence;
#define FTE3600_BRISK_OK FPI_BRISK_OK
#define FTE3600_BRISK_INVALID_ARGUMENT FPI_BRISK_INVALID_ARGUMENT
#define FTE3600_BRISK_LOW_CONTRAST FPI_BRISK_LOW_CONTRAST
#define FTE3600_BRISK_INSUFFICIENT_FEATURES FPI_BRISK_INSUFFICIENT_FEATURES
#define FTE3600_BRISK_NO_CONSENSUS FPI_BRISK_NO_CONSENSUS

typedef struct
{
  guint   mutual_matches;
  guint   inliers;
  guint   competing_inliers;
  guint   occupied_cells;
  guint   occupied_quadrants;
  gdouble inlier_ratio;
  gdouble mean_hamming;
  gdouble rms_error;
  gdouble median_error;
  gdouble angle;
  gdouble translate_x;
  gdouble translate_y;
  gdouble x_span;
  gdouble y_span;
  gdouble query_min_variance;
  gdouble query_anisotropy;
  gdouble reference_min_variance;
  gdouble reference_anisotropy;
  /* Family policy: min(1, raw eigenvalue ratio * (long / short side)^2).
   * The same sensor aspect ratio applies to a mosaic; its storage canvas
   * does not change pixel scale. Legacy API results preserve raw ratios. */
  gdouble  normalized_query_anisotropy;
  gdouble  normalized_reference_anisotropy;
  /* Family policy only; major principal variances in pixel squared. */
  gdouble  query_max_variance;
  gdouble  reference_max_variance;
  gboolean diagnostic_policy_passed;
  gboolean authentication_accepted;
} Fte3600BriskMatchResult;

/* Introspection helpers make the clean-room construction reproducible and
 * testable.  Angles are radians; the three rings contain 10, 15, and 20 points
 * at radii 4, 8, and 13 pixels respectively.  Public operations that perform
 * floating-point work temporarily use FE_TONEAREST and restore the caller's
 * rounding mode before returning; this is part of extractor schema v1. */
gboolean fpi_fte3600_brisk_pattern_point (guint   index,
                                          gfloat *x,
                                          gfloat *y);

gboolean fpi_fte3600_brisk_descriptor_pair (guint  bit,
                                            guint *first,
                                            guint *second);

Fte3600BriskStatus fpi_fte3600_brisk_describe_at (const guint8        *image,
                                                  gsize                length,
                                                  gfloat               x,
                                                  gfloat               y,
                                                  Fte3600BriskFeature *feature);

Fte3600BriskStatus fpi_fte3600_brisk_extract (const guint8           *image,
                                              gsize                   length,
                                              Fte3600BriskFeatureSet *features);

Fte3600BriskStatus fpi_fte3600_brisk_extract_for_profile (const Fte3600MatchProfile *profile,
                                                          const FpiBriskImage       *image,
                                                          Fte3600BriskFeatureSet    *features);
gboolean fpi_fte3600_brisk_validate_feature_set_for_profile (const Fte3600MatchProfile    *profile,
                                                             const Fte3600BriskFeatureSet *features,
                                                             guint                        *physical_count);
gboolean fpi_fte3600_brisk_validate_mosaic_feature_set_for_profile (const Fte3600MatchProfile    *profile,
                                                                    const Fte3600BriskFeatureSet *features,
                                                                    guint                        *physical_count);
Fte3600BriskStatus fpi_fte3600_brisk_match_for_profile (const Fte3600MatchProfile    *profile,
                                                        const Fte3600BriskFeatureSet *query,
                                                        const Fte3600BriskFeatureSet *reference,
                                                        Fte3600BriskMatchResult      *result);
Fte3600BriskStatus fpi_fte3600_brisk_match_mosaic_for_profile (const Fte3600MatchProfile    *profile,
                                                               const Fte3600BriskFeatureSet *query,
                                                               const Fte3600BriskFeatureSet *mosaic,
                                                               Fte3600BriskMatchResult      *result);
guint16 fpi_fte3600_brisk_diagnostic_policy_version (const Fte3600MatchProfile *profile);
guint16 fpi_fte3600_brisk_authentication_policy_version (const Fte3600MatchProfile *profile);

/* Validate the extractor schema and every feature's finite coordinate and
 * orientation range.  If requested, @physical_count receives the number of
 * connected components formed by feature locations less than 1.5 pixels
 * apart.  Multiple orientation variants at one detector location therefore
 * count as one piece of physical evidence. */
gboolean fpi_fte3600_brisk_validate_feature_set (const Fte3600BriskFeatureSet *features,
                                                 guint                        *physical_count);

gboolean fpi_fte3600_brisk_validate_mosaic_feature_set (const Fte3600BriskFeatureSet *features,
                                                        guint                        *physical_count);

Fte3600BriskStatus fpi_fte3600_brisk_match (const Fte3600BriskFeatureSet *query,
                                            const Fte3600BriskFeatureSet *reference,
                                            Fte3600BriskMatchResult      *result);

Fte3600BriskStatus fpi_fte3600_brisk_match_mosaic (const Fte3600BriskFeatureSet *query,
                                                   const Fte3600BriskFeatureSet *mosaic,
                                                   Fte3600BriskMatchResult      *result);

/* Diagnostic policy only: useful for calibration experiments, never an
 * authentication decision. */
gboolean fpi_fte3600_brisk_result_meets_diagnostic_policy (const Fte3600BriskMatchResult *result);

/* Authentication gate.  A default build always returns FALSE.  An explicitly
 * opted-in personal build applies the same frozen strict gates as diagnostic
 * policy version 3 without claiming population calibration. */
gboolean fpi_fte3600_brisk_result_meets_authentication_policy (const Fte3600BriskMatchResult *result);

/*
 * Zero-mean Integral Image Local Contrast Normalization (LCN) using a 13x13 window
 * (~1.5 ridge wavelengths at 508 DPI) with noise-floor regularization.
 * Called by the extractor; callers must pass their original image to extract.
 */
void fpi_fte3600_normalize_image_contrast (const guint8 *src,
                                           guint8       *dst,
                                           guint         width,
                                           guint         height);

G_END_DECLS
