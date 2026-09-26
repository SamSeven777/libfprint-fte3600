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
  guint            poll_timeout_id;

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
static gboolean poll_timeout_cb (gpointer user_data);


static void
capture_read_cb (FpiUsbTransfer *transfer,
                 FpDevice       *dev,
                 gpointer        user_data,
                 GError         *error)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        fpi_device_action_error (dev, error);
      else
        g_error_free (error);
      return;
    }

  /* Accumulate data chunks into raw buffer */
  gsize copy_len = MIN (transfer->actual_length, RAW_IMAGE_SIZE - self->raw_buffer_len);
  memcpy (self->raw_buffer + self->raw_buffer_len, transfer->buffer, copy_len);
  self->raw_buffer_len += copy_len;

  if (self->raw_buffer_len >= RAW_IMAGE_SIZE)
    {
      guint8 brisk_image[FTE3600_BRISK_IMAGE_SIZE];
      Fte3600BriskFeatureSet features;
      Fte3600BriskStatus bstatus;
      FpiDeviceAction action = fpi_device_get_current_action (dev);

      fp_dbg ("Frame received (%zu bytes); extracting BRISK features", self->raw_buffer_len);

      focaltech0752_process_raw_to_brisk (self->raw_buffer, brisk_image);
      bstatus = fpi_fte3600_brisk_extract (brisk_image, sizeof (brisk_image), &features);

      if (bstatus != FTE3600_BRISK_OK)
        {
          fp_dbg ("BRISK feature extraction failed (%d); retrying", bstatus);
          if (action == FPI_DEVICE_ACTION_ENROLL)
            fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                        fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
          else if (action == FPI_DEVICE_ACTION_VERIFY)
            fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL,
                                      fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
          else if (action == FPI_DEVICE_ACTION_IDENTIFY)
            fpi_device_identify_report (dev, NULL, NULL,
                                        fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));

          self->finger_on_sensor = FALSE;
          g_clear_pointer (&self->raw_buffer, g_free);
          self->raw_buffer_len = 0;
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
                                          fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
              self->finger_on_sensor = FALSE;
              start_finger_detection (self);
            }
          else if (tstatus != FTE3600_TEMPLATE_OK && tstatus != FTE3600_TEMPLATE_NEED_MORE_SAMPLES)
            {
              fp_dbg ("Enrollment template rejected feature set (%d)", tstatus);
              fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                          fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
              self->finger_on_sensor = FALSE;
              start_finger_detection (self);
            }
          else
            {
              self->enroll_stage++;
              fp_dbg ("Enrolled stage %u of %u", self->enroll_stage, (guint) NR_ENROLL_STAGES);

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

                      fpi_device_enroll_complete (dev, print, NULL);
                    }
                  else
                    {
                      fpi_device_enroll_complete (dev, NULL,
                                                  fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
                    }

                  g_clear_pointer (&self->enroll_template, fpi_fte3600_template_free);
                  self->enroll_stage = 0;
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

          Fte3600TemplateCompareResult comp_res;
          Fte3600TemplateStatus tstatus;

          tstatus = fpi_fte3600_template_compare_features (self->verify_template,
                                                           &features,
                                                           FTE3600_TEMPLATE_LOAD_AUTHENTICATION,
                                                           &comp_res);

          gboolean match = (tstatus == FTE3600_TEMPLATE_OK && comp_res.authentication_accepted);
          fp_info ("Verify result: %s (inliers: %u, median_err: %.2f)",
                   match ? "MATCH" : "NO_MATCH",
                   comp_res.best.inliers,
                   comp_res.best.median_residual);

          fpi_device_verify_report (dev,
                                    match ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                                    print, NULL);
          fpi_device_verify_complete (dev, NULL);

          g_clear_pointer (&self->verify_template, fpi_fte3600_template_free);
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
                  GVariant *data_var = NULL;
                  g_object_get (p, "fpi-data", &data_var, NULL);
                  if (data_var == NULL)
                    continue;

                  gsize data_len = 0;
                  gconstpointer data = g_variant_get_fixed_array (data_var, &data_len, 1);
                  g_autoptr(GBytes) wire = g_bytes_new (data, data_len);
                  g_autoptr(Fte3600Template) templ = NULL;

                  if (fpi_fte3600_template_decode (wire, FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &templ) == FTE3600_TEMPLATE_OK)
                    {
                      Fte3600TemplateCompareResult comp_res;
                      if (fpi_fte3600_template_compare_features (templ, &features,
                                                                 FTE3600_TEMPLATE_LOAD_AUTHENTICATION,
                                                                 &comp_res) == FTE3600_TEMPLATE_OK &&
                          comp_res.authentication_accepted)
                        {
                          matched_print = p;
                          g_variant_unref (data_var);
                          break;
                        }
                    }
                  g_variant_unref (data_var);
                }
            }

          fpi_device_identify_report (dev, matched_print, NULL, NULL);
          fpi_device_identify_complete (dev, NULL);
        }

      g_clear_pointer (&self->raw_buffer, g_free);
      self->raw_buffer_len = 0;
    }
  else
    {
      /* Continue reading bulk chunks */
      FpiUsbTransfer *read_transfer = fpi_usb_transfer_new (dev);
      fpi_usb_transfer_fill_bulk (read_transfer, EP_IN, RAW_IMAGE_SIZE);
      fpi_usb_transfer_submit (read_transfer, 5000, NULL, capture_read_cb, NULL);
    }
}

static void
capture_cmd_cb (FpiUsbTransfer *transfer,
                FpDevice       *dev,
                gpointer        user_data,
                GError         *error)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        fpi_device_action_error (dev, error);
      else
        g_error_free (error);
      return;
    }

  g_clear_pointer (&self->raw_buffer, g_free);
  self->raw_buffer = g_malloc (RAW_IMAGE_SIZE);
  self->raw_buffer_len = 0;

  FpiUsbTransfer *read_transfer = fpi_usb_transfer_new (dev);
  fpi_usb_transfer_fill_bulk (read_transfer, EP_IN, RAW_IMAGE_SIZE);
  fpi_usb_transfer_submit (read_transfer, 5000, NULL, capture_read_cb, NULL);
}

static void
capture_image (FpiDeviceFocaltech0752 *self)
{
  fp_dbg ("Sending capture command to sensor");
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (FP_DEVICE (self));
  fpi_usb_transfer_fill_bulk_full (transfer, EP_OUT, (guint8 *) cmd_capture, CMD_CAPTURE_LEN, NULL);
  fpi_usb_transfer_submit (transfer, 1000, NULL, capture_cmd_cb, NULL);
}

static void
poll_status_cb (FpiUsbTransfer *transfer,
                FpDevice       *dev,
                gpointer        user_data,
                GError         *error)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        fpi_device_action_error (dev, error);
      else
        g_error_free (error);
      return;
    }

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
    self->poll_timeout_id = g_timeout_add (POLL_INTERVAL_MS, poll_timeout_cb, dev);
}

static gboolean
poll_timeout_cb (gpointer user_data)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (user_data);
  self->poll_timeout_id = 0;
  if (!self->deactivating)
    start_finger_detection (self);
  return G_SOURCE_REMOVE;
}

static void
poll_cmd_cb (FpiUsbTransfer *transfer,
             FpDevice       *dev,
             gpointer        user_data,
             GError         *error)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        fpi_device_action_error (dev, error);
      else
        g_error_free (error);
      return;
    }

  if (self->deactivating)
    return;

  FpiUsbTransfer *read_transfer = fpi_usb_transfer_new (dev);
  fpi_usb_transfer_fill_bulk (read_transfer, EP_IN, EP_IN_MAX_BUF_SIZE);
  fpi_usb_transfer_submit (read_transfer, 1000, NULL, poll_status_cb, NULL);
}

static void
start_finger_detection (FpiDeviceFocaltech0752 *self)
{
  if (self->deactivating)
    return;

  FpiUsbTransfer *transfer = fpi_usb_transfer_new (FP_DEVICE (self));
  fpi_usb_transfer_fill_bulk_full (transfer, EP_OUT, (guint8 *) cmd_status_poll, CMD_STATUS_POLL_LEN, NULL);
  fpi_usb_transfer_submit (transfer, 1000, NULL, poll_cmd_cb, NULL);
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

  g_clear_pointer (&self->raw_buffer, g_free);
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
  GVariant *data_var = NULL;

  fp_info ("Starting verification (BRISK host matcher) - place finger on sensor");

  fpi_device_get_verify_data (dev, &print);
  g_object_get (print, "fpi-data", &data_var, NULL);

  if (!data_var)
    {
      fpi_device_verify_complete (dev, fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
      return;
    }

  gsize data_len = 0;
  gconstpointer data = g_variant_get_fixed_array (data_var, &data_len, 1);
  g_autoptr(GBytes) wire = g_bytes_new (data, data_len);
  g_variant_unref (data_var);

  g_clear_pointer (&self->verify_template, fpi_fte3600_template_free);
  Fte3600TemplateStatus status = fpi_fte3600_template_decode (wire,
                                                              FTE3600_TEMPLATE_LOAD_AUTHENTICATION,
                                                              &self->verify_template);

  if (status != FTE3600_TEMPLATE_OK || !fpi_fte3600_template_is_ready (self->verify_template))
    {
      g_clear_pointer (&self->verify_template, fpi_fte3600_template_free);
      fpi_device_verify_complete (dev, fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
      return;
    }

  self->deactivating = FALSE;
  self->finger_on_sensor = FALSE;

  start_finger_detection (self);
}

static void
dev_identify (FpDevice *dev)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);

  fp_info ("Starting identification (BRISK host matcher) - place finger on sensor");

  self->deactivating = FALSE;
  self->finger_on_sensor = FALSE;

  start_finger_detection (self);
}

static void
dev_cancel (FpDevice *dev)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);
  FpiDeviceAction action;
  g_autoptr(GError) error = NULL;

  fp_dbg ("Cancelling in-progress operation");

  self->deactivating = TRUE;

  if (self->poll_timeout_id != 0)
    {
      g_source_remove (self->poll_timeout_id);
      self->poll_timeout_id = 0;
    }

  action = fpi_device_get_current_action (dev);
  error = fpi_device_error_new (FP_DEVICE_ERROR_GENERAL);

  switch (action)
    {
    case FPI_DEVICE_ACTION_ENROLL:
      g_clear_pointer (&self->enroll_template, fpi_fte3600_template_free);
      self->enroll_stage = 0;
      fpi_device_enroll_complete (dev, NULL, error);
      g_steal_pointer (&error);
      break;

    case FPI_DEVICE_ACTION_VERIFY:
      g_clear_pointer (&self->verify_template, fpi_fte3600_template_free);
      fpi_device_verify_complete (dev, error);
      g_steal_pointer (&error);
      break;

    case FPI_DEVICE_ACTION_IDENTIFY:
      fpi_device_identify_complete (dev, error);
      g_steal_pointer (&error);
      break;

    default:
      break;
    }
}

static const FpIdEntry id_table[] = {
  { .vid = FOCALTECH_VENDOR_ID, .pid = FOCALTECH_PRODUCT_ID },
  { .vid = 0, .pid = 0, .driver_data = 0 },
};

static void
fpi_device_focaltech0752_init (FpiDeviceFocaltech0752 *self)
{
  self->raw_buffer = NULL;
  self->raw_buffer_len = 0;
  self->deactivating = FALSE;
  self->finger_on_sensor = FALSE;
  self->poll_timeout_id = 0;
  self->enroll_template = NULL;
  self->enroll_stage = 0;
  self->verify_template = NULL;
}

static void
fpi_device_focaltech0752_finalize (GObject *object)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (object);

  if (self->poll_timeout_id != 0)
    g_source_remove (self->poll_timeout_id);

  g_clear_pointer (&self->raw_buffer, g_free);
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
