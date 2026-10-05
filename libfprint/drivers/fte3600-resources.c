/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Read-only association of stock spidev with the reset GPIO and IRQ-only UIO companions.
 */
#include "fte3600-resources.h"

#include <errno.h>
#include <linux/spi/spidev.h>
#include <stdlib.h>
#include <sys/sysmacros.h>

static gchar *
read_attribute (const gchar *directory, const gchar *name, GError **error)
{
  g_autofree gchar *path = g_build_filename (directory, name, NULL);
  gchar *text = NULL;
  gsize size;

  if (!g_file_get_contents (path, &text, &size, error))
    return NULL;
  if (!size || size > 4096 || strlen (text) != size)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid ACPI resource attribute %s", path);
      g_free (text);
      return NULL;
    }
  return g_strstrip (text);
}

static gboolean
read_number (const gchar *directory, const gchar *name, guint64 maximum,
             guint64 *value, GError **error)
{
  g_autofree gchar *text = read_attribute (directory, name, error);

  if (!text)
    return FALSE;
  if (!g_ascii_string_to_unsigned (text, 10, 0, maximum, value, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid numeric ACPI resource attribute %s", name);
      return FALSE;
    }
  return TRUE;
}

static gchar *
resolve_path (const gchar *path, GError **error)
{
  gchar *result = realpath (path, NULL);

  if (!result)
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                 "Cannot resolve device resource %s: %s", path, g_strerror (errno));
  return result;
}

static gboolean
check_ready (const gchar *path, guint64 *generation, GError **error)
{
  g_autofree gchar *status = NULL;
  guint64 first, last;

  /* A suspend between these reads must not create a valid new session. */
  if (!read_number (path, "fte3600_generation", G_MAXUINT64, &first, error))
    return FALSE;
  status = read_attribute (path, "fte3600_status", error);
  if (!status)
    return FALSE;
  if (!read_number (path, "fte3600_generation", G_MAXUINT64, &last, error))
    return FALSE;
  if (!g_str_equal (status, "ready") || first != last)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
                           "FTE3600 ACPI resources are suspended or changed; reopen the device");
      return FALSE;
    }
  *generation = first;
  return TRUE;
}

gboolean
fpi_fte3600_resources_resolve (const gchar *sysfs_root, dev_t spi_device,
                               dev_t gpio_device, dev_t irq_device, Fte3600Resources *resources,
                               GError **error)
{
  g_autofree gchar *spi_number = g_strdup_printf ("%u:%u", major (spi_device), minor (spi_device));
  g_autofree gchar *gpio_number = g_strdup_printf ("%u:%u", major (gpio_device), minor (gpio_device));
  g_autofree gchar *irq_number = g_strdup_printf ("%u:%u", major (irq_device), minor (irq_device));
  g_autofree gchar *irq_path = g_build_filename (sysfs_root, "dev", "char", irq_number, NULL);
  g_autofree gchar *irq_node = NULL, *irq_parent_path = NULL, *irq_parent = NULL;
  g_autofree gchar *irq_name = NULL, *irq_version = NULL, *irq_source = NULL;
  g_autofree gchar *irq_subsystem_path = NULL, *irq_subsystem = NULL, *irq_subsystem_name = NULL;
  g_autofree gchar *maps_path = NULL, *ports_path = NULL;
  g_autofree gchar *spi_path = g_build_filename (sysfs_root, "dev", "char", spi_number, "device", NULL);
  g_autofree gchar *gpio_path = g_build_filename (sysfs_root, "dev", "char", gpio_number, NULL);
  g_autofree gchar *firmware_path = g_build_filename (spi_path, "firmware_node", NULL);
  g_autofree gchar *driver_path = g_build_filename (spi_path, "driver", NULL);
  g_autofree gchar *driver = resolve_path (driver_path, error);
  g_autofree gchar *driver_name = NULL;
  g_autofree gchar *spi_acpi = NULL;
  g_autofree gchar *spi_parent = NULL;
  g_autofree gchar *gpio_node = NULL;
  g_autofree gchar *gpio_subsystem_path = NULL;
  g_autofree gchar *gpio_subsystem = NULL;
  g_autofree gchar *gpio_subsystem_name = NULL;
  g_autofree gchar *glue = NULL;
  g_autofree gchar *glue_parent = NULL;
  g_autofree gchar *glue_driver_path = NULL;
  g_autofree gchar *glue_driver = NULL;
  g_autofree gchar *glue_driver_name = NULL;
  g_autofree gchar *hid = NULL;
  guint64 abi, mode, speed, polarity, generation, last_generation, lines, cs_control;

  if (!driver)
    return FALSE;
  driver_name = g_path_get_basename (driver);
  if (!g_str_equal (driver_name, "spidev"))
    goto invalid;
  spi_acpi = resolve_path (firmware_path, error);
  if (!spi_acpi)
    return FALSE;
  spi_parent = resolve_path (spi_path, error);
  if (!spi_parent)
    return FALSE;
  gpio_node = resolve_path (gpio_path, error);
  if (!gpio_node)
    return FALSE;
  /* GPIO character devices are gpio-bus children, not class devices; there
   * need not be a class-style 'device' symlink. Their real parent is glue. */
  gpio_subsystem_path = g_build_filename (gpio_node, "subsystem", NULL);
  gpio_subsystem = resolve_path (gpio_subsystem_path, error);
  if (!gpio_subsystem)
    return FALSE;
  gpio_subsystem_name = g_path_get_basename (gpio_subsystem);
  if (!g_str_equal (gpio_subsystem_name, "gpio"))
    goto invalid;
  glue = g_path_get_dirname (gpio_node);
  glue_parent = g_path_get_dirname (glue);
  glue_driver_path = g_build_filename (glue, "driver", NULL);
  glue_driver = resolve_path (glue_driver_path, error);
  if (!glue_driver)
    return FALSE;
  glue_driver_name = g_path_get_basename (glue_driver);
  if (!g_str_equal (spi_parent, glue_parent) || !g_str_equal (glue_driver_name, "fte3600-glue"))
    goto invalid;
  hid = read_attribute (spi_acpi, "hid", error);
  if (!hid)
    return FALSE;
  if (!g_str_equal (hid, "FTE3600"))
    goto invalid;
  irq_node = resolve_path (irq_path, error);
  if (!irq_node)
    return FALSE;
  irq_parent_path = g_build_filename (irq_node, "device", NULL);
  irq_parent = resolve_path (irq_parent_path, error);
  if (!irq_parent)
    return FALSE;
  irq_subsystem_path = g_build_filename (irq_node, "subsystem", NULL);
  irq_subsystem = resolve_path (irq_subsystem_path, error);
  if (!irq_subsystem)
    return FALSE;
  irq_subsystem_name = g_path_get_basename (irq_subsystem);
  if (!g_str_equal (irq_parent, glue) || !g_str_equal (irq_subsystem_name, "uio"))
    goto invalid;
  irq_name = read_attribute (irq_node, "name", error);
  if (!irq_name)
    return FALSE;
  irq_version = read_attribute (irq_node, "version", error);
  if (!irq_version)
    return FALSE;
  maps_path = g_build_filename (irq_node, "maps", NULL);
  ports_path = g_build_filename (irq_node, "portio", NULL);
  if (!g_str_equal (irq_name, "fte3600-irq") || !g_str_equal (irq_version, "2") ||
      g_file_test (maps_path, G_FILE_TEST_EXISTS) || g_file_test (ports_path, G_FILE_TEST_EXISTS))
    goto invalid;
  if (!check_ready (glue, &generation, error))
    return FALSE;
  irq_source = read_attribute (glue, "fte3600_irq_source", error);
  if (!irq_source)
    return FALSE;
  if (!g_str_equal (irq_source, "gpio") && !g_str_equal (irq_source, "acpi"))
    goto invalid;
  if (!read_number (glue, "fte3600_glue_abi", G_MAXUINT32, &abi, error) ||
      !read_number (glue, "fte3600_ngpio", G_MAXUINT32, &lines, error) ||
      !read_number (glue, "fte3600_cs_control", 1, &cs_control, error) ||
      !read_number (glue, "fte3600_acpi_mode", G_MAXUINT32, &mode, error) ||
      !read_number (glue, "fte3600_acpi_speed_hz", G_MAXUINT32, &speed, error) ||
      !read_number (glue, "fte3600_irq_active_low", 1, &polarity, error) ||
      !check_ready (glue, &last_generation, error))
    return FALSE;
  if (abi != 2 || lines != 1 || !speed || generation != last_generation ||
      (mode & ~SPI_CS_HIGH) != SPI_MODE_0)
    goto invalid;
  *resources = (Fte3600Resources){
    .glue_path = g_steal_pointer (&glue),
    .generation = generation,
    .acpi_mode = mode,
    .acpi_speed_hz = speed,
    .irq_active_low = polarity,
    .cs_control = cs_control,
  };
  return TRUE;

invalid:
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "SPI, reset GPIO and UIO devices do not describe one supported FTE3600 ACPI companion");
  return FALSE;
}

gboolean
fpi_fte3600_resources_check (const Fte3600Resources *resources, GError **error)
{
  guint64 generation;

  g_autoptr(GError) failure = NULL;

  if (!resources->glue_path)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                           "FTE3600 transport has no active resource session");
      return FALSE;
    }
  if (!check_ready (resources->glue_path, &generation, &failure))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
                   "FTE3600 resource session is unavailable: %s", failure->message);
      return FALSE;
    }
  if (generation != resources->generation)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
                           "FTE3600 resources changed across suspend; close and reopen the device");
      return FALSE;
    }
  return TRUE;
}

gboolean
fpi_fte3600_resources_buffer_size (const gchar *sysfs_root, guint32 *size, GError **error)
{
  g_autofree gchar *path = g_build_filename (sysfs_root, "module", "spidev", "parameters", NULL);
  guint64 number;

  if (!read_number (path, "bufsiz", G_MAXINT32, &number, error))
    return FALSE;
  if (number < 64)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "The spidev buffer is too small for FTE3600 discovery");
      return FALSE;
    }
  *size = MIN (number, 32768);
  return TRUE;
}

void
fpi_fte3600_resources_clear (Fte3600Resources *resources)
{
  g_clear_pointer (&resources->glue_path, g_free);
  *resources = (Fte3600Resources){ 0 };
}
