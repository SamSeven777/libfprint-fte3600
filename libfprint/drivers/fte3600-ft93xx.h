/* Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "fte3600-sensor.h"

typedef struct _Fte3600Backend Fte3600Backend;
const Fte3600Backend *fpi_fte3600_ft93xx_backend (Fte3600Sensor sensor);
