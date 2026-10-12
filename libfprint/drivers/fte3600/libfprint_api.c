/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Actual public libfprint API + actual FTE3600 driver + actual Sam7 matcher.
 * Hardware replacement is linked into the separate test library only. */
#include "fprint.h"
#include <glib/gstdio.h>

static gchar *control;
static GThread *main_thread;
static guint progress_count, match_count, heartbeat_count;
static guint retry_count;
static FpDevice *busy_device;
static gchar *trace_path;
static FpPrint *enroll (FpDevice    *device,
                        const gchar *scenario);

static void
mode (const gchar *value)
{
  g_assert_true (g_file_set_contents (control, value, -1, NULL));
}
static void
reset_trace (void)
{
  g_assert_true (g_file_set_contents (trace_path, "", 0, NULL));
}
static guint
trace_count (const gchar *event)
{
  g_autofree gchar *contents = NULL;
  guint count = 0;

  g_assert_true (g_file_get_contents (trace_path, &contents, NULL, NULL));
  g_auto(GStrv) lines = g_strsplit (contents, "\n", -1);
  for (guint i = 0; lines[i]; i++)
    count += g_str_equal (lines[i], event);
  return count;
}
static void
progress (FpDevice *device, gint stage, FpPrint *print, gpointer data, GError *error)
{
  (void) device;
  (void) print;
  (void) data;
  g_assert_true (g_thread_self () == main_thread);
  g_assert_no_error (error);
  g_assert_cmpuint (stage, ==, ++progress_count);
}
static void
match_cb (FpDevice *device, FpPrint *match, FpPrint *print, gpointer data, GError *error)
{
  (void) device;
  (void) match;
  (void) print;
  (void) data;
  (void) error;
  g_assert_true (g_thread_self () == main_thread);
  match_count++;
}
static gboolean
cancel_timeout (gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = fp_device_capture_sync (busy_device, TRUE, NULL, &error);
  g_assert_null (image);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_BUSY);
  g_clear_error (&error);
  image = fp_device_capture_sync (busy_device, FALSE, NULL, &error);
  g_assert_null (image);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_BUSY);
  heartbeat_count++;
  g_cancellable_cancel (data);
  return G_SOURCE_REMOVE;
}

static FpDevice *
only_device (FpContext *context)
{
  GPtrArray *devices = fp_context_get_devices (context);

  g_assert_cmpuint (devices->len, ==, 1);
  return g_ptr_array_index (devices, 0);
}

static void
test_capture_wait_contract (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpContext) context = fp_context_new ();
  FpDevice *device = only_device (context);
  mode ("positive");
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  reset_trace ();
  for (guint i = 0; i < 2; i++)
    {
      g_autoptr(FpImage) immediate = fp_device_capture_sync (device, FALSE, NULL, &error);
      g_assert_null (immediate);
      g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
      g_clear_error (&error);
      g_assert_cmpuint (trace_count ("capture-begin"), ==, i);
      g_assert_cmpuint (trace_count ("release-begin"), ==, 0);
      g_assert_cmpuint (trace_count ("close"), ==, 0);
      g_assert_cmpuint (trace_count ("open"), ==, 0);
      if (i == 0)
        {
          g_autoptr(FpImage) waited = fp_device_capture_sync (device, TRUE, NULL, &error);
          g_assert_no_error (error);
          g_assert_nonnull (waited);
          g_assert_cmpuint (trace_count ("capture-ok"), ==, 1);
        }
    }
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
}

static void
finger_changed (FpDevice *device, GParamSpec *pspec, gpointer data)
{
  GArray *states = data;
  FpFingerStatusFlags flags = fp_device_get_finger_status (device);

  (void) pspec;
  g_assert_true (g_thread_self () == main_thread);
  g_array_append_val (states, flags);
}

static guint
finger_count (GArray *states, FpFingerStatusFlags flags)
{
  guint count = 0;

  for (guint i = 0; i < states->len; i++)
    count += g_array_index (states, FpFingerStatusFlags, i) == flags;
  return count;
}

static gboolean
cancel_release (gpointer data)
{
  g_assert_cmpuint (fp_device_get_finger_status (busy_device), ==, FP_FINGER_STATUS_PRESENT);
  g_assert_cmpuint (trace_count ("capture-begin"), ==, 0);
  g_assert_cmpuint (trace_count ("release-ok"), ==, 0);
  return cancel_timeout (data);
}

static void
test_release_observations (void)
{
  for (guint supported = 0; supported < 2; supported++)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(FpContext) context = fp_context_new ();
      g_autoptr(GArray) states = g_array_new (FALSE, FALSE, sizeof (FpFingerStatusFlags));
      FpDevice *device = only_device (context);
      busy_device = device;
      mode (supported ? "positive" : "release-unsupported");
      g_assert_true (fp_device_open_sync (device, NULL, &error));
      g_assert_no_error (error);
      g_signal_connect (device, "notify::finger-status", G_CALLBACK (finger_changed), states);
      reset_trace ();
      g_autoptr(FpPrint) print = enroll (device, "enroll");
      g_assert_cmpuint (trace_count ("capture-ok"), ==, 8);
      g_assert_cmpuint (trace_count ("release-begin"), ==, 7);
      g_assert_cmpuint (trace_count ("release-ok"), ==, supported ? 7 : 0);
      g_assert_cmpuint (trace_count ("release-unsupported"), ==, supported ? 0 : 7);
      g_assert_cmpuint (finger_count (states, FP_FINGER_STATUS_PRESENT), ==, supported ? 8 : 0);
      g_assert_cmpuint (finger_count (states, FP_FINGER_STATUS_NEEDED), ==, supported ? 8 : 1);
      /* NONE at action completion is framework housekeeping, not lift evidence. */
      g_assert_cmpuint (finger_count (states, FP_FINGER_STATUS_NONE), ==, 1);
      if (supported)
        {
          g_autoptr(GCancellable) cancel = g_cancellable_new ();
          reset_trace ();
          g_array_set_size (states, 0);
          mode ("release-block");
          g_timeout_add (20, cancel_release, cancel);
          g_autoptr(FpImage) image = fp_device_capture_sync (device, TRUE, cancel, &error);
          g_assert_null (image);
          g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
          g_clear_error (&error);
          g_assert_cmpuint (trace_count ("capture-begin"), ==, 0);
          g_assert_cmpuint (trace_count ("close"), ==, 1);
          g_assert_cmpuint (finger_count (states, FP_FINGER_STATUS_NEEDED), ==, 0);
          while (g_main_context_iteration (NULL, FALSE))
            ;
          g_assert_cmpuint (fp_device_get_finger_status (device), ==, FP_FINGER_STATUS_NONE);
          mode ("positive");
          image = fp_device_capture_sync (device, TRUE, NULL, &error);
          g_assert_no_error (error);
          g_assert_nonnull (image);
          g_assert_cmpuint (trace_count ("open"), ==, 1);
          reset_trace ();
          g_array_set_size (states, 0);
          mode ("release-fail");
          g_clear_object (&image);
          image = fp_device_capture_sync (device, TRUE, NULL, &error);
          g_assert_null (image);
          g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
          g_clear_error (&error);
          g_assert_cmpuint (trace_count ("capture-begin"), ==, 0);
          g_assert_cmpuint (trace_count ("close"), ==, 1);
          g_assert_cmpuint (finger_count (states, FP_FINGER_STATUS_NEEDED), ==, 0);
        }
      g_assert_true (fp_device_close_sync (device, NULL, &error));
      g_assert_no_error (error);
      g_signal_handlers_disconnect_by_data (device, states);
    }
}

static void
fw9369_retry_progress (FpDevice *device, gint stage, FpPrint *print, gpointer data, GError *error)
{
  (void) device;
  (void) print;
  (void) data;
  g_assert_true (g_thread_self () == main_thread);
  if (error)
    {
      g_assert_cmpuint (stage, ==, 1);
      g_assert_error (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_REMOVE_FINGER);
      retry_count++;
    }
  else
    {
      g_assert_cmpuint (stage, ==, ++progress_count);
    }
}

/* Adapter path proof only: the distinct production-object runtime test checks
 * real FW9369 FDT observations and maintenance. This backend supplies images. */
static void
test_fw9369_action_lifecycle (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpContext) context = fp_context_new ();
  FpDevice *device = only_device (context);
  busy_device = device;
  mode ("fw9369");
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_autoptr(FpPrint) input = fp_print_new (device);
  g_object_ref_sink (input);
  reset_trace ();
  mode ("enroll-retry");
  progress_count = retry_count = 0;
  g_autoptr(FpPrint) print = fp_device_enroll_sync (device, input, NULL,
                                                    fw9369_retry_progress, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (print);
  g_assert_cmpuint (progress_count, ==, 8);
  g_assert_cmpuint (retry_count, ==, 1);
  g_assert_cmpuint (trace_count ("capture-ok"), ==, 9);
  g_assert_cmpuint (trace_count ("release-ok"), ==, 8);
  g_assert_cmpuint (trace_count ("open"), ==, 0);

  reset_trace ();
  mode ("positive");
  gboolean matched = FALSE;
  g_assert_true (fp_device_verify_sync (device, print, NULL, NULL, NULL,
                                        &matched, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (matched);
  g_assert_cmpuint (trace_count ("release-ok"), ==, 1);
  g_assert_cmpuint (trace_count ("capture-ok"), ==, 1);
  g_assert_cmpuint (trace_count ("open"), ==, 0);
  reset_trace ();
  g_autoptr(FpImage) image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (image);
  g_assert_cmpuint (trace_count ("release-ok"), ==, 1);
  g_assert_cmpuint (trace_count ("capture-ok"), ==, 1);

  reset_trace ();
  mode ("capture-fail");
  g_clear_object (&image);
  image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_null (image);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_clear_error (&error);
  g_assert_cmpuint (trace_count ("release-ok"), ==, 1);
  g_assert_cmpuint (trace_count ("capture-ok"), ==, 0);
  g_assert_cmpuint (trace_count ("close"), ==, 1);

  mode ("fw9369");
  image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (image);
  reset_trace ();
  mode ("release-block");
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  g_timeout_add (20, cancel_timeout, cancel);
  g_clear_object (&image);
  image = fp_device_capture_sync (device, TRUE, cancel, &error);
  g_assert_null (image);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&error);
  g_assert_cmpuint (trace_count ("capture-begin"), ==, 0);
  g_assert_cmpuint (trace_count ("close"), ==, 1);

  mode ("fw9369");
  image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (image);
  reset_trace ();
  /* Closing discards the retained state; it must not block for finger-up. */
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (trace_count ("release-begin"), ==, 0);
  g_assert_cmpuint (trace_count ("close"), ==, 1);
}

static gboolean
suspend_active (gpointer data)
{
  FpDevice *device = data;

  g_autoptr(GError) error = NULL;
  g_assert_false (fp_device_suspend_sync (device, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
  return G_SOURCE_REMOVE;
}

static void
test_suspend_fallback (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpContext) context = fp_context_new ();
  FpDevice *device = only_device (context);
  mode ("positive");
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  /* Idle suspend is framework-only: actions are blocked until paired resume. */
  reset_trace ();
  g_assert_true (fp_device_suspend_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_autoptr(FpImage) image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_null (image);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_BUSY);
  g_clear_error (&error);
  g_assert_cmpuint (trace_count ("capture-begin"), ==, 0);
  g_assert_true (fp_device_resume_sync (device, NULL, &error));
  g_assert_no_error (error);
  /* A retained handle may fail after physical suspend. It is sticky: changing
   * the scenario cannot repair it; the driver must retire and reopen it. */
  mode ("sticky-fail");
  image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_null (image);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_clear_error (&error);
  g_assert_cmpuint (trace_count ("close"), ==, 1);
  mode ("positive");
  image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (image);
  g_assert_cmpuint (trace_count ("open"), ==, 1);
  /* Both capture waiting and release waiting use framework cancellation. */
  for (guint release = 0; release < 2; release++)
    {
      reset_trace ();
      mode (release ? "release-block" : "block");
      g_timeout_add (20, suspend_active, device);
      g_clear_object (&image);
      image = fp_device_capture_sync (device, TRUE, NULL, &error);
      g_assert_null (image);
      g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_BUSY);
      g_clear_error (&error);
      g_assert_true (fp_device_is_open (device));
      g_assert_cmpuint (trace_count ("close"), ==, 1);
      g_assert_cmpuint (trace_count ("capture-ok"), ==, 0);
      g_assert_true (fp_device_resume_sync (device, NULL, &error));
      g_assert_no_error (error);
      mode ("positive");
      image = fp_device_capture_sync (device, TRUE, NULL, &error);
      g_assert_no_error (error);
      g_assert_nonnull (image);
      g_assert_cmpuint (trace_count ("open"), ==, 1);
    }
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
}

typedef struct
{
  gint          hits, misses, evictions;
  GCancellable *cancel_on_hit;
  GCancellable *cancel_on_miss;
} CacheLog;

static gboolean
cancel_on_main (gpointer data)
{
  g_assert_true (g_thread_self () == main_thread);
  g_cancellable_cancel (data);
  return G_SOURCE_REMOVE;
}

static void
cancel_at_cache_boundary (GCancellable *cancel)
{
  /* Exercise cancellation at an observed worker boundary without recursively
   * logging from inside GLib's log handler. This barrier is test-only. */
  g_idle_add_full (G_PRIORITY_DEFAULT, cancel_on_main, g_object_ref (cancel), g_object_unref);
  while (!g_cancellable_is_cancelled (cancel))
    g_usleep (1000);
}

static void
cache_log (const gchar *domain, GLogLevelFlags level,
           const gchar *message, gpointer data)
{
  CacheLog *log = data;

  (void) domain;
  (void) level;
  if (g_str_equal (message, "Validated template cache hit"))
    {
      g_atomic_int_inc (&log->hits);
      if (log->cancel_on_hit)
        cancel_at_cache_boundary (log->cancel_on_hit);
    }
  else if (g_str_equal (message, "Validated template cache miss"))
    {
      g_atomic_int_inc (&log->misses);
      if (log->cancel_on_miss)
        cancel_at_cache_boundary (log->cancel_on_miss);
    }
  else if (g_str_equal (message, "Validated template cache eviction"))
    {
      g_atomic_int_inc (&log->evictions);
    }
}

static guint16
codec_u16 (const guint8 *data)
{
  return ((guint16) data[0] << 8) | data[1];
}

/* Permute the eight valid sample records without changing their features or
 * policy. This gives distinct, valid codec keys for eviction tests without
 * claiming 17 different physical fingers. */
static FpPrint *
permuted_print (FpPrint *original, guint permutation)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) payload = NULL;
  g_autofree guint8 *serialized = NULL;
  gsize serialized_size;
  g_assert_true (fp_print_serialize (original, &serialized, &serialized_size, &error));
  g_assert_no_error (error);
  FpPrint *copy = fp_print_deserialize (serialized, serialized_size, &error);
  g_assert_no_error (error);
  g_object_get (original, "fpi-data", &payload, NULL);
  const guint8 *source = g_variant_get_data (payload);
  gsize size = g_variant_get_size (payload);
  gsize offsets[8], lengths[8], at = 40;
  guint remaining[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
  g_assert_cmpuint (codec_u16 (source + 10), ==, 40);
  g_assert_cmpuint (codec_u16 (source + 32), ==, 8);
  for (guint i = 0; i < 8; i++)
    {
      offsets[i] = at;
      lengths[i] = 4 + codec_u16 (source + at) * codec_u16 (source + 36);
      at += lengths[i];
      g_assert_cmpuint (at, <=, size - 32);
    }
  g_assert_cmpuint (at, ==, size - 32);
  guint8 *destination = g_memdup2 (source, size);
  at = 40;
  for (guint i = 0; i < 8; i++)
    {
      guint choice = permutation % (8 - i);
      guint sample = remaining[choice];
      permutation /= 8 - i;
      memcpy (destination + at, source + offsets[sample], lengths[sample]);
      at += lengths[sample];
      memmove (&remaining[choice], &remaining[choice + 1], (7 - i - choice) * sizeof (guint));
    }
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
  gsize digest_size = 32;
  g_checksum_update (checksum, destination, size - 32);
  g_checksum_get_digest (checksum, destination + size - 32, &digest_size);
  g_autoptr(GBytes) bytes = g_bytes_new_take (destination, size);
  g_object_set (copy, "fpi-data", g_variant_new_from_bytes (G_VARIANT_TYPE ("ay"), bytes, TRUE), NULL);
  return copy;
}

static void
verify_positive (FpDevice *device, FpPrint *print)
{
  g_autoptr(GError) error = NULL;
  gboolean matched = FALSE;
  mode ("positive");
  g_assert_true (fp_device_verify_sync (device, print, NULL, NULL, NULL, &matched, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (matched);
}

static void
test_template_cache (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpContext) context = fp_context_new ();
  FpDevice *device = only_device (context);
  mode ("positive");
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_autoptr(FpPrint) print = enroll (device, "enroll");
  CacheLog log = { 0 };
  guint handler = g_log_set_handler ("libfprint-fte3600", G_LOG_LEVEL_DEBUG, cache_log, &log);
  verify_positive (device, print);
  g_assert_cmpint (log.misses, ==, 1);
  g_autoptr(FpPrint) copy = permuted_print (print, 0);
  verify_positive (device, copy);
  g_assert_cmpint (log.hits, ==, 1);
  /* Mutating the same public object cannot reuse validation of its old bytes. */
  const guint8 invalid[] = { 1, 2, 3, 4 };
  g_object_set (copy, "fpi-data", g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, invalid, 4, 1), NULL);
  gboolean matched;
  reset_trace ();
  g_assert_false (fp_device_verify_sync (device, copy, NULL, NULL, NULL, &matched, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID);
  g_clear_error (&error);
  g_assert_cmpuint (trace_count ("capture-begin"), ==, 0);
  g_assert_cmpint (log.misses, ==, 2);
  g_autoptr(GPtrArray) gallery = g_ptr_array_new_with_free_func (g_object_unref);
  for (guint i = 0; i < 17; i++)
    g_ptr_array_add (gallery, permuted_print (print, i));
  FpPrint *identified = NULL;
  g_assert_true (fp_device_identify_sync (device, gallery, NULL, NULL, NULL,
                                          &identified, NULL, &error));
  g_assert_no_error (error);
  g_assert_nonnull (identified);
  g_clear_object (&identified);
  g_assert_cmpint (log.evictions, ==, 1);
  gint misses = log.misses;
  verify_positive (device, print);
  g_assert_cmpint (log.misses, ==, misses + 1);
  g_assert_cmpint (log.evictions, ==, 2);
  /* Cancellation on a hit retires hardware but keeps immutable cache entries
   * reusable; cancellation before decode never inserts a partial entry. */
  for (guint hit = 0; hit < 2; hit++)
    {
      g_autoptr(GCancellable) cancel = g_cancellable_new ();
      g_autoptr(FpPrint) candidate = permuted_print (print, hit ? 0 : 100);
      if (hit)
        log.cancel_on_hit = cancel;
      else
        log.cancel_on_miss = cancel;
      reset_trace ();
      g_assert_false (fp_device_verify_sync (device, candidate, cancel, NULL, NULL,
                                             &matched, NULL, &error));
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      g_clear_error (&error);
      log.cancel_on_hit = log.cancel_on_miss = NULL;
      g_assert_cmpuint (trace_count ("capture-ok"), ==, 0);
      misses = log.misses;
      verify_positive (device, candidate);
      g_assert_cmpint (log.misses, ==, misses + !hit);
    }
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  misses = log.misses;
  verify_positive (device, print);
  g_assert_cmpint (log.misses, ==, misses + 1);
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_log_remove_handler ("libfprint-fte3600", handler);
}
static void
retry_progress (FpDevice *device, gint stage, FpPrint *print, gpointer data, GError *error)
{
  (void) device;
  (void) print;
  g_assert_true (g_thread_self () == main_thread);
  if (error)
    {
      g_assert_cmpuint (stage, ==, 1);
      g_assert_error (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_REMOVE_FINGER);
      retry_count++;
      g_cancellable_cancel (data);
    }
  else
    {
      g_assert_cmpuint (stage, ==, 1);
    }
}

static FpPrint *
enroll (FpDevice *device, const gchar *scenario)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) input = fp_print_new (device);
  g_object_ref_sink (input);
  progress_count = 0;
  mode (scenario);
  FpPrint *print = fp_device_enroll_sync (device, input, NULL, progress, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (print);
  g_assert_cmpuint (progress_count, ==, 8);
  g_assert_false (fp_print_get_device_stored (print));
  return print;
}

static void
test_public_api (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpContext) context = fp_context_new ();
  GPtrArray *devices = fp_context_get_devices (context);
  g_assert_cmpuint (devices->len, ==, 1);
  FpDevice *device = g_ptr_array_index (devices, 0);
  busy_device = device;
  g_assert_cmpstr (fp_device_get_driver (device), ==, "fte3600");
  g_assert_cmpstr (fp_device_get_device_id (device), ==, "fte3600-test-physical-1");
  g_assert_false (fp_device_has_feature (device, FP_DEVICE_FEATURE_STORAGE));
  g_assert_true (fp_device_has_feature (device, FP_DEVICE_FEATURE_IDENTIFY));

  mode ("open-fail");
  g_assert_false (fp_device_open_sync (device, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_clear_error (&error);
  g_assert_false (fp_device_is_open (device));
  mode ("unsupported");
  g_assert_false (fp_device_open_sync (device, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
  g_clear_error (&error);
  mode ("enroll");
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_autoptr(FpPrint) enrolled = enroll (device, "enroll");
  g_autofree guint8 *bytes = NULL;
  gsize length = 0;
  g_assert_true (fp_print_serialize (enrolled, &bytes, &length, &error));
  g_assert_no_error (error);
  g_autoptr(FpPrint) stored = fp_print_deserialize (bytes, length, &error);
  g_assert_no_error (error);
  g_assert_nonnull (stored);
  g_assert_true (fp_print_equal (enrolled, stored));
  fp_print_set_username (stored, "synthetic-enrollment-user");
  /* Generic print deserialization succeeds; the driver still validates its
   * own bounded, versioned payload before any authentication decision. */
  g_autoptr(FpPrint) damaged = fp_print_deserialize (bytes, length, &error);
  const guint8 invalid_payload[] = { 0, 1, 2, 3 };
  g_object_set (damaged, "fpi-data", g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                                invalid_payload, sizeof invalid_payload, 1), NULL);
  mode ("positive");
  gboolean invalid_matched = TRUE;
  g_assert_false (fp_device_verify_sync (device, damaged, NULL, NULL, NULL,
                                         &invalid_matched, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID);
  g_clear_error (&error);
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);

  for (guint i = 0; i < 3; i++)
    {
      gboolean matched = FALSE;
      mode ("positive");
      match_count = 0;
      g_assert_true (fp_device_verify_sync (device, stored, NULL, match_cb, NULL,
                                            &matched, NULL, &error));
      g_assert_no_error (error);
      g_assert_true (matched);
      g_assert_cmpuint (match_count, ==, 1);
      mode ("negative");
      match_count = 0;
      g_assert_true (fp_device_verify_sync (device, stored, NULL, match_cb, NULL,
                                            &matched, NULL, &error));
      g_assert_no_error (error);
      g_assert_false (matched);
      g_assert_cmpuint (match_count, ==, 1);
    }
  mode ("blank");
  gboolean matched = TRUE;
  g_assert_false (fp_device_verify_sync (device, stored, NULL, NULL, NULL,
                                         &matched, NULL, &error));
  g_assert_error (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_GENERAL);
  g_clear_error (&error);
  g_autoptr(FpPrint) other = enroll (device, "enroll-other");
  g_autoptr(GPtrArray) gallery = g_ptr_array_new ();
  g_ptr_array_add (gallery, other);
  g_ptr_array_add (gallery, stored);
  mode ("positive");
  FpPrint *identified = NULL;
  g_autoptr(FpPrint) scanned = NULL;
  g_assert_true (fp_device_identify_sync (device, gallery, NULL, NULL, NULL,
                                          &identified, &scanned, &error));
  g_assert_no_error (error);
  g_assert_true (identified == stored);
  g_assert_nonnull (scanned);
  g_assert_false (fp_print_get_device_stored (scanned));
  g_assert_true (fp_print_equal (scanned, stored));
  g_assert_true (scanned != stored);
  g_assert_null (fp_print_get_username (scanned));
  g_clear_object (&identified);
  g_clear_object (&scanned);
  g_ptr_array_remove_index (gallery, 1);
  g_assert_true (fp_device_identify_sync (device, gallery, NULL, NULL, NULL,
                                          &identified, &scanned, &error));
  g_assert_no_error (error);
  g_assert_null (identified);
  g_assert_nonnull (scanned);
  g_assert_false (fp_print_equal (scanned, stored));
  /* Unrecognized query evidence is deliberately not an eight-sample
   * enrollment and must never authenticate as one. */
  g_assert_false (fp_device_verify_sync (device, scanned, NULL, NULL, NULL,
                                         &matched, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID);
  g_clear_error (&error);
  g_clear_object (&scanned);
  g_ptr_array_set_size (gallery, 0);
  g_assert_true (fp_device_identify_sync (device, gallery, NULL, NULL, NULL,
                                          &identified, &scanned, &error));
  g_assert_no_error (error);
  g_assert_null (identified);
  g_assert_nonnull (scanned);
  mode ("positive");
  g_autoptr(FpImage) image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (fp_image_get_width (image), ==, 64);
  g_assert_cmpuint (fp_image_get_height (image), ==, 80);

  for (guint i = 0; i < 3; i++)
    {
      g_autoptr(GCancellable) cancel = g_cancellable_new ();
      mode ("block");
      g_timeout_add (20, cancel_timeout, cancel);
      g_assert_false (fp_device_verify_sync (device, stored, cancel, NULL, NULL,
                                             &matched, NULL, &error));
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      g_clear_error (&error);
      /* The public device remains open after cancellation. A new action
       * must recover the retired OS handle without requiring another claim. */
      mode ("positive");
      g_assert_true (fp_device_verify_sync (device, stored, NULL, NULL, NULL,
                                            &matched, NULL, &error));
      g_assert_no_error (error);
      g_assert_true (matched);
      g_assert_true (fp_device_close_sync (device, NULL, &error));
      g_assert_no_error (error);
      mode ("positive");
      g_assert_true (fp_device_open_sync (device, NULL, &error));
      g_assert_no_error (error);
    }
  g_assert_cmpuint (heartbeat_count, ==, 3);
  /* Duplicate captures neither advance the stage nor complete enrollment. */
  {
    g_autoptr(GCancellable) cancel = g_cancellable_new ();
    g_autoptr(FpPrint) input = g_object_ref_sink (fp_print_new (device));
    mode ("duplicate");
    g_autoptr(FpPrint) failed = fp_device_enroll_sync (device, input, cancel,
                                                       retry_progress, cancel, &error);
    g_assert_null (failed);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpuint (retry_count, ==, 1);
    g_clear_error (&error);
  }
  mode ("capture-fail");
  g_clear_object (&image);
  image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_null (image);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_clear_error (&error);
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
  mode ("other-profile");
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  mode ("positive");
  g_assert_false (fp_device_verify_sync (device, stored, NULL, NULL, NULL,
                                         &matched, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID);
  g_clear_error (&error);
  mode ("close-fail");
  g_assert_false (fp_device_close_sync (device, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL);
  g_clear_error (&error);
  g_assert_false (fp_device_is_open (device));
  mode ("positive");
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  main_thread = g_thread_self ();
  g_autofree gchar *directory = g_dir_make_tmp ("fte3600-api-XXXXXX", NULL);
  control = g_build_filename (directory, "control", NULL);
  trace_path = g_strconcat (control, ".trace", NULL);
  mode ("enroll");
  g_setenv ("FP_FTE3600_TEST_CONTROL", control, TRUE);
  g_test_add_func ("/fte3600/public-api", test_public_api);
  g_test_add_func ("/fte3600/capture-wait-contract", test_capture_wait_contract);
  g_test_add_func ("/fte3600/release-observations", test_release_observations);
  g_test_add_func ("/fte3600/fw9369-action-lifecycle", test_fw9369_action_lifecycle);
  g_test_add_func ("/fte3600/suspend-fallback", test_suspend_fallback);
  g_test_add_func ("/fte3600/template-cache", test_template_cache);
  int result = g_test_run ();
  g_unlink (trace_path);
  g_free (trace_path);
  g_unlink (control);
  g_rmdir (directory);
  g_free (control);
  return result;
}
