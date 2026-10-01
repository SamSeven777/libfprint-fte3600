/*
 * Template loading concurrency regressions using generated feature data.
 * SPDX-FileCopyrightText: 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Included after the lifecycle test's mock-device helpers. The decode wrapper
 * only controls scheduling; it still calls the real decoder.
 */

#if FTE3600_ENABLE_PERSONAL_AUTH

__typeof__ (fpi_fte3600_template_decode_cancellable) __wrap_fpi_fte3600_template_decode_cancellable;
__typeof__ (fpi_fte3600_template_decode_cancellable) __real_fpi_fte3600_template_decode_cancellable;

static struct
{
  GMutex   mutex;
  GCond    cond;
  GThread *main_thread;
  gint     enabled;
  gint     entered;
  gint     cancelled;
  gboolean released;
} load_gate;

Fte3600TemplateStatus
__wrap_fpi_fte3600_template_decode_cancellable (GBytes                    *wire,
                                                Fte3600TemplateLoadPurpose purpose,
                                                Fte3600Template          **templ,
                                                Fte3600TemplateCancelFunc  is_cancelled,
                                                gpointer                   cancel_data)
{
  if (g_atomic_int_get (&load_gate.enabled))
    {
      /* Fail immediately if loading ever returns to the main thread. */
      g_assert_true (g_thread_self () != load_gate.main_thread);
      g_mutex_lock (&load_gate.mutex);
      g_atomic_int_set (&load_gate.entered, TRUE);
      while (!load_gate.released)
        g_cond_wait (&load_gate.cond, &load_gate.mutex);
      g_mutex_unlock (&load_gate.mutex);
    }
  Fte3600TemplateStatus status = __real_fpi_fte3600_template_decode_cancellable (
    wire, purpose, templ, is_cancelled, cancel_data);
  if (g_atomic_int_get (&load_gate.enabled) && status == FTE3600_TEMPLATE_CANCELLED)
    g_atomic_int_set (&load_gate.cancelled, TRUE);
  return status;
}

static FpPrint *
make_maximum_load_print (FpDevice *device,
                         gboolean  previous_policy)
{
  g_autoptr(Fte3600Template) templ = fpi_fte3600_template_new ();
  g_autoptr(GBytes) wire = NULL;
  g_autoptr(GVariant) data = NULL;
  g_autofree guint8 *bytes = NULL;
  const guint8 *encoded;
  gsize size;
  FpPrint *print;

  /* A valid maximum-sized template: eight distinct samples of a synthetic
   * grid, with deterministic descriptors and small descriptor differences. */
  for (guint sample = 0; sample < FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES; sample++)
    {
      Fte3600BriskFeatureSet features = { 0 };
      features.extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
      features.n_features = FTE3600_BRISK_MAX_FEATURES;
      for (guint i = 0; i < features.n_features; i++)
        {
          Fte3600BriskFeature *point = &features.features[i];
          guint32 state = 0x9e3779b9u ^ (i + 1) * 0x45d9f3bu;

          point->x = 2.0f + 3.0f * (i % 20);
          point->y = 2.0f + 4.0f * (i / 20);
          for (guint d = 0; d < FTE3600_BRISK_DESCRIPTOR_BYTES; d++)
            {
              state ^= state << 13;
              state ^= state >> 17;
              state ^= state << 5;
              point->descriptor[d] = state >> 24;
            }
          point->descriptor[0] ^= sample;
        }
      g_assert_cmpint (fpi_fte3600_template_add_features (templ, &features, NULL), ==,
                       sample + 1 == FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES ?
                       FTE3600_TEMPLATE_OK : FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
    }
  g_assert_cmpint (fpi_fte3600_template_encode (templ, &wire), ==,
                   FTE3600_TEMPLATE_OK);
  encoded = g_bytes_get_data (wire, &size);
  g_assert_cmpuint (size, ==, FTE3600_TEMPLATE_CURRENT_MAX_WIRE_SIZE);
  bytes = g_memdup2 (encoded, size);
  if (previous_policy)
    {
      bytes[28] = 3;
      bytes[29] = 0;
    }
  data = g_variant_ref_sink (g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                        bytes, size, 1));
  print = g_object_ref_sink (fp_print_new (device));
  fpi_print_set_type (print, FPI_PRINT_RAW);
  g_object_set (print, "fpi-data", data, NULL);
  return print;
}

static void
wait_for_load_flag_timeout (gint *flag, gint64 timeout)
{
  const gint64 deadline = g_get_monotonic_time () + timeout;

  while (!g_atomic_int_get (flag))
    {
      g_assert_cmpint (g_get_monotonic_time (), <, deadline);
      while (g_main_context_iteration (NULL, FALSE))
        ;
      g_usleep (1000);
    }
}

static void
wait_for_load_flag (gint *flag)
{
  wait_for_load_flag_timeout (flag, 5 * G_TIME_SPAN_SECOND);
}

static void
load_cancelled (GObject      *object,
                GAsyncResult *result,
                gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  gboolean match = FALSE;

  g_assert_false (fp_device_verify_finish (FP_DEVICE (object), result,
                                           &match, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_atomic_int_set ((gint *) user_data, TRUE);
}

static gboolean
cancel_load_in_main_context (gpointer user_data)
{
  g_cancellable_cancel (G_CANCELLABLE (user_data));
  return G_SOURCE_REMOVE;
}

static void
load_device_finalized (gpointer user_data,
                       GObject *object)
{
  g_atomic_int_set ((gint *) user_data, TRUE);
}

static void
test_verify_load_cancel (void)
{
  FpDevice *device = new_device ();

  g_autoptr(FpPrint) print = make_maximum_load_print (device, FALSE);
  g_autoptr(GError) error = NULL;
  gint completed = FALSE;
  gint finalized = FALSE;
  guint resets;

  open_device (device);
  resets = sensor.resets;
  load_gate.main_thread = g_thread_self ();
  load_gate.released = FALSE;
  g_atomic_int_set (&load_gate.entered, FALSE);
  g_atomic_int_set (&load_gate.cancelled, FALSE);
  g_atomic_int_set (&load_gate.enabled, TRUE);

  fp_device_verify (device, print, sensor.cancellable, NULL, NULL, NULL,
                    load_cancelled, &completed);
  wait_for_load_flag (&load_gate.entered);

  /* The decoder remains blocked while the main context dispatches cancellation
   * and finishes the action. No sensor operation may have started. */
  g_idle_add (cancel_load_in_main_context, sensor.cancellable);
  wait_for_load_flag (&completed);
  g_assert_cmpuint (sensor.images, ==, 0);
  g_assert_cmpuint (sensor.resets, ==, resets);
  g_assert_cmpint (fpi_device_get_current_action (device), ==, FPI_DEVICE_ACTION_NONE);
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);

  /* Drop the device and print while the task still owns its private wire.
   * Let the real decoder observe cancellation, then wait for task/device cleanup. */
  g_object_weak_ref (G_OBJECT (device), load_device_finalized, &finalized);
  g_clear_object (&print);
  finish_device (device);
  g_mutex_lock (&load_gate.mutex);
  load_gate.released = TRUE;
  g_cond_signal (&load_gate.cond);
  g_mutex_unlock (&load_gate.mutex);
  /* Cancellation responsiveness above remains limited to five seconds.
   * Final cleanup may include instrumented worker teardown. */
  wait_for_load_flag_timeout (&finalized, 30 * G_TIME_SPAN_SECOND);
  g_assert_true (g_atomic_int_get (&load_gate.cancelled));
  g_atomic_int_set (&load_gate.enabled, FALSE);
}

static void
test_verify_load_previous_policy (void)
{
  FpDevice *device = new_device ();

  g_autoptr(FpPrint) print = make_maximum_load_print (device, TRUE);
  g_autoptr(GError) error = NULL;
  gboolean match = FALSE;
  guint resets;

  open_device (device);
  resets = sensor.resets;
  g_assert_false (fp_device_verify_sync (device, print, NULL, NULL, NULL,
                                         &match, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
  g_assert_cmpuint (sensor.images, ==, 0);
  g_assert_cmpuint (sensor.resets, ==, resets);
  g_assert_cmpint (fpi_device_get_current_action (device), ==, FPI_DEVICE_ACTION_NONE);
  finish_device (device);
}

#endif
