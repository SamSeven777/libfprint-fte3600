/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Independent libfprint adapter. Accepted input: official libfprint 1.94.10,
 * local device/auth APIs, and the explicitly approved Sam7 BRISK dependency. */
#define FP_COMPONENT "fte3600"
#include "fte3600.h"
#include "fte3600_auth.h"
#include "fte3600_linux_runtime.h"
#include <errno.h>
#include <string.h>

struct _FpiDeviceFte3600
{
  FpDevice                      parent;
  struct fte3600_linux_runtime *runtime;
  Fte3600AuthIdentity           identity;
  Fte3600AuthEnrollment        *enrollment;
  GCancellable                 *worker_cancel;
  gboolean                      worker_active;
  gboolean                      needs_release;
  GHashTable                   *template_cache; /* Worker-owned; immutable values, exact byte keys. */
  GQueue                        cache_order;
};
G_DEFINE_TYPE (FpiDeviceFte3600, fpi_device_fte3600, FP_TYPE_DEVICE)

typedef struct
{
  FpiDeviceAction                    action;
  gchar                             *resource;
  GPtrArray                         *inputs; /* Immutable bytes copied from main-context FpPrints. */
  GBytes                            *encoded;
  struct fte3600_linux_runtime_frame frame;
  guint                              completed;
  gint                               best;
  gboolean                           matched;
  GError                            *retry;
  GAsyncQueue                       *finger_events;
} Work;

static void
work_free (Work *work)
{
  g_free (work->resource);
  g_clear_pointer (&work->inputs, g_ptr_array_unref);
  g_clear_pointer (&work->encoded, g_bytes_unref);
  fte3600_linux_runtime_frame_clear (&work->frame);
  g_clear_error (&work->retry);
  g_clear_pointer (&work->finger_events, g_async_queue_unref);
  g_free (work);
}

/* Sources are explicitly attached to the action's main context: invoke() can
* run synchronously on a worker that happens to acquire an idle context.
* Completion drains remaining observations before ending the action, so a
* delayed source cannot publish a previous action's status in a later one. */
static gboolean
report_observations (gpointer data)
{
  GTask *task = data;
  Work *work = g_task_get_task_data (task);
  gpointer event;

  while ((event = g_async_queue_try_pop (work->finger_events)))
    fpi_device_report_finger_status (g_task_get_source_object (task),
                                     GPOINTER_TO_UINT (event) - 1);
  return G_SOURCE_REMOVE;
}

static void
observe_finger (GTask *task, FpFingerStatusFlags status)
{
  Work *work = g_task_get_task_data (task);

  g_autoptr(GSource) source = g_idle_source_new ();
  g_async_queue_push (work->finger_events, GUINT_TO_POINTER (status + 1));
  g_source_set_priority (source, G_PRIORITY_DEFAULT);
  g_source_set_callback (source, report_observations, g_object_ref (task), g_object_unref);
  g_source_attach (source, g_task_get_context (task));
}

static gboolean
auth_cancelled (gpointer data)
{
  return g_cancellable_is_cancelled (data);
}

static void
clear_template_cache (FpiDeviceFte3600 *self)
{
  g_queue_clear (&self->cache_order);
  g_clear_pointer (&self->template_cache, g_hash_table_unref);
}

/* A small per-device FIFO saves repeat validation without trusting an FpPrint
 * object identity or keeping an unbounded gallery. Work holds its own refs,
 * so eviction cannot invalidate templates still used by this action. */
static Fte3600AuthTemplate *
validated_template (FpiDeviceFte3600 *self, GBytes *bytes,
                    GCancellable *cancel, GError **error)
{
  Fte3600AuthTemplate *print;

  if (g_cancellable_set_error_if_cancelled (cancel, error))
    return NULL;
  if (!self->template_cache)
    self->template_cache = g_hash_table_new_full (g_bytes_hash, g_bytes_equal,
                                                  (GDestroyNotify) g_bytes_unref,
                                                  (GDestroyNotify) fte3600_auth_template_free);
  print = g_hash_table_lookup (self->template_cache, bytes);
  if (print && !fte3600_auth_identity_equal (&self->identity,
                                             fte3600_auth_template_identity (print)))
    {
      clear_template_cache (self);
      return validated_template (self, bytes, cancel, error);
    }
  if (print)
    {
      fp_dbg ("Validated template cache hit");
      return fte3600_auth_template_ref (print);
    }
  fp_dbg ("Validated template cache miss");
  print = fte3600_auth_template_decode_full (bytes, auth_cancelled, cancel, error);
  if (!print || !fte3600_auth_identity_equal (&self->identity,
                                              fte3600_auth_template_identity (print)))
    return print;
  if (g_hash_table_size (self->template_cache) == 16)
    {
      g_hash_table_remove (self->template_cache, g_queue_pop_head (&self->cache_order));
      fp_dbg ("Validated template cache eviction");
    }
  GBytes *key = g_bytes_ref (bytes);
  g_hash_table_insert (self->template_cache, key, fte3600_auth_template_ref (print));
  g_queue_push_tail (&self->cache_order, key);
  return print;
}

static GError *
auth_error (GError *error)
{
  GError *mapped;

  if (!error || error->domain != FTE3600_AUTH_ERROR)
    return error;
  if (error->code == FTE3600_AUTH_CANCELLED)
    mapped = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, error->message);
  else if (error->code == FTE3600_AUTH_LOW_QUALITY ||
           error->code == FTE3600_AUTH_DUPLICATE ||
           error->code == FTE3600_AUTH_INCONSISTENT)
    mapped = fpi_device_retry_new_msg (error->code == FTE3600_AUTH_DUPLICATE ?
                                       FP_DEVICE_RETRY_REMOVE_FINGER : FP_DEVICE_RETRY_GENERAL,
                                       "%s", error->message);
  else
    mapped = fpi_device_error_new_msg (error->code == FTE3600_AUTH_UNSUPPORTED ?
                                       FP_DEVICE_ERROR_NOT_SUPPORTED : FP_DEVICE_ERROR_DATA_INVALID,
                                       "%s", error->message);
  g_error_free (error);
  return mapped;
}

static GError *
runtime_error (const struct fte3600_linux_runtime_diagnostic *diag)
{
  if (diag->code == FTE3600_RUNTIME_CANCELLED)
    return g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "Fingerprint operation cancelled");
  if (diag->code == FTE3600_RUNTIME_TIMEOUT)
    return g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Fingerprint capture timed out");
  if (diag->code == FTE3600_RUNTIME_RELEASE_UNSUPPORTED ||
      diag->device_error == FTE3600_DEVICE_UNSUPPORTED)
    return fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_SUPPORTED, "%s",
                                     fte3600_linux_runtime_error_string (diag->code));
  if (diag->system_error == ENODEV || diag->system_error == ENXIO)
    return fpi_device_error_new (FP_DEVICE_ERROR_REMOVED);
  return fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL, "%s (OS error %d, session %u)",
                                   fte3600_linux_runtime_error_string (diag->code),
                                   diag->system_error, diag->device_error);
}

static void
worker (GTask *task, gpointer source, gpointer task_data, GCancellable *cancel)
{
  FpiDeviceFte3600 *self = source;
  Work *work = task_data;
  struct fte3600_linux_runtime_diagnostic diag = { 0 };

  g_autoptr(GError) error = NULL;
  g_autoptr(Fte3600AuthProbe) probe = NULL;
  g_autoptr(GPtrArray) templates = NULL;
  Fte3600AuthMatch match = { 0 };
  gboolean hardware_failed = FALSE;

  if (work->action == FPI_DEVICE_ACTION_CLOSE)
    {
      fte3600_auth_enrollment_free (self->enrollment);
      self->enrollment = NULL;
      clear_template_cache (self);
      if (self->runtime && fte3600_linux_runtime_close (self->runtime, &diag))
        error = runtime_error (&diag);
      self->runtime = NULL;
      self->needs_release = FALSE;
      goto out;
    }
  if (g_cancellable_set_error_if_cancelled (cancel, &error))
    goto out;
  if (work->action == FPI_DEVICE_ACTION_OPEN || !self->runtime)
    {
      struct fte3600_linux_runtime_config config = { .spidev_path = work->resource };
      struct fte3600_device_status status;
      Fte3600AuthIdentity opened_identity;
      if (fte3600_linux_runtime_open (&config, cancel, &self->runtime, &diag))
        error = runtime_error (&diag);
      else if (fte3600_linux_runtime_get_status (self->runtime, &status) ||
               !fte3600_auth_identity_from_status (&status, &opened_identity, &error))
        if (!error)
          error = fpi_device_error_new (FP_DEVICE_ERROR_GENERAL);
      if (!error)
        g_cancellable_set_error_if_cancelled (cancel, &error);
      if (!error && work->action != FPI_DEVICE_ACTION_OPEN &&
          !fte3600_auth_identity_equal (&self->identity, &opened_identity))
        error = fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                          "Sensor profile changed during recovery; reopen the device");
      if (!error)
        self->identity = opened_identity;
      if (error && self->runtime)
        {
          fte3600_linux_runtime_close (self->runtime, &diag);
          self->runtime = NULL;
        }
      if (error || work->action == FPI_DEVICE_ACTION_OPEN)
        goto out;
    }

  if (work->action == FPI_DEVICE_ACTION_VERIFY || work->action == FPI_DEVICE_ACTION_IDENTIFY)
    {
      templates = g_ptr_array_new_with_free_func ((GDestroyNotify) fte3600_auth_template_free);
      for (guint i = 0; i < work->inputs->len; i++)
        {
          Fte3600AuthTemplate *print = validated_template (self,
                                                           g_ptr_array_index (work->inputs, i), cancel, &error);
          if (!print)
            goto out;
          g_ptr_array_add (templates, print);
        }
    }

  if (self->needs_release)
    {
      enum fte3600_linux_runtime_error release =
        fte3600_linux_runtime_wait_release (self->runtime, cancel, 30000, &diag);
      if (release && release != FTE3600_RUNTIME_RELEASE_UNSUPPORTED)
        {
          error = runtime_error (&diag);
          hardware_failed = TRUE;
          goto out;
        }
      self->needs_release = FALSE;
      if (!release)
        observe_finger (task, FP_FINGER_STATUS_NEEDED);
    }
  if (fte3600_linux_runtime_capture (self->runtime, cancel, 30000, &work->frame, &diag))
    {
      error = runtime_error (&diag);
      hardware_failed = TRUE;
      goto out;
    }
  self->needs_release = TRUE;
  Fte3600AuthIdentity frame_identity;
  if (!fte3600_auth_identity_from_status (&work->frame.status, &frame_identity, &error) ||
      !fte3600_auth_identity_equal (&self->identity, &frame_identity))
    {
      if (!error)
        error = fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                          "Capture identity differs from the opened sensor profile");
      hardware_failed = TRUE;
      goto out;
    }
  /* Only this backend validates a reviewed contact predicate before accepting
   * an IRQ frame. Other families' IRQs do not establish finger presence. */
  if (work->frame.status.model == FTE3600_MODEL_FT9368)
    observe_finger (task, FP_FINGER_STATUS_PRESENT);
  if (work->action == FPI_DEVICE_ACTION_CAPTURE)
    goto out;

  if (work->action == FPI_DEVICE_ACTION_ENROLL)
    {
      if (!self->enrollment)
        self->enrollment = fte3600_auth_enrollment_new (&self->identity, &error);
      if (!self->enrollment)
        goto out;
      if (!fte3600_auth_enrollment_add_full (self->enrollment, work->frame.pixels,
                                             work->frame.length, work->frame.stride,
                                             &work->completed, auth_cancelled, cancel, &error))
        goto out;
      if (work->completed == FTE3600_AUTH_ENROLL_SAMPLES)
        {
          g_autoptr(Fte3600AuthTemplate) print =
            fte3600_auth_enrollment_finish (self->enrollment, &error);
          if (print)
            work->encoded = fte3600_auth_template_encode (print);
        }
      goto out;
    }

  probe = fte3600_auth_probe_new_full (&self->identity, work->frame.pixels,
                                       work->frame.length, work->frame.stride,
                                       auth_cancelled, cancel, &error);
  if (!probe)
    goto out;
  if (work->action == FPI_DEVICE_ACTION_VERIFY)
    fte3600_auth_match_full (probe, g_ptr_array_index (templates, 0), &match,
                             auth_cancelled, cancel, &error);
  else
    fte3600_auth_identify_full (probe, templates, &work->best, &match,
                                auth_cancelled, cancel, &error);
  work->matched = match.matched;
  if (!error && !work->matched)
    work->encoded = fte3600_auth_probe_encode (probe);

out:
  error = auth_error (g_steal_pointer (&error));
  if (!error)
    g_cancellable_set_error_if_cancelled (cancel, &error);
  if (error && work->action == FPI_DEVICE_ACTION_OPEN && self->runtime)
    {
      fte3600_linux_runtime_close (self->runtime, &diag);
      self->runtime = NULL;
      self->needs_release = FALSE;
    }
  /* Backend failures are sticky. Retire the handle before publishing an
   * error; a later action opens a new handle and verifies the same profile. */
  if (self->runtime && work->action != FPI_DEVICE_ACTION_OPEN &&
      work->action != FPI_DEVICE_ACTION_CLOSE &&
      (hardware_failed || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)))
    {
      fte3600_linux_runtime_close (self->runtime, &diag);
      self->runtime = NULL;
      self->needs_release = FALSE;
    }
  if (error && error->domain == FP_DEVICE_RETRY)
    {
      work->retry = g_steal_pointer (&error);
      g_task_return_boolean (task, TRUE);
    }
  else if (error)
    {
      g_task_return_error (task, g_steal_pointer (&error));
    }
  else
    {
      g_task_return_boolean (task, TRUE);
    }
}

static void start_work (FpDevice *device);

static void
complete (GObject *source, GAsyncResult *result, gpointer user_data)
{
  FpDevice *device = FP_DEVICE (source);
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (device);
  Work *work = g_task_get_task_data (G_TASK (result));

  g_autoptr(GError) error = NULL;
  FpPrint *print = NULL;
  (void) user_data;
  g_task_propagate_boolean (G_TASK (result), &error);
  report_observations (G_TASK (result));
  self->worker_active = FALSE;
  g_clear_object (&self->worker_cancel);

  if (work->action == FPI_DEVICE_ACTION_OPEN)
    {
      fpi_device_open_complete (device, g_steal_pointer (&error));
    }
  else if (work->action == FPI_DEVICE_ACTION_CLOSE)
    {
      fpi_device_close_complete (device, g_steal_pointer (&error));
    }
  else if (work->action == FPI_DEVICE_ACTION_ENROLL)
    {
      if (!error)
        {
          fpi_device_get_enroll_data (device, &print);
          fpi_device_enroll_progress (device, work->completed, NULL,
                                      g_steal_pointer (&work->retry));
          if (!work->encoded)
            {
              start_work (device);
              return;
            }
          fpi_print_set_type (print, FPI_PRINT_RAW);
          fpi_print_set_device_stored (print, FALSE);
          g_object_set (print, "fpi-data",
                        g_variant_new_from_bytes (G_VARIANT_TYPE ("ay"), work->encoded, TRUE), NULL);
        }
      g_clear_pointer (&self->enrollment, fte3600_auth_enrollment_free);
      FpPrint *completed_print = error ? NULL : g_object_ref (print);
      fpi_device_enroll_complete (device, completed_print, g_steal_pointer (&error));
    }
  else if (work->action == FPI_DEVICE_ACTION_VERIFY)
    {
      if (!error)
        {
          FpiMatchResult decision = work->retry ? FPI_MATCH_ERROR :
                                    work->matched ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL;
          FpPrint *scan = NULL;
          /* RAW positive results identify the canonical stored enrollment.
          * Current libfprint requires equality with the recognized print;
          * a separate object avoids copying the user's stored metadata. */
          GBytes *scan_bytes = work->matched ?
                               g_ptr_array_index (work->inputs, 0) : work->encoded;
          if (scan_bytes)
            {
              scan = fp_print_new (device);
              fpi_print_set_type (scan, FPI_PRINT_RAW);
              g_object_set (scan, "fpi-data", g_variant_new_from_bytes (
                              G_VARIANT_TYPE ("ay"), scan_bytes, TRUE), NULL);
            }
          fpi_device_verify_report (device, decision, scan, g_steal_pointer (&work->retry));
        }
      fpi_device_verify_complete (device, g_steal_pointer (&error));
    }
  else if (work->action == FPI_DEVICE_ACTION_IDENTIFY)
    {
      GPtrArray *gallery;
      fpi_device_get_identify_data (device, &gallery);
      if (!error)
        {
          FpPrint *scan = NULL;
          GBytes *scan_bytes = work->matched ?
                               g_ptr_array_index (work->inputs, work->best) : work->encoded;
          if (scan_bytes)
            {
              scan = fp_print_new (device);
              fpi_print_set_type (scan, FPI_PRINT_RAW);
              g_object_set (scan, "fpi-data", g_variant_new_from_bytes (
                              G_VARIANT_TYPE ("ay"), scan_bytes, TRUE), NULL);
            }
          fpi_device_identify_report (device,
                                      work->matched ? g_ptr_array_index (gallery, work->best) : NULL,
                                      scan, g_steal_pointer (&work->retry));
        }
      fpi_device_identify_complete (device, g_steal_pointer (&error));
    }
  else if (work->action == FPI_DEVICE_ACTION_CAPTURE)
    {
      FpImage *image = NULL;
      if (work->retry)
        error = g_steal_pointer (&work->retry);
      if (!error)
        {
          image = fp_image_new (self->identity.width, self->identity.height);
          for (guint y = 0; y < self->identity.height; y++)
            memcpy (image->data + y * self->identity.width,
                    work->frame.pixels + y * work->frame.stride, self->identity.width);
        }
      fpi_device_capture_complete (device, image, g_steal_pointer (&error));
    }
}

static gboolean
append_print (GPtrArray *inputs, FpPrint *print, GError **error)
{
  g_autoptr(GVariant) value = NULL;
  g_object_get (print, "fpi-data", &value, NULL);
  if (!value || !g_variant_is_of_type (value, G_VARIANT_TYPE ("ay")) ||
      g_variant_get_size (value) > FTE3600_AUTH_MAX_TEMPLATE_BYTES)
    {
      *error = fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID);
      return FALSE;
    }
  g_ptr_array_add (inputs, g_variant_get_data_as_bytes (value));
  return TRUE;
}

static void
start_work (FpDevice *device)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (device);

  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;
  Work *work = g_new0 (Work, 1);
  g_assert (!self->worker_active);
  work->action = fpi_device_get_current_action (device);
  work->best = -1;
  work->resource = g_strdup (fte3600_driver_resource (device));
  work->inputs = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  work->finger_events = g_async_queue_new ();
  if (work->action == FPI_DEVICE_ACTION_CAPTURE)
    {
      gboolean wait_for_finger;
      fpi_device_get_capture_data (device, &wait_for_finger);
      if (!wait_for_finger)
        error = fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_SUPPORTED,
                                          "Immediate capture is not implemented");
    }
  else if (work->action == FPI_DEVICE_ACTION_VERIFY)
    {
      FpPrint *print;
      fpi_device_get_verify_data (device, &print);
      append_print (work->inputs, print, &error);
    }
  else if (work->action == FPI_DEVICE_ACTION_IDENTIFY)
    {
      GPtrArray *gallery;
      fpi_device_get_identify_data (device, &gallery);
      for (guint i = 0; i < gallery->len && !error; i++)
        append_print (work->inputs, g_ptr_array_index (gallery, i), &error);
    }
  if (error)
    {
      work_free (work);
      fpi_device_action_error (device, g_steal_pointer (&error));
      return;
    }
  self->worker_cancel = g_object_ref (fpi_device_get_cancellable (device));
  self->worker_active = TRUE;
  if (work->action != FPI_DEVICE_ACTION_OPEN && work->action != FPI_DEVICE_ACTION_CLOSE)
    fpi_device_report_finger_status (device, self->needs_release &&
                                     self->identity.backend_model == 0x9368 ?
                                     FP_FINGER_STATUS_PRESENT : FP_FINGER_STATUS_NEEDED);
  task = g_task_new (device, self->worker_cancel, complete, NULL);
  /* Never publish cancellation while a worker still owns hardware. */
  g_task_set_return_on_cancel (task, FALSE);
  g_task_set_check_cancellable (task, FALSE);
  g_task_set_task_data (task, work, (GDestroyNotify) work_free);
  g_task_run_in_thread (task, worker);
}

static void
cancel (FpDevice *device)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (device);

  if (self->worker_cancel)
    g_cancellable_cancel (self->worker_cancel);
}

static void
finalize (GObject *object)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (object);
  struct fte3600_linux_runtime_diagnostic diag;

  g_assert (!self->worker_active); /* GTask keeps the object alive. */
  if (self->runtime)
    fte3600_linux_runtime_close (self->runtime, &diag);
  g_clear_pointer (&self->enrollment, fte3600_auth_enrollment_free);
  clear_template_cache (self);
  g_clear_object (&self->worker_cancel);
  G_OBJECT_CLASS (fpi_device_fte3600_parent_class)->finalize (object);
}

static void
fpi_device_fte3600_init (FpiDeviceFte3600 *self)
{
  (void) self;
}
static void
probe (FpDevice *device)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *id = fte3600_driver_probe_id (device, &error);
  fpi_device_probe_complete (device, id, NULL, g_steal_pointer (&error));
}
static void
fpi_device_fte3600_class_init (FpiDeviceFte3600Class *klass)
{
  FpDeviceClass *device = FP_DEVICE_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = finalize;
  device->id = "fte3600";
  device->full_name = "FocalTech FTE3600 (host BRISK)";
  device->type = fte3600_driver_device_type;
  device->id_table = fte3600_driver_ids;
  device->nr_enroll_stages = FTE3600_AUTH_ENROLL_SAMPLES;
  device->scan_type = FP_SCAN_TYPE_PRESS;
  device->probe = probe;
  device->open = start_work;
  device->close = start_work;
  device->enroll = start_work;
  device->verify = start_work;
  device->identify = start_work;
  device->capture = start_work;
  device->cancel = cancel;
  fpi_device_class_auto_initialize_features (device);
}
