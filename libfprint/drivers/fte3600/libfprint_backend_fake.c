/* SPDX-License-Identifier: LGPL-2.1-or-later
 * TEST ONLY. Linked instead of production resource IDs and Linux backend.
 * Commands are mathematical scenarios, not a production emulation switch. */
#include "fte3600.h"
#include "fte3600_linux_runtime.h"
#include "auth_test_image.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const FpIdEntry fte3600_driver_ids[] = {
  { .virtual_envvar = "FP_FTE3600_TEST_CONTROL" },
  { .virtual_envvar = NULL }
};
const FpDeviceType fte3600_driver_device_type = FP_DEVICE_TYPE_VIRTUAL;
const gchar *
fte3600_driver_resource (FpDevice *device)
{
  return fpi_device_get_virtual_env (device);
}
gchar *
fte3600_driver_probe_id (FpDevice *device, GError **error)
{
  (void) device;
  (void) error;
  return g_strdup ("fte3600-test-physical-1");
}

struct fte3600_linux_runtime
{
  gchar                       *control;
  gchar                       *last_mode;
  guint                        sequence, sample;
  gboolean                     failed;
  struct fte3600_device_status status;
};

static void
trace (const gchar *control_path, const gchar *event)
{
  g_autofree gchar *path = g_strconcat (control_path, ".trace", NULL);
  FILE *stream = g_fopen (path, "a");

  g_assert_nonnull (stream);
  g_assert_cmpint (fprintf (stream, "%s\n", event), >, 0);
  g_assert_cmpint (fclose (stream), ==, 0);
}

static enum fte3600_linux_runtime_error
failure (struct fte3600_linux_runtime_diagnostic *diag,
         enum fte3600_linux_runtime_error         code)
{
  memset (diag, 0, sizeof (*diag));
  diag->code = code;
  return code;
}
static gchar *
read_mode (const gchar *path)
{
  gchar *mode = NULL;

  if (!g_file_get_contents (path, &mode, NULL, NULL))
    return g_strdup ("open-fail");
  return g_strstrip (mode);
}
enum fte3600_linux_runtime_error
fte3600_linux_runtime_open (const struct fte3600_linux_runtime_config *config,
                            GCancellable *cancel, struct fte3600_linux_runtime **out,
                            struct fte3600_linux_runtime_diagnostic *diag)
{
  g_autofree gchar *mode = read_mode (config->spidev_path);

  trace (config->spidev_path, "open");
  *out = NULL;
  if (g_cancellable_is_cancelled (cancel))
    return failure (diag, FTE3600_RUNTIME_CANCELLED);
  if (g_str_equal (mode, "open-fail"))
    return failure (diag, FTE3600_RUNTIME_IO);
  struct fte3600_linux_runtime *runtime = g_new0 (struct fte3600_linux_runtime, 1);
  runtime->control = g_strdup (config->spidev_path);
  runtime->status = (struct fte3600_device_status){
    .state = FTE3600_DEVICE_IDLE, .model = FTE3600_MODEL_FT9368,
    .chip_id = 0x9368, .raw_id = 0x9368, .width = 64, .height = 80,
    .startup_complete = 1, .calibration_complete = 1,
  };
  if (g_str_equal (mode, "unsupported"))
    runtime->status.width = 3;
  if (g_str_equal (mode, "fw9369"))
    {
      runtime->status.model = FTE3600_MODEL_FW9369;
      runtime->status.chip_id = 0x9369;
      runtime->status.raw_id = 0x9362;
    }
  if (g_str_equal (mode, "release-unsupported"))
    {
      runtime->status.model = FTE3600_MODEL_FT9361;
      runtime->status.chip_id = runtime->status.raw_id = 0x9361;
    }
  if (g_str_equal (mode, "other-profile"))
    {
      runtime->status.width = runtime->status.height = 88;
      runtime->status.model = FTE3600_MODEL_FT9338;
      runtime->status.chip_id = runtime->status.raw_id = 0x9338;
    }
  *out = runtime;
  return failure (diag, FTE3600_RUNTIME_OK);
}
enum fte3600_linux_runtime_error
fte3600_linux_runtime_get_status (const struct fte3600_linux_runtime *runtime,
                                  struct fte3600_device_status       *status)
{
  *status = runtime->status;
  return FTE3600_RUNTIME_OK;
}

enum fte3600_linux_runtime_error
fte3600_linux_runtime_capture (struct fte3600_linux_runtime *runtime,
                               GCancellable *cancel, uint32_t timeout,
                               struct fte3600_linux_runtime_frame *frame,
                               struct fte3600_linux_runtime_diagnostic *diag)
{
  g_autofree gchar *mode = read_mode (runtime->control);

  (void) timeout;
  trace (runtime->control, "capture-begin");
  if (g_str_equal (mode, "block"))
    while (!g_cancellable_is_cancelled (cancel))
      g_usleep (1000);
  if (g_cancellable_is_cancelled (cancel))
    {
      runtime->failed = TRUE;
      return failure (diag, FTE3600_RUNTIME_CANCELLED);
    }
  if (g_str_equal (mode, "capture-fail") || g_str_equal (mode, "sticky-fail"))
    runtime->failed = TRUE;
  if (runtime->failed)
    return failure (diag, FTE3600_RUNTIME_IO);
  if (g_strcmp0 (runtime->last_mode, mode))
    {
      g_free (runtime->last_mode);
      runtime->last_mode = g_strdup (mode);
      runtime->sample = 0;
    }
  frame->stride = runtime->status.width;
  frame->length = frame->stride * runtime->status.height;
  frame->pixels = g_malloc0 (frame->length);
  guint sample = 8;
  if (g_str_has_prefix (mode, "enroll"))
    sample = runtime->sample++ % 8;
  if (g_str_equal (mode, "enroll-retry") && runtime->sample > 1)
    sample = (runtime->sample - 2) % 8;
  if (g_str_equal (mode, "duplicate"))
    sample = 0;
  if (!g_str_equal (mode, "blank"))
    fte3600_auth_test_image (frame->pixels, runtime->status.width, runtime->status.height,
                             g_str_equal (mode, "negative") || g_str_equal (mode, "enroll-other") ?
                             FTE3600_AUTH_TEST_OTHER_SEED : FTE3600_AUTH_TEST_SEED, sample);
  runtime->status.image_sequence = ++runtime->sequence;
  frame->status = runtime->status;
  trace (runtime->control, "capture-ok");
  return failure (diag, FTE3600_RUNTIME_OK);
}
enum fte3600_linux_runtime_error
fte3600_linux_runtime_wait_release (struct fte3600_linux_runtime *runtime,
                                    GCancellable *cancel, uint32_t timeout,
                                    struct fte3600_linux_runtime_diagnostic *diag)
{
  g_autofree gchar *mode = read_mode (runtime->control);

  (void) timeout;
  trace (runtime->control, "release-begin");
  if (runtime->status.model != FTE3600_MODEL_FT9368 &&
      runtime->status.model != FTE3600_MODEL_FW9369)
    {
      trace (runtime->control, "release-unsupported");
      return failure (diag, FTE3600_RUNTIME_RELEASE_UNSUPPORTED);
    }
  if (g_str_equal (mode, "release-block"))
    while (!g_cancellable_is_cancelled (cancel))
      g_usleep (1000);
  if (g_cancellable_is_cancelled (cancel))
    {
      runtime->failed = TRUE;
      return failure (diag, FTE3600_RUNTIME_CANCELLED);
    }
  if (g_str_equal (mode, "release-fail"))
    runtime->failed = TRUE;
  if (runtime->failed)
    return failure (diag, FTE3600_RUNTIME_IO);
  trace (runtime->control, "release-ok");
  return failure (diag, FTE3600_RUNTIME_OK);
}
void
fte3600_linux_runtime_frame_clear (struct fte3600_linux_runtime_frame *frame)
{
  free (frame->pixels);
  memset (frame, 0, sizeof (*frame));
}
enum fte3600_linux_runtime_error
fte3600_linux_runtime_close (struct fte3600_linux_runtime            *runtime,
                             struct fte3600_linux_runtime_diagnostic *diag)
{
  enum fte3600_linux_runtime_error code = FTE3600_RUNTIME_OK;

  if (runtime)
    {
      g_autofree gchar *mode = read_mode (runtime->control);
      trace (runtime->control, "close");
      if (g_str_equal (mode, "close-fail"))
        code = FTE3600_RUNTIME_IO;
      g_free (runtime->control);
      g_free (runtime->last_mode);
      g_free (runtime);
    }
  return failure (diag, code);
}
const char *
fte3600_linux_runtime_error_string (enum fte3600_linux_runtime_error code)
{
  (void) code;
  return "synthetic backend failure (TEST ONLY)";
}
