/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 libfprint contributors
 * Run the real USB driver, transfer helpers and public device API against a
 * deterministic USB boundary. Fixtures contain mathematical data only.
 */
#include "drivers_api.h"
#include "drivers/focaltech0752-img.h"
#include "drivers/fte3600-template.h"

GType fpi_device_focaltech0752_get_type (void);
#define WRAPPED(name) __typeof__ (name) __wrap_ ## name
WRAPPED (g_usb_device_open);
WRAPPED (g_usb_device_close);
WRAPPED (g_usb_device_claim_interface);
WRAPPED (g_usb_device_release_interface);
WRAPPED (g_usb_device_bulk_transfer_async);
WRAPPED (g_usb_device_bulk_transfer_finish);
WRAPPED (g_usb_device_bulk_transfer);
#undef WRAPPED

static struct
{
  FpiUsbTransfer *transfer;
  GCancellable *cancellable;
  GAsyncReadyCallback callback;
  guint submissions;
  guint releases;
} usb;

gboolean __wrap_g_usb_device_open (GUsbDevice *device, GError **error) { return TRUE; }
gboolean __wrap_g_usb_device_close (GUsbDevice *device, GError **error) { return TRUE; }
gboolean
__wrap_g_usb_device_claim_interface (GUsbDevice *device, gint interface,
                                    GUsbDeviceClaimInterfaceFlags flags, GError **error)
{
  return TRUE;
}
gboolean
__wrap_g_usb_device_release_interface (GUsbDevice *device, gint interface,
                                      GUsbDeviceClaimInterfaceFlags flags, GError **error)
{
  g_assert_null (usb.transfer);
  usb.releases++;
  return TRUE;
}

void
__wrap_g_usb_device_bulk_transfer_async (GUsbDevice *device, guint8 endpoint,
                                        guint8 *data, gsize length, guint timeout,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback, gpointer user_data)
{
  g_assert_null (usb.transfer);
  g_assert_nonnull (cancellable);
  usb.transfer = user_data;
  usb.callback = callback;
  usb.cancellable = g_object_ref (cancellable);
  usb.submissions++;
  if (endpoint == 0x81 && length > 64)
    g_assert_true (usb.transfer->sensitive);
}

gssize
__wrap_g_usb_device_bulk_transfer_finish (GUsbDevice *device, GAsyncResult *result,
                                         GError **error)
{
  return g_task_propagate_int (G_TASK (result), error);
}

gboolean
__wrap_g_usb_device_bulk_transfer (GUsbDevice *device, guint8 endpoint,
                                  guint8 *data, gsize length, gsize *actual_length,
                                  guint timeout, GCancellable *cancellable, GError **error)
{
  *actual_length = length;
  return TRUE;
}

static void
reply (gssize length, gboolean finger, gboolean cancelled)
{
  FpiUsbTransfer *transfer = g_steal_pointer (&usb.transfer);
  GAsyncReadyCallback callback = usb.callback;
  g_autoptr(GTask) result = g_task_new (NULL, NULL, NULL, NULL);

  g_assert_nonnull (transfer);
  usb.callback = NULL;
  g_clear_object (&usb.cancellable);
  if (length < 0)
    length = transfer->length;
  if (transfer->endpoint == 0x81)
    {
      memset (transfer->buffer, 0, transfer->length);
      if (transfer->length == 64)
        {
          transfer->buffer[0] = 0x02;
          transfer->buffer[3] = 0x04;
          transfer->buffer[4] = finger ? 1 : 0;
        }
    }
  if (cancelled)
    g_task_return_new_error (result, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Mock USB cancellation");
  else
    g_task_return_int (result, length);
  callback (NULL, G_ASYNC_RESULT (result), transfer);
}

static void
drain (void)
{
  while (g_main_context_iteration (NULL, FALSE));
}

typedef struct
{
  FpiDeviceAction action;
  guint completions;
  guint reports;
  GError *error;
} Result;

static void
finished (GObject *object, GAsyncResult *result, gpointer data)
{
  Result *out = data;
  FpDevice *dev = FP_DEVICE (object);
  g_autoptr(FpPrint) print = NULL;

  out->completions++;
  if (out->action == FPI_DEVICE_ACTION_ENROLL)
    print = fp_device_enroll_finish (dev, result, &out->error);
  else if (out->action == FPI_DEVICE_ACTION_VERIFY)
    fp_device_verify_finish (dev, result, NULL, NULL, &out->error);
  else if (out->action == FPI_DEVICE_ACTION_IDENTIFY)
    fp_device_identify_finish (dev, result, NULL, NULL, &out->error);
  else
    g_assert_not_reached ();
}

static void
match_reported (FpDevice *dev, FpPrint *match, FpPrint *print,
                gpointer data, GError *error)
{
  Result *out = data;
  out->reports++;
  g_assert_error (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_CENTER_FINGER);
}

static FpPrint *
ready_print (FpDevice *dev)
{
  g_autoptr(Fte3600Template) templ = fpi_fte3600_template_new ();
  g_autoptr(GBytes) wire = NULL;
  FpPrint *print = g_object_ref_sink (fp_print_new (dev));
  gsize length;
  gconstpointer bytes;

  for (guint stage = 0; stage < FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES; stage++)
    {
      Fte3600BriskFeatureSet features = { 0 };
      features.extractor_schema_version = FTE3600_BRISK_EXTRACTOR_SCHEMA_VERSION;
      features.n_features = 24;
      for (guint i = 0; i < features.n_features; i++)
        {
          Fte3600BriskFeature *f = &features.features[i];
          guint32 state = 0x9e3779b9u ^ (i + 1) * 0x45d9f3bu;
          f->x = 8.0f + 12.0f * (i % 4);
          f->y = 10.0f + 10.0f * (i / 4);
          for (guint j = 0; j < FTE3600_BRISK_DESCRIPTOR_BYTES; j++)
            {
              state ^= state << 13;
              state ^= state >> 17;
              state ^= state << 5;
              f->descriptor[j] = state >> 24;
            }
          f->descriptor[0] ^= stage;
        }
      Fte3600TemplateStatus status = fpi_fte3600_template_add_features (templ, &features, NULL);
      g_assert_true (status == FTE3600_TEMPLATE_OK || status == FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
    }
  g_assert_cmpint (fpi_fte3600_template_encode (templ, &wire), ==, FTE3600_TEMPLATE_OK);
  bytes = g_bytes_get_data (wire, &length);
  fpi_print_set_type (print, FPI_PRINT_RAW);
  g_object_set (print, "fpi-data", g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, bytes, length, 1), NULL);
  return print;
}

static FpDevice *
open_device (void)
{
  FpDevice *dev = g_object_new (fpi_device_focaltech0752_get_type (), NULL);
  g_autoptr(GError) error = NULL;
  g_assert_null (usb.transfer);
  memset (&usb, 0, sizeof (usb));
  g_assert_true (fp_device_open_sync (dev, NULL, &error));
  g_assert_no_error (error);
  return dev;
}

static void
start (FpDevice *dev, GCancellable *cancellable, Result *out)
{
  g_autoptr(FpPrint) print = out->action == FPI_DEVICE_ACTION_ENROLL ?
                           g_object_ref_sink (fp_print_new (dev)) : ready_print (dev);
  g_autoptr(GPtrArray) gallery = g_ptr_array_new ();
  g_ptr_array_add (gallery, print);
  if (out->action == FPI_DEVICE_ACTION_ENROLL)
    fp_device_enroll (dev, print, cancellable, NULL, NULL, NULL, finished, out);
  else if (out->action == FPI_DEVICE_ACTION_VERIFY)
    fp_device_verify (dev, print, cancellable, match_reported, out, NULL, finished, out);
  else
    fp_device_identify (dev, gallery, cancellable, match_reported, out, NULL, finished, out);
  g_assert_nonnull (usb.transfer);
}

static void
close_device (FpDevice *dev)
{
  g_autoptr(GError) error = NULL;
  g_assert_true (fp_device_close_sync (dev, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (usb.releases, ==, 1);
  g_assert_null (usb.transfer);
  drain ();
}

static void
test_cancel (gconstpointer data)
{
  guint test = GPOINTER_TO_UINT (data);
  guint point = test % 6;
  Result out = { .action = test / 6 == 0 ? FPI_DEVICE_ACTION_ENROLL :
                          test / 6 == 1 ? FPI_DEVICE_ACTION_VERIFY : FPI_DEVICE_ACTION_IDENTIFY };
  g_autoptr(FpDevice) dev = open_device ();
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  start (dev, cancellable, &out);
  if (point >= 1)
    reply (-1, FALSE, FALSE); /* poll command */
  if (point >= 2)
    reply (7, point != 5, FALSE); /* poll response, or timer wait */
  if (point >= 3 && point != 5)
    reply (-1, FALSE, FALSE); /* capture command */
  if (point == 4)
    reply (128, FALSE, FALSE); /* partially accumulated image */
  guint submitted = usb.submissions;
  g_cancellable_cancel (cancellable);
  drain ();
  if (point != 5)
    {
      g_assert_cmpuint (out.completions, ==, 0);
      g_assert_nonnull (usb.transfer);
      g_assert_true (g_cancellable_is_cancelled (usb.cancellable));
      /* A backend may finish successfully just as cancellation happens. */
      reply (-1, FALSE, point % 2 == 0);
      drain ();
    }
  g_assert_cmpuint (out.completions, ==, 1);
  g_assert_cmpuint (out.reports, ==, 0);
  g_assert_error (out.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&out.error);
  g_assert_cmpuint (usb.submissions, ==, submitted);
  close_device (dev);
}

static void
test_retry (gconstpointer data)
{
  Result out = { .action = GPOINTER_TO_INT (data) ? FPI_DEVICE_ACTION_IDENTIFY : FPI_DEVICE_ACTION_VERIFY };
  g_autoptr(FpDevice) dev = open_device ();
  for (guint attempt = 0; attempt < 2; attempt++)
    {
      start (dev, NULL, &out);
      reply (-1, FALSE, FALSE);
      reply (7, TRUE, FALSE);
      reply (-1, FALSE, FALSE);
      reply (-1, FALSE, FALSE); /* constant image yields a retry */
      drain ();
      g_assert_cmpuint (out.completions, ==, attempt + 1);
      g_assert_cmpuint (out.reports, ==, attempt + 1);
      g_assert_error (out.error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_CENTER_FINGER);
      g_clear_error (&out.error);
      g_assert_null (usb.transfer);
    }
  close_device (dev);
}

static void
test_empty_response (void)
{
  Result out = { .action = FPI_DEVICE_ACTION_ENROLL };
  g_autoptr(FpDevice) dev = open_device ();
  start (dev, NULL, &out);
  reply (-1, FALSE, FALSE);
  reply (0, FALSE, FALSE);
  drain ();
  g_assert_cmpuint (out.completions, ==, 1);
  g_assert_error (out.error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT);
  g_clear_error (&out.error);
  close_device (dev);
}

static void
test_invalid_template (void)
{
  Result out = { .action = FPI_DEVICE_ACTION_VERIFY };
  g_autoptr(FpDevice) dev = open_device ();
  g_autoptr(FpPrint) print = g_object_ref_sink (fp_print_new (dev));
  fpi_print_set_type (print, FPI_PRINT_RAW);
  g_object_set (print, "fpi-data", g_variant_new_string ("invalid container"), NULL);
  fp_device_verify (dev, print, NULL, NULL, NULL, NULL, finished, &out);
  drain ();
  g_assert_cmpuint (out.completions, ==, 1);
  g_assert_error (out.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID);
  g_clear_error (&out.error);
  g_assert_cmpuint (usb.submissions, ==, 0);
  close_device (dev);
}

static void
cleared_free (gpointer data)
{
  const guint8 *bytes = data;
  for (guint i = 0; i < 6; i++)
    g_assert_cmpuint (bytes[i], ==, 0);
  g_free (data);
}

static void
test_sensitive_transfer (void)
{
  if (g_test_subprocess ())
    {
      const guint8 secret[] = { 0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe };
      g_autoptr(FpDevice) dev = open_device ();
      g_autoptr(FpiUsbTransfer) transfer = fpi_usb_transfer_new (dev);
      g_autoptr(GError) error = NULL;
      g_setenv ("FP_DEBUG_TRANSFER", "1", TRUE);
      g_setenv ("G_MESSAGES_DEBUG", "all", TRUE);
      fpi_usb_transfer_fill_bulk_full (transfer, 0x03, g_memdup2 (secret, sizeof (secret)),
                                       sizeof (secret), cleared_free);
      fpi_usb_transfer_set_sensitive (transfer, TRUE);
      g_assert_true (fpi_usb_transfer_submit_sync (transfer, 1000, &error));
      g_assert_no_error (error);
      close_device (dev);
      return;
    }
  g_test_trap_subprocess (NULL, 0, (GTestSubprocessFlags) 0);
  g_test_trap_assert_passed ();
  g_test_trap_assert_stdout ("*buffer contents redacted (sensitive)*");
  g_test_trap_assert_stdout_unmatched ("*deadbeefcafe*");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  for (guint i = 0; i < 18; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/focaltech0752/cancel/action-%u/point-%u", i / 6, i % 6);
      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_cancel);
    }
  g_test_add_data_func ("/focaltech0752/retry/verify", GINT_TO_POINTER (0), test_retry);
  g_test_add_data_func ("/focaltech0752/retry/identify", GINT_TO_POINTER (1), test_retry);
  g_test_add_func ("/focaltech0752/empty-response", test_empty_response);
  g_test_add_func ("/focaltech0752/invalid-template", test_invalid_template);
  g_test_add_func ("/focaltech0752/sensitive-transfer", test_sensitive_transfer);
  return g_test_run ();
}
