/* Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600-private.h"
#include "fte3600-ft9368.h"
#include "fte3600-fw9369.h"
#include "fte3600-ft93xx.h"

const Fte3600Backend *
fpi_fte3600_backend_for_sensor (Fte3600Sensor sensor)
{
  switch (sensor)
    {
    case FTE3600_SENSOR_FT9338:
    case FTE3600_SENSOR_FT9348:
    case FTE3600_SENSOR_FT9361:
    case FTE3600_SENSOR_FT9536:
      return fpi_fte3600_legacy_backend (sensor);

    case FTE3600_SENSOR_FT9365:
    case FTE3600_SENSOR_FT9769:
      return fpi_fte3600_ft93xx_backend (sensor);

    case FTE3600_SENSOR_FT9368:
      return fpi_fte3600_ft9368_backend ();

    case FTE3600_SENSOR_FT9369:
      return fpi_fte3600_fw9369_backend ();

    case FTE3600_SENSOR_UNKNOWN:
    case FTE3600_SENSOR_COUNT:
    default:
      return NULL;
    }
}
