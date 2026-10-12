/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 FTE3600 Linux contributors
 * Independent BRISK-style image features and rigid-match evidence.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define FPI_BRISK_PATTERN_POINTS 45
#define FPI_BRISK_DESCRIPTOR_BITS 256
#define FPI_BRISK_DESCRIPTOR_BYTES (FPI_BRISK_DESCRIPTOR_BITS / 8)
#define FPI_BRISK_MAX_FEATURES 160
#define FPI_BRISK_ORIENTATION_LIMIT ((gfloat) 3.14159265358979323846)
#define FPI_BRISK_EXTRACTOR_SCHEMA_VERSION 3
#define FPI_BRISK_PAIR_SEED ((guint32) 0x46544231u)
#define FPI_BRISK_PAIR_TABLE_SHA256 \
  "89a0eb6d633305294aeb095acaef2b872237ebd4499ea1bab68f403f791f21d8"

/* Bounded image work: at most 65,536 source pixels. This also bounds the
 * integral-image accumulators and the two-octave working set. Match canvases
 * can be larger for feature mosaics, without allocating pixel buffers.
 */
#define FPI_BRISK_MIN_IMAGE_DIMENSION 32
#define FPI_BRISK_MAX_IMAGE_DIMENSION 256
#define FPI_BRISK_MAX_CANVAS_DIMENSION 1024

/* Frozen correspondence/consensus construction, not an authentication policy.
 * An OK result reports a fitted model; callers must apply their own decision.
 */
#define FPI_BRISK_MAX_HAMMING 64
#define FPI_BRISK_RATIO_PERCENT 80
#define FPI_BRISK_MIN_HAMMING_MARGIN 8
#define FPI_BRISK_MIN_MUTUAL_MATCHES 5

typedef struct
{
  const guint8 *data;
  gsize         length;
  guint         width;
  guint         height;
  gsize         stride;
} FpiBriskImage;

typedef enum {
  FPI_BRISK_OK,
  FPI_BRISK_INVALID_ARGUMENT,
  FPI_BRISK_LOW_CONTRAST,
  FPI_BRISK_INSUFFICIENT_FEATURES,
  FPI_BRISK_NO_CONSENSUS,
} FpiBriskStatus;

typedef struct
{
  gfloat x;
  gfloat y;
  gfloat orientation;
  guint8 descriptor[FPI_BRISK_DESCRIPTOR_BYTES];
} FpiBriskFeature;

G_STATIC_ASSERT (sizeof (FpiBriskFeature) == 44);

typedef struct
{
  guint           extractor_schema_version;
  guint           n_features;
  FpiBriskFeature features[FPI_BRISK_MAX_FEATURES];
} FpiBriskFeatureSet;

typedef struct
{
  guint   query_index;
  guint   reference_index;
  guint16 hamming;
} FpiBriskCorrespondence;

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
  /* Population covariance of matched inlier locations, in pixel squared.
  * Adapters may transform this evidence for their coordinate systems; the
  * core does not infer a sensor shape or an authentication threshold. */
  gdouble query_covariance_xx;
  gdouble query_covariance_xy;
  gdouble query_covariance_yy;
  gdouble reference_covariance_xx;
  gdouble reference_covariance_xy;
  gdouble reference_covariance_yy;
} FpiBriskMatchEvidence;


/* One-channel 8-bit image. Stride is in bytes; length must cover the last
 * row's active pixels, not necessarily its padding. Data remains caller-owned.
 * Normalization accepts 1..256 pixels per axis; extraction/description require
 * at least 32. Strided and packed views of equal pixels produce equal output.
 * Extraction owns the single normalization pass; pass original pixels.
 * Pixel scale must agree between compared images. No DPI conversion or
 * hardware-specific contrast correction is inferred by the core.
 * Normalization supports overlapping input/output and preserves row padding.
 * Feature/evidence output storage must not overlap any input storage.
 */
gboolean fpi_brisk_image_valid (const FpiBriskImage *image);
FpiBriskStatus fpi_brisk_normalize (const FpiBriskImage *image,
                                    guint8              *destination,
                                    gsize                destination_length,
                                    gsize                destination_stride);
FpiBriskStatus fpi_brisk_extract (const FpiBriskImage *image,
                                  FpiBriskFeatureSet  *features);
FpiBriskStatus fpi_brisk_describe_at (const FpiBriskImage *image,
                                      gfloat               x,
                                      gfloat               y,
                                      FpiBriskFeature     *feature);

/* Feature sets are in-memory values, not a serialized format. Width/height
 * define their coordinate domain and must be preserved by a caller's codec.
 * Schema 3 freezes normalization, ordering, descriptor layout and rounding;
 * the preexisting 64x80 profile remains byte-for-byte compatible.
 * A returned OK status is match evidence only, never an identity decision.
 */
gboolean fpi_brisk_validate_feature_set (const FpiBriskFeatureSet *features,
                                         guint                     width,
                                         guint                     height,
                                         guint                    *physical_count);
FpiBriskStatus fpi_brisk_match (const FpiBriskFeatureSet *query,
                                guint                     query_width,
                                guint                     query_height,
                                const FpiBriskFeatureSet *reference,
                                guint                     reference_width,
                                guint                     reference_height,
                                FpiBriskMatchEvidence    *evidence);

/* Public floating-point operations use FE_TONEAREST and restore the caller's
 * rounding mode. Rings use 10/15/20 points at radii 4/8/13 image pixels.
 */
gboolean fpi_brisk_pattern_point (guint   index,
                                  gfloat *x,
                                  gfloat *y);
gboolean fpi_brisk_descriptor_pair (guint  bit,
                                    guint *first,
                                    guint *second);

G_END_DECLS
