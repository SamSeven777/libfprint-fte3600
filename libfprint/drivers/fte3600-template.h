/*
 * Versioned BRISK template container for the FocalTech FTE3600 family
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#include "fte3600-brisk.h"
#include "fte3600-ipa.h"

G_BEGIN_DECLS

/* Wire v1 is canonical and little-endian.  Its 40-byte header is:
 * magic[8], wire/header/total sizes, model/geometry/feature size, extractor,
 * diagnostic and authentication policy versions, subtemplate count, flags,
 * and reserved.  Each of the eight records has a u32 record size, u16 feature
 * count, u16 recomputed physical count, then 44-byte features containing three
 * IEEE binary32 values and a 32-byte descriptor.  Decoder limits include room
 * for twelve maximum-sized records for safe future parsing, while v1/v2
 * strictly require eight and cannot exceed CURRENT_MAX_WIRE_SIZE. Wire v2
 * preserves this layout, requires a registered model/geometry pair, and uses
 * the final reserved u32 as the image-processing revision.
 * Wire v3 uses a 48-byte header and dual-engine container storing both
 * BRISK and 2D-IPA features with Grand Synergy v3 fusion policy. */
#define FTE3600_TEMPLATE_WIRE_VERSION 1
#define FTE3600_TEMPLATE_PROFILE_WIRE_VERSION 2
#define FTE3600_TEMPLATE_WIRE_VERSION_V3 3
#define FTE3600_TEMPLATE_WIRE_HEADER_SIZE 40
#define FTE3600_TEMPLATE_V3_WIRE_HEADER_SIZE 48
#define FTE3600_TEMPLATE_FUSION_POLICY_VERSION 3
#define FTE3600_TEMPLATE_FEATURE_RECORD_SIZE 44
#define FTE3600_TEMPLATE_IPA_RECORD_HEADER_SIZE 8
#define FTE3600_TEMPLATE_IPA_FEATURE_RECORD_SIZE 140
#define FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES 8
#define FTE3600_TEMPLATE_MIN_PHYSICAL_FEATURES 11
#define FTE3600_TEMPLATE_MAX_ORIENTATIONS_PER_LOCATION 36
#define FTE3600_TEMPLATE_MAX_WIRE_SIZE 84616
#define FTE3600_TEMPLATE_CURRENT_MAX_WIRE_SIZE 56424
#define FTE3600_TEMPLATE_V3_CURRENT_MAX_WIRE_SIZE 101296
#define FTE3600_TEMPLATE_V3_MAX_WIRE_SIZE 151920

typedef enum {
  FTE3600_TEMPLATE_OK,
  FTE3600_TEMPLATE_NEED_MORE_SAMPLES,
  FTE3600_TEMPLATE_RETRY_LOW_CONTRAST,
  FTE3600_TEMPLATE_RETRY_INSUFFICIENT_FEATURES,
  FTE3600_TEMPLATE_RETRY_DUPLICATE,
  FTE3600_TEMPLATE_RETRY_INCONSISTENT,
  FTE3600_TEMPLATE_INVALID_WIRE,
  FTE3600_TEMPLATE_UNSUPPORTED_SCHEMA,
  FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR,
  FTE3600_TEMPLATE_UNSUPPORTED_POLICY,
  FTE3600_TEMPLATE_NOT_CALIBRATED,
} Fte3600TemplateStatus;

typedef enum {
  FTE3600_TEMPLATE_LOAD_DIAGNOSTIC,
  FTE3600_TEMPLATE_LOAD_AUTHENTICATION,
} Fte3600TemplateLoadPurpose;

typedef struct _Fte3600Template Fte3600Template;

typedef enum {
  FTE3600_ENGINE_MODE_BRISK_ONLY  = 0,
  FTE3600_ENGINE_MODE_IPA_ONLY    = 1,
  FTE3600_ENGINE_MODE_DUAL_FUSION = 2,
} Fte3600EngineMode;

gboolean fpi_fte3600_engine_mode_parse (const gchar       *value,
                                        Fte3600EngineMode *mode);

#define FTE3600_TEMPLATE_SUBTEMPLATE_NONE G_MAXUINT
#define FTE3600_TEMPLATE_SUBTEMPLATE_MOSAIC (G_MAXUINT - 1)

typedef struct
{
  guint                   n_compared; /* Includes the mosaic, when present. */
  guint                   diagnostic_passes;
  guint                   best_subtemplate; /* Index, MOSAIC, or NONE. */
  Fte3600BriskMatchResult best;
  Fte3600IpaMatchResult   best_ipa;
  gboolean                brisk_accepted;
  gboolean                ipa_accepted;
  gboolean                authentication_accepted;
  Fte3600EngineMode       engine_mode;
} Fte3600TemplateCompareResult;

/* Template operations that inspect floating-point feature fields temporarily
 * use FE_TONEAREST and restore the caller's rounding mode before returning. */

Fte3600Template *fpi_fte3600_template_new (void);
/* New profile containers use wire v2: the same 40-byte header as v1, with
 * model/width/height at offsets 16/18/20 and processing_version as a complete
 * little-endian u32 at offset 36 (upper 16 bits must be zero). Old new() keeps
 * wire v1 FT9361 and its canonical bytes. Both use extractor schema 3. */
Fte3600Template *fpi_fte3600_template_new_for_profile (const Fte3600MatchProfile *profile);
const Fte3600MatchProfile *fpi_fte3600_template_get_profile (const Fte3600Template *templ);
Fte3600Template *fpi_fte3600_template_copy (const Fte3600Template *templ);
void             fpi_fte3600_template_free (Fte3600Template *templ);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Fte3600Template, fpi_fte3600_template_free)

/* Add one extractor result.  Exact semantic duplicates are rejected.
 * @nearest_match is optional and reports the strongest diagnostic comparison
 * with an existing sample (or is cleared for the first sample).  Personal
 * authentication builds require each sample after the first to pass the same
 * strict gate used for verification against at least one accepted sample.
 * This is a fail-closed consistency check, not a claim of population FAR/FRR
 * calibration.  Default builds do not enable that policy or authentication. */
Fte3600TemplateStatus fpi_fte3600_template_add_features (Fte3600Template              *templ,
                                                         const Fte3600BriskFeatureSet *features,
                                                         Fte3600BriskMatchResult      *nearest_match);

gboolean fpi_fte3600_template_is_ready (const Fte3600Template *templ);

/* On success, encode returns a newly owned GBytes in @wire. */
Fte3600TemplateStatus fpi_fte3600_template_encode (const Fte3600Template *templ,
                                                   GBytes               **wire);

/* On success, decode returns a newly owned opaque template in @templ.  An
 * authentication load fails closed while authentication policy version zero
 * is stored/compiled. */
Fte3600TemplateStatus fpi_fte3600_template_decode (GBytes                    *wire,
                                                   Fte3600TemplateLoadPurpose purpose,
                                                   Fte3600Template          **templ);

Fte3600TemplateStatus fpi_fte3600_template_compare_features (const Fte3600Template        *templ,
                                                             const Fte3600BriskFeatureSet *query,
                                                             Fte3600TemplateLoadPurpose    purpose,
                                                             Fte3600TemplateCompareResult *result);
/* The untagged FeatureSet cannot identify its capture source. Drivers should
 * use this entry point with the actual selected sensor profile; differing
 * models are rejected even when their image geometries are identical. */
Fte3600TemplateStatus fpi_fte3600_template_compare_features_for_profile (const Fte3600Template        *templ,
                                                                         const Fte3600MatchProfile    *profile,
                                                                         const Fte3600BriskFeatureSet *query,
                                                                         Fte3600TemplateLoadPurpose    purpose,
                                                                         Fte3600TemplateCompareResult *result);

Fte3600TemplateStatus fpi_fte3600_template_add_dual_features (Fte3600Template              *templ,
                                                              const Fte3600BriskFeatureSet *brisk_features,
                                                              const Fte3600IpaFeatureSet   *ipa_features,
                                                              Fte3600BriskMatchResult      *nearest_match);

Fte3600TemplateStatus fpi_fte3600_template_compare_with_mode (const Fte3600Template        *templ,
                                                              const Fte3600BriskFeatureSet *query_brisk,
                                                              const Fte3600IpaFeatureSet   *query_ipa,
                                                              Fte3600TemplateLoadPurpose    purpose,
                                                              Fte3600EngineMode             mode,
                                                              Fte3600TemplateCompareResult *result);

Fte3600TemplateStatus fpi_fte3600_template_compare_dual_features (const Fte3600Template        *templ,
                                                                  const Fte3600BriskFeatureSet *query_brisk,
                                                                  const Fte3600IpaFeatureSet   *query_ipa,
                                                                  Fte3600TemplateLoadPurpose    purpose,
                                                                  Fte3600TemplateCompareResult *result);

Fte3600TemplateStatus fpi_fte3600_template_compare_ipa_features (const Fte3600Template        *templ,
                                                                 const Fte3600IpaFeatureSet   *query_ipa,
                                                                 Fte3600TemplateLoadPurpose    purpose,
                                                                 Fte3600TemplateCompareResult *result);

/* The mosaic is reconstructed in canonical order when enrollment completes.
 * Returns a borrowed reference, or NULL before completion/without a mosaic. */
const Fte3600BriskFeatureSet *fpi_fte3600_template_get_mosaic (const Fte3600Template *templ);

G_END_DECLS
