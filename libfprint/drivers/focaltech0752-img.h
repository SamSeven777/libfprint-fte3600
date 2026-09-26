/*
 * FocalTech FT9362 (2808:0752) Image Preprocessing
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>
#include <stdint.h>
#include "fte3600-brisk.h"

#define RAW_IMAGE_SIZE        12166
#define FT_RAW_HEADER         6

#define FT9362_ACTIVE_HEIGHT  76
#define FT9362_ACTIVE_WIDTH   40
#define FT9362_ACTIVE_SIZE    (FT9362_ACTIVE_HEIGHT * FT9362_ACTIVE_WIDTH) /* 3040 */

void focaltech0752_process_raw_to_brisk (const uint8_t *raw_data,
                                         guint8 brisk_image[FTE3600_BRISK_IMAGE_SIZE]);
