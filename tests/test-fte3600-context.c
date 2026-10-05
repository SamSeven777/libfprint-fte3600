/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 FTE3600 Linux contributors
 * Real FpContext enumeration against synthetic udev metadata, with no device I/O.
 */
#include <errno.h>
#include <string.h>
#include <gudev/gudev.h>
#include "fpi-context.h"
#include "fpi-device.h"

GType fpi_device_fte3600_get_type (void);
GArray *__wrap_fpi_get_driver_types (void);
GUsbContext *__wrap_g_usb_context_new (GError **error);
GUdevClient *__wrap_g_udev_client_new (const gchar * const *subsystems);
GList *__wrap_g_udev_client_query_by_subsystem (GUdevClient *client,
                                                const gchar *subsystem);
GUdevDevice *__wrap_g_udev_device_get_parent (GUdevDevice *device);
const gchar *__wrap_g_udev_device_get_device_file (GUdevDevice *device);
const gchar *__wrap_g_udev_device_get_subsystem (GUdevDevice *device);
const gchar *__wrap_g_udev_device_get_driver (GUdevDevice *device);
const gchar *__wrap_g_udev_device_get_sysfs_path (GUdevDevice *device);
const gchar *__wrap_g_udev_device_get_sysfs_attr (GUdevDevice *device,
                                                  const gchar *name);
gboolean __wrap_g_file_get_contents (const gchar *path,
                                     gchar      **contents,
                                     gsize       *length,
                                     GError     **error);
int __wrap_open (const char *path,
                 int         flags,
                 ...) G_GNUC_NORETURN;
int __wrap_open64 (const char *path,
                   int         flags,
                   ...) G_GNUC_NORETURN;

typedef struct
{
  gchar       *path, *sysfs;
  const gchar *subsystem, *driver, *abi, *ngpio, *cs_control, *hid, *name, *version;
  GUdevDevice *parent;
} MockNode;
typedef struct
{
  MockNode *spi, *spidev, *glue, *gpio, *uio;
} Pair;
static GPtrArray *nodes;
static guint live_nodes, discovery_queries;

static MockNode *
node_data (GUdevDevice *device)
{
  MockNode *node = g_object_get_data (G_OBJECT (device), "mock-udev-node");

  g_assert_nonnull (node);
  return node;
}
static void
node_destroy (gpointer user_data)
{
  MockNode *node = user_data;

  g_clear_object (&node->parent);
  g_free (node->path);
  g_free (node->sysfs);
  g_free (node);
  live_nodes--;
}
static GUdevDevice *
node_new (const gchar *subsystem, const gchar *driver, const gchar *sysfs,
          const gchar *path, GUdevDevice *parent)
{
  GObject *object = g_object_new (G_TYPE_OBJECT, NULL);
  MockNode *node = g_new0 (MockNode, 1);

  node->subsystem = subsystem;
  node->driver = driver;
  node->path = g_strdup (path);
  node->sysfs = g_strdup (sysfs);
  node->parent = parent ? g_object_ref (parent) : NULL;
  node->abi = "2";
  node->ngpio = "1";
  node->cs_control = "1";
  node->hid = "FTE3600\n";
  node->name = "fte3600-irq";
  node->version = "2";
  live_nodes++;
  g_object_set_data_full (object, "mock-udev-node", node, node_destroy);
  g_ptr_array_add (nodes, object);
  return (GUdevDevice *) object;
}
static Pair
pair_new (guint id)
{
  g_autofree gchar *spi_sysfs = g_strdup_printf ("/sys/devices/test/spi%u.0", id);
  g_autofree gchar *spi_path = g_strdup_printf ("/dev/spidev%u.0", id);
  g_autofree gchar *gpio_path = g_strdup_printf ("/dev/gpiochip%u", id);
  g_autofree gchar *uio_path = g_strdup_printf ("/dev/uio%u", id);
  g_autofree gchar *glue_sysfs = g_build_filename (spi_sysfs, "fte3600-glue", NULL);
  g_autofree gchar *gpio_sysfs = g_build_filename (glue_sysfs, "gpiochip", NULL);
  g_autofree gchar *uio_sysfs = g_build_filename (glue_sysfs, "uio", "uio0", NULL);
  g_autofree gchar *char_sysfs = g_build_filename (spi_sysfs, "spidev", NULL);
  GUdevDevice *spi = node_new ("spi", "spidev", spi_sysfs, NULL, NULL);
  GUdevDevice *spidev = node_new ("spidev", NULL, char_sysfs, spi_path, spi);
  GUdevDevice *glue = node_new ("platform", "fte3600-glue", glue_sysfs, NULL, spi);
  GUdevDevice *gpio = node_new ("gpio", NULL, gpio_sysfs, gpio_path, glue);
  GUdevDevice *uio = node_new ("uio", NULL, uio_sysfs, uio_path, glue);

  return (Pair){ node_data (spi), node_data (spidev), node_data (glue), node_data (gpio), node_data (uio) };
}
static void
metadata_probe (FpDevice *device)
{
  fpi_device_probe_complete (device, NULL, NULL, NULL);
}
GArray *
__wrap_fpi_get_driver_types (void)
{
  GArray *drivers = g_array_new (FALSE, FALSE, sizeof (GType));
  GType type = fpi_device_fte3600_get_type ();
  FpDeviceClass *cls = g_type_class_ref (type);

  cls->probe = metadata_probe;
  g_type_class_unref (cls);
  g_array_append_val (drivers, type);
  return drivers;
}
GUsbContext *
__wrap_g_usb_context_new (GError **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "USB disabled by enumeration fixture");
  return NULL;
}
GUdevClient *
__wrap_g_udev_client_new (const gchar * const *subsystems)
{
  g_assert_null (subsystems);
  return (GUdevClient *) g_object_new (G_TYPE_OBJECT, NULL);
}
GList *
__wrap_g_udev_client_query_by_subsystem (GUdevClient *client, const gchar *subsystem)
{
  GList *result = NULL;

  g_assert_true (G_IS_OBJECT (client));
  g_assert_nonnull (nodes);
  discovery_queries++;
  for (guint i = 0; i < nodes->len; i++)
    {
      GUdevDevice *node = g_ptr_array_index (nodes, i);
      if (g_strcmp0 (node_data (node)->subsystem, subsystem) == 0)
        result = g_list_prepend (result, g_object_ref (node));
    }
  return g_list_reverse (result);
}
GUdevDevice *
__wrap_g_udev_device_get_parent (GUdevDevice *device)
{
  GUdevDevice *parent = node_data (device)->parent;

  return parent ? g_object_ref (parent) : NULL;
}
const gchar *
__wrap_g_udev_device_get_device_file (GUdevDevice *device)
{
  return node_data (device)->path;
}
const gchar *
__wrap_g_udev_device_get_subsystem (GUdevDevice *device)
{
  return node_data (device)->subsystem;
}
const gchar *
__wrap_g_udev_device_get_driver (GUdevDevice *device)
{
  return node_data (device)->driver;
}
const gchar *
__wrap_g_udev_device_get_sysfs_path (GUdevDevice *device)
{
  return node_data (device)->sysfs;
}
const gchar *
__wrap_g_udev_device_get_sysfs_attr (GUdevDevice *device, const gchar *name)
{
  if (g_str_equal (name, "fte3600_cs_control"))
    return node_data (device)->cs_control;
  if (g_str_equal (name, "fte3600_ngpio"))
    return node_data (device)->ngpio;
  if (g_str_equal (name, "name"))
    return node_data (device)->name;
  if (g_str_equal (name, "version"))
    return node_data (device)->version;
  g_assert_cmpstr (name, ==, "fte3600_glue_abi");
  return node_data (device)->abi;
}
gboolean
__wrap_g_file_get_contents (const gchar *path, gchar **contents, gsize *length, GError **error)
{
  for (guint i = 0; i < nodes->len; i++)
    {
      MockNode *node = node_data (g_ptr_array_index (nodes, i));
      g_autofree gchar *expected = g_build_filename (node->sysfs, "firmware_node", "hid", NULL);
      if (g_str_equal (path, expected) && node->hid)
        {
          *contents = g_strdup (node->hid);
          if (length)
            *length = strlen (*contents);
          return TRUE;
        }
    }
  g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_NOENT, "No firmware identity");
  return FALSE;
}
int
__wrap_open (const char *path, int flags, ...)
{
  g_error ("Enumeration unexpectedly opened %s", path);
}
int
__wrap_open64 (const char *path, int flags, ...)
{
  __wrap_open (path, flags);
}
static void
fixture_begin (void)
{
  g_assert_null (nodes);
  g_assert_cmpuint (live_nodes, ==, 0);
  nodes = g_ptr_array_new_with_free_func (g_object_unref);
  discovery_queries = 0;
}
static void
fixture_end (void)
{
  g_clear_pointer (&nodes, g_ptr_array_unref);
  g_assert_cmpuint (live_nodes, ==, 0);
}
static FpContext *
context_new (void)
{
  FpContext *context;

  g_test_expect_message ("libfprint-context", G_LOG_LEVEL_MESSAGE, "Could not initialise USB Subsystem: USB disabled*");
  context = fp_context_new ();
  g_test_assert_expected_messages ();
  return context;
}
static void
test_rejected_pair (gconstpointer data)
{
  guint fault = GPOINTER_TO_UINT (data);

  g_autoptr(FpContext) context = NULL;
  Pair pair;
  fixture_begin ();
  pair = pair_new (1);
  switch (fault)
    {
    case 0: g_clear_object (&pair.spidev->parent);
      break;

    case 1: pair.spi->subsystem = "platform";
      break;

    case 2: pair.spi->driver = "fte3600";
      break;

    case 3: pair.spi->hid = "OTHER3600";
      break;

    case 4: pair.spi->hid = "FTE36000";
      break;

    case 5: pair.spi->hid = NULL;
      break;

    case 6: pair.glue->abi = "1";
      break;

    case 7: pair.glue->abi = NULL;
      break;

    case 8: pair.glue->driver = "another-glue";
      break;

    case 9: pair.glue->subsystem = "acpi";
      break;

    case 10: pair.glue->ngpio = "100";
      break;

    case 11: g_clear_object (&pair.gpio->parent);
      break;

    case 12: g_clear_object (&pair.glue->parent);
      break;

    case 13: g_clear_pointer (&pair.spidev->path, g_free);
      break;

    case 14: g_free (pair.gpio->path);
      pair.gpio->path = g_strdup ("gpiochip1");
      break;

    case 15: node_new ("gpio", NULL, "/sys/duplicate", "/dev/gpiochip99", pair.gpio->parent);
      break;

    case 16:
      g_clear_object (&pair.glue->parent);
      pair.glue->parent = g_object_ref (node_new ("spi", "spidev", "/sys/different-spi", NULL, NULL));
      break;

    case 17: g_clear_object (&pair.uio->parent);
      break;

    case 18: pair.uio->name = "unrelated-uio";
      break;

    case 19: pair.uio->version = "1";
      break;

    case 20: g_clear_pointer (&pair.uio->path, g_free);
      break;

    case 21: g_free (pair.uio->path);
      pair.uio->path = g_strdup ("uio1");
      break;

    case 22: node_new ("uio", NULL, "/sys/duplicate-uio", "/dev/uio99", pair.uio->parent);
      break;

    case 23:
      g_clear_object (&pair.uio->parent);
      pair.uio->parent = g_object_ref (node_new ("platform", "fte3600-glue", "/sys/other-glue", NULL, pair.glue->parent));
      break;

    case 24: pair.uio->subsystem = "misc";
      break;

    case 25: pair.glue->cs_control = NULL;
      break;

    case 26: pair.glue->cs_control = "2";
      break;

    default: g_assert_not_reached ();
    }
  context = context_new ();
  g_assert_cmpuint (fp_context_get_devices (context)->len, ==, 0);
  g_assert_cmpuint (discovery_queries, ==, 4);
  g_clear_object (&context);
  fixture_end ();
}
static void
test_multiple_devices (void)
{
  g_autoptr(FpContext) context = NULL;
  GPtrArray *devices;
  guint found = 0;
  fixture_begin ();
  pair_new (2);
  pair_new (5).glue->cs_control = "0";
  pair_new (7).spi->hid = "FTE36000";
  context = context_new ();
  devices = fp_context_get_devices (context);
  g_assert_cmpuint (devices->len, ==, 2);
  for (guint i = 0; i < devices->len; i++)
    {
      FpDevice *device = g_ptr_array_index (devices, i);
      const gchar *spi = fpi_device_get_udev_data (device, FPI_DEVICE_UDEV_SUBTYPE_FTE3600);
      const gchar *gpio = fpi_device_get_udev_data (device, FPI_DEVICE_UDEV_SUBTYPE_GPIO);
      const gchar *uio = fpi_device_get_udev_data (device, FPI_DEVICE_UDEV_SUBTYPE_UIO);
      g_assert_false (fp_device_is_open (device));
      if (g_str_equal (spi, "/dev/spidev2.0"))
        {
          g_assert_cmpstr (gpio, ==, "/dev/gpiochip2");
          g_assert_cmpstr (uio, ==, "/dev/uio2");
          g_assert_cmpuint (found & 1, ==, 0);
          found |= 1;
        }
      else
        {
          g_assert_cmpstr (spi, ==, "/dev/spidev5.0");
          g_assert_cmpstr (gpio, ==, "/dev/gpiochip5");
          g_assert_cmpstr (uio, ==, "/dev/uio5");
          g_assert_cmpuint (found & 2, ==, 0);
          found |= 2;
        }
    }
  g_assert_cmpuint (found, ==, 3);
  fp_context_enumerate (context);
  g_assert_true (devices == fp_context_get_devices (context));
  g_assert_cmpuint (discovery_queries, ==, 4);
  g_clear_object (&context);
  fixture_end ();
}
int
main (int argc, char **argv)
{
  const gchar *faults[] = {
    "missing-spi-parent", "wrong-bus", "wrong-driver", "wrong-hid", "hid-prefix", "missing-hid",
    "wrong-abi", "missing-abi", "wrong-glue-driver", "wrong-glue-bus", "whole-gpio-controller",
    "missing-gpio-parent", "missing-glue-parent", "missing-node", "relative-node", "ambiguous-gpio", "cross-device",
    "missing-uio-parent", "wrong-uio-name", "wrong-uio-version", "missing-uio-node", "relative-uio-node",
    "ambiguous-uio", "cross-glue-uio", "wrong-uio-subsystem",
    "missing-cs-control", "invalid-cs-control",
  };

  g_setenv ("FP_DRIVERS_ALLOWLIST", "fte3600", TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/fte3600/context/multiple", test_multiple_devices);
  for (guint i = 0; i < G_N_ELEMENTS (faults); i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/fte3600/context/reject/%s", faults[i]);
      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_rejected_pair);
    }
  return g_test_run ();
}
