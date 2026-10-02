/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Public API open/close only. No SPI/GPIO commands, enrollment or matching.
 * Vendor stdout/stderr is suppressed here; events use a private duplicate fd.
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <unistd.h>
typedef struct _FpContext FpContext;
typedef struct _FpDevice FpDevice;
static FpContext *(*context_new)(void);
static GPtrArray *(*context_devices)(FpContext *);
static const gchar *(*device_driver)(FpDevice *);
static gboolean (*device_open)(FpDevice *, GCancellable *, GError **);
static gboolean (*device_close)(FpDevice *, GCancellable *, GError **);
static int event_fd = -1;
static void event(const char *stage) { dprintf(event_fd, "MEDION_BASELINE:%s\n", stage); }
static gboolean cancel(gpointer data)
{
  g_cancellable_cancel(G_CANCELLABLE(data));
  return G_SOURCE_CONTINUE;
}
static void report_error(const char *stage, GError **error)
{
  dprintf(event_fd, "MEDION_BASELINE:%s code=%d\n", stage, *error ? (*error)->code : 0);
  g_clear_error(error);
}
#define LOAD(target, symbol) do { \
  *(void **)(&(target)) = dlsym(library, (symbol)); \
  if (!(target)) { event("ABI_MISSING_SYMBOL"); return 1; } \
} while (0)
int main(int argc, char **argv)
{
  const struct rlimit no_core = {0, 0};
  if (setrlimit(RLIMIT_CORE, &no_core) < 0 || prctl(PR_SET_DUMPABLE, 0) < 0) return 1;
  event_fd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
  int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
  if (event_fd < 0 || null_fd < 0 || dup2(null_fd, 1) < 0 || dup2(null_fd, 2) < 0)
    return 1;
  close(null_fd);
  FpDevice *device = NULL;
  GError *error = NULL;
  gboolean opened = FALSE;
  int result = 1;
  if (argc != 3 || argv[1][0] != '/' ||
      (strcmp(argv[2], "--check-abi") && strcmp(argv[2], "--open")))
    return 64;
  g_setenv("FP_DRIVERS_WHITELIST", "focaltech", TRUE);
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    const char *why = dlerror();
    const struct { const char *soname, *stage; } dependencies[] = {
      {"libgusb.so.2:", "MISSING_LIBGUSB"}, {"libgudev-1.0.so.0:", "MISSING_LIBGUDEV"},
      {"libpixman-1.so.0:", "MISSING_PIXMAN"}, {"libnss3.so:", "MISSING_NSS"},
      {"libnspr4.so:", "MISSING_NSPR"},
    };
    for (guint i = 0; why && i < G_N_ELEMENTS(dependencies); i++)
      if (g_str_has_prefix(why, dependencies[i].soname)) event(dependencies[i].stage);
    event("ABI_LOAD_FAILED"); return 1;
  }
  LOAD(context_new, "fp_context_new");
  LOAD(context_devices, "fp_context_get_devices");
  LOAD(device_driver, "fp_device_get_driver");
  LOAD(device_open, "fp_device_open_sync");
  LOAD(device_close, "fp_device_close_sync");
  event("ABI_OK");
  if (!strcmp(argv[2], "--check-abi")) return 0;
  GCancellable *cancellable = g_cancellable_new();
  guint sigint = g_unix_signal_add(SIGINT, cancel, cancellable);
  guint sigterm = g_unix_signal_add(SIGTERM, cancel, cancellable);
  FpContext *context = context_new();
  if (!context) { event("CONTEXT_FAILED"); goto out; }
  GPtrArray *devices = context_devices(context);
  if (!devices) { event("ENUMERATION_FAILED"); goto out; }
  for (guint i = 0; i < devices->len; i++) {
    FpDevice *candidate = g_ptr_array_index(devices, i);
    if (g_strcmp0(device_driver(candidate), "focaltech")) continue;
    if (device) { event("MULTIPLE_DEVICES"); goto out; }
    device = candidate;
  }
  if (!device) { event("NO_DEVICE"); goto out; }
  event("OPEN_START");
  if (!device_open(device, cancellable, &error)) {
    report_error("OPEN_FAILED", &error); goto out;
  }
  opened = TRUE;
  event("OPEN_OK");
  result = 0;
out:
  if (opened) {
    if (!device_close(device, NULL, &error)) {
      report_error("CLOSE_FAILED", &error); result = 1;
    } else event("CLOSE_OK");
  }
  if (context) g_object_unref(context);
  g_source_remove(sigint);
  g_source_remove(sigterm);
  g_object_unref(cancellable);
  /* Registered GTypes retain code pointers until process exit: no dlclose. */
  return result;
}
