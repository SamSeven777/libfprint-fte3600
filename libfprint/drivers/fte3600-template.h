/*
 * Versioned clean-room BRISK template container for FocalTech FT9361
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#include "fte3600-brisk.h"

G_BEGIN_DECLS

/* Wire v1 is canonical and little-endian.  Its 40-byte header is:
 * magic[8], wire/header/total sizes, model/geometry/feature size, extractor,
 * diagnostic and authentication policy versions, subtemplate count, flags,
 * and reserved.  Each of the eight records has a u32 record size, u16 feature
 * count, u16 recomputed physical count, then 44-byte features containing three
 * IEEE binary32 values and a 32-byte descriptor.  Decoder limits include room
 * for twelve maximum-sized records for safe future parsing, while v1 strictly
 * requires eight and therefore cannot exceed CURRENT_MAX_WIRE_SIZE. */
#define FTE3600_TEMPLATE_WIRE_VERSION              1
#define FTE3600_TEMPLATE_WIRE_HEADER_SIZE          40
#define FTE3600_TEMPLATE_FEATURE_RECORD_SIZE       44
#define FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES      8
#define FTE3600_TEMPLATE_MIN_PHYSICAL_FEATURES     11
#define FTE3600_TEMPLATE_MAX_ORIENTATIONS_PER_LOCATION 36
#define FTE3600_TEMPLATE_MAX_WIRE_SIZE          84616
#define FTE3600_TEMPLATE_CURRENT_MAX_WIRE_SIZE  56424

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

typedef struct {
  guint                    n_compared;
  guint                    diagnostic_passes;
  guint                    best_subtemplate;
  Fte3600BriskMatchResult  best;
  gboolean                 authentication_accepted;
} Fte3600TemplateCompareResult;

/* Template operations that inspect floating-point feature fields temporarily
 * use FE_TONEAREST and restore the caller's rounding mode before returning. */

Fte3600Template *fte3600_template_new (void);
Fte3600Template *fte3600_template_copy (const Fte3600Template *templ);
void             fte3600_template_free (Fte3600Template       *templ);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Fte3600Template, fte3600_template_free)

/* Add one extractor result.  Exact semantic duplicates are rejected.
 * @nearest_match is optional and reports the strongest diagnostic comparison
 * with an existing sample (or is cleared for the first sample).  Personal
 * authentication builds require each sample after the first to pass the same
 * strict gate used for verification against at least one accepted sample.
 * This is a fail-closed consistency check, not a claim of population FAR/FRR
 * calibration.  Default builds do not enable that policy or authentication. */
Fte3600TemplateStatus fte3600_template_add_features (
    Fte3600Template              *templ,
    const Fte3600BriskFeatureSet *features,
    Fte3600BriskMatchResult      *nearest_match);

gboolean fte3600_template_is_ready (const Fte3600Template *templ);

/* On success, encode returns a newly owned GBytes in @wire. */
Fte3600TemplateStatus fte3600_template_encode (const Fte3600Template *templ,
                                                GBytes               **wire);

/* On success, decode returns a newly owned opaque template in @templ.  An
 * authentication load fails closed while authentication policy version zero
 * is stored/compiled. */
Fte3600TemplateStatus fte3600_template_decode (GBytes                      *wire,
                                                Fte3600TemplateLoadPurpose  purpose,
                                                Fte3600Template           **templ);

Fte3600TemplateStatus fte3600_template_compare_features (
    const Fte3600Template        *templ,
    const Fte3600BriskFeatureSet *query,
    Fte3600TemplateLoadPurpose    purpose,
    Fte3600TemplateCompareResult *result);

G_END_DECLS
