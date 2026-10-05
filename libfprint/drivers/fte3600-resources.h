/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <gio/gio.h>
#include <sys/types.h>

typedef struct
{
  gchar   *glue_path;
  guint64  generation;
  guint32  acpi_mode;
  guint32  acpi_speed_hz;
  gboolean irq_active_low;
  gboolean cs_control;
} Fte3600Resources;

/* sysfs_root is explicit for isolated tests; the driver always passes /sys.
 * Device numbers come from fstat of the opened character devices. */
gboolean fpi_fte3600_resources_resolve (const gchar      *sysfs_root,
                                        dev_t             spi_device,
                                        dev_t             gpio_device,
                                        dev_t             irq_device,
                                        Fte3600Resources *resources,
                                        GError          **error);
gboolean fpi_fte3600_resources_check (const Fte3600Resources *resources,
                                      GError                **error);
gboolean fpi_fte3600_resources_buffer_size (const gchar *sysfs_root,
                                            guint32     *size,
                                            GError     **error);
void fpi_fte3600_resources_clear (Fte3600Resources *resources);
