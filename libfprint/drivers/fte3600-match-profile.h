/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "fte3600-sensor.h"

G_BEGIN_DECLS

/* Immutable driver-side image compatibility. Model identifies the catalog
 * backend, not a raw silicon ID; FW9369 therefore uses model 0x9369. */
typedef struct
{
  Fte3600Sensor sensor;
  guint16       model;
  guint16       width;
  guint16       height;
  guint16       processing_version;
} Fte3600MatchProfile;

const Fte3600MatchProfile *fpi_fte3600_match_profile_get (Fte3600Sensor sensor);
const Fte3600MatchProfile *fpi_fte3600_match_profile_find (guint16 model);
/* Accepts exact value copies of a supported profile; rejects any changed
 * model, sensor, geometry or processing revision. Returns canonical storage. */
const Fte3600MatchProfile *fpi_fte3600_match_profile_resolve (const Fte3600MatchProfile *profile);

G_END_DECLS
