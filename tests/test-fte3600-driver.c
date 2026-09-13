/*
 * Capability-policy tests for the FTE3600 driver
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <sys/stat.h>

#include "fpi-device.h"
#include "drivers/fte3600.h"

#ifndef FTE3600_ENABLE_PERSONAL_AUTH
#define FTE3600_ENABLE_PERSONAL_AUTH 0
#endif

GType fpi_device_fte3600_get_type (void);

static void
test_firmware_valid (void)
{
  const gchar *path = g_getenv ("FTE3600_TEST_FIRMWARE");
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) firmware = NULL;

  if (!path || !*path)
    {
      g_test_skip ("Set FTE3600_TEST_FIRMWARE to a locally supplied FT9361 firmware file");
      return;
    }

  firmware = fte3600_load_firmware (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (firmware);
  g_assert_cmpuint (g_bytes_get_size (firmware), ==, FT9361_FIRMWARE_SIZE);
}

static void
test_firmware_invalid (gconstpointer user_data)
{
  const gsize length = GPOINTER_TO_SIZE (user_data);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) firmware = NULL;
  g_autofree gchar *directory = g_dir_make_tmp ("fte3600-firmware-XXXXXX", &error);
  g_autofree gchar *path = NULL;
  g_autofree gchar *contents = g_malloc0 (MAX (length, 1));

  g_assert_no_error (error);
  path = g_build_filename (directory, "invalid.bin", NULL);
  g_assert_true (g_file_set_contents (path, contents, length, &error));
  g_assert_no_error (error);
  firmware = fte3600_load_firmware (path, &error);
  g_assert_null (firmware);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_cmpint (g_unlink (path), ==, 0);
  g_assert_cmpint (g_rmdir (directory), ==, 0);
}

static void
test_firmware_missing (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) firmware = NULL;
  g_autofree gchar *directory = g_dir_make_tmp ("fte3600-firmware-XXXXXX", &error);
  g_autofree gchar *path = NULL;

  g_assert_no_error (error);
  path = g_build_filename (directory, "missing.bin", NULL);
  firmware = fte3600_load_firmware (path, &error);
  g_assert_null (firmware);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_assert_cmpint (g_rmdir (directory), ==, 0);
}

static void
test_firmware_fifo (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) firmware = NULL;
  g_autofree gchar *directory = g_dir_make_tmp ("fte3600-firmware-XXXXXX", &error);
  g_autofree gchar *path = NULL;

  g_assert_no_error (error);
  path = g_build_filename (directory, "fifo", NULL);
  g_assert_cmpint (mkfifo (path, 0600), ==, 0);
  firmware = fte3600_load_firmware (path, &error);
  g_assert_null (firmware);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_cmpint (g_unlink (path), ==, 0);
  g_assert_cmpint (g_rmdir (directory), ==, 0);
}

static void
test_published_capabilities (void)
{
  FpDeviceClass *klass = g_type_class_ref (fpi_device_fte3600_get_type ());

  g_assert_nonnull (klass);
  g_assert_nonnull (klass->open);
  g_assert_nonnull (klass->close);
  g_assert_nonnull (klass->capture);
  g_assert_nonnull (klass->cancel);
  g_assert_null (klass->identify);

#if FTE3600_ENABLE_PERSONAL_AUTH
  g_assert_nonnull (klass->enroll);
  g_assert_nonnull (klass->verify);
  g_assert_cmpuint (klass->nr_enroll_stages, ==, 8);
#else
  g_assert_null (klass->enroll);
  g_assert_null (klass->verify);
  g_assert_cmpuint (klass->nr_enroll_stages, ==, 0);
#endif

  g_type_class_unref (klass);
}

static void
test_udev_rule_pattern (void)
{
  FpDeviceClass *klass = g_type_class_ref (fpi_device_fte3600_get_type ());
  const FpIdEntry *entry;
  gboolean found_spi = FALSE;

  g_assert_nonnull (klass);
  g_assert_nonnull (klass->id_table);

  for (entry = klass->id_table; entry->udev_types != 0; entry++)
    {
      if (entry->udev_types & FPI_DEVICE_UDEV_SUBTYPE_SPIDEV)
        {
          g_autofree gchar *pattern = NULL;

          found_spi = TRUE;
          g_assert_cmpstr (entry->spi_acpi_id, ==, "FTE3600");

          pattern = g_strdup_printf ("acpi:%s:*", entry->spi_acpi_id);

          /* Must match single ACPI HID without _CID */
          g_assert_true (g_pattern_match_simple (pattern, "acpi:FTE3600:"));

          /* Must match repeated ACPI _CID (as seen on Medion Akoya E3224) */
          g_assert_true (g_pattern_match_simple (pattern, "acpi:FTE3600:FTE3600:"));

          /* Must match generic alternative _CID */
          g_assert_true (g_pattern_match_simple (pattern, "acpi:FTE3600:PNP0C02:"));

          /* Boundary checks: must NOT match longer HID or foreign devices */
          g_assert_false (g_pattern_match_simple (pattern, "acpi:FTE36000:"));
          g_assert_false (g_pattern_match_simple (pattern, "acpi:ELAN7001:"));
          g_assert_false (g_pattern_match_simple (pattern, "spi:FTE3600"));
        }
    }

  g_assert_true (found_spi);
  g_type_class_unref (klass);
}

static void
test_udev_rules_generator_output (void)
{
  const gchar *bin_path = g_getenv ("FPRINT_LIST_UDEV_RULES_BIN");
  g_autofree gchar *standard_output = NULL;
  g_autofree gchar *standard_error = NULL;
  gint exit_status = 0;
  GError *error = NULL;
  const gchar *line_start;
  const gchar *pattern_start;
  const gchar *pattern_end;
  g_autofree gchar *extracted_pattern = NULL;
  gboolean ok;

  if (!bin_path || !*bin_path)
    {
      g_test_skip ("FPRINT_LIST_UDEV_RULES_BIN not set; skipping generator output test");
      return;
    }

  gchar *argv[] = { (gchar *) bin_path, NULL };

  ok = g_spawn_sync (NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                     &standard_output, &standard_error, &exit_status, &error);
  g_assert_no_error (error);
  g_assert_true (ok);
  g_assert_cmpint (exit_status, ==, 0);
  g_assert_nonnull (standard_output);

  /* The same rule must require an unbound SPI device before scheduling
   * module loading, driver_override or bind writes. */
  line_start = strstr (standard_output,
                       "ACTION==\"add|change\", SUBSYSTEM==\"spi\", DRIVER==\"\", "
                       "ENV{MODALIAS}==\"acpi:FTE3600:*\"");
  g_assert_nonnull (line_start);

  /* Extract the pattern directly from the generator output and verify semantics */
  pattern_start = strstr (line_start, "ENV{MODALIAS}==\"") + strlen ("ENV{MODALIAS}==\"");
  pattern_end = strchr (pattern_start, '"');
  g_assert_nonnull (pattern_end);
  extracted_pattern = g_strndup (pattern_start, pattern_end - pattern_start);
  g_assert_cmpstr (extracted_pattern, ==, "acpi:FTE3600:*");

  /* Must match single ACPI HID without _CID */
  g_assert_true (g_pattern_match_simple (extracted_pattern, "acpi:FTE3600:"));

  /* Must match repeated ACPI _CID (as seen on Medion Akoya E3224) */
  g_assert_true (g_pattern_match_simple (extracted_pattern, "acpi:FTE3600:FTE3600:"));

  /* Must match generic alternative _CID */
  g_assert_true (g_pattern_match_simple (extracted_pattern, "acpi:FTE3600:PNP0C02:"));

  /* Boundary checks: must NOT match longer HID or foreign devices */
  g_assert_false (g_pattern_match_simple (extracted_pattern, "acpi:FTE36000:"));
  g_assert_false (g_pattern_match_simple (extracted_pattern, "acpi:ELAN7001:"));
  g_assert_false (g_pattern_match_simple (extracted_pattern, "spi:FTE3600"));
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/fte3600-driver/firmware/valid", test_firmware_valid);
  g_test_add_func ("/fte3600-driver/firmware/missing", test_firmware_missing);
  g_test_add_func ("/fte3600-driver/firmware/fifo", test_firmware_fifo);
  g_test_add_data_func ("/fte3600-driver/firmware/empty", GSIZE_TO_POINTER (0),
                        test_firmware_invalid);
  g_test_add_data_func ("/fte3600-driver/firmware/truncated",
                        GSIZE_TO_POINTER (FT9361_FIRMWARE_SIZE - 1),
                        test_firmware_invalid);
  g_test_add_data_func ("/fte3600-driver/firmware/oversized",
                        GSIZE_TO_POINTER (FT9361_FIRMWARE_SIZE + 1),
                        test_firmware_invalid);
  g_test_add_data_func ("/fte3600-driver/firmware/wrong-checksum",
                        GSIZE_TO_POINTER (FT9361_FIRMWARE_SIZE),
                        test_firmware_invalid);
  g_test_add_func ("/fte3600-driver/published-capabilities",
                   test_published_capabilities);
  g_test_add_func ("/fte3600-driver/udev-rule-pattern",
                   test_udev_rule_pattern);
  g_test_add_func ("/fte3600-driver/udev-rules-generator-output",
                   test_udev_rules_generator_output);
  return g_test_run ();
}
