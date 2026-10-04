/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Image identities are kept outside the hardware-independent BRISK core. */
#include "fte3600-match-profile.h"

static const Fte3600MatchProfile profiles[] = {
  { FTE3600_SENSOR_FT9338, 0x9338, 88, 88, 1 },
  { FTE3600_SENSOR_FT9348, 0x9348, 96, 96, 1 },
  { FTE3600_SENSOR_FT9361, 0x9361, 64, 80, 1 },
  { FTE3600_SENSOR_FT9536, 0x9536, 64, 128, 1 },
  { FTE3600_SENSOR_FT9365, 0x9365, 64, 80, 1 },
  { FTE3600_SENSOR_FT9368, 0x9368, 64, 80, 1 },
  { FTE3600_SENSOR_FT9369, 0x9369, 64, 80, 1 },
  { FTE3600_SENSOR_FT9769, 0x9769, 40, 196, 1 },
};

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
  const Fte3600MatchProfile *canonical;

  if (!profile)
    return NULL;
  canonical = fpi_fte3600_match_profile_get (profile->sensor);
  if (!canonical || canonical->model != profile->model ||
      canonical->width != profile->width || canonical->height != profile->height ||
      canonical->processing_version != profile->processing_version)
    return NULL;
  return canonical;
}
