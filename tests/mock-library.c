/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/prctl.h>
static GPtrArray *devices;
static const char *mode(void) { const char *s = g_getenv("MOCK_MODE"); return s ? s : "ok"; }
GObject *fp_context_new(void)
{
  g_assert_cmpstr(g_getenv("FP_DRIVERS_WHITELIST"), ==, "focaltech");
  struct rlimit limit;
  g_assert_cmpint(getrlimit(RLIMIT_CORE, &limit), ==, 0);
  g_assert_cmpuint(limit.rlim_cur, ==, 0);
  g_assert_cmpuint(limit.rlim_max, ==, 0);
  g_assert_cmpint(prctl(PR_GET_DUMPABLE), ==, 0);
  puts("PRIVATE_VENDOR_OUTPUT");
  fprintf(stderr, "PRIVATE_VENDOR_ERROR\n");
  if (!strcmp(mode(), "context")) return NULL;
  GObject *context = g_object_new(G_TYPE_OBJECT, NULL);
  devices = g_ptr_array_new_with_free_func(g_object_unref);
  if (strcmp(mode(), "none")) g_ptr_array_add(devices, g_object_new(G_TYPE_OBJECT, NULL));
  if (!strcmp(mode(), "multiple")) g_ptr_array_add(devices, g_object_new(G_TYPE_OBJECT, NULL));
  g_object_set_data_full(context, "devices", devices, (GDestroyNotify)g_ptr_array_unref);
  return context;
}
GPtrArray *fp_context_get_devices(GObject *context) { (void)context; return devices; }
const char *fp_device_get_driver(GObject *device) { (void)device; return "focaltech"; }
gboolean fp_device_open_sync(GObject *device, GCancellable *cancel, GError **error)
{
  (void)device; (void)cancel;
  if (!strcmp(mode(), "open")) {
    g_set_error_literal(error, G_IO_ERROR, 42, "PRIVATE_VENDOR_ERROR"); return FALSE;
  }
  return TRUE;
}
#ifndef MISSING_CLOSE
gboolean fp_device_close_sync(GObject *device, GCancellable *cancel, GError **error)
{
  (void)device; (void)cancel;
  if (!strcmp(mode(), "close")) {
    g_set_error_literal(error, G_IO_ERROR, 43, "PRIVATE_VENDOR_ERROR"); return FALSE;
  }
  return TRUE;
}
#endif
