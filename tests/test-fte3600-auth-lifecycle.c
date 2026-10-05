/*
 * Public FTE3600 authentication lifecycle with a simulated capture boundary.
 * SPDX-FileCopyrightText: 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Discovery and the chip backend are replaced; the real FpDevice API, driver,
 * state machines, worker ownership, extractor and template code run normally.
 * No hardware, firmware payload or biometric data is used.
 */
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "drivers/fte3600-private.h"
#include "drivers/fte3600-match-profile.h"
#include "drivers/fte3600-template.h"
#include "drivers/fte3600-ipa.h"

#define MOCK_FD 9017
#define MOCK_PATH "/mock/fte3600-auth"

static gboolean shutdown_fixture;

#define WRAPPED(name) __typeof__ (name) __wrap_ ## name
WRAPPED (open);
WRAPPED (close);
WRAPPED (ioctl);
WRAPPED (fpi_fte3600_transport_open);
WRAPPED (fpi_fte3600_transport_close);
WRAPPED (fpi_fte3600_backend_for_sensor);
WRAPPED (fpi_fte3600_discovery_new);
#undef WRAPPED
int __wrap_open64 (const char *path,
                   int         flags,
                   ...);

static struct
{
  Fte3600Sensor sensor;
  gboolean      opened;
  guint         opens, closes, captures, resets, allocations, destructions;
  guint         next_frame, n_frames;
  guint         releases, release_polls, held_polls, release_fault;
  gboolean      finger_down, release_active, duplicate_once;
  const guint8 *frames;
  gboolean      retry_next, retry_armed, failed_cleanup, wrong_geometry, cancel_capture;
  guint         shutdowns;
  gboolean      shutdown_error, close_error;
  GCancellable *cancellable;
} mock;

int
__wrap_open (const char *path, int flags, ...)
{
  g_assert_cmpstr (path, ==, MOCK_PATH);
  g_assert_cmpint (flags, ==, O_RDWR | O_CLOEXEC);
  g_assert_false (mock.opened);
  mock.opened = TRUE;
  mock.opens++;
  return MOCK_FD;
}

int
__wrap_open64 (const char *path, int flags, ...)
{
  return __wrap_open (path, flags);
}

int
__wrap_close (int fd)
{
  g_assert_cmpint (fd, ==, MOCK_FD);
  g_assert_true (mock.opened);
  mock.opened = FALSE;
  mock.closes++;
  return 0;
}

int
__wrap_ioctl (int fd, unsigned long operation, ...)
{
  /* This suite mocks the entire transport, never individual hardware calls. */
  g_test_fail ();
  errno = EIO;
  return -1;
}

gboolean
__wrap_fpi_fte3600_transport_open (FpiDeviceFte3600 *self, GError **error)
{
  g_assert_cmpstr (fpi_device_get_udev_data (FP_DEVICE (self), FPI_DEVICE_UDEV_SUBTYPE_FTE3600), ==, MOCK_PATH);
  self->spi_fd = __wrap_open (MOCK_PATH, O_RDWR | O_CLOEXEC);
  self->max_transfer = 32768;
  return TRUE;
}

gboolean
__wrap_fpi_fte3600_transport_close (FpiDeviceFte3600 *self, GError **error)
{
  if (self->spi_fd >= 0)
    __wrap_close (self->spi_fd);
  self->spi_fd = -1;
  if (mock.close_error)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Mock transport close failure");
      return FALSE;
    }
  return TRUE;
}

static void
discover_run (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);

  self->identity = (Fte3600Identity){
    .sensor = mock.sensor,
    .evidence = FTE3600_IDENTITY_SPECIAL_CHIP_ID,
    .response = fpi_fte3600_match_profile_get (mock.sensor)->model,
  };
  self->idle_verified = TRUE;
  fpi_ssm_mark_completed (ssm);
}

FpiSsm *
__wrap_fpi_fte3600_discovery_new (FpiDeviceFte3600 *self, gboolean boot_only)
{
  g_assert_false (boot_only);
  return fpi_ssm_new (FP_DEVICE (self), discover_run, 1);
}

static gboolean
prepare_capture (FpiDeviceFte3600 *self, GError **error)
{
  g_assert_null (self->backend_data);
  self->backend_data = g_malloc0 (1);
  mock.allocations++;
  return TRUE;
}

static void
destroy (FpiDeviceFte3600 *self)
{
  if (!self->backend_data)
    return;
  g_clear_pointer (&self->backend_data, g_free);
  mock.destructions++;
}

static void
idle_run (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);

  self->armed = FALSE;
  self->idle_verified = TRUE;
  fpi_ssm_mark_completed (ssm);
}

static FpiSsm *
create_init (FpiDeviceFte3600 *self)
{
  return fpi_ssm_new (FP_DEVICE (self), idle_run, 1);
}

static FpiSsm *
create_reset (FpiDeviceFte3600 *self)
{
  mock.resets++;
  return fpi_ssm_new (FP_DEVICE (self), idle_run, 1);
}

static void
shutdown_run (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);

  /* Final quiesce must precede descriptor release and must not stand in for
   * the reusable-idle operation between frames. */
  g_assert_true (mock.opened);
  g_assert_cmpint (self->spi_fd, ==, MOCK_FD);
  g_assert_cmpint (fpi_device_get_current_action (dev), ==, FPI_DEVICE_ACTION_CLOSE);
  mock.shutdowns++;
  self->armed = FALSE;
  self->idle_verified = FALSE;
  if (mock.shutdown_error)
    fpi_ssm_mark_failed (ssm, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                                   "Mock chip shutdown failure"));
  else
    fpi_ssm_mark_completed (ssm);
}

static FpiSsm *
create_shutdown (FpiDeviceFte3600 *self)
{
  return fpi_ssm_new (FP_DEVICE (self), shutdown_run, 1);
}

static void
release_run (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);

  g_assert_true (self->waiting_for_release);
  g_assert_true (self->enroll_needs_release);
  g_assert_true (mock.release_active);
  if (mock.release_fault == 1)
    g_cancellable_cancel (mock.cancellable);
  if (!mock.release_fault && ++mock.release_polls <= mock.held_polls)
    {
      g_assert_true (mock.finger_down);
      fpi_ssm_jump_to_state_delayed (ssm, 0, 1);
      return;
    }
  mock.release_active = FALSE;
  self->armed = mock.release_fault == 4;
  self->idle_verified = mock.release_fault != 3;
  if (fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;
  if (mock.release_fault == 2)
    {
      fpi_ssm_mark_failed (ssm, g_error_new_literal (
                             G_IO_ERROR, G_IO_ERROR_FAILED, "Mock release transfer failed"));
      return;
    }
  if (mock.release_fault == 5)
    {
      mock.release_fault = 0;
      fpi_ssm_mark_failed (ssm, fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
      return;
    }
  mock.finger_down = FALSE;
  fpi_ssm_mark_completed (ssm);
}

static FpiSsm *
create_wait_release (FpiDeviceFte3600 *self)
{
  g_assert_true (mock.finger_down);
  mock.releases++;
  mock.release_polls = 0;
  mock.release_active = TRUE;
  return fpi_ssm_new (FP_DEVICE (self), release_run, 1);
}

static void
capture_run (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (mock.sensor);

  if (fpi_ssm_get_cur_state (ssm) == 0)
    {
      /* Exercise asynchronous completion and avoid recursive retry stacks. */
      fpi_ssm_next_state_delayed (ssm, 1);
      return;
    }
  self->armed = FALSE;
  self->idle_verified = !mock.failed_cleanup;
  if (mock.cancel_capture)
    g_cancellable_cancel (mock.cancellable);
  if (mock.retry_next)
    {
      mock.retry_next = FALSE;
      if (mock.retry_armed)
        {
          self->armed = TRUE;
          self->idle_verified = FALSE;
        }
      fpi_ssm_mark_failed (ssm, fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
      return;
    }
  if (fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;
  g_assert_cmpuint (mock.next_frame, <, mock.n_frames);
  g_assert_false (mock.release_active);
  if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_ENROLL)
    g_assert_false (mock.finger_down);
  mock.finger_down = TRUE;
  fpi_fte3600_clear_captured_image (self);
  self->captured_image = fp_image_new (profile->width - !!mock.wrong_geometry,
                                       profile->height);
  memcpy (self->captured_image->data,
          mock.frames + (gsize) mock.next_frame++ * profile->width * profile->height,
          (gsize) self->captured_image->width * self->captured_image->height);
  if (mock.duplicate_once && mock.next_frame == 1)
    {
      mock.duplicate_once = FALSE;
      mock.next_frame = 0;
    }
  fpi_ssm_mark_completed (ssm);
}

static FpiSsm *
create_capture (FpiDeviceFte3600 *self)
{
  mock.captures++;
  return fpi_ssm_new (FP_DEVICE (self), capture_run, 2);
}

const Fte3600Backend *
__wrap_fpi_fte3600_backend_for_sensor (Fte3600Sensor sensor)
{
  static const Fte3600Backend backend = {
    .bytes_per_pixel = 1,
    .prepare_capture = prepare_capture,
    .destroy = destroy,
    .create_init = create_init,
    .create_capture = create_capture,
    .create_wait_release = create_wait_release,
    .create_reset = create_reset,
  };
  static Fte3600Backend shutdown_backend;

  g_assert_cmpint (sensor, ==, mock.sensor);
  if (shutdown_fixture)
    {
      shutdown_backend = backend;
      shutdown_backend.create_shutdown = create_shutdown;
      return &shutdown_backend;
    }
  return &backend;
}

static void
init_complete (GObject *object, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;

  g_assert_true (g_async_initable_init_finish (G_ASYNC_INITABLE (object), result, &error));
  g_assert_no_error (error);
  *(gboolean *) data = TRUE;
}

static FpDevice *
new_device (Fte3600Sensor sensor)
{
  gboolean initialized = FALSE;

  g_autoptr(GError) error = NULL;
  FpDevice *device;

  mock = (typeof (mock)){ .sensor = sensor };
  mock.cancellable = g_cancellable_new ();
  device = g_object_new (fpi_device_fte3600_get_type (),
                         "fpi-udev-data-spidev", MOCK_PATH, NULL);
  g_async_initable_init_async (G_ASYNC_INITABLE (device), G_PRIORITY_DEFAULT,
                               NULL, init_complete, &initialized);
  while (!initialized)
    g_main_context_iteration (NULL, TRUE);
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  return device;
}

static void
finish_device (FpDevice *device)
{
  g_autoptr(GError) error = NULL;

  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_object_unref (device);
  g_assert_false (mock.opened);
  g_assert_cmpuint (mock.opens, ==, mock.closes);
  g_assert_cmpuint (mock.allocations, ==, mock.destructions);
  g_clear_object (&mock.cancellable);
}

static void
test_final_shutdown (gconstpointer user_data)
{
  guint scenario = GPOINTER_TO_UINT (user_data);
  FpDevice *device;
  FpiDeviceFte3600 *self;

  g_autoptr(GError) error = NULL;

  shutdown_fixture = TRUE;
  device = new_device (FTE3600_SENSOR_FT9369);
  self = FPI_DEVICE_FTE3600 (device);
  g_assert_true (self->idle_verified);
  g_assert_cmpuint (mock.shutdowns, ==, 0);
  if (scenario == 0)
    {
      const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (mock.sensor);
      g_autofree guint8 *pixels = g_malloc0 ((gsize) profile->width * profile->height);
      g_autoptr(FpImage) image = NULL;

      mock.frames = pixels;
      mock.n_frames = 1;
      image = fp_device_capture_sync (device, TRUE, NULL, &error);
      g_assert_no_error (error);
      g_assert_nonnull (image);
      g_assert_cmpuint (mock.shutdowns, ==, 0);
      g_assert_true (self->idle_verified);
    }
  if (scenario == 1)
    self->idle_verified = FALSE;
  if (scenario == 2 || scenario == 3)
    mock.shutdown_error = TRUE;
  if (scenario == 3 || scenario == 5)
    mock.close_error = TRUE;
  if (scenario == 4)
    self->session_failed = TRUE;

  if (scenario == 2 || scenario == 3 || scenario == 5)
    {
      g_assert_false (fp_device_close_sync (device, NULL, &error));
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
      g_assert_cmpstr (error->message, ==, scenario == 5 ?
                       "Mock transport close failure" : "Mock chip shutdown failure");
      g_clear_error (&error);
    }
  else
    {
      g_assert_true (fp_device_close_sync (device, NULL, &error));
      g_assert_no_error (error);
    }
  g_assert_cmpuint (mock.shutdowns, ==, scenario == 4 ? 0 : 1);
  g_assert_cmpuint (mock.resets, ==, 0);
  g_assert_false (mock.opened);
  g_assert_false (fp_device_is_open (device));
  mock.close_error = FALSE;
  mock.shutdown_error = FALSE;
  if (scenario == 0)
    {
      g_assert_true (fp_device_open_sync (device, NULL, &error));
      g_assert_no_error (error);
      g_assert_true (self->idle_verified);
      g_assert_true (fp_device_close_sync (device, NULL, &error));
      g_assert_no_error (error);
      g_assert_cmpuint (mock.shutdowns, ==, 2);
    }
  g_object_unref (device);
  g_assert_cmpuint (mock.opens, ==, mock.closes);
  g_assert_cmpuint (mock.allocations, ==, mock.destructions);
  g_clear_object (&mock.cancellable);
  shutdown_fixture = FALSE;
}

#if FTE3600_ENABLE_PERSONAL_AUTH
static void
make_pattern (guint8 *pixels, guint width, guint height)
{
  const guint columns = MAX (2, (width - 20) / 14);
  const guint rows = MAX (3, (height - 20) / 14);

  /* Mathematical texture at native resolution, including narrow 40x196. */
  for (guint y = 0; y < height; y++)
    for (guint x = 0; x < width; x++)
      {
        gdouble value = 125 + 11 * sin (0.29 * x + 0.17 * y) +
                        9 * cos (0.13 * x - 0.23 * y);

        for (guint row = 0; row < rows; row++)
          for (guint column = 0; column < columns; column++)
            {
              const gdouble cx = 14 + (width - 28.0) * column / (columns - 1);
              const gdouble cy = 14 + (height - 28.0) * row / (rows - 1);
              const gdouble sigma = 1.4 + 0.27 * ((column + 3 * row) % 7);
              const gdouble amplitude = (column + row) % 2 ? 65 : -62;
              const gdouble dx = x - cx, dy = y - cy;

              value += amplitude * exp (-(dx * dx + dy * dy) / (2 * sigma * sigma));
            }
        pixels[y * width + x] = CLAMP (floor (value + 0.5), 1, 254);
      }
}

static guint8 *
make_frames_with_density (const Fte3600MatchProfile *profile, gboolean dense)
{
  gsize size = (gsize) profile->width * profile->height;
  g_autofree guint8 *original = g_malloc (size);
  guint8 *frames = g_malloc (8 * size);

  if (dense)
    {
      /* Dense synthetic texture fills the BRISK feature budget while also
       * yielding IPA points. Its dual template exceeds the BRISK-only cap. */
      for (guint y = 0; y < profile->height; y++)
        for (guint x = 0; x < profile->width; x++)
          {
            gdouble value = 125 + 11 * sin (0.29 * x + 0.17 * y) +
                            9 * cos (0.13 * x - 0.23 * y);

            for (guint row = 0; row < (profile->height - 14) / 5; row++)
              for (guint column = 0; column < (profile->width - 14) / 5; column++)
                {
                  const gdouble dx = x - (7.0 + 5 * column);
                  const gdouble dy = y - (7.0 + 5 * row);
                  const gdouble sigma = 1.8 + 0.1 * ((column + 3 * row) % 7);
                  const gdouble amplitude = (column + row) % 2 ? 85 : -82;

                  value += amplitude * exp (-(dx * dx + dy * dy) / (2 * sigma * sigma));
                }
            original[y * profile->width + x] = CLAMP (floor (value + 0.5), 1, 254);
          }
    }
  else
    {
      make_pattern (original, profile->width, profile->height);
    }
  for (guint sample = 0; sample < 8; sample++)
    for (guint y = 0; y < profile->height; y++)
      for (guint x = 0; x < profile->width; x++)
        {
          gint sx = (gint) x - sample % 4, sy = (gint) y - sample / 4;

          frames[sample * size + y * profile->width + x] =
            sx >= 0 && sy >= 0 ? original[sy * profile->width + sx] : 125;
        }
  return frames;
}

static guint8 *
make_frames (const Fte3600MatchProfile *profile)
{
  return make_frames_with_density (profile, FALSE);
}

static GBytes *
make_wire_version (const Fte3600MatchProfile *profile, const guint8 *frames,
                   gboolean legacy, gboolean include_ipa)
{
  g_autoptr(Fte3600Template) templ = legacy ? fpi_fte3600_template_new () :
                                     fpi_fte3600_template_new_for_profile (profile);
  GBytes *wire = NULL;
  gsize size = (gsize) profile->width * profile->height;

  for (guint sample = 0; sample < 8; sample++)
    {
      Fte3600BriskFeatureSet features;
      const Fte3600IpaFeatureSet *p_ipa = NULL;
#if FTE3600_ENABLE_IPA_AUTH
      Fte3600IpaFeatureSet ipa_features;
#endif
      FpiBriskImage image = { frames + sample * size, size,
                              profile->width, profile->height, profile->width };

      g_assert_cmpint (fpi_fte3600_brisk_extract_for_profile (profile, &image, &features),
                       ==, FTE3600_BRISK_OK);
#if FTE3600_ENABLE_IPA_AUTH
      if (include_ipa && !legacy && fpi_fte3600_ipa_supports_profile (profile) &&
          fpi_fte3600_ipa_extract (image.data, image.length, &ipa_features) == FTE3600_IPA_OK)
        p_ipa = &ipa_features;
#endif
      g_assert_cmpint (fpi_fte3600_template_add_dual_features (templ, &features, p_ipa, NULL), ==,
                       sample == 7 ? FTE3600_TEMPLATE_OK : FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
    }
  g_assert_cmpint (fpi_fte3600_template_encode (templ, &wire), ==, FTE3600_TEMPLATE_OK);
  return wire;
}

static GBytes *
make_wire (const Fte3600MatchProfile *profile, const guint8 *frames)
{
  return make_wire_version (profile, frames, FALSE, TRUE);
}

static FpPrint *
print_for_wire (FpDevice *device, GBytes *wire)
{
  FpPrint *print = g_object_ref_sink (fp_print_new (device));
  gsize size;
  gconstpointer bytes = g_bytes_get_data (wire, &size);

  g_autoptr(GVariant) variant = g_variant_ref_sink (
    g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, bytes, size, 1));

  fpi_print_set_type (print, FPI_PRINT_RAW);
  g_object_set (print, "fpi-data", variant, NULL);
  return print;
}

typedef struct
{
  guint stages, retries;
} Progress;

static void
enroll_progress (FpDevice *device, gint completed, FpPrint *print,
                 gpointer user_data, GError *error)
{
  Progress *progress = user_data;

  if (error)
    {
      g_assert_cmpuint (error->domain, ==, FP_DEVICE_RETRY);
      g_assert_true (error->code == FP_DEVICE_RETRY_CENTER_FINGER ||
                     error->code == FP_DEVICE_RETRY_REMOVE_FINGER);
      g_assert_cmpuint (completed, ==, progress->stages);
      progress->retries++;
    }
  else
    {
      g_assert_cmpuint (completed, ==, ++progress->stages);
    }
}

static void
test_roundtrip (gconstpointer data)
{
  Fte3600Sensor sensor = GPOINTER_TO_UINT (data) & 0xff;
  gboolean release_retry = !!(GPOINTER_TO_UINT (data) & 0x200);
  gboolean large_template = !!(GPOINTER_TO_UINT (data) & 0x400);
  gboolean ipa_only = !!(GPOINTER_TO_UINT (data) & 0x800);

  /* Isolate the process-wide mode override and set it before starting any
   * workers. An IPA-only success cannot be supplied by the BRISK fallback. */
  if (ipa_only && !g_test_subprocess ())
    {
      g_test_trap_subprocess (NULL, 30 * G_USEC_PER_SEC, 0);
      g_test_trap_assert_passed ();
      return;
    }
  if (ipa_only)
    g_setenv ("FP_FTE3600_MATCHER", "ipa", TRUE);

  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (sensor);
  FpDevice *device = new_device (sensor);
  g_autofree guint8 *frames = make_frames_with_density (profile, large_template);

  g_autoptr(GBytes) expected = make_wire (profile, frames);
  g_autoptr(FpPrint) print = g_object_ref_sink (fp_print_new (device));
  g_autoptr(FpPrint) enrolled = NULL;
  g_autoptr(FpPrint) restored = NULL;
  g_autofree guchar *serialized = NULL;
  g_autoptr(GVariant) actual = NULL;
  g_autoptr(GError) error = NULL;
  Progress progress = { 0 };
  gboolean match = FALSE;
  gsize actual_size, expected_size, serialized_size;
  gconstpointer actual_bytes, expected_bytes;

  if (large_template)
    {
      const guint8 *wire_data = g_bytes_get_data (expected, &expected_size);

      g_assert_cmpuint (wire_data[8] | ((guint) wire_data[9] << 8), ==,
                        sensor == FTE3600_SENSOR_FT9361 ? FTE3600_TEMPLATE_WIRE_VERSION_V3 :
                        FTE3600_TEMPLATE_PROFILE_DUAL_WIRE_VERSION);
      g_assert_cmpuint (expected_size, >, FTE3600_TEMPLATE_CURRENT_MAX_WIRE_SIZE);
      g_assert_cmpuint (expected_size, <=, FTE3600_TEMPLATE_V3_CURRENT_MAX_WIRE_SIZE);
    }

  g_assert_true (fp_device_has_feature (device, FP_DEVICE_FEATURE_VERIFY));
  g_assert_cmpuint (fp_device_get_nr_enroll_stages (device), ==, 8);
  mock.frames = frames;
  mock.n_frames = 8;
  mock.held_polls = 3;
  mock.retry_next = TRUE;
  mock.retry_armed = !!(GPOINTER_TO_UINT (data) & 0x100);
  mock.release_fault = release_retry ? 5 : 0;
  enrolled = fp_device_enroll_sync (device, print, mock.cancellable,
                                    enroll_progress, &progress, &error);
  g_assert_no_error (error);
  g_assert_nonnull (enrolled);
  g_assert_cmpuint (progress.stages, ==, 8);
  g_assert_cmpuint (progress.retries, ==, 1 + release_retry);
  g_assert_cmpuint (mock.captures, ==, 9);
  g_assert_cmpuint (mock.releases, ==, 7 + release_retry);
  g_assert_cmpuint (mock.resets, ==, mock.retry_armed ? 1 : 0);
  g_object_get (enrolled, "fpi-data", &actual, NULL);
  actual_bytes = g_variant_get_fixed_array (actual, &actual_size, 1);
  expected_bytes = g_bytes_get_data (expected, &expected_size);
  g_assert_cmpmem (actual_bytes, actual_size, expected_bytes, expected_size);

  /* Exercise the same persistence boundary used by applications. Verification
   * must decode the restored wire template, including its sensor policy. */
  g_assert_true (fp_print_serialize (enrolled, &serialized, &serialized_size, &error));
  g_assert_no_error (error);
  restored = fp_print_deserialize (serialized, serialized_size, &error);
  g_assert_no_error (error);
  g_assert_nonnull (restored);
  g_assert_true (fp_print_equal (enrolled, restored));

  mock.next_frame = 0;
  g_assert_true (fp_device_verify_sync (device, restored, NULL, NULL, NULL,
                                        &match, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (match);
  g_assert_cmpuint (mock.captures, ==, 10);

  mock.retry_next = TRUE;
  g_assert_false (fp_device_verify_sync (device, restored, NULL, NULL, NULL,
                                         &match, NULL, &error));
  g_assert_error (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_CENTER_FINGER);
  g_assert_cmpuint (mock.captures, ==, 11);
  g_clear_error (&error);
  mock.next_frame = 0;
  g_assert_true (fp_device_verify_sync (device, restored, NULL, NULL, NULL,
                                        &match, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (match);
  finish_device (device);
}

static void
test_legacy_verify (void)
{
  FpDevice *device = new_device (FTE3600_SENSOR_FT9361);
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (mock.sensor);
  g_autofree guint8 *frames = make_frames (profile);

  g_autoptr(GBytes) wire = make_wire_version (profile, frames, TRUE, FALSE);
  g_autoptr(FpPrint) print = print_for_wire (device, wire);
  g_autoptr(GError) error = NULL;
  const guint8 *bytes = g_bytes_get_data (wire, NULL);
  gboolean match = FALSE;

  g_assert_cmpuint (bytes[8], ==, FTE3600_TEMPLATE_WIRE_VERSION);
  g_assert_cmpuint (bytes[9], ==, 0);
  mock.frames = frames;
  mock.n_frames = 1;
  g_assert_true (fp_device_verify_sync (device, print, NULL, NULL, NULL,
                                        &match, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (match);
  g_assert_cmpuint (mock.captures, ==, 1);
  finish_device (device);
}

static void
test_profile_brisk_verify (gconstpointer data)
{
  gboolean ipa_only = GPOINTER_TO_UINT (data);

  if (ipa_only && !g_test_subprocess ())
    {
      g_test_trap_subprocess (NULL, 30 * G_USEC_PER_SEC, 0);
      g_test_trap_assert_passed ();
      return;
    }
  if (ipa_only)
    g_setenv ("FP_FTE3600_MATCHER", "ipa", TRUE);

  FpDevice *device = new_device (FTE3600_SENSOR_FT9369);
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (mock.sensor);
  g_autofree guint8 *frames = make_frames (profile);
  g_autoptr(GBytes) wire = make_wire_version (profile, frames, FALSE, FALSE);
  g_autoptr(FpPrint) print = print_for_wire (device, wire);
  g_autoptr(GError) error = NULL;
  const guint8 *bytes = g_bytes_get_data (wire, NULL);
  gboolean match = FALSE;

  /* An existing FW9369 BRISK enrollment remains usable with the optional dual
   * build. Selecting IPA alone cannot manufacture its missing IPA features. */
  g_assert_cmpuint (bytes[8], ==, FTE3600_TEMPLATE_PROFILE_WIRE_VERSION);
  g_assert_cmpuint (bytes[9], ==, 0);
  mock.frames = frames;
  mock.n_frames = 1;
  g_assert_true (fp_device_verify_sync (device, print, NULL, NULL, NULL,
                                        &match, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (match, ==, !ipa_only);
  g_assert_cmpuint (mock.captures, ==, 1);
  finish_device (device);
}

#if FTE3600_ENABLE_IPA_AUTH
static void
test_unsupported_ipa_profile (void)
{
  if (!g_test_subprocess ())
    {
      g_test_trap_subprocess (NULL, 30 * G_USEC_PER_SEC, 0);
      g_test_trap_assert_passed ();
      return;
    }
  g_setenv ("FP_FTE3600_MATCHER", "ipa", TRUE);

  FpDevice *device = new_device (FTE3600_SENSOR_FT9365);
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (mock.sensor);
  g_autofree guint8 *frames = make_frames (profile);
  g_autoptr(GBytes) wire = make_wire (profile, frames);
  g_autoptr(FpPrint) print = print_for_wire (device, wire);
  g_autoptr(GError) error = NULL;
  gboolean match = FALSE;

  /* Matching geometry alone does not opt another sensor into IPA. Report an
   * unsupported policy rather than asking for more finger samples forever. */
  mock.frames = frames;
  mock.n_frames = 1;
  g_assert_false (fp_device_verify_sync (device, print, NULL, NULL, NULL,
                                         &match, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
  g_assert_false (match);
  g_assert_cmpuint (mock.captures, ==, 1);
  finish_device (device);
}
#else
static void
test_ipa_opt_in_required (gconstpointer data)
{
  if (!g_test_subprocess ())
    {
      g_test_trap_subprocess (NULL, 30 * G_USEC_PER_SEC, 0);
      g_test_trap_assert_passed ();
      return;
    }
  g_setenv ("FP_FTE3600_MATCHER", data, TRUE);

  FpDevice *device = new_device (FTE3600_SENSOR_FT9369);
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (mock.sensor);
  g_autofree guint8 *frames = make_frames (profile);
  g_autoptr(GBytes) wire = make_wire (profile, frames);
  g_autoptr(FpPrint) print = print_for_wire (device, wire);
  g_autoptr(GError) error = NULL;
  gboolean match = FALSE;

  /* A runtime strategy override cannot enable experimental authentication
   * in a build whose personal authentication policy is BRISK-only. */
  mock.frames = frames;
  mock.n_frames = 1;
  g_assert_false (fp_device_verify_sync (device, print, NULL, NULL, NULL,
                                         &match, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
  g_assert_nonnull (strstr (error->message, "build opt-in"));
  g_assert_false (match);
  g_assert_cmpuint (mock.captures, ==, 1);
  finish_device (device);
}
#endif

static void
test_duplicate_release (void)
{
  FpDevice *device = new_device (FTE3600_SENSOR_FT9361);
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (mock.sensor);
  g_autofree guint8 *frames = make_frames (profile);

  g_autoptr(FpPrint) print = g_object_ref_sink (fp_print_new (device));
  g_autoptr(FpPrint) enrolled = NULL;
  g_autoptr(GError) error = NULL;
  Progress progress = { 0 };

  mock.frames = frames;
  mock.n_frames = 8;
  mock.duplicate_once = TRUE;
  mock.held_polls = 2;
  enrolled = fp_device_enroll_sync (device, print, mock.cancellable,
                                    enroll_progress, &progress, &error);
  g_assert_no_error (error);
  g_assert_nonnull (enrolled);
  g_assert_cmpuint (progress.stages, ==, 8);
  g_assert_cmpuint (progress.retries, ==, 1);
  g_assert_cmpuint (mock.captures, ==, 9);
  /* The rejected duplicate consumed a touch as well as accepted samples. */
  g_assert_cmpuint (mock.releases, ==, 8);
  finish_device (device);
}

static void
test_release_error (gconstpointer data)
{
  guint fault = GPOINTER_TO_UINT (data);
  FpDevice *device = new_device (FTE3600_SENSOR_FT9361);
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (mock.sensor);
  g_autofree guint8 *frames = make_frames (profile);

  g_autoptr(FpPrint) print = g_object_ref_sink (fp_print_new (device));
  g_autoptr(FpPrint) enrolled = NULL;
  g_autoptr(FpImage) image = NULL;
  g_autoptr(GError) error = NULL;
  Progress progress = { 0 };

  mock.frames = frames;
  mock.n_frames = 8;
  mock.release_fault = fault;
  enrolled = fp_device_enroll_sync (device, print, mock.cancellable,
                                    enroll_progress, &progress, &error);
  g_assert_null (enrolled);
  if (fault == 1)
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  else if (fault == 2)
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  else
    g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_assert_cmpuint (progress.stages, ==, 1);
  g_assert_cmpuint (progress.retries, ==, 0);
  g_assert_cmpuint (mock.captures, ==, 1);
  g_assert_cmpuint (mock.releases, ==, 1);
  g_assert_false (FPI_DEVICE_FTE3600 (device)->waiting_for_release);
  g_assert_false (FPI_DEVICE_FTE3600 (device)->enroll_needs_release);

  g_clear_error (&error);
  g_cancellable_reset (mock.cancellable);
  mock.release_fault = 0;
  image = fp_device_capture_sync (device, TRUE, NULL, &error);
  if (fault >= 3)
    {
      g_assert_null (image);
      g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
      g_assert_cmpuint (mock.captures, ==, 1);
    }
  else
    {
      g_assert_no_error (error);
      g_assert_nonnull (image);
      g_assert_cmpuint (mock.captures, ==, 2);
    }
  finish_device (device);
}

/* Each scenario runs through both public authentication entry points. */
static void
test_boundary_error (gconstpointer data)
{
  guint scenario = GPOINTER_TO_UINT (data);
  gboolean verify = scenario & 1;
  guint kind = scenario / 2;
  FpDevice *device = new_device (FTE3600_SENSOR_FT9361);
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (mock.sensor);
  g_autofree guint8 *frames = make_frames (profile);

  g_autoptr(GBytes) wire = make_wire (profile, frames);
  g_autoptr(FpPrint) print = verify ? print_for_wire (device, wire) :
                             g_object_ref_sink (fp_print_new (device));
  g_autoptr(FpPrint) enrolled = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;
  Progress progress = { 0 };
  gboolean match = FALSE;

  mock.frames = frames;
  mock.n_frames = 8;
  mock.wrong_geometry = kind == 0;
  mock.failed_cleanup = kind == 1;
  mock.cancel_capture = kind == 2;
  mock.retry_next = kind != 0;
  if (verify)
    {
      g_assert_false (fp_device_verify_sync (device, print, mock.cancellable, NULL, NULL,
                                             &match, NULL, &error));
    }
  else
    {
      enrolled = fp_device_enroll_sync (device, print, mock.cancellable,
                                        enroll_progress, &progress, &error);
      g_assert_null (enrolled);
    }
  if (kind == 0)
    g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID);
  else if (kind == 1)
    g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  else
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint (progress.retries, ==, 0);
  g_assert_cmpuint (progress.stages, ==, 0);
  g_assert_cmpuint (mock.captures, ==, 1);
  g_assert_cmpint (fpi_device_get_current_action (device), ==, FPI_DEVICE_ACTION_NONE);

  g_clear_error (&error);
  g_cancellable_reset (mock.cancellable);
  mock.wrong_geometry = FALSE;
  mock.cancel_capture = FALSE;
  image = fp_device_capture_sync (device, TRUE, NULL, &error);
  if (kind == 1)
    {
      g_assert_null (image);
      g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
      g_assert_cmpuint (mock.captures, ==, 1);
    }
  else
    {
      g_assert_no_error (error);
      g_assert_nonnull (image);
      g_assert_cmpuint (mock.captures, ==, 2);
    }
  finish_device (device);
}

static void
test_foreign_template (gconstpointer data)
{
  Fte3600Sensor sensor = GPOINTER_TO_UINT (data) >> 8;
  const Fte3600MatchProfile *foreign = fpi_fte3600_match_profile_get (GPOINTER_TO_UINT (data) & 0xff);
  FpDevice *device = new_device (sensor ? sensor : FTE3600_SENSOR_FT9361);
  g_autofree guint8 *frames = make_frames (foreign);

  g_autoptr(GBytes) wire = make_wire (foreign, frames);
  g_autoptr(FpPrint) print = print_for_wire (device, wire);
  g_autoptr(GError) error = NULL;
  gboolean match = FALSE;

  /* Print metadata is deliberately compatible. The stored profile must still
   * reject another chip, even for the same native 64x80 geometry, before I/O. */
  g_assert_true (fp_print_compatible (print, device));
  g_assert_false (fp_device_verify_sync (device, print, NULL, NULL, NULL,
                                         &match, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID);
  g_assert_cmpuint (mock.captures, ==, 0);
  finish_device (device);
}

static void
test_previous_family_policy (void)
{
  FpDevice *device = new_device (FTE3600_SENSOR_FT9769);
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (mock.sensor);
  g_autofree guint8 *frames = make_frames (profile);

  g_autoptr(GBytes) current = make_wire (profile, frames);
  g_autoptr(GBytes) previous = NULL;
  g_autoptr(FpPrint) print = NULL;
  g_autoptr(GError) error = NULL;
  gsize size;
  const guint8 *bytes = g_bytes_get_data (current, &size);
  guint8 *old = g_memdup2 (bytes, size);
  gboolean match = FALSE;

  /* The previous v2 policy used axis-scaled covariance. Reject it before
  * capture, with a useful re-enrollment message; v1 has its own test. */
  old[26] = 6;
  old[27] = 0;
  old[28] = 7;
  old[29] = 0;
  previous = g_bytes_new_take (old, size);
  print = print_for_wire (device, previous);
  g_assert_false (fp_device_verify_sync (device, print, NULL, NULL, NULL,
                                         &match, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
  g_assert_nonnull (strstr (error->message, "enroll the finger again"));
  g_assert_cmpuint (mock.captures, ==, 0);
  finish_device (device);
}
#else
static void
test_auth_disabled (gconstpointer data)
{
  FpDevice *device = new_device (GPOINTER_TO_UINT (data));

  g_autoptr(FpPrint) print = g_object_ref_sink (fp_print_new (device));
  g_autoptr(FpPrint) enrolled = NULL;
  g_autoptr(GError) error = NULL;
  gboolean match = FALSE;

  g_assert_false (fp_device_has_feature (device, FP_DEVICE_FEATURE_VERIFY));
  g_assert_cmpuint (fp_device_get_nr_enroll_stages (device), ==, 0);
  enrolled = fp_device_enroll_sync (device, print, NULL, NULL, NULL, &error);
  g_assert_null (enrolled);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
  g_clear_error (&error);
  g_assert_false (fp_device_verify_sync (device, print, NULL, NULL, NULL,
                                         &match, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
  g_assert_cmpuint (mock.captures, ==, 0);
  finish_device (device);
}
#endif

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  for (guint scenario = 0; scenario < 6; scenario++)
    {
      g_autofree gchar *path = g_strdup_printf ("/fte3600-auth-lifecycle/shutdown/%u", scenario);

      g_test_add_data_func (path, GUINT_TO_POINTER (scenario), test_final_shutdown);
    }
  for (guint sensor = FTE3600_SENSOR_UNKNOWN + 1; sensor < FTE3600_SENSOR_COUNT; sensor++)
    {
      g_autofree gchar *path = g_strdup_printf ("/fte3600-auth-lifecycle/%s",
                                                fpi_fte3600_sensor_get (sensor)->name);
#if FTE3600_ENABLE_PERSONAL_AUTH
      g_test_add_data_func (path, GUINT_TO_POINTER (sensor), test_roundtrip);
#else
      g_test_add_data_func (path, GUINT_TO_POINTER (sensor), test_auth_disabled);
#endif
    }
#if FTE3600_ENABLE_PERSONAL_AUTH
  g_test_add_func ("/fte3600-auth-lifecycle/legacy-v1-verify", test_legacy_verify);
  g_test_add_data_func ("/fte3600-auth-lifecycle/fw9369-brisk-v2-verify",
                        GUINT_TO_POINTER (FALSE), test_profile_brisk_verify);
  g_test_add_func ("/fte3600-auth-lifecycle/previous-family-policy", test_previous_family_policy);
  g_test_add_func ("/fte3600-auth-lifecycle/release/duplicate", test_duplicate_release);
  for (guint fault = 1; fault <= 4; fault++)
    {
      g_autofree gchar *path = g_strdup_printf ("/fte3600-auth-lifecycle/release/error/%u", fault);

      g_test_add_data_func (path, GUINT_TO_POINTER (fault), test_release_error);
    }
  g_test_add_data_func ("/fte3600-auth-lifecycle/retry-armed-reset",
                        GUINT_TO_POINTER (FTE3600_SENSOR_FT9361 | 0x100), test_roundtrip);
#if FTE3600_ENABLE_IPA_AUTH
  g_test_add_data_func ("/fte3600-auth-lifecycle/large-v3-enroll-verify",
                        GUINT_TO_POINTER (FTE3600_SENSOR_FT9361 | 0x400), test_roundtrip);
  g_test_add_data_func ("/fte3600-auth-lifecycle/fw9369-large-v4-enroll-verify",
                        GUINT_TO_POINTER (FTE3600_SENSOR_FT9369 | 0x400), test_roundtrip);
  g_test_add_data_func ("/fte3600-auth-lifecycle/fw9369-ipa-enroll-verify",
                        GUINT_TO_POINTER (FTE3600_SENSOR_FT9369 | 0x800), test_roundtrip);
  g_test_add_data_func ("/fte3600-auth-lifecycle/fw9369-brisk-v2-no-ipa",
                        GUINT_TO_POINTER (TRUE), test_profile_brisk_verify);
  g_test_add_func ("/fte3600-auth-lifecycle/unsupported-ipa-profile", test_unsupported_ipa_profile);
#else
  g_test_add_data_func ("/fte3600-auth-lifecycle/ipa-opt-in-required/ipa",
                        "ipa", test_ipa_opt_in_required);
  g_test_add_data_func ("/fte3600-auth-lifecycle/ipa-opt-in-required/dual",
                        "dual", test_ipa_opt_in_required);
#endif
  g_test_add_data_func ("/fte3600-auth-lifecycle/release/retry",
                        GUINT_TO_POINTER (FTE3600_SENSOR_FT9369 | 0x200), test_roundtrip);
  for (guint scenario = 0; scenario < 6; scenario++)
    {
      g_autofree gchar *path = g_strdup_printf ("/fte3600-auth-lifecycle/error/%u", scenario);

      g_test_add_data_func (path, GUINT_TO_POINTER (scenario), test_boundary_error);
    }
  for (guint sensor = FTE3600_SENSOR_FT9365; sensor <= FTE3600_SENSOR_FT9369; sensor++)
    {
      g_autofree gchar *path = g_strdup_printf ("/fte3600-auth-lifecycle/foreign/%u", sensor);

      g_test_add_data_func (path, GUINT_TO_POINTER (sensor), test_foreign_template);
    }
  g_test_add_data_func ("/fte3600-auth-lifecycle/foreign/ft9361-on-fw9369",
                        GUINT_TO_POINTER ((FTE3600_SENSOR_FT9369 << 8) | FTE3600_SENSOR_FT9361),
                        test_foreign_template);
#endif
  return g_test_run ();
}
