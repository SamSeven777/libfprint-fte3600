/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600.h"
#include <stdlib.h>
const FpIdEntry fte3600_driver_ids[] = {
  { .udev_types = FPI_DEVICE_UDEV_SUBTYPE_SPIDEV, .spi_acpi_hid = "FTE3600" },
  { .udev_types = FPI_DEVICE_UDEV_SUBTYPE_SPIDEV, .spi_acpi_hid = "FTE6600" },
  { .udev_types = 0 }
};
const FpDeviceType fte3600_driver_device_type = FP_DEVICE_TYPE_UDEV;
const gchar *
fte3600_driver_resource (FpDevice *device)
{
  return fpi_device_get_udev_data (device, FPI_DEVICE_UDEV_SUBTYPE_SPIDEV);
}

gchar *
fte3600_driver_probe_id (FpDevice *device, GError **error)
{
  const gchar *resource = fte3600_driver_resource (device);

  if (!resource)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Missing SPI resource");
      return NULL;
    }
  g_autofree gchar *name = g_path_get_basename (resource);
  g_autofree gchar *sysfs = g_build_filename ("/sys/class/spidev", name, "device", NULL);
  g_autofree gchar *path = realpath (sysfs, NULL);
  while (path && g_str_has_prefix (path, "/sys/"))
    {
      g_autofree gchar *hid_path = g_build_filename (path, "firmware_node/hid", NULL);
      g_autofree gchar *hid = NULL;
      if (g_file_get_contents (hid_path, &hid, NULL, NULL) &&
          (g_str_equal (g_strstrip (hid), "FTE3600") || g_str_equal (hid, "FTE6600")))
        {
          g_autofree gchar *node = g_build_filename (path, "firmware_node", NULL);
          g_autofree gchar *physical = realpath (node, NULL);
          g_autofree gchar *digest = physical ?
                                     g_compute_checksum_for_string (G_CHECKSUM_SHA256, physical, -1) : NULL;
          if (digest)
            return g_strconcat ("fte3600-acpi-", digest, NULL);
        }
      gchar *parent = g_path_get_dirname (path);
      g_free (path);
      path = parent;
    }
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "FTE3600 firmware identity is missing from SPI ancestry");
  return NULL;
}
