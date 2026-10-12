/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Derived from SamSeven777/libfprint-focaltech-fte3600,
 * libfprint/drivers/fte3600-match-profile.c at
 * 7e12c09abbd53516076b6d67a480003dc272cc8e.
 * Copyright (C) 2026 FTE3600 Linux contributors
 *
 * Local extension: exact reviewed inner-chip geometries and a distinct
 * processing revision for the independent device session. Pinned files stay
 * unchanged. Link this resolver instead of the upstream profile catalog.
 */
#include "fte3600_auth.h"
#define FTE3600_SENSOR_UNKNOWN SAM7_FTE3600_SENSOR_UNKNOWN
#include "fte3600-match-profile.h"
#undef FTE3600_SENSOR_UNKNOWN

/* Extended enum values are private catalog keys, not public upstream sensor
 * classifications. The unchanged adapter treats these as opaque identities. */
#define INNER(chip, w, h) { (Fte3600Sensor) (chip), chip, w, h, FTE3600_AUTH_PROCESSING_VERSION }
static const Fte3600MatchProfile profiles[] = {
  { FTE3600_SENSOR_FT9338, 0x9338, 88, 88, FTE3600_AUTH_PROCESSING_VERSION },
  { FTE3600_SENSOR_FT9348, 0x9348, 96, 96, FTE3600_AUTH_PROCESSING_VERSION },
  { FTE3600_SENSOR_FT9361, 0x9361, 64, 80, FTE3600_AUTH_PROCESSING_VERSION },
  { FTE3600_SENSOR_FT9536, 0x9536, 64, 128, FTE3600_AUTH_PROCESSING_VERSION },
  { FTE3600_SENSOR_FT9368, 0x9368, 64, 80, FTE3600_AUTH_PROCESSING_VERSION },
  { FTE3600_SENSOR_FT9369, 0x9369, 64, 80, FTE3600_AUTH_PROCESSING_VERSION },
  { FTE3600_SENSOR_FT9365, 0x9365, 64, 80, FTE3600_AUTH_PROCESSING_VERSION },
  INNER (0x9349, 80, 128), INNER (0x9363, 64, 80), INNER (0x9372, 56, 56),
  INNER (0x9391, 40, 196), INNER (0x9392, 40, 196), INNER (0x9395, 36, 176),
  INNER (0x9396, 36, 96), INNER (0x9397, 32, 176), INNER (0x9398, 36, 144),
};
#undef INNER

const Fte3600MatchProfile *
fpi_fte3600_match_profile_get (Fte3600Sensor sensor)
{
  for (guint i = 0; i < G_N_ELEMENTS (profiles); i++)
    if (profiles[i].sensor == sensor)
      return &profiles[i];
  return NULL;
}

const Fte3600MatchProfile *
fpi_fte3600_match_profile_find (guint16 model)
{
  for (guint i = 0; i < G_N_ELEMENTS (profiles); i++)
    if (profiles[i].model == model)
      return &profiles[i];
  return NULL;
}

const Fte3600MatchProfile *
fpi_fte3600_match_profile_resolve (const Fte3600MatchProfile *profile)
{
  if (!profile)
    return NULL;
  const Fte3600MatchProfile *canonical = fpi_fte3600_match_profile_get (profile->sensor);
  if (!canonical || canonical->model != profile->model ||
      canonical->width != profile->width || canonical->height != profile->height ||
      canonical->processing_version != profile->processing_version)
    return NULL;
  return canonical;
}
