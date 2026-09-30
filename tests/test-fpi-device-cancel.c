/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 libfprint contributors
 *
 * Exercise the real device cancellation code with the existing fake driver.
 * Bounded barriers cover dispatch before attach returns, action completion
 * before source publication, and action completion before source attachment.
 * Each window covers both external forwarding and direct internal cancellation.
 */
#include "fp-device-private.h"
#include "test-device-fake.h"

typedef enum {
  DISPATCH_BEFORE_ATTACH_RETURNS,
  COMPLETE_BEFORE_SOURCE_PUBLISHED,
  COMPLETE_BEFORE_SOURCE_ATTACHED,
} CancelWindow;

typedef struct
{
  CancelWindow window;
  gboolean     external;
} CancelCase;

typedef struct
{
  GMutex        lock;
  GCond         cond;
  CancelWindow  window;
  FpDevice     *device;
  GCancellable *target;
  GSource      *source;
  gboolean      hook_arrived;
  gboolean      release_worker;
  gboolean      completing;
  guint         attach_calls;
  guint         cancel_calls;
} CancelRun;

typedef struct
{
  gboolean done;
  FpImage *image;
  GError  *error;
} CaptureResult;

/* Only calls made by this test's cancelling worker are intercepted. Other
 * GLib sources, normal completion, and the fake driver's open/close are real. */
static GPrivate cancel_worker = G_PRIVATE_INIT (NULL);
static CancelRun *current_run;

GSource *__real_g_idle_source_new (void);
guint __real_g_source_attach (GSource      *source,
                              GMainContext *context);
void __real_g_cancellable_disconnect (GCancellable *cancellable,
                                      gulong        handler_id);
GSource *__wrap_g_idle_source_new (void);
guint __wrap_g_source_attach (GSource      *source,
                              GMainContext *context);
void __wrap_g_cancellable_disconnect (GCancellable *cancellable,
                                      gulong        handler_id);

static FpDevicePrivate *
device_private (FpDevice *device)
{
  FpDeviceClass *klass = g_type_class_peek_static (FP_TYPE_DEVICE);

  return G_STRUCT_MEMBER_P (device, g_type_class_get_instance_private_offset (klass));
}

static void
wait_for_flag (CancelRun *run, const gboolean *flag)
{
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;

  while (!*flag)
    g_assert_true (g_cond_wait_until (&run->cond, &run->lock, deadline));
}

static void
pause_worker (CancelRun *run)
{
  g_mutex_lock (&run->lock);
  run->hook_arrived = TRUE;
  g_cond_broadcast (&run->cond);
  wait_for_flag (run, &run->release_worker);
  g_mutex_unlock (&run->lock);
}

GSource *
__wrap_g_idle_source_new (void)
{
  GSource *source = __real_g_idle_source_new ();
  CancelRun *run = g_private_get (&cancel_worker);

  if (run)
    {
      g_assert_null (run->source);
      /* Keep an inspection reference, including after the source is destroyed. */
      run->source = g_source_ref (source);
      if (run->window == COMPLETE_BEFORE_SOURCE_PUBLISHED)
        pause_worker (run);
    }
  return source;
}

guint
__wrap_g_source_attach (GSource *source, GMainContext *context)
{
  CancelRun *run = g_private_get (&cancel_worker);
  guint source_id;

  if (!run)
    return __real_g_source_attach (source, context);

  g_assert_true (source == run->source);
  run->attach_calls++;
  if (run->window == COMPLETE_BEFORE_SOURCE_ATTACHED)
    pause_worker (run);

  /* Completion must wait for publication/attachment before destroying it. */
  g_assert_false (g_source_is_destroyed (source));
  source_id = __real_g_source_attach (source, context);
  if (run->window == DISPATCH_BEFORE_ATTACH_RETURNS)
    {
      g_mutex_lock (&run->lock);
      gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
      while (run->cancel_calls == 0)
        g_assert_true (g_cond_wait_until (&run->cond, &run->lock, deadline));
      g_mutex_unlock (&run->lock);
    }
  return source_id;
}

void
__wrap_g_cancellable_disconnect (GCancellable *cancellable, gulong handler_id)
{
  CancelRun *run = current_run;

  if (run && run->completing && run->window != DISPATCH_BEFORE_ATTACH_RETURNS)
    {
      /* Release the cancelling worker before the real disconnect waits for
       * its signal handler. Waiting for action completion here would deadlock. */
      g_mutex_lock (&run->lock);
      g_assert_true (run->hook_arrived);
      run->release_worker = TRUE;
      g_cond_broadcast (&run->cond);
      g_mutex_unlock (&run->lock);
    }
  __real_g_cancellable_disconnect (cancellable, handler_id);
}

static void
fake_capture_pending (FpDevice *device)
{
  g_assert_cmpint (fpi_device_get_current_action (device), ==, FPI_DEVICE_ACTION_CAPTURE);
}

static void
fake_cancel (FpDevice *device)
{
  CancelRun *run = current_run;

  g_assert_nonnull (run);
  g_assert_true (device == run->device);
  g_assert_cmpint (fpi_device_get_current_action (device), ==, FPI_DEVICE_ACTION_CAPTURE);
  g_assert_null (device_private (device)->current_idle_cancel_source);
  g_mutex_lock (&run->lock);
  run->cancel_calls++;
  g_cond_broadcast (&run->cond);
  g_mutex_unlock (&run->lock);
}

static gpointer
cancel_in_worker (gpointer data)
{
  CancelRun *run = data;

  g_private_set (&cancel_worker, run);
  g_cancellable_cancel (run->target);
  g_private_set (&cancel_worker, NULL);
  return NULL;
}

static void
capture_finished (GObject *object, GAsyncResult *result, gpointer data)
{
  CaptureResult *capture = data;

  capture->image = fp_device_capture_finish (FP_DEVICE (object), result, &capture->error);
  capture->done = TRUE;
}

static gboolean
test_deadline (gpointer unused)
{
  g_error ("Cancellation regression test exceeded its main-context deadline");
  return G_SOURCE_REMOVE;
}

static void
test_cancel_window (gconstpointer data)
{
  const CancelCase *test = data;
  CancelRun run = { .window = test->window };
  CaptureResult capture = {0};

  g_autoptr(FpDevice) device = g_object_new (FPI_TYPE_DEVICE_FAKE, NULL);
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  g_autoptr(GError) error = NULL;
  FpDeviceClass *klass = FP_DEVICE_GET_CLASS (device);
  FpDeviceClass saved_class = *klass;
  GThread *worker;
  guint deadline_source;

  g_assert_null (current_run);
  g_mutex_init (&run.lock);
  g_cond_init (&run.cond);
  run.device = device;
  current_run = &run;
  klass->capture = fake_capture_pending;
  klass->cancel = fake_cancel;
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  fp_device_capture (device, TRUE, cancellable, capture_finished, &capture);
  run.target = test->external ? cancellable : fpi_device_get_cancellable (device);
  g_assert_nonnull (run.target);
  g_object_ref (run.target);
  deadline_source = g_timeout_add_seconds (10, test_deadline, NULL);
  worker = g_thread_new ("device-cancel-regression", cancel_in_worker, &run);

  if (test->window == DISPATCH_BEFORE_ATTACH_RETURNS)
    {
      gboolean dispatched = FALSE;
      while (!dispatched)
        {
          g_main_context_iteration (NULL, TRUE);
          g_mutex_lock (&run.lock);
          dispatched = run.cancel_calls != 0;
          g_mutex_unlock (&run.lock);
        }
      g_thread_join (worker);
    }
  else
    {
      g_mutex_lock (&run.lock);
      wait_for_flag (&run, &run.hook_arrived);
      g_mutex_unlock (&run.lock);
    }

  run.completing = TRUE;
  fpi_device_capture_complete (device, NULL,
                               g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                    "Controlled cancellation completion"));
  run.completing = FALSE;
  if (test->window != DISPATCH_BEFORE_ATTACH_RETURNS)
    g_thread_join (worker);

  g_assert_cmpuint (run.attach_calls, ==, 1);
  g_assert_cmpuint (run.cancel_calls, ==,
                    test->window == DISPATCH_BEFORE_ATTACH_RETURNS ? 1 : 0);
  g_assert_nonnull (run.source);
  g_assert_true (g_source_is_destroyed (run.source));
  g_assert_null (device_private (device)->current_idle_cancel_source);
  while (!capture.done)
    g_main_context_iteration (NULL, TRUE);
  g_assert_null (capture.image);
  g_assert_error (capture.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&capture.error);
  g_assert_cmpint (fpi_device_get_current_action (device), ==, FPI_DEVICE_ACTION_NONE);

  /* Drain the same context with another operation: no stale cancel callback
   * may run against a completed action or the next capture. */
  capture.done = FALSE;
  fp_device_capture (device, TRUE, NULL, capture_finished, &capture);
  fpi_device_capture_complete (device, fp_image_new (2, 2), NULL);
  while (!capture.done)
    g_main_context_iteration (NULL, TRUE);
  g_assert_no_error (capture.error);
  g_assert_nonnull (capture.image);
  g_clear_object (&capture.image);
  g_assert_cmpuint (run.cancel_calls, ==,
                    test->window == DISPATCH_BEFORE_ATTACH_RETURNS ? 1 : 0);
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (g_source_remove (deadline_source));
  g_source_unref (run.source);
  g_object_unref (run.target);
  *klass = saved_class;
  current_run = NULL;
  g_cond_clear (&run.cond);
  g_mutex_clear (&run.lock);
}

int
main (int argc, char **argv)
{
  static const CancelCase cases[] = {
    { DISPATCH_BEFORE_ATTACH_RETURNS, TRUE },
    { DISPATCH_BEFORE_ATTACH_RETURNS, FALSE },
    { COMPLETE_BEFORE_SOURCE_PUBLISHED, TRUE },
    { COMPLETE_BEFORE_SOURCE_PUBLISHED, FALSE },
    { COMPLETE_BEFORE_SOURCE_ATTACHED, TRUE },
    { COMPLETE_BEFORE_SOURCE_ATTACHED, FALSE },
  };
  const gchar *names[] = {
    "dispatch-before-attach-returns/external", "dispatch-before-attach-returns/internal",
    "complete-before-publish/external", "complete-before-publish/internal",
    "complete-before-attach/external", "complete-before-attach/internal",
  };

  g_test_init (&argc, &argv, NULL);
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree gchar *name = g_strconcat ("/device-cancel/", names[i], NULL);
      g_test_add_data_func (name, &cases[i], test_cancel_window);
    }
  return g_test_run ();
}
