/*
 * FocalTech FT9362 (2808:0752) USB Fingerprint Driver
 * Uses host-side BRISK feature extraction and geometric consensus matcher.
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "focaltech0752"

#include "drivers_api.h"
#include "fte3600-brisk.h"
#include "fte3600-template.h"
#include "focaltech0752-img.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#define FOCALTECH_VENDOR_ID   0x2808
#define FOCALTECH_PRODUCT_ID  0x0752

#define EP_IN   0x81
#define EP_OUT  0x03

#define EP_IN_MAX_BUF_SIZE    64

#define CMD_STATUS_POLL_LEN   7
#define CMD_CAPTURE_LEN       5
#define RESPONSE_LEN          7

#define RESP_STX              0x02
#define RESP_STATUS_TYPE      0x04
#define RESP_FINGER_POS       4
#define FINGER_PRESENT        0x01

#define POLL_INTERVAL_MS      50
#define NR_ENROLL_STAGES      FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES /* 8 stages */

static const guint8 cmd_status_poll[] = { 0x02, 0x00, 0x03, 0x80, 0x02, 0x01, 0x80 };
static const guint8 cmd_capture[]     = { 0x02, 0x00, 0x01, 0x81, 0x80 };

struct _FpiDeviceFocaltech0752
{
  FpDevice         parent;

  /* USB communication state */
  gboolean         deactivating;
  gboolean         finger_on_sensor;
  guint8          *raw_buffer;
  gsize            raw_buffer_len;
  GSource         *poll_source;
  gboolean         action_active;
  gboolean         transfer_pending;
  FpiUsbTransferCallback transfer_callback;

  /* Enrollment state */
  Fte3600Template *enroll_template;
  guint            enroll_stage;

  /* Verification state */
  Fte3600Template *verify_template;
};

G_DECLARE_FINAL_TYPE (FpiDeviceFocaltech0752, fpi_device_focaltech0752, FPI, DEVICE_FOCALTECH0752, FpDevice);
G_DEFINE_TYPE (FpiDeviceFocaltech0752, fpi_device_focaltech0752, FP_TYPE_DEVICE);

/* Forward declarations */
static void start_finger_detection (FpiDeviceFocaltech0752 *self);
static void capture_image (FpiDeviceFocaltech0752 *self);
static void poll_timeout_cb (FpDevice *dev, gpointer user_data);


static void
secure_clear (gpointer data, gsize size)
{
  volatile guint8 *bytes = data;
  while (size-- > 0)
    *bytes++ = 0;
}

static void
clear_features (Fte3600BriskFeatureSet *features)
{
  secure_clear (features, sizeof (*features));
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (Fte3600BriskFeatureSet, clear_features)

static void
clear_raw_buffer (FpiDeviceFocaltech0752 *self)
{
  if (self->raw_buffer)
    secure_clear (self->raw_buffer, RAW_IMAGE_SIZE);
  g_clear_pointer (&self->raw_buffer, g_free);
  self->raw_buffer_len = 0;
}

/* A terminal result is published only after the single pending transfer has
 * returned. This prevents a caller's close/new action from racing its callback. */
static void
stop_action (FpiDeviceFocaltech0752 *self)
{
  g_assert (!self->transfer_pending);
  self->action_active = FALSE;
  self->deactivating = TRUE;
  g_clear_pointer (&self->poll_source, g_source_destroy);
  clear_raw_buffer (self);
  g_clear_pointer (&self->enroll_template, fpi_fte3600_template_free);
  g_clear_pointer (&self->verify_template, fpi_fte3600_template_free);
  self->enroll_stage = 0;
  fpi_device_report_finger_status (FP_DEVICE (self), FP_FINGER_STATUS_NONE);
}

static void
action_error (FpiDeviceFocaltech0752 *self, GError *error)
{
  if (!self->action_active)
    {
      g_clear_error (&error);
      return;
    }
  stop_action (self);
  fpi_device_action_error (FP_DEVICE (self), error);
}

static GError *
cancel_error (void)
{
  return g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                             "Fingerprint operation was cancelled");
}

static void
transfer_complete (FpiUsbTransfer *transfer, FpDevice *dev,
                   gpointer user_data, GError *error)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);
  FpiUsbTransferCallback callback = self->transfer_callback;
  GCancellable *cancellable = fpi_device_get_cancellable (dev);

  g_assert (self->transfer_pending);
  self->transfer_pending = FALSE;
  self->transfer_callback = NULL;
  if (self->deactivating || g_cancellable_is_cancelled (cancellable))
    {
      g_clear_error (&error);
      action_error (self, cancel_error ());
      return;
    }
  if (!error && (transfer->actual_length <= 0 ||
                 transfer->actual_length > transfer->length ||
                 (!(transfer->endpoint & FPI_USB_ENDPOINT_IN) &&
                  transfer->actual_length != transfer->length)))
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT,
                                 "Invalid or empty USB response");
  if (error)
    {
      action_error (self, error);
      return;
    }
  callback (transfer, dev, NULL, NULL);
}

static void
submit_transfer (FpiDeviceFocaltech0752 *self, FpiUsbTransfer *transfer,
                 guint timeout_ms, FpiUsbTransferCallback callback)
{
  GCancellable *cancellable = fpi_device_get_cancellable (FP_DEVICE (self));

  g_assert (self->action_active && !self->transfer_pending);
  if (self->deactivating || g_cancellable_is_cancelled (cancellable))
    {
      fpi_usb_transfer_unref (transfer);
      action_error (self, cancel_error ());
      return;
    }
  self->transfer_pending = TRUE;
  self->transfer_callback = callback;
  fpi_usb_transfer_submit (transfer, timeout_ms, cancellable,
                           transfer_complete, NULL);
}

static Fte3600Template *
decode_print (FpDevice *dev, FpPrint *print)
{
  g_autoptr(GVariant) data = NULL;
  g_autoptr(GBytes) wire = NULL;
  Fte3600Template *templ = NULL;
  gsize length;
  gconstpointer bytes;

  if (!print || !fp_print_compatible (print, dev) ||
      fpi_print_get_type (print) != FPI_PRINT_RAW)
    return NULL;
  g_object_get (print, "fpi-data", &data, NULL);
  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE ("ay")) ||
      !g_variant_is_normal_form (data))
    return NULL;
  bytes = g_variant_get_fixed_array (data, &length, 1);
  if (!bytes || length < FTE3600_TEMPLATE_WIRE_HEADER_SIZE ||
      length > FTE3600_TEMPLATE_CURRENT_MAX_WIRE_SIZE)
    return NULL;
  wire = g_bytes_new (bytes, length);
  if (fpi_fte3600_template_decode (wire, FTE3600_TEMPLATE_LOAD_AUTHENTICATION,
                                  &templ) != FTE3600_TEMPLATE_OK ||
      !fpi_fte3600_template_is_ready (templ))
    g_clear_pointer (&templ, fpi_fte3600_template_free);
  return templ;
}

static void
capture_read_cb (FpiUsbTransfer *transfer,
                 FpDevice       *dev,
                 gpointer        user_data,
                 GError         *error)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);

  /* Accumulate data chunks into raw buffer */
  gsize copy_len = transfer->actual_length;
  g_assert (copy_len <= RAW_IMAGE_SIZE - self->raw_buffer_len);
  memcpy (self->raw_buffer + self->raw_buffer_len, transfer->buffer, copy_len);
  self->raw_buffer_len += copy_len;

  if (self->raw_buffer_len >= RAW_IMAGE_SIZE)
    {
      guint8 brisk_image[FTE3600_BRISK_IMAGE_SIZE];
      g_auto(Fte3600BriskFeatureSet) features = { 0 };
      Fte3600BriskStatus bstatus;
      FpiDeviceAction action = fpi_device_get_current_action (dev);

      fp_dbg ("Frame received (%zu bytes); extracting BRISK features", self->raw_buffer_len);

      focaltech0752_process_raw_to_brisk (self->raw_buffer, brisk_image);
      bstatus = fpi_fte3600_brisk_extract (brisk_image, sizeof (brisk_image), &features);
      secure_clear (brisk_image, sizeof (brisk_image));
      clear_raw_buffer (self);

      if (bstatus != FTE3600_BRISK_OK)
        {
          fp_dbg ("BRISK feature extraction failed (%d); retrying", bstatus);
          if (action == FPI_DEVICE_ACTION_ENROLL)
            fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                        fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
          else if (action == FPI_DEVICE_ACTION_VERIFY)
            {
              fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL,
                                        fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
              stop_action (self);
              fpi_device_verify_complete (dev, NULL);
              return;
            }
          else if (action == FPI_DEVICE_ACTION_IDENTIFY)
            {
              fpi_device_identify_report (dev, NULL, NULL,
                                          fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
              stop_action (self);
              fpi_device_identify_complete (dev, NULL);
              return;
            }

          self->finger_on_sensor = FALSE;
          start_finger_detection (self);
          return;
        }

      if (action == FPI_DEVICE_ACTION_ENROLL)
        {
          Fte3600BriskMatchResult nearest_match;
          Fte3600TemplateStatus tstatus;

          tstatus = fpi_fte3600_template_add_features (self->enroll_template, &features, &nearest_match);

          if (tstatus == FTE3600_TEMPLATE_RETRY_DUPLICATE)
            {
              fp_dbg ("Enrollment sample duplicate; prompt user to shift finger");
              fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                          fpi_device_retry_new_msg (FP_DEVICE_RETRY_REMOVE_FINGER,
                                                                    "Shift finger slightly"));
              self->finger_on_sensor = FALSE;
              start_finger_detection (self);
            }
          else if (tstatus == FTE3600_TEMPLATE_RETRY_INCONSISTENT)
            {
              fp_info ("Enrollment sample inconsistent (inliers: %u, median_err: %.2f, mutual: %u)",
                       nearest_match.inliers, nearest_match.median_error, nearest_match.mutual_matches);
              fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                          fpi_device_retry_new_msg (FP_DEVICE_RETRY_CENTER_FINGER,
                                                                    "Keep finger centered and flat near previous position"));
              self->finger_on_sensor = FALSE;
              start_finger_detection (self);
            }
          else if (tstatus != FTE3600_TEMPLATE_OK && tstatus != FTE3600_TEMPLATE_NEED_MORE_SAMPLES)
            {
              fp_info ("Enrollment template rejected feature set (%d, inliers: %u)",
                       tstatus, nearest_match.inliers);
              fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                          fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
              self->finger_on_sensor = FALSE;
              start_finger_detection (self);
            }
          else
            {
              self->enroll_stage++;
              fp_dbg ("Enrolled stage %u of %u", self->enroll_stage, (guint) NR_ENROLL_STAGES);
              const Fte3600BriskFeatureSet *mosaic =
                fpi_fte3600_template_get_mosaic (self->enroll_template);
              if (mosaic != NULL)
                fp_dbg ("Stitched mosaic now contains %u fused features", mosaic->n_features);

              if (fpi_fte3600_template_is_ready (self->enroll_template))
                {
                  g_autoptr(GBytes) wire = NULL;
                  tstatus = fpi_fte3600_template_encode (self->enroll_template, &wire);
                  if (tstatus == FTE3600_TEMPLATE_OK && wire != NULL)
                    {
                      FpPrint *enroll_template = NULL;
                      fpi_device_get_enroll_data (dev, &enroll_template);

                      FpPrint *print = fp_print_new (dev);
                      fpi_print_set_type (print, FPI_PRINT_RAW);
                      fpi_print_set_device_stored (print, FALSE);

                      g_object_set (print,
                                    "finger", fp_print_get_finger (enroll_template),
                                    "username", fp_print_get_username (enroll_template),
                                    "description", fp_print_get_description (enroll_template),
                                    NULL);

                      gsize wire_len = 0;
                      gconstpointer wire_data = g_bytes_get_data (wire, &wire_len);
                      GVariant *data_var = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                                      wire_data, wire_len, 1);
                      g_object_set (print, "fpi-data", data_var, NULL);

                      stop_action (self);
                      fpi_device_enroll_complete (dev, print, NULL);
                    }
                  else
                    {
                      stop_action (self);
                      fpi_device_enroll_complete (dev, NULL,
                                                  fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
                    }

                }
              else
                {
                  fpi_device_enroll_progress (dev, self->enroll_stage, NULL, NULL);
                  self->finger_on_sensor = FALSE;
                  start_finger_detection (self);
                }
            }
        }
      else if (action == FPI_DEVICE_ACTION_VERIFY)
        {
          FpPrint *print = NULL;
          fpi_device_get_verify_data (dev, &print);

          Fte3600TemplateCompareResult comp_res = { 0 };
          Fte3600TemplateStatus tstatus;

          tstatus = fpi_fte3600_template_compare_features (self->verify_template,
                                                           &features,
                                                           FTE3600_TEMPLATE_LOAD_AUTHENTICATION,
                                                           &comp_res);

          gboolean match = (tstatus == FTE3600_TEMPLATE_OK && comp_res.authentication_accepted);
          fp_info ("Verify result: %s (inliers: %u, median_err: %.2f)",
                   match ? "MATCH" : "NO_MATCH",
                   comp_res.best.inliers,
                   comp_res.best.median_error);

          fpi_device_verify_report (dev,
                                    match ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                                    print, NULL);
          stop_action (self);
          fpi_device_verify_complete (dev, NULL);

        }
      else if (action == FPI_DEVICE_ACTION_IDENTIFY)
        {
          GPtrArray *prints = NULL;
          fpi_device_get_identify_data (dev, &prints);

          FpPrint *matched_print = NULL;

          if (prints != NULL)
            {
              for (guint i = 0; i < prints->len; i++)
                {
                  FpPrint *p = g_ptr_array_index (prints, i);
                  g_autoptr(Fte3600Template) templ = decode_print (dev, p);
                  Fte3600TemplateCompareResult comp_res = { 0 };

                  if (!templ)
                    {
                      action_error (self, fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
                      return;
                    }
                  if (fpi_fte3600_template_compare_features (templ, &features,
                                                             FTE3600_TEMPLATE_LOAD_AUTHENTICATION,
                                                             &comp_res) == FTE3600_TEMPLATE_OK &&
                      comp_res.authentication_accepted)
                    {
                      matched_print = p;
                      break;
                    }
                }
            }

          fpi_device_identify_report (dev, matched_print, NULL, NULL);
          stop_action (self);
          fpi_device_identify_complete (dev, NULL);
        }

    }
  else
    {
      /* Continue reading bulk chunks */
      FpiUsbTransfer *read_transfer = fpi_usb_transfer_new (dev);
      fpi_usb_transfer_fill_bulk (read_transfer, EP_IN, RAW_IMAGE_SIZE - self->raw_buffer_len);
      fpi_usb_transfer_set_sensitive (read_transfer, TRUE);
      submit_transfer (self, read_transfer, 5000, capture_read_cb);
    }
}

static void
capture_cmd_cb (FpiUsbTransfer *transfer,
                FpDevice       *dev,
                gpointer        user_data,
                GError         *error)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);

  clear_raw_buffer (self);
  self->raw_buffer = g_malloc0 (RAW_IMAGE_SIZE);
  self->raw_buffer_len = 0;

  FpiUsbTransfer *read_transfer = fpi_usb_transfer_new (dev);
  fpi_usb_transfer_fill_bulk (read_transfer, EP_IN, RAW_IMAGE_SIZE - self->raw_buffer_len);
  fpi_usb_transfer_set_sensitive (read_transfer, TRUE);
  submit_transfer (self, read_transfer, 5000, capture_read_cb);
}

static void
capture_image (FpiDeviceFocaltech0752 *self)
{
  fp_dbg ("Sending capture command to sensor");
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (FP_DEVICE (self));
  fpi_usb_transfer_fill_bulk_full (transfer, EP_OUT, (guint8 *) cmd_capture, CMD_CAPTURE_LEN, NULL);
  submit_transfer (self, transfer, 1000, capture_cmd_cb);
}

static void
poll_status_cb (FpiUsbTransfer *transfer,
                FpDevice       *dev,
                gpointer        user_data,
                GError         *error)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);

  if (self->deactivating)
    return;

  if (transfer->actual_length >= RESPONSE_LEN &&
      transfer->buffer[0] == RESP_STX &&
      transfer->buffer[3] == RESP_STATUS_TYPE)
    {
      guint8 finger_status = transfer->buffer[RESP_FINGER_POS];

      if (finger_status == FINGER_PRESENT && !self->finger_on_sensor)
        {
          fp_dbg ("Finger detected on sensor");
          self->finger_on_sensor = TRUE;
          capture_image (self);
          return;
        }
      else if (finger_status != FINGER_PRESENT)
        {
          self->finger_on_sensor = FALSE;
        }
    }

  if (!self->deactivating)
    self->poll_source = fpi_device_add_timeout (dev, POLL_INTERVAL_MS,
                                               poll_timeout_cb, NULL, NULL);
}

static void
poll_timeout_cb (FpDevice *dev, gpointer user_data)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);
  self->poll_source = NULL;
  if (!self->deactivating)
    start_finger_detection (self);
}

static void
poll_cmd_cb (FpiUsbTransfer *transfer,
             FpDevice       *dev,
             gpointer        user_data,
             GError         *error)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);

  if (self->deactivating)
    return;

  FpiUsbTransfer *read_transfer = fpi_usb_transfer_new (dev);
  fpi_usb_transfer_fill_bulk (read_transfer, EP_IN, EP_IN_MAX_BUF_SIZE);
  submit_transfer (self, read_transfer, 1000, poll_status_cb);
}

static void
start_finger_detection (FpiDeviceFocaltech0752 *self)
{
  if (self->deactivating)
    return;

  FpiUsbTransfer *transfer = fpi_usb_transfer_new (FP_DEVICE (self));
  fpi_usb_transfer_fill_bulk_full (transfer, EP_OUT, (guint8 *) cmd_status_poll, CMD_STATUS_POLL_LEN, NULL);
  submit_transfer (self, transfer, 1000, poll_cmd_cb);
}

static void
dev_open (FpDevice *dev)
{
  GError *error = NULL;

  fp_dbg ("Opening FocalTech FT9362 USB device");

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (dev), 0, 0, &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }

  fpi_device_open_complete (dev, NULL);
}

static void
dev_close (FpDevice *dev)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);
  GError *error = NULL;

  fp_dbg ("Closing FocalTech FT9362 USB device");
  g_assert (!self->action_active && !self->transfer_pending);
  g_clear_pointer (&self->poll_source, g_source_destroy);

  clear_raw_buffer (self);
  g_clear_pointer (&self->enroll_template, fpi_fte3600_template_free);
  g_clear_pointer (&self->verify_template, fpi_fte3600_template_free);

  g_usb_device_release_interface (fpi_device_get_usb_device (dev), 0, 0, &error);
  fpi_device_close_complete (dev, error);
}

static void
dev_enroll (FpDevice *dev)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);

  fp_dbg ("Starting enrollment (BRISK host matcher)");

  self->deactivating = FALSE;
  self->action_active = TRUE;
  self->finger_on_sensor = FALSE;
  g_clear_pointer (&self->enroll_template, fpi_fte3600_template_free);
  self->enroll_template = fpi_fte3600_template_new ();
  self->enroll_stage = 0;

  start_finger_detection (self);
}

static void
dev_verify (FpDevice *dev)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);
  FpPrint *print = NULL;

  fpi_device_get_verify_data (dev, &print);
  g_clear_pointer (&self->verify_template, fpi_fte3600_template_free);
  self->verify_template = decode_print (dev, print);
  if (!self->verify_template)
    {
      fpi_device_verify_complete (dev, fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
      return;
    }
  self->deactivating = FALSE;
  self->action_active = TRUE;
  self->finger_on_sensor = FALSE;
  start_finger_detection (self);
}

static void
dev_identify (FpDevice *dev)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);

  fp_info ("Starting identification (BRISK host matcher) - place finger on sensor");

  self->deactivating = FALSE;
  self->action_active = TRUE;
  self->finger_on_sensor = FALSE;

  start_finger_detection (self);
}

static void
dev_cancel (FpDevice *dev)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);

  if (!self->action_active)
    return;
  self->deactivating = TRUE;
  g_clear_pointer (&self->poll_source, g_source_destroy);
  /* The core's cancellable has already been cancelled. An in-flight USB
   * callback owns completion until it drains, including a late successful reply. */
  if (!self->transfer_pending)
    action_error (self, cancel_error ());
}

static const FpIdEntry id_table[] = {
  { .vid = FOCALTECH_VENDOR_ID, .pid = FOCALTECH_PRODUCT_ID },
  { .vid = 0, .pid = 0, .driver_data = 0 },
};

static void
fpi_device_focaltech0752_init (FpiDeviceFocaltech0752 *self)
{
  self->deactivating = TRUE;
}

static void
fpi_device_focaltech0752_finalize (GObject *object)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (object);

  g_assert (!self->transfer_pending);
  g_clear_pointer (&self->poll_source, g_source_destroy);

  clear_raw_buffer (self);
  g_clear_pointer (&self->enroll_template, fpi_fte3600_template_free);
  g_clear_pointer (&self->verify_template, fpi_fte3600_template_free);

  G_OBJECT_CLASS (fpi_device_focaltech0752_parent_class)->finalize (object);
}

static void
fpi_device_focaltech0752_class_init (FpiDeviceFocaltech0752Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = fpi_device_focaltech0752_finalize;

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "FocalTech FT9362 Fingerprint Sensor";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = NR_ENROLL_STAGES;
  dev_class->open = dev_open;
  dev_class->close = dev_close;
  dev_class->enroll = dev_enroll;
  dev_class->verify = dev_verify;
  dev_class->identify = dev_identify;
  dev_class->cancel = dev_cancel;

  fpi_device_class_auto_initialize_features (dev_class);
}
