/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Test CLI reporting against runtime outcomes; no hardware I/O. */
#define main probe_main
#include "../tools/fte3600-probe.c"
#undef main

static enum { READY, FIRMWARE, UNCONFIRMED, CLOSE_ERROR, STATUS_ERROR } outcome;
static unsigned opened, closed;

enum fte3600_linux_runtime_error
__wrap_fte3600_linux_runtime_open (const struct fte3600_linux_runtime_config *c,
                                 GCancellable *cancel,
                                 struct fte3600_linux_runtime **out,
                                 struct fte3600_linux_runtime_diagnostic *d)
{
  (void) cancel;
  g_assert_cmpstr (c->spidev_path, ==, "/dev/spidev2.0");
  ++opened;
  if (outcome == FIRMWARE || outcome == UNCONFIRMED)
    {
      d->has_session_status = 1;
      d->model = FTE3600_MODEL_FT9361;
      d->code = FTE3600_RUNTIME_SESSION;
      d->device_error = FTE3600_DEVICE_UNSUPPORTED;
      if (outcome == FIRMWARE)
        {
          d->code = FTE3600_RUNTIME_FIRMWARE;
          d->device_error = FTE3600_DEVICE_MISSING_FIRMWARE;
          strcpy (d->firmware_file, "ft9361.bin");
        }
      else
        d->component_error = FTE3600_FACTORY_INTEGRATION_UNCONFIRMED_IDENTITY;
      return d->code;
    }
  *out = (struct fte3600_linux_runtime *) &opened;
  return FTE3600_RUNTIME_OK;
}

enum fte3600_linux_runtime_error
__wrap_fte3600_linux_runtime_get_status (const struct fte3600_linux_runtime *r,
                                       struct fte3600_device_status *s)
{
  g_assert_nonnull (r);
  if (outcome == STATUS_ERROR)
    return FTE3600_RUNTIME_SESSION;
  s->model = FTE3600_MODEL_FW9369;
  s->raw_id = 0x9362;
  s->chip_id = 0x9369;
  s->startup_complete = s->calibration_complete = 1;
  return FTE3600_RUNTIME_OK;
}

enum fte3600_linux_runtime_error
__wrap_fte3600_linux_runtime_close (struct fte3600_linux_runtime *r,
                                  struct fte3600_linux_runtime_diagnostic *d)
{
  g_assert_nonnull (r);
  ++closed;
  d->code = outcome == CLOSE_ERROR ? FTE3600_RUNTIME_IO : FTE3600_RUNTIME_OK;
  return d->code;
}

static void
run_probe (void)
{
  char program[] = "fte3600-probe", path[] = "/dev/spidev2.0";
  char *args[] = {program, path, NULL};
  int result = probe_main (2, args);
  g_assert_cmpint (result, ==, outcome == READY ? 0 : 1);
  g_assert_cmpuint (opened, ==, 1);
  g_assert_cmpuint (closed, ==, outcome == FIRMWARE || outcome == UNCONFIRMED ? 0 : 1);
}

static void
reporting (gconstpointer data)
{
  outcome = GPOINTER_TO_UINT (data);
  if (g_test_subprocess ())
    {
      run_probe ();
      return;
    }
  const char *patterns[] = {
    "*Driver profile: FW9369*Raw ID: 0x9362*Chip ID: 0x9369*Startup: OK*",
    "*Startup: FAILED*Driver profile: FT9361*Firmware file: ft9361.bin*",
    "*Startup: FAILED*Identity unconfirmed*",
    "*Startup: OK*Cleanup: FAILED*",
    "*Running driver startup*"
  };
  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_passed ();
  g_test_trap_assert_stdout (patterns[outcome]);
  if (outcome == FIRMWARE || outcome == UNCONFIRMED || outcome == STATUS_ERROR)
    g_test_trap_assert_stdout_unmatched ("*Startup: OK*");
}

static void
arguments (void)
{
  char program[] = "fte3600-probe", option[] = "--help", path[] = "spi-FTE3600:01";
  char *help[] = {program, option, NULL};
  char *bad[] = {program, path, NULL};
  g_assert_cmpint (probe_main (2, help), ==, 0);
  g_assert_cmpint (probe_main (2, bad), ==, 2);
  g_assert_cmpuint (opened, ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_data_func ("/probe/ready", GUINT_TO_POINTER (READY), reporting);
  g_test_add_data_func ("/probe/firmware", GUINT_TO_POINTER (FIRMWARE), reporting);
  g_test_add_data_func ("/probe/unconfirmed", GUINT_TO_POINTER (UNCONFIRMED), reporting);
  g_test_add_data_func ("/probe/close-error", GUINT_TO_POINTER (CLOSE_ERROR), reporting);
  g_test_add_data_func ("/probe/status-error", GUINT_TO_POINTER (STATUS_ERROR), reporting);
  g_test_add_func ("/probe/arguments", arguments);
  return g_test_run ();
}
