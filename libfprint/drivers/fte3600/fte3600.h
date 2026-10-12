/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "drivers_api.h"
G_DECLARE_FINAL_TYPE (FpiDeviceFte3600, fpi_device_fte3600, FPI, DEVICE_FTE3600, FpDevice)

/* Production resource binding lives in a separate translation unit. Tests
 * replace that unit and the OS backend, never the capture/authentication driver. */
extern const FpIdEntry fte3600_driver_ids[];
extern const FpDeviceType fte3600_driver_device_type;
const gchar *fte3600_driver_resource (FpDevice *device);
gchar *fte3600_driver_probe_id (FpDevice *device,
                                GError  **error);
