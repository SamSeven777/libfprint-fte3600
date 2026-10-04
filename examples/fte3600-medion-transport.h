/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Medion E3224 experimental diagnostic transport; not a libfprint device route.
 */
#pragma once

#include "drivers/fte3600-private.h"

typedef struct
{
  const gchar *spi_path;
  const gchar *reset_gpiochip;
  const gchar *irq_gpiochip;
} Fte3600MedionTransportConfig;

/* The launcher must first validate this exact SPI device's FTE3600 ACPI HID,
 * its spidev binding and the Medion resource description: GPO1/INT3453 pin 39
 * reset and GPO2 pin 0 rising IRQ. Paths are not trusted based on gpiochip
 * numbers. It must also exclude competing SPI clients. attach() performs no
 * hardware I/O and copies the paths. Configuration survives probe/open/close.
 */
gboolean fte3600_medion_transport_attach (FpiDeviceFte3600                  *self,
                                        const Fte3600MedionTransportConfig *config,
                                        GError                           **error);

/* Call after the core has closed its SPI fd and before unref. Releases the
 * saved configuration and reports the first error from any resource cleanup.
 * The transport release callback itself is idempotent and retains this error.
 */
gboolean fte3600_medion_transport_detach (FpiDeviceFte3600 *self,
                                        GError          **error);
