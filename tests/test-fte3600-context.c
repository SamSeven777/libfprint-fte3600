/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 FTE3600 Linux contributors
 *
 * Exercise the real FpContext metadata enumeration. USB/udev discovery and
 * the sensor probe are replaced; the separate lifecycle suite covers probe I/O.
 */
#include <errno.h>
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
const gchar *__wrap_g_udev_device_get_sysfs_attr (GUdevDevice *device,
                                                  const gchar *name);
int __wrap_open (const char *path,
                 int         flags,
                 ...);
int __wrap_open64 (const char *path,
                   int         flags,
                   ...);

typedef struct
{
  const gchar *path;
  const gchar *abi;
  const gchar *subsystem;
  const gchar *driver;
  gboolean     parent_present;
} NodeSpec;

typedef struct
{
  NodeSpec     spec;
  GUdevDevice *parent;
} MockNode;

static GPtrArray *fixture_nodes;
static guint live_nodes;
static guint discovery_queries;

static void
node_destroy (gpointer user_data)
{
  MockNode *node = user_data;

  g_clear_object (&node->parent);
  g_free (node);
  live_nodes--;
}

static GUdevDevice *
node_new (const NodeSpec *spec)
{
  GObject *object = g_object_new (G_TYPE_OBJECT, NULL);
  MockNode *node = g_new0 (MockNode, 1);

  node->spec = *spec;
  if (spec->parent_present)
    {
      NodeSpec parent_spec = *spec;

      parent_spec.parent_present = FALSE;
      node->parent = node_new (&parent_spec);
    }
  live_nodes++;
  g_object_set_data_full (object, "mock-udev-node", node, node_destroy);
  return (GUdevDevice *) object;
}

static MockNode *
node_data (GUdevDevice *device)
{
  MockNode *node = g_object_get_data (G_OBJECT (device), "mock-udev-node");

  g_assert_nonnull (node);
  return node;
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
  FpDeviceClass *device_class = g_type_class_ref (type);

  device_class->probe = metadata_probe;
  g_type_class_unref (device_class);
  g_array_append_val (drivers, type);
  return drivers;
}

GUsbContext *
__wrap_g_usb_context_new (GError **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "USB disabled by bridge enumeration fixture");
  return NULL;
}

GUdevClient *
__wrap_g_udev_client_new (const gchar * const *subsystems)
{
  g_assert_null (subsystems);
  return (GUdevClient *) g_object_new (G_TYPE_OBJECT, NULL);
}

GList *
__wrap_g_udev_client_query_by_subsystem (GUdevClient *client,
                                         const gchar *subsystem)
{
  GList *result = NULL;

  g_assert_true (G_IS_OBJECT (client));
  g_assert_nonnull (fixture_nodes);
  discovery_queries++;
  if (g_str_equal (subsystem, "misc"))
    for (guint i = 0; i < fixture_nodes->len; i++)
      result = g_list_prepend (result, g_object_ref (g_ptr_array_index (fixture_nodes, i)));
  else
    g_assert_true (g_str_equal (subsystem, "spidev") ||
                   g_str_equal (subsystem, "hidraw"));
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
  return node_data (device)->spec.path;
}

const gchar *
__wrap_g_udev_device_get_subsystem (GUdevDevice *device)
{
  return node_data (device)->spec.subsystem;
}

const gchar *
__wrap_g_udev_device_get_driver (GUdevDevice *device)
{
  return node_data (device)->spec.driver;
}

const gchar *
__wrap_g_udev_device_get_sysfs_attr (GUdevDevice *device,
                                     const gchar *name)
{
  g_assert_cmpstr (name, ==, "fte3600_abi");
  return node_data (device)->spec.abi;
}

int
__wrap_open (const char *path, int flags, ...)
{
  g_test_message ("Enumeration unexpectedly opened %s", path);
  g_test_fail ();
  errno = EPERM;
  return -1;
}

int
__wrap_open64 (const char *path, int flags, ...)
{
  return __wrap_open (path, flags);
}

static void
fixture_begin (void)
{
  g_assert_null (fixture_nodes);
  g_assert_cmpuint (live_nodes, ==, 0);
  fixture_nodes = g_ptr_array_new_with_free_func (g_object_unref);
  discovery_queries = 0;
}

static void
fixture_end (void)
{
  g_clear_pointer (&fixture_nodes, g_ptr_array_unref);
  /* Query results and temporary parent references must all be released. */
  g_assert_cmpuint (live_nodes, ==, 0);
}

static FpContext *
context_new (void)
{
  FpContext *context;

  g_test_expect_message ("libfprint-context", G_LOG_LEVEL_MESSAGE,
                         "Could not initialise USB Subsystem: USB disabled*");
  context = fp_context_new ();
  g_test_assert_expected_messages ();
  return context;
}

static void
test_rejected_node (gconstpointer data)
{
  g_autoptr(FpContext) context = NULL;

  fixture_begin ();
  g_ptr_array_add (fixture_nodes, node_new (data));
  context = context_new ();
  g_assert_cmpuint (fp_context_get_devices (context)->len, ==, 0);
  g_assert_cmpuint (discovery_queries, ==, 3);
  g_clear_object (&context);
  fixture_end ();
}

static void
device_added (FpContext *context, FpDevice *device, guint *count)
{
  g_assert_true (FP_IS_CONTEXT (context));
  g_assert_true (FP_IS_DEVICE (device));
  (*count)++;
}

static void
test_multiple_devices (void)
{
  const NodeSpec nodes[] = {
    { "/dev/fte3600-spi1.0", "1", "spi", "fte3600", TRUE },
    { "/dev/unrelated", "1", "platform", "fte3600", TRUE },
    { "/dev/fte3600-spi5.2", "1", "spi", "fte3600", TRUE },
    { "/dev/fte3600-bad", "2", "spi", "fte3600", TRUE },
  };
  const gchar *expected[] = { nodes[0].path, nodes[2].path };

  g_autoptr(FpContext) context = NULL;
  GPtrArray *devices;
  guint added = 0;
  guint found = 0;

  fixture_begin ();
  for (guint i = 0; i < G_N_ELEMENTS (nodes); i++)
    g_ptr_array_add (fixture_nodes, node_new (&nodes[i]));
  context = context_new ();
  g_signal_connect (context, "device-added", G_CALLBACK (device_added), &added);
  fp_context_enumerate (context);
  devices = fp_context_get_devices (context);
  g_assert_cmpuint (devices->len, ==, G_N_ELEMENTS (expected));
  g_assert_cmpuint (added, ==, G_N_ELEMENTS (expected));
  for (guint i = 0; i < devices->len; i++)
    {
      FpDevice *device = g_ptr_array_index (devices, i);
      const gchar *path = fpi_device_get_udev_data (device, FPI_DEVICE_UDEV_SUBTYPE_FTE3600);

      g_assert_cmpstr (fp_device_get_driver (device), ==, "fte3600");
      g_assert_false (fp_device_is_open (device));
      for (guint j = 0; j < G_N_ELEMENTS (expected); j++)
        if (g_strcmp0 (path, expected[j]) == 0)
          {
            g_assert_cmpuint (found & (1u << j), ==, 0);
            found |= 1u << j;
          }
    }
  g_assert_cmpuint (found, ==, 3);
  /* Public calls may repeat, but a single context must enumerate only once. */
  fp_context_enumerate (context);
  g_assert_true (fp_context_get_devices (context) == devices);
  g_assert_cmpuint (discovery_queries, ==, 3);
  g_assert_cmpuint (added, ==, 2);
  g_clear_object (&context);
  fixture_end ();
}

int
main (int argc, char **argv)
{
  static const struct
  {
    const gchar *name;
    NodeSpec     spec;
  } invalid[] = {
    { "missing-parent", { "/dev/fte3600-test", "1", "spi", "fte3600", FALSE } },
    { "wrong-parent", { "/dev/fte3600-test", "1", "platform", "fte3600", TRUE } },
    { "missing-subsystem", { "/dev/fte3600-test", "1", NULL, "fte3600", TRUE } },
    { "wrong-driver", { "/dev/fte3600-test", "1", "spi", "spidev", TRUE } },
    { "missing-driver", { "/dev/fte3600-test", "1", "spi", NULL, TRUE } },
    { "wrong-abi", { "/dev/fte3600-test", "2", "spi", "fte3600", TRUE } },
    { "missing-abi", { "/dev/fte3600-test", NULL, "spi", "fte3600", TRUE } },
    { "empty-abi", { "/dev/fte3600-test", "", "spi", "fte3600", TRUE } },
    { "missing-path", { NULL, "1", "spi", "fte3600", TRUE } },
    { "empty-path", { "", "1", "spi", "fte3600", TRUE } },
    { "relative-path", { "fte3600-test", "1", "spi", "fte3600", TRUE } },
  };

  g_setenv ("FP_DRIVERS_ALLOWLIST", "fte3600", TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/fte3600/context/multiple", test_multiple_devices);
  for (guint i = 0; i < G_N_ELEMENTS (invalid); i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/fte3600/context/reject/%s", invalid[i].name);

      g_test_add_data_func (name, &invalid[i].spec, test_rejected_node);
    }
  return g_test_run ();
}
