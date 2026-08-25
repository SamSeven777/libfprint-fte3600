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

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/fte3600-driver/published-capabilities",
                   test_published_capabilities);
  return g_test_run ();
}
