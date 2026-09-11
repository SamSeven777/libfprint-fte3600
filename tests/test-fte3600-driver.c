/*
 * Capability-policy tests for the FTE3600 driver
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>

#include "fpi-device.h"

#ifndef FTE3600_ENABLE_PERSONAL_AUTH
#define FTE3600_ENABLE_PERSONAL_AUTH 0
#endif

GType fpi_device_fte3600_get_type (void);

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

  ok = g_spawn_command_line_sync (bin_path, &standard_output, &standard_error,
                                  &exit_status, &error);
  g_assert_no_error (error);
  g_assert_true (ok);
  g_assert_cmpint (exit_status, ==, 0);
  g_assert_nonnull (standard_output);

  /* Assert generator output actually contains the wildcard rule */
  line_start = strstr (standard_output, "ENV{MODALIAS}==\"acpi:FTE3600:*\"");
  g_assert_nonnull (line_start);

  /* Extract the pattern directly from the generator output and verify semantics */
  pattern_start = line_start + strlen ("ENV{MODALIAS}==\"");
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
  g_test_add_func ("/fte3600-driver/published-capabilities",
                   test_published_capabilities);
  g_test_add_func ("/fte3600-driver/udev-rule-pattern",
                   test_udev_rule_pattern);
  g_test_add_func ("/fte3600-driver/udev-rules-generator-output",
                   test_udev_rules_generator_output);
  return g_test_run ();
}
