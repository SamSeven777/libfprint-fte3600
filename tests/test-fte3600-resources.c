/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Synthetic sysfs trees exercise the actual stock-device association parser.
 */
#include <glib/gstdio.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "drivers/fte3600-resources.h"

typedef struct
{
  gchar *root;
  gchar *spi;
  gchar *glue;
  gchar *acpi;
  gchar *irq;
} Fixture;

static void
write_attr (const gchar *directory, const gchar *name, const gchar *value)
{
  g_autofree gchar *path = g_build_filename (directory, name, NULL);

  g_autoptr(GError) error = NULL;

  g_assert_true (g_file_set_contents (path, value, -1, &error));
  g_assert_no_error (error);
}

static void
link_path (const gchar *target, const gchar *directory, const gchar *name)
{
  g_autofree gchar *path = g_build_filename (directory, name, NULL);

  g_assert_cmpint (symlink (target, path), ==, 0);
}

static void
remove_tree (const gchar *path)
{
  if (g_file_test (path, G_FILE_TEST_IS_SYMLINK) || !g_file_test (path, G_FILE_TEST_IS_DIR))
    {
      g_assert_cmpint (g_unlink (path), ==, 0);
    }
  else
    {
      GDir *dir = g_dir_open (path, 0, NULL);
      const gchar *name;

      g_assert_nonnull (dir);
      while ((name = g_dir_read_name (dir)))
        {
          g_autofree gchar *child = g_build_filename (path, name, NULL);
          remove_tree (child);
        }
      g_dir_close (dir);
      g_assert_cmpint (g_rmdir (path), ==, 0);
    }
}

static void
fixture_setup (Fixture *f, gconstpointer unused)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *numbers = NULL, *spi_node = NULL, *gpio_node = NULL;
  g_autofree gchar *spi_driver = NULL, *glue_driver = NULL, *buffer = NULL;
  g_autofree gchar *gpio_bus = NULL, *uio_class = NULL;

  f->root = g_dir_make_tmp ("fte3600-resources-XXXXXX", &error);
  g_assert_no_error (error);
  f->spi = g_build_filename (f->root, "devices", "spi7", "spi-FTE3600:00", NULL);
  f->glue = g_build_filename (f->spi, "fte3600-glue.0", NULL);
  f->irq = g_build_filename (f->glue, "uio", "uio42", NULL);
  f->acpi = g_build_filename (f->root, "devices", "ACPI", "FTE3600:00", NULL);
  numbers = g_build_filename (f->root, "dev", "char", NULL);
  spi_node = g_build_filename (f->spi, "spidev", "spidev7.1", NULL);
  gpio_node = g_build_filename (f->glue, "gpiochip82", NULL);
  spi_driver = g_build_filename (f->root, "bus", "spi", "drivers", "spidev", NULL);
  glue_driver = g_build_filename (f->root, "bus", "platform", "drivers", "fte3600-glue", NULL);
  buffer = g_build_filename (f->root, "module", "spidev", "parameters", NULL);
  gpio_bus = g_build_filename (f->root, "bus", "gpio", NULL);
  uio_class = g_build_filename (f->root, "class", "uio", NULL);
  const gchar *directories[] = { f->glue, f->acpi, numbers, spi_node, gpio_node, spi_driver, glue_driver, buffer, gpio_bus, f->irq, uio_class };
  for (guint i = 0; i < G_N_ELEMENTS (directories); i++)
    g_assert_cmpint (g_mkdir_with_parents (directories[i], 0700), ==, 0);
  link_path (spi_node, numbers, "153:9");
  link_path (gpio_node, numbers, "254:82");
  link_path (f->irq, numbers, "247:42");
  link_path (f->glue, f->irq, "device");
  link_path (uio_class, f->irq, "subsystem");
  write_attr (f->irq, "name", "fte3600-irq\n");
  write_attr (f->irq, "version", "2\n");
  link_path (f->spi, spi_node, "device");
  /* Real gpio-bus character devices have no class-style 'device' link. */
  link_path (gpio_bus, gpio_node, "subsystem");
  link_path (f->acpi, f->spi, "firmware_node");
  link_path (spi_driver, f->spi, "driver");
  link_path (glue_driver, f->glue, "driver");
  write_attr (f->acpi, "hid", "FTE3600\n");
  write_attr (f->glue, "fte3600_glue_abi", "2\n");
  write_attr (f->glue, "fte3600_ngpio", "1\n");
  write_attr (f->glue, "fte3600_cs_control", "1\n");
  write_attr (f->glue, "fte3600_acpi_mode", "4\n");
  write_attr (f->glue, "fte3600_acpi_speed_hz", "500000\n");
  write_attr (f->glue, "fte3600_irq_active_low", "1\n");
  write_attr (f->glue, "fte3600_irq_source", "acpi\n");
  write_attr (f->glue, "fte3600_generation", "18446744073709551614\n");
  write_attr (f->glue, "fte3600_status", "ready\n");
  write_attr (buffer, "bufsiz", "32768\n");
}

static void
fixture_teardown (Fixture *f, gconstpointer unused)
{
  remove_tree (f->root);
  g_free (f->spi);
  g_free (f->glue);
  g_free (f->acpi);
  g_free (f->irq);
  g_free (f->root);
}

static gboolean
resolve (Fixture *f, Fte3600Resources *resources, GError **error)
{
  return fpi_fte3600_resources_resolve (f->root, makedev (153, 9), makedev (254, 82), makedev (247, 42), resources, error);
}

static void
test_valid (Fixture *f, gconstpointer unused)
{
  Fte3600Resources resources = { 0 };

  g_autoptr(GError) error = NULL;
  guint32 size;

  if (GPOINTER_TO_UINT (unused) == 2)
    write_attr (f->glue, "fte3600_cs_control", "0\n");
  if (GPOINTER_TO_UINT (unused) == 1)
    write_attr (f->glue, "fte3600_irq_source", "gpio\n");
  g_assert_true (resolve (f, &resources, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (resources.glue_path, ==, f->glue);
  g_assert_cmpuint (resources.acpi_mode, ==, 4);
  g_assert_cmpuint (resources.acpi_speed_hz, ==, 500000);
  g_assert_true (resources.irq_active_low);
  g_assert_cmpint (resources.cs_control, ==, GPOINTER_TO_UINT (unused) != 2);
  g_assert_cmpuint (resources.generation, ==, G_MAXUINT64 - 1);
  g_assert_true (fpi_fte3600_resources_check (&resources, &error));
  g_assert_true (fpi_fte3600_resources_buffer_size (f->root, &size, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (size, ==, 32768);
  fpi_fte3600_resources_clear (&resources);
  g_assert_null (resources.glue_path);
}

typedef struct
{
  const gchar *name;
  const gchar *attribute;
  const gchar *value;
} BadAttribute;

static void
test_bad_attribute (Fixture *f, gconstpointer data)
{
  const BadAttribute *bad = data;
  Fte3600Resources resources = { 0 };

  g_autoptr(GError) error = NULL;

  if (!bad->value)
    {
      g_autofree gchar *path = g_build_filename (f->glue, bad->attribute, NULL);
      g_assert_cmpint (g_unlink (path), ==, 0);
    }
  else
    {
      write_attr (g_str_equal (bad->attribute, "hid") ? f->acpi :
                  (g_str_equal (bad->attribute, "name") || g_str_equal (bad->attribute, "version")) ? f->irq : f->glue, bad->attribute, bad->value);
    }
  g_assert_false (resolve (f, &resources, &error));
  g_assert_nonnull (error);
  g_assert_null (resources.glue_path);
}

static void
test_wrong_association (Fixture *f, gconstpointer data)
{
  Fte3600Resources resources = { 0 };

  g_autoptr(GError) error = NULL;
  g_autofree gchar *path = NULL;
  g_autofree gchar *foreign = g_build_filename (f->root, "foreign", NULL);
  guint scenario = GPOINTER_TO_UINT (data);

  g_assert_cmpint (g_mkdir (foreign, 0700), ==, 0);
  if (scenario == 0)
    {
      path = g_build_filename (f->root, "dev", "char", "254:82", NULL);
      g_assert_cmpint (g_unlink (path), ==, 0);
      g_assert_cmpint (symlink (foreign, path), ==, 0);
    }
  else if (scenario < 3)
    {
      path = g_build_filename (scenario == 1 ? f->spi : f->glue, "driver", NULL);
      g_assert_cmpint (g_unlink (path), ==, 0);
      g_assert_cmpint (symlink (foreign, path), ==, 0);
    }
  else if (scenario == 3)
    {
      path = g_build_filename (f->irq, "device", NULL);
      g_assert_cmpint (g_unlink (path), ==, 0);
      link_path (foreign, f->irq, "device");
    }
  else
    {
      path = g_build_filename (f->irq, scenario == 4 ? "maps" : "portio", NULL);
      g_assert_cmpint (g_mkdir (path, 0700), ==, 0);
    }
  g_assert_false (resolve (f, &resources, &error));
  g_assert_nonnull (error);
  g_assert_null (resources.glue_path);
}

static void
test_epoch (Fixture *f, gconstpointer data)
{
  Fte3600Resources resources = { 0 };

  g_autoptr(GError) error = NULL;

  g_assert_true (resolve (f, &resources, &error));
  if (GPOINTER_TO_UINT (data))
    write_attr (f->glue, "fte3600_status", "suspended\n");
  else
    write_attr (f->glue, "fte3600_generation", "18446744073709551615\n");
  g_assert_false (fpi_fte3600_resources_check (&resources, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE);
  fpi_fte3600_resources_clear (&resources);
}

static void
test_buffer (Fixture *f, gconstpointer data)
{
  g_autofree gchar *path = g_build_filename (f->root, "module", "spidev", "parameters", NULL);

  g_autoptr(GError) error = NULL;
  const gchar *value = data;
  guint32 size;

  write_attr (path, "bufsiz", value);
  if (g_str_equal (value, "4096") || g_str_equal (value, "65536"))
    {
      g_assert_true (fpi_fte3600_resources_buffer_size (f->root, &size, &error));
      g_assert_no_error (error);
      g_assert_cmpuint (size, ==, g_str_equal (value, "4096") ? 4096 : 32768);
    }
  else
    {
      g_assert_false (fpi_fte3600_resources_buffer_size (f->root, &size, &error));
      g_assert_nonnull (error);
    }
}

int
main (int argc, char **argv)
{
  static const BadAttribute invalid[] = {
    { "hid-prefix", "hid", "FTE36000" }, { "foreign-hid", "hid", "ELAN7001" },
    { "uio-name", "name", "other" }, { "uio-version", "version", "1" },
    { "abi", "fte3600_glue_abi", "1" }, { "missing-lines", "fte3600_ngpio", "2" },
    { "spi-phase", "fte3600_acpi_mode", "1" }, { "spi-clock", "fte3600_acpi_mode", "2" },
    { "zero-speed", "fte3600_acpi_speed_hz", "0" }, { "overflow-speed", "fte3600_acpi_speed_hz", "4294967296" },
    { "cs-control", "fte3600_cs_control", "2" },
    { "cs-control-missing", "fte3600_cs_control", NULL },
    { "irq-source", "fte3600_irq_source", "pci" },
    { "irq-polarity", "fte3600_irq_active_low", "2" }, { "negative-epoch", "fte3600_generation", "-1" },
    { "overflow-epoch", "fte3600_generation", "18446744073709551616" },
    { "partial-number", "fte3600_generation", "7garbage" }, { "empty", "fte3600_status", "" },
    { "suspended", "fte3600_status", "suspended" }, { "unknown-state", "fte3600_status", "unknown" },
  };
  static const gchar *buffers[] = { "4096", "65536", "63", "-1", "4294967296", "4096x" };

  g_test_init (&argc, &argv, NULL);
  g_test_add ("/fte3600-resources/valid", Fixture, NULL, fixture_setup, test_valid, fixture_teardown);
  g_test_add ("/fte3600-resources/gpio-irq", Fixture, GUINT_TO_POINTER (1), fixture_setup, test_valid, fixture_teardown);
  g_test_add ("/fte3600-resources/fixed-cs", Fixture, GUINT_TO_POINTER (2), fixture_setup, test_valid, fixture_teardown);
  for (guint i = 0; i < G_N_ELEMENTS (invalid); i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/fte3600-resources/reject/%s", invalid[i].name);
      g_test_add (name, Fixture, &invalid[i], fixture_setup, test_bad_attribute, fixture_teardown);
    }
  for (guint i = 0; i < 6; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/fte3600-resources/association/%u", i);
      g_test_add (name, Fixture, GUINT_TO_POINTER (i), fixture_setup, test_wrong_association, fixture_teardown);
    }
  for (guint i = 0; i < 2; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/fte3600-resources/epoch/%u", i);
      g_test_add (name, Fixture, GUINT_TO_POINTER (i), fixture_setup, test_epoch, fixture_teardown);
    }
  for (guint i = 0; i < G_N_ELEMENTS (buffers); i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/fte3600-resources/buffer/%u", i);
      g_test_add (name, Fixture, buffers[i], fixture_setup, test_buffer, fixture_teardown);
    }
  return g_test_run ();
}
