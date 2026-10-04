/*
 * Bounded validation of externally supplied FocalTech firmware
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <gio/gio.h>

#include "fte3600-sensor.h"

/* An allocation bound independent of filesystem contents. Current audited
 * payloads are smaller than 32 KiB. This is not a transport packet limit. */
#define FTE3600_FIRMWARE_MAX_SIZE (1024U * 1024U)

/* Validates one specified payload; does not select a sensor, grant upload
 * permission, or infer compatibility from a path or matching file length. */
GBytes *fpi_fte3600_firmware_load (const Fte3600Firmware *firmware,
                                   const gchar           *path,
                                   GError               **error);

/* Compatibility entry point for existing FT9361 tests/callers. */
GBytes *fpi_fte3600_load_firmware (const gchar *path,
                                   GError     **error);
