/*
 * Versioned clean-room BRISK template container for FocalTech FT9361
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#include "fte3600-brisk.h"
#include "fte3600-ipa.h"

G_BEGIN_DECLS

/* Wire v1 remains canonical BRISK-only. The experimental unversioned IPA
 * wire v2 is rejected. Wire v3 adds IPA schema/policy/fusion versions in a
 * 48-byte header, followed by a BRISK record and an IPA record for EACH
 * sample (including an empty IPA record when unavailable). */
#define FTE3600_TEMPLATE_WIRE_VERSION_V1 1
#define FTE3600_TEMPLATE_WIRE_VERSION_V2 2
#define FTE3600_TEMPLATE_WIRE_VERSION_V3 3
#define FTE3600_TEMPLATE_WIRE_VERSION FTE3600_TEMPLATE_WIRE_VERSION_V1
#define FTE3600_TEMPLATE_WIRE_HEADER_SIZE 40
#define FTE3600_TEMPLATE_V3_WIRE_HEADER_SIZE 48
#define FTE3600_TEMPLATE_FUSION_POLICY_VERSION 1
#define FTE3600_TEMPLATE_FEATURE_RECORD_SIZE 44
#define FTE3600_TEMPLATE_IPA_RECORD_HEADER_SIZE 8
#define FTE3600_TEMPLATE_IPA_FEATURE_RECORD_SIZE 140
#define FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES 8
#define FTE3600_TEMPLATE_MIN_PHYSICAL_FEATURES 11
#define FTE3600_TEMPLATE_MAX_ORIENTATIONS_PER_LOCATION 36
#define FTE3600_TEMPLATE_MAX_WIRE_SIZE 84616
#define FTE3600_TEMPLATE_CURRENT_MAX_WIRE_SIZE 56424
#define FTE3600_TEMPLATE_V2_CURRENT_MAX_WIRE_SIZE 101288
#define FTE3600_TEMPLATE_V2_MAX_WIRE_SIZE 151912
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

/* NULL selects BRISK unless the separate IPA authentication opt-in is built.
 * Unknown modes fail closed. Diagnostic callers can explicitly select all
 * three modes; authentication additionally checks the build-time gates. */
gboolean fpi_fte3600_engine_mode_parse (const gchar *value,
                                       Fte3600EngineMode *mode);

typedef struct
{
  guint                   n_compared;
  guint                   diagnostic_passes;
  guint                   best_subtemplate;
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
Fte3600Template *fpi_fte3600_template_copy (const Fte3600Template *templ);
void             fpi_fte3600_template_free (Fte3600Template *templ);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Fte3600Template, fpi_fte3600_template_free)

/* Add one extractor result (BRISK-only or Dual BRISK + 2D-IPA). */
Fte3600TemplateStatus fpi_fte3600_template_add_dual_features (Fte3600Template              *templ,
                                                              const Fte3600BriskFeatureSet *brisk_features,
                                                              const Fte3600IpaFeatureSet   *ipa_features,
                                                              Fte3600BriskMatchResult      *nearest_match);

Fte3600TemplateStatus fpi_fte3600_template_add_features (Fte3600Template              *templ,
                                                         const Fte3600BriskFeatureSet *features,
                                                         Fte3600BriskMatchResult      *nearest_match);

gboolean fpi_fte3600_template_is_ready (const Fte3600Template *templ);

/* On success, encode returns a newly owned GBytes in @wire. */
Fte3600TemplateStatus fpi_fte3600_template_encode (const Fte3600Template *templ,
                                                   GBytes               **wire);

/* On success, decode returns a newly owned opaque template in @templ. */
Fte3600TemplateStatus fpi_fte3600_template_decode (GBytes                    *wire,
                                                   Fte3600TemplateLoadPurpose purpose,
                                                   Fte3600Template          **templ);

/* Compare query against gallery using a specific engine mode (BRISK, IPA, or DUAL). */
Fte3600TemplateStatus fpi_fte3600_template_compare_with_mode (const Fte3600Template        *templ,
                                                              const Fte3600BriskFeatureSet *query_brisk,
                                                              const Fte3600IpaFeatureSet   *query_ipa,
                                                              Fte3600TemplateLoadPurpose    purpose,
                                                              Fte3600EngineMode             mode,
                                                              Fte3600TemplateCompareResult *result);

/* Dual-engine helper (BRISK OR 2D-IPA) */
Fte3600TemplateStatus fpi_fte3600_template_compare_dual_features (const Fte3600Template        *templ,
                                                                  const Fte3600BriskFeatureSet *query_brisk,
                                                                  const Fte3600IpaFeatureSet   *query_ipa,
                                                                  Fte3600TemplateLoadPurpose    purpose,
                                                                  Fte3600TemplateCompareResult *result);

/* Mono-engine helper: BRISK only */
Fte3600TemplateStatus fpi_fte3600_template_compare_features (const Fte3600Template        *templ,
                                                             const Fte3600BriskFeatureSet *query,
                                                             Fte3600TemplateLoadPurpose    purpose,
                                                             Fte3600TemplateCompareResult *result);

/* Mono-engine helper: 2D-IPA only */
Fte3600TemplateStatus fpi_fte3600_template_compare_ipa_features (const Fte3600Template      *templ,
                                                                 const Fte3600IpaFeatureSet *query_ipa,
                                                                 Fte3600TemplateLoadPurpose  purpose,
                                                                 Fte3600TemplateCompareResult *result);

G_END_DECLS

