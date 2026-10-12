/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <umockdev.h>
#include "fpi-context.h"
#include "fpi-device.h"

typedef struct { FpDevice parent;
} TestUdevDevice;
typedef FpDeviceClass TestUdevDeviceClass;
GType test_udev_device_get_type (void);
G_DEFINE_TYPE (TestUdevDevice, test_udev_device, FP_TYPE_DEVICE)

static const FpIdEntry *test_ids;

static void
test_probe (FpDevice *device)
{
  const gchar *path = fpi_device_get_udev_data (device, FPI_DEVICE_UDEV_SUBTYPE_SPIDEV);

  g_assert_cmpuint (fpi_device_get_driver_data (device), ==, 42);
  fpi_device_probe_complete (device, path ? path : "hidraw", NULL, NULL);
}

static void
test_udev_device_class_init (TestUdevDeviceClass *cls)
{
  cls->id = "test_udev";
  cls->full_name = "Synthetic udev discovery device";
  cls->type = FP_DEVICE_TYPE_UDEV;
  cls->features = FP_DEVICE_FEATURE_CAPTURE;
  cls->probe = test_probe;
}

static void
test_udev_device_init (TestUdevDevice *self)
{
}

/* This executable links the real context directly, with only the driver
 * registry replaced. Device enumeration and asynchronous probe are unchanged. */
GArray *
fpi_get_driver_types (void)
{
  GArray *types = g_array_new (TRUE, FALSE, sizeof (GType));
  GType type = test_udev_device_get_type ();

  g_autoptr(FpDeviceClass) cls = g_type_class_ref (type);

  cls->id_table = test_ids;
  g_array_append_val (types, type);
  return types;
}

static const FpIdEntry exact_ids[] = {
  { .udev_types = FPI_DEVICE_UDEV_SUBTYPE_SPIDEV, .spi_acpi_hid = "ABC0001", .driver_data = 42 },
  { .udev_types = 0 }
};

static const FpIdEntry legacy_ids[] = {
  { .udev_types = FPI_DEVICE_UDEV_SUBTYPE_SPIDEV, .spi_acpi_id = "ELAN7001", .driver_data = 42 },
  { .udev_types = 0 }
};

static const FpIdEntry hidraw_ids[] = {
  { .udev_types = FPI_DEVICE_UDEV_SUBTYPE_HIDRAW, .hid_id = { .vid = 0x1234, .pid = 0x5678 }, .driver_data = 42 },
  { .udev_types = 0 }
};

static void
add_record (UMockdevTestbed *testbed, const gchar *record)
{
  g_autoptr(GError) error = NULL;

  umockdev_testbed_add_from_string (testbed, record, &error);
  g_assert_no_error (error);
}

static void
add_spi (UMockdevTestbed *testbed,
         const gchar     *name,
         const gchar     *hid,
         guint            number,
         gboolean         device_file)
{
  g_autofree gchar *attribute = hid ? g_strdup_printf ("A: firmware_node/hid=%s\\n\n", hid) : g_strdup ("");
  g_autofree gchar *node = device_file ?
                           g_strdup_printf ("N: spidev%u.0\nE: DEVNAME=/dev/spidev%u.0\n", number, number) : g_strdup ("");
  g_autofree gchar *record = g_strdup_printf (
    "P: /devices/%s\nE: SUBSYSTEM=spi\n%s\n"
    "P: /devices/%s/spidev/spidev%u.0\nE: SUBSYSTEM=spidev\n%s\n",
    name, attribute, name, number, node);

  add_record (testbed, record);
}

static void
assert_devices (guint expected)
{
  g_autoptr(FpContext) context = fp_context_new ();
  GPtrArray *devices = fp_context_get_devices (context);

  g_assert_cmpuint (devices->len, ==, expected);
  if (expected == 2)
    g_assert_cmpstr (fp_device_get_device_id (g_ptr_array_index (devices, 0)), !=,
                     fp_device_get_device_id (g_ptr_array_index (devices, 1)));
}

static void
test_generic_spi_multiple (void)
{
  UMockdevTestbed *testbed = umockdev_testbed_new ();

  test_ids = exact_ids;
  add_spi (testbed, "spi0.0", "ABC0001", 0, TRUE);
  add_spi (testbed, "spi1.0", "ABC0001", 1, TRUE);
  assert_devices (2);
  g_object_unref (testbed);
}

static void
test_nonmatching_spi (void)
{
  UMockdevTestbed *testbed = umockdev_testbed_new ();

  test_ids = exact_ids;
  add_spi (testbed, "spi-ABC0001:00", "ABC00010", 0, TRUE);
  add_spi (testbed, "spi1.0", "XYZ0001", 1, TRUE);
  add_spi (testbed, "spi2.0", NULL, 2, TRUE);
  add_spi (testbed, "spi3.0", "ABC0001", 3, FALSE);
  add_record (testbed, "P: /devices/orphan\nE: SUBSYSTEM=spidev\nN: spidev4.0\nE: DEVNAME=/dev/spidev4.0\n\n");
  assert_devices (0);
  g_object_unref (testbed);
}

static void
test_legacy_spi (void)
{
  UMockdevTestbed *testbed = umockdev_testbed_new ();

  test_ids = legacy_ids;
  add_spi (testbed, "spi-ELAN7001:00", NULL, 0, TRUE);
  add_spi (testbed, "spi-ELAN7001:01", NULL, 1, TRUE);
  assert_devices (2);
  g_object_unref (testbed);
}

static void
test_hidraw_missing_parent (void)
{
  UMockdevTestbed *testbed = umockdev_testbed_new ();

  test_ids = hidraw_ids;
  add_record (testbed, "P: /devices/hidraw0\nE: SUBSYSTEM=hidraw\nN: hidraw0\nE: DEVNAME=/dev/hidraw0\n\n");
  assert_devices (0);
  g_object_unref (testbed);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/context/udev/generic-spi-multiple", test_generic_spi_multiple);
  g_test_add_func ("/context/udev/nonmatching-spi", test_nonmatching_spi);
  g_test_add_func ("/context/udev/legacy-spi", test_legacy_spi);
  g_test_add_func ("/context/udev/hidraw-missing-parent", test_hidraw_missing_parent);
  return g_test_run ();
}
