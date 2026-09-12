/*
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

#include <glib.h>
#include <gpiod.h>
#include <string.h>

/*
 * spidev does not expose the GPIO descriptors from its ACPI companion to
 * userspace.  Keep verified DMI-specific ACPI resource mappings in a table,
 * while resolving the gpiochip device dynamically so gpiochip numbering is
 * never assumed.  The A1 values below are the output-only, active-low reset
 * GpioIo pin 0x55 and the GpioInt pin 0x56 (Edge, ActiveHigh), both on
 * \_SB.PCI0.GPI0.
 */
typedef struct
{
  const gchar *sys_vendor;
  const gchar *product_name;
  const gchar *product_version;
  const gchar *board_name;
  const gchar *reset_controller_acpi_path;
  guint        reset_offset;
  gboolean     reset_active_low;
  gboolean     allow_hardware_reset;
  const gchar *irq_controller_acpi_path;
  guint        irq_offset;
} Fte3600GpioProfile;

static const Fte3600GpioProfile fte3600_gpio_profiles[] = {
  {
    .sys_vendor = "ONE-NETBOOK TECHNOLOGY CO., LTD.",
    .product_name = "A1",
    .product_version = NULL,
    .board_name = NULL,
    .reset_controller_acpi_path = "\\_SB_.PCI0.GPI0",
    .reset_offset = 0x55,
    .reset_active_low = TRUE,
    .allow_hardware_reset = TRUE,
    .irq_controller_acpi_path = "\\_SB_.PCI0.GPI0",
    .irq_offset = 0x56,
  },
  {
    .sys_vendor = "MEDION",
    .product_name = "E3224",
    .product_version = "FT",
    .board_name = "YS13G",
    .reset_controller_acpi_path = "\\_SB_.GPO1",
    .reset_offset = 0x27,
    .reset_active_low = TRUE,
    .allow_hardware_reset = FALSE, /* Safety gate: disabled until polarity is confirmed */
    .irq_controller_acpi_path = "\\_SB_.GPO2",
    .irq_offset = 0x00,
  },
};

static inline enum gpiod_line_value
fte3600_reset_line_value (const Fte3600GpioProfile *profile,
                          gboolean                  asserted)
{
  g_assert (profile != NULL);

  if (profile->reset_active_low)
    return asserted ? GPIOD_LINE_VALUE_INACTIVE : GPIOD_LINE_VALUE_ACTIVE;
  else
    return asserted ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
}

static inline const Fte3600GpioProfile *
fte3600_lookup_gpio_profile (const gchar *sys_vendor,
                             const gchar *product_name,
                             const gchar *product_version,
                             const gchar *board_name)
{
  for (guint i = 0; i < G_N_ELEMENTS (fte3600_gpio_profiles); i++)
    {
      const Fte3600GpioProfile *profile = &fte3600_gpio_profiles[i];

      if (g_strcmp0 (sys_vendor, profile->sys_vendor) != 0 ||
          g_strcmp0 (product_name, profile->product_name) != 0)
        continue;
      if (profile->product_version &&
          g_strcmp0 (product_version, profile->product_version) != 0)
        continue;
      if (profile->board_name &&
          g_strcmp0 (board_name, profile->board_name) != 0)
        continue;
      return profile;
    }
  return NULL;
}

static inline gboolean
fte3600_acpi_path_equal (const gchar *path_a,
                         const gchar *path_b)
{
  g_auto (GStrv) parts_a = NULL;
  g_auto (GStrv) parts_b = NULL;
  guint len_a, len_b;

  if (!path_a || !path_b || !*path_a || !*path_b)
    return FALSE;
  if (g_str_equal (path_a, path_b))
    return TRUE;

  while (*path_a == '\\')
    path_a++;
  while (*path_b == '\\')
    path_b++;

  parts_a = g_strsplit (path_a, ".", -1);
  parts_b = g_strsplit (path_b, ".", -1);
  len_a = g_strv_length (parts_a);
  len_b = g_strv_length (parts_b);
  if (len_a != len_b)
    return FALSE;

  for (guint i = 0; i < len_a; i++)
    {
      gchar *s_a = parts_a[i];
      gchar *s_b = parts_b[i];
      gsize slen_a = strlen (s_a);
      gsize slen_b = strlen (s_b);

      while (slen_a > 0 && s_a[slen_a - 1] == '_')
        {
          s_a[slen_a - 1] = '\0';
          slen_a--;
        }
      while (slen_b > 0 && s_b[slen_b - 1] == '_')
        {
          s_b[slen_b - 1] = '\0';
          slen_b--;
        }

      if (g_strcmp0 (s_a, s_b) != 0)
        return FALSE;
    }

  return TRUE;
}
