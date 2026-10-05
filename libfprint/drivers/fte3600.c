/*
 * FocalTech FTE3600 sensor-family SPI fingerprint driver
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define FP_COMPONENT "fte3600"

#include "fte3600-private.h"
#include "fte3600-template.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

G_DEFINE_TYPE (FpiDeviceFte3600, fpi_device_fte3600, FP_TYPE_DEVICE);

typedef struct
{
  const Fte3600MatchProfile *profile;
  guint8                    *data;
  FpiBriskImage              view;
} Fte3600MatchInput;

typedef struct
{
  Fte3600MatchInput     input;
  Fte3600Template      *enroll_template;
  Fte3600TemplateStatus status;
  Fte3600TemplateStatus encode_status;
  Fte3600BriskStatus    extract_status;
  GBytes               *encoded_template;
} Fte3600EnrollJob;

#if FTE3600_ENABLE_PERSONAL_AUTH
typedef struct
{
  GBytes                    *wire;
  const Fte3600MatchProfile *profile;
} Fte3600VerifyLoad;

typedef struct
{
  Fte3600MatchInput            input;
  Fte3600Template             *verify_template;
  Fte3600BriskStatus           extract_status;
  Fte3600IpaStatus             ipa_extract_status;
  Fte3600TemplateStatus        compare_status;
  Fte3600EngineMode            engine_mode;
  Fte3600TemplateCompareResult comparison;
} Fte3600VerifyJob;
#endif

static void fte3600_start_reset (FpiDeviceFte3600   *self,
                                 Fte3600ResetPurpose purpose,
                                 GError             *operation_error);
static void fte3600_complete_action_error (FpiDeviceFte3600 *self,
                                           GError           *error);

void
fpi_fte3600_clear_captured_image (FpiDeviceFte3600 *self)
{
  gsize image_size;

  if (self->captured_image == NULL)
    return;

  image_size = (gsize) self->captured_image->width *
               self->captured_image->height;
  if (self->captured_image->data != NULL)
    fpi_fte3600_secure_clear (self->captured_image->data, image_size);
  g_clear_object (&self->captured_image);
}

static const Fte3600MatchProfile *
fte3600_device_match_profile (FpiDeviceFte3600 *self)
{
  const Fte3600MatchProfile *profile;

  if (!self->sensor || !(self->sensor->capabilities & FTE3600_SENSOR_CAP_CAPTURE))
    return NULL;
  profile = fpi_fte3600_match_profile_get (self->sensor->sensor);
  if (!profile || profile->width != self->sensor->width ||
      profile->height != self->sensor->height)
    return NULL;
  return profile;
}

static void
fte3600_match_input_clear (Fte3600MatchInput *input)
{
  if (input->data)
    fpi_fte3600_secure_clear (input->data, input->view.length);
  g_clear_pointer (&input->data, g_free);
  input->view = (FpiBriskImage){ 0 };
  input->profile = NULL;
}

/* A worker owns an exact-sized copy and immutable profile. It must not read
 * device state after dispatch, including when cancellation closes the device. */
static gboolean
fte3600_match_input_take (FpiDeviceFte3600  *self,
                          Fte3600MatchInput *input,
                          GError           **error)
{
  const Fte3600MatchProfile *profile = fte3600_device_match_profile (self);

  if (!profile || !self->captured_image || !self->captured_image->data ||
      self->captured_image->width != profile->width ||
      self->captured_image->height != profile->height ||
      self->image_size != (gsize) profile->width * profile->height)
    {
      g_set_error_literal (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                           "FTE3600 captured image does not match its sensor profile");
      return FALSE;
    }
  input->profile = profile;
  input->data = g_memdup2 (self->captured_image->data, self->image_size);
  input->view = (FpiBriskImage){ input->data, self->image_size,
                                 profile->width, profile->height, profile->width };
  fpi_fte3600_clear_captured_image (self);
  return TRUE;
}

static void
fte3600_finish_open_error (FpiDeviceFte3600 *self, GError *error)
{
  g_autoptr(GError) cleanup = NULL;

  if (!fpi_fte3600_transport_close (self, &cleanup) &&
      !g_error_matches (cleanup, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE))
    fp_warn ("Failed to release FTE3600 transport after open failure: %s", cleanup->message);
  fpi_fte3600_release_transport (self);
  fpi_device_open_complete (FP_DEVICE (self), error);
}

static void
fte3600_init_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);

  g_clear_pointer (&self->firmware_bytes, g_bytes_unref);
  if (error)
    {
      self->idle_verified = FALSE;
      fpi_fte3600_deassert_hardware_reset_best_effort (
        self, "recovering from initialization failure");
      /* A failed recovery or conflicting chip identity invalidates runtime
       * commands. Do not send another protocol reset after such a failure. */
      if (self->session_failed)
        fte3600_finish_open_error (self, error);
      else
        fte3600_start_reset (self, FTE3600_RESET_FOR_OPEN_ERROR, error);
      return;
    }

  fpi_device_open_complete (dev, NULL);
}

static void fte3600_start_capture (FpiDeviceFte3600 *self);

static GError *
fte3600_enroll_retry_error (Fte3600TemplateStatus status)
{
  switch (status)
    {
    case FTE3600_TEMPLATE_RETRY_LOW_CONTRAST:
      return fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL);

    case FTE3600_TEMPLATE_RETRY_INSUFFICIENT_FEATURES:
      return fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER);

    case FTE3600_TEMPLATE_RETRY_DUPLICATE:
      return fpi_device_retry_new (FP_DEVICE_RETRY_REMOVE_FINGER);

    case FTE3600_TEMPLATE_RETRY_INCONSISTENT:
      return fpi_device_retry_new_msg (
        FP_DEVICE_RETRY_CENTER_FINGER,
        "Place the same finger near the previously accepted position");

    case FTE3600_TEMPLATE_OK:
    case FTE3600_TEMPLATE_NEED_MORE_SAMPLES:
    case FTE3600_TEMPLATE_INVALID_WIRE:
    case FTE3600_TEMPLATE_UNSUPPORTED_SCHEMA:
    case FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR:
    case FTE3600_TEMPLATE_UNSUPPORTED_POLICY:
    case FTE3600_TEMPLATE_NOT_CALIBRATED:
      g_assert_not_reached ();
    }

  g_assert_not_reached ();
}

static GError *
fte3600_enroll_fatal_error (const Fte3600EnrollJob *job)
{
  switch (job->status)
    {
    case FTE3600_TEMPLATE_INVALID_WIRE:
      return fpi_device_error_new_msg (
        FP_DEVICE_ERROR_DATA_INVALID,
        "FTE3600 BRISK enrollment data was invalid (extract %u, template %u)",
        (guint) job->extract_status, (guint) job->status);

    case FTE3600_TEMPLATE_UNSUPPORTED_SCHEMA:
    case FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR:
    case FTE3600_TEMPLATE_UNSUPPORTED_POLICY:
    case FTE3600_TEMPLATE_NOT_CALIBRATED:
      return fpi_device_error_new_msg (
        FP_DEVICE_ERROR_NOT_SUPPORTED,
        "FTE3600 BRISK enrollment format is unsupported (template %u)",
        (guint) job->status);

    case FTE3600_TEMPLATE_OK:
    case FTE3600_TEMPLATE_NEED_MORE_SAMPLES:
    case FTE3600_TEMPLATE_RETRY_LOW_CONTRAST:
    case FTE3600_TEMPLATE_RETRY_INSUFFICIENT_FEATURES:
    case FTE3600_TEMPLATE_RETRY_DUPLICATE:
    case FTE3600_TEMPLATE_RETRY_INCONSISTENT:
      g_assert_not_reached ();
    }

  g_assert_not_reached ();
}

static Fte3600TemplateStatus
fte3600_extract_status_to_template_status (Fte3600BriskStatus status)
{
  switch (status)
    {
    case FTE3600_BRISK_OK:
      g_assert_not_reached ();

    case FTE3600_BRISK_LOW_CONTRAST:
      return FTE3600_TEMPLATE_RETRY_LOW_CONTRAST;

    case FTE3600_BRISK_INSUFFICIENT_FEATURES:
      return FTE3600_TEMPLATE_RETRY_INSUFFICIENT_FEATURES;

    case FTE3600_BRISK_NO_CONSENSUS:
    case FTE3600_BRISK_INVALID_ARGUMENT:
      return FTE3600_TEMPLATE_INVALID_WIRE;
    }

  g_assert_not_reached ();
}

static void
fte3600_enroll_job_free (Fte3600EnrollJob *job)
{
  if (job == NULL)
    return;

  g_clear_pointer (&job->enroll_template, fpi_fte3600_template_free);
  g_clear_pointer (&job->encoded_template, g_bytes_unref);
  fte3600_match_input_clear (&job->input);
  g_free (job);
}

static void
fte3600_enroll_worker (GTask        *task,
                       gpointer      source_object,
                       gpointer      task_data,
                       GCancellable *cancellable)
{
  Fte3600EnrollJob *job = task_data;
  Fte3600BriskFeatureSet features = { 0 };
  Fte3600IpaFeatureSet ipa_features = { 0 };
  const Fte3600IpaFeatureSet *p_ipa = NULL;

  (void) source_object;
  (void) cancellable;

  if (g_task_return_error_if_cancelled (task))
    goto out;

  job->extract_status =
    fpi_fte3600_brisk_extract_for_profile (job->input.profile, &job->input.view, &features);
#if FTE3600_ENABLE_IPA_AUTH
  if (job->input.profile->sensor == FTE3600_SENSOR_FT9361 &&
      fpi_fte3600_ipa_extract (job->input.view.data, job->input.view.length, &ipa_features) == FTE3600_IPA_OK)
    p_ipa = &ipa_features;
#endif

  fte3600_match_input_clear (&job->input);
  if (g_task_return_error_if_cancelled (task))
    goto out;

  if (job->extract_status != FTE3600_BRISK_OK)
    {
      job->status =
        fte3600_extract_status_to_template_status (job->extract_status);
    }
  else
    {
      job->status = fpi_fte3600_template_add_dual_features (job->enroll_template,
                                                            &features,
                                                            p_ipa,
                                                            NULL);
      if (g_task_return_error_if_cancelled (task))
        goto out;

      if (job->status == FTE3600_TEMPLATE_OK)
        {
          job->encode_status =
            fpi_fte3600_template_encode (job->enroll_template,
                                         &job->encoded_template);
        }
    }

  if (!g_task_return_error_if_cancelled (task))
    g_task_return_boolean (task, TRUE);

out:
  fpi_fte3600_secure_clear (&features, sizeof (features));
  fpi_fte3600_secure_clear (&ipa_features, sizeof (ipa_features));
}

static void
fte3600_enroll_process (FpiDeviceFte3600 *self,
                        Fte3600EnrollJob *job)
{
  FpDevice *dev = FP_DEVICE (self);
  const guint completed_stages = self->enroll_stages_passed + 1;

  if (job->status == FTE3600_TEMPLATE_OK &&
      job->encode_status != FTE3600_TEMPLATE_OK)
    {
      fte3600_complete_action_error (
        self, fpi_device_error_new_msg (
          FP_DEVICE_ERROR_DATA_INVALID,
          "FTE3600 encoder rejected a completed template (%u)",
          (guint) job->encode_status));
      return;
    }

  switch (job->status)
    {
    case FTE3600_TEMPLATE_RETRY_LOW_CONTRAST:
    case FTE3600_TEMPLATE_RETRY_INSUFFICIENT_FEATURES:
    case FTE3600_TEMPLATE_RETRY_DUPLICATE:
    case FTE3600_TEMPLATE_RETRY_INCONSISTENT:
      fp_info ("Enrollment sample rejected by BRISK/template (%u/%u)",
               (guint) job->extract_status, (guint) job->status);
      fpi_device_enroll_progress (dev, self->enroll_stages_passed, NULL,
                                  fte3600_enroll_retry_error (job->status));
      fte3600_start_capture (self);
      return;

    case FTE3600_TEMPLATE_INVALID_WIRE:
    case FTE3600_TEMPLATE_UNSUPPORTED_SCHEMA:
    case FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR:
    case FTE3600_TEMPLATE_UNSUPPORTED_POLICY:
    case FTE3600_TEMPLATE_NOT_CALIBRATED:
      fte3600_complete_action_error (self,
                                     fte3600_enroll_fatal_error (job));
      return;

    case FTE3600_TEMPLATE_NEED_MORE_SAMPLES:
      if (completed_stages >= FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES ||
          job->encoded_template != NULL)
        {
          fte3600_complete_action_error (
            self, fpi_device_error_new_msg (
              FP_DEVICE_ERROR_DATA_INVALID,
              "FTE3600 template did not finish at its declared stage"));
          return;
        }
      break;

    case FTE3600_TEMPLATE_OK:
      if (completed_stages != FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES ||
          job->encoded_template == NULL)
        {
          fte3600_complete_action_error (
            self, fpi_device_error_new_msg (
              FP_DEVICE_ERROR_DATA_INVALID,
              "FTE3600 template finished at an unexpected stage"));
          return;
        }
      break;
    }

  g_clear_pointer (&self->enroll_template, fpi_fte3600_template_free);
  self->enroll_template = g_steal_pointer (&job->enroll_template);
  self->enroll_stages_passed = completed_stages;
  fpi_device_enroll_progress (dev, self->enroll_stages_passed, NULL, NULL);

  const Fte3600BriskFeatureSet *mosaic =
    fpi_fte3600_template_get_mosaic (self->enroll_template);
  if (mosaic != NULL)
    fp_dbg ("Stitched mosaic now contains %u fused features", mosaic->n_features);

  if (job->status == FTE3600_TEMPLATE_NEED_MORE_SAMPLES)
    {
      fte3600_start_capture (self);
      return;
    }

  {
    g_autoptr(GVariant) data = NULL;
    FpPrint *print = NULL;
    const guint8 *wire_data;
    gsize wire_size;

    wire_data = g_bytes_get_data (job->encoded_template, &wire_size);
    if (wire_data == NULL || wire_size < FTE3600_TEMPLATE_WIRE_HEADER_SIZE ||
        wire_size > FTE3600_TEMPLATE_V3_CURRENT_MAX_WIRE_SIZE)
      {
        fte3600_complete_action_error (
          self, fpi_device_error_new_msg (
            FP_DEVICE_ERROR_DATA_INVALID,
            "FTE3600 encoder produced an invalid template length"));
        return;
      }

    data = g_variant_ref_sink (
      g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, wire_data, wire_size,
                                 sizeof (*wire_data)));
    g_assert (g_variant_is_of_type (data, G_VARIANT_TYPE ("ay")));
    fpi_device_get_enroll_data (dev, &print);
    fpi_print_set_type (print, FPI_PRINT_RAW);
    g_object_set (print, "fpi-data", data, NULL);

    g_clear_pointer (&self->enroll_template, fpi_fte3600_template_free);
    self->enroll_needs_release = FALSE;
    self->enroll_stages_passed = 0;
    self->armed = FALSE;
    fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
  }
}

static void
fte3600_enroll_complete (GObject      *source_object,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (source_object);
  GTask *task = G_TASK (result);
  Fte3600EnrollJob *job = g_task_get_task_data (task);

  g_autoptr(GError) error = NULL;

  (void) user_data;

  if (!g_task_propagate_boolean (task, &error))
    {
      fte3600_complete_action_error (self, g_steal_pointer (&error));
      return;
    }

  fte3600_enroll_process (self, job);
}

static void
fte3600_enroll_capture_async (FpiDeviceFte3600 *self)
{
  FpDevice *dev = FP_DEVICE (self);
  Fte3600EnrollJob *job;

  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_ENROLL);
  g_assert (self->captured_image != NULL);

  job = g_new0 (Fte3600EnrollJob, 1);
  if (!fte3600_match_input_take (self, &job->input, &error))
    {
      fte3600_enroll_job_free (job);
      fte3600_complete_action_error (self, g_steal_pointer (&error));
      return;
    }
  job->enroll_template = fpi_fte3600_template_copy (self->enroll_template);
  if (job->enroll_template == NULL ||
      fpi_fte3600_template_get_profile (job->enroll_template) != job->input.profile)
    {
      fte3600_enroll_job_free (job);
      fte3600_complete_action_error (
        self, fpi_device_error_new_msg (
          FP_DEVICE_ERROR_DATA_INVALID,
          "FTE3600 enrollment template does not match its sensor profile"));
      return;
    }

  task = g_task_new (self, fpi_device_get_cancellable (dev),
                     fte3600_enroll_complete, NULL);
  g_task_set_task_data (task, job, (GDestroyNotify) fte3600_enroll_job_free);
  g_task_set_return_on_cancel (task, FALSE);
  g_task_run_in_thread (task, fte3600_enroll_worker);
}

#if FTE3600_ENABLE_PERSONAL_AUTH
static void
fte3600_verify_load_free (Fte3600VerifyLoad *load)
{
  g_clear_pointer (&load->wire, g_bytes_unref);
  g_free (load);
}

static GError *
fte3600_verify_template_error (Fte3600TemplateStatus status)
{
  switch (status)
    {
    case FTE3600_TEMPLATE_INVALID_WIRE:
    case FTE3600_TEMPLATE_NEED_MORE_SAMPLES:
    case FTE3600_TEMPLATE_RETRY_LOW_CONTRAST:
    case FTE3600_TEMPLATE_RETRY_INSUFFICIENT_FEATURES:
    case FTE3600_TEMPLATE_RETRY_DUPLICATE:
    case FTE3600_TEMPLATE_RETRY_INCONSISTENT:
      return fpi_device_error_new_msg (
        FP_DEVICE_ERROR_DATA_INVALID,
        "FTE3600 verification template or query was invalid (%u)",
        (guint) status);

    case FTE3600_TEMPLATE_UNSUPPORTED_SCHEMA:
    case FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR:
    case FTE3600_TEMPLATE_NOT_CALIBRATED:
      return fpi_device_error_new_msg (
        FP_DEVICE_ERROR_NOT_SUPPORTED,
        "FTE3600 verification template policy is unsupported (%u)",
        (guint) status);

    case FTE3600_TEMPLATE_UNSUPPORTED_POLICY:
      return fpi_device_error_new_msg (
        FP_DEVICE_ERROR_NOT_SUPPORTED,
        "The stored fingerprint uses an unsupported matching policy; enroll the finger again");

    case FTE3600_TEMPLATE_OK:
      g_assert_not_reached ();
    }

  g_assert_not_reached ();
}

static GError *
fte3600_verify_get_wire (FpiDeviceFte3600 *self,
                         GBytes          **wire)
{
  FpDevice *dev = FP_DEVICE (self);
  FpPrint *print = NULL;

  g_autoptr(GVariant) data = NULL;
  const guint8 *wire_data;
  gsize wire_size = 0;

  fpi_device_get_verify_data (dev, &print);
  if (print == NULL || !fp_print_compatible (print, dev) ||
      fpi_print_get_type (print) != FPI_PRINT_RAW)
    return fpi_device_error_new_msg (
      FP_DEVICE_ERROR_DATA_INVALID,
      "FTE3600 verification requires a compatible raw template");

  g_object_get (print, "fpi-data", &data, NULL);
  if (data == NULL ||
      !g_variant_is_of_type (data, G_VARIANT_TYPE ("ay")) ||
      !g_variant_is_normal_form (data))
    return fpi_device_error_new_msg (
      FP_DEVICE_ERROR_DATA_INVALID,
      "FTE3600 verification template has an invalid container");

  wire_data = g_variant_get_fixed_array (data, &wire_size,
                                         sizeof (*wire_data));
  if (wire_data == NULL || wire_size < FTE3600_TEMPLATE_WIRE_HEADER_SIZE ||
      wire_size > FTE3600_TEMPLATE_V3_CURRENT_MAX_WIRE_SIZE)
    return fpi_device_error_new_msg (
      FP_DEVICE_ERROR_DATA_INVALID,
      "FTE3600 verification template has an invalid length");

  *wire = g_bytes_new (wire_data, wire_size);
  return NULL;
}

static void
fte3600_verify_load_worker (GTask        *task,
                            gpointer      source_object,
                            gpointer      task_data,
                            GCancellable *cancellable)
{
  const Fte3600VerifyLoad *load = task_data;

  g_autoptr(Fte3600Template) templ = NULL;
  Fte3600TemplateStatus status;

  (void) source_object;
  (void) cancellable;

  if (g_task_return_error_if_cancelled (task))
    return;

  /* Reconstruction can be expensive. Only task-owned data is touched here,
   * so cancellation may finish the action while this worker unwinds. */
  status = fpi_fte3600_template_decode (
    load->wire, FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &templ);
  if (g_task_return_error_if_cancelled (task))
    return;
  if (status != FTE3600_TEMPLATE_OK)
    {
      g_task_return_error (task, fte3600_verify_template_error (status));
      return;
    }
  if (!fpi_fte3600_template_is_ready (templ))
    {
      g_task_return_error (task, fpi_device_error_new_msg (
                             FP_DEVICE_ERROR_DATA_INVALID,
                             "FTE3600 verification template was incomplete"));
      return;
    }
  if (fpi_fte3600_template_get_profile (templ) != load->profile)
    {
      g_task_return_error (task, fpi_device_error_new_msg (
                             FP_DEVICE_ERROR_DATA_INVALID,
                             "FTE3600 verification template belongs to a different sensor profile"));
      return;
    }

  g_task_return_pointer (task, g_steal_pointer (&templ),
                         (GDestroyNotify) fpi_fte3600_template_free);
}

static void
fte3600_verify_load_complete (GObject      *source_object,
                              GAsyncResult *result,
                              gpointer      user_data)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (source_object);

  g_autoptr(GError) error = NULL;

  (void) user_data;

  self->verify_template = g_task_propagate_pointer (G_TASK (result), &error);
  if (error != NULL)
    {
      fpi_device_verify_complete (FP_DEVICE (self), g_steal_pointer (&error));
      return;
    }
  fte3600_start_capture (self);
}

static void
fte3600_verify_job_free (Fte3600VerifyJob *job)
{
  if (job == NULL)
    return;

  g_clear_pointer (&job->verify_template, fpi_fte3600_template_free);
  fte3600_match_input_clear (&job->input);
  fpi_fte3600_secure_clear (&job->comparison, sizeof (job->comparison));
  g_free (job);
}

static void
fte3600_verify_worker (GTask        *task,
                       gpointer      source_object,
                       gpointer      task_data,
                       GCancellable *cancellable)
{
  Fte3600VerifyJob *job = task_data;
  Fte3600BriskFeatureSet features = { 0 };
  Fte3600IpaFeatureSet ipa_features = { 0 };
  const Fte3600IpaFeatureSet *p_ipa = NULL;

  (void) source_object;
  (void) cancellable;

  if (g_task_return_error_if_cancelled (task))
    goto out;

  if (!fpi_fte3600_engine_mode_parse (g_getenv ("FP_FTE3600_MATCHER"),
                                      &job->engine_mode))
    {
      g_task_return_error (task, fpi_device_error_new_msg (
                             FP_DEVICE_ERROR_NOT_SUPPORTED, "Unknown FTE3600 matcher mode"));
      goto out;
    }
  if (job->engine_mode != FTE3600_ENGINE_MODE_BRISK_ONLY && !FTE3600_ENABLE_IPA_AUTH)
    {
      g_task_return_error (task, fpi_device_error_new_msg (
                             FP_DEVICE_ERROR_NOT_SUPPORTED,
                             "IPA authentication requires its separate experimental build opt-in"));
      goto out;
    }

  job->extract_status = FTE3600_BRISK_INSUFFICIENT_FEATURES;
  job->ipa_extract_status = FTE3600_IPA_ERR_TOO_FEW_POINTS;

  if (job->engine_mode != FTE3600_ENGINE_MODE_IPA_ONLY)
    job->extract_status =
      fpi_fte3600_brisk_extract_for_profile (job->input.profile, &job->input.view, &features);

  if (job->engine_mode != FTE3600_ENGINE_MODE_BRISK_ONLY &&
      job->input.profile->sensor == FTE3600_SENSOR_FT9361)
    {
      job->ipa_extract_status =
        fpi_fte3600_ipa_extract (job->input.view.data, job->input.view.length, &ipa_features);
      if (job->ipa_extract_status == FTE3600_IPA_OK)
        p_ipa = &ipa_features;
    }

  if (g_task_return_error_if_cancelled (task))
    goto out;

  if (job->extract_status == FTE3600_BRISK_INVALID_ARGUMENT ||
      job->ipa_extract_status == FTE3600_IPA_ERR_PARAM)
    {
      job->compare_status = FTE3600_TEMPLATE_INVALID_WIRE;
    }
  else
    {
      job->compare_status = fpi_fte3600_template_compare_with_mode (
        job->verify_template,
        job->extract_status == FTE3600_BRISK_OK ? &features : NULL,
        p_ipa, FTE3600_TEMPLATE_LOAD_AUTHENTICATION,
        job->engine_mode, &job->comparison);
    }

  if (!g_task_return_error_if_cancelled (task))
    g_task_return_boolean (task, TRUE);

out:
  fte3600_match_input_clear (&job->input);
  fpi_fte3600_secure_clear (&features, sizeof (features));
  fpi_fte3600_secure_clear (&ipa_features, sizeof (ipa_features));
}

static void
fte3600_verify_report_retry (FpiDeviceFte3600 *self,
                             FpDeviceRetry     retry)
{
  FpDevice *dev = FP_DEVICE (self);

  fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL,
                            fpi_device_retry_new (retry));
  fpi_device_verify_complete (dev, NULL);
}

static void
fte3600_verify_complete (GObject      *source_object,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (source_object);
  FpDevice *dev = FP_DEVICE (self);
  GTask *task = G_TASK (result);
  Fte3600VerifyJob *job = g_task_get_task_data (task);

  g_autoptr(GError) error = NULL;

  (void) user_data;

  if (!g_task_propagate_boolean (task, &error))
    {
      fte3600_complete_action_error (self, g_steal_pointer (&error));
      return;
    }

  /* 1. If authentication was accepted, report SUCCESS immediately */
  if (job->compare_status == FTE3600_TEMPLATE_OK && job->comparison.authentication_accepted)
    {
      fp_dbg ("Personal verification compared %u references; diagnostic passes %u -> MATCH",
              job->comparison.n_compared, job->comparison.diagnostic_passes);
      fpi_device_verify_report (dev, FPI_MATCH_SUCCESS, NULL, NULL);
      fpi_device_verify_complete (dev, NULL);
      return;
    }

  /* 2. If comparison executed cleanly but was rejected, report NO_MATCH */
  if (job->compare_status == FTE3600_TEMPLATE_OK)
    {
      fp_dbg ("Personal verification compared %u references; diagnostic passes %u -> NO_MATCH",
              job->comparison.n_compared, job->comparison.diagnostic_passes);
      fpi_device_verify_report (dev, FPI_MATCH_FAIL, NULL, NULL);
      fpi_device_verify_complete (dev, NULL);
      return;
    }

  /* 3. Check extractor status first for capture quality issues (e.g. low contrast) */
  switch (job->extract_status)
    {
    case FTE3600_BRISK_LOW_CONTRAST:
    case FTE3600_BRISK_NO_CONSENSUS:
      fte3600_verify_report_retry (self, FP_DEVICE_RETRY_GENERAL);
      return;

    case FTE3600_BRISK_INSUFFICIENT_FEATURES:
      fte3600_verify_report_retry (self, FP_DEVICE_RETRY_CENTER_FINGER);
      return;

    case FTE3600_BRISK_INVALID_ARGUMENT:
      fte3600_complete_action_error (
        self, fpi_device_error_new_msg (
          FP_DEVICE_ERROR_DATA_INVALID,
          "FTE3600 extractor rejected a verification image"));
      return;

    case FTE3600_BRISK_OK:
      break;
    }

  if (job->compare_status == FTE3600_TEMPLATE_RETRY_INSUFFICIENT_FEATURES)
    {
      fte3600_verify_report_retry (self, FP_DEVICE_RETRY_CENTER_FINGER);
      return;
    }

  if (job->compare_status != FTE3600_TEMPLATE_OK)
    {
      fte3600_complete_action_error (
        self, fte3600_verify_template_error (job->compare_status));
      return;
    }
}

static void
fte3600_verify_capture_async (FpiDeviceFte3600 *self)
{
  FpDevice *dev = FP_DEVICE (self);
  Fte3600VerifyJob *job;

  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY);
  g_assert (self->captured_image != NULL);

  job = g_new0 (Fte3600VerifyJob, 1);
  job->extract_status = FTE3600_BRISK_INVALID_ARGUMENT;
  job->compare_status = FTE3600_TEMPLATE_INVALID_WIRE;
  if (!fte3600_match_input_take (self, &job->input, &error))
    {
      fte3600_verify_job_free (job);
      fte3600_complete_action_error (self, g_steal_pointer (&error));
      return;
    }
  job->verify_template = g_steal_pointer (&self->verify_template);
  if (job->verify_template == NULL)
    {
      fte3600_verify_job_free (job);
      fte3600_complete_action_error (
        self, fpi_device_error_new_msg (
          FP_DEVICE_ERROR_DATA_INVALID,
          "FTE3600 verification template state was missing"));
      return;
    }

  task = g_task_new (self, fpi_device_get_cancellable (dev),
                     fte3600_verify_complete, NULL);
  g_task_set_task_data (task, job, (GDestroyNotify) fte3600_verify_job_free);
  g_task_set_return_on_cancel (task, FALSE);
  g_task_run_in_thread (task, fte3600_verify_worker);
}
#endif

static void
fte3600_complete_action_error (FpiDeviceFte3600 *self, GError *error)
{
  FpDevice *dev = FP_DEVICE (self);

  if (self->armed && self->spi_fd >= 0 && !self->capturing && !self->session_failed)
    {
      fte3600_start_reset (self, FTE3600_RESET_FOR_ACTION_ERROR, error);
      return;
    }

  self->armed = FALSE;
  fpi_fte3600_clear_captured_image (self);
  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);

  /* A retry describes a usable sensor with an unsuitable frame. Failed
   * cleanup is terminal; cancellation must not rearm another enrollment scan. */
  if (error && error->domain == FP_DEVICE_RETRY)
    {
      GCancellable *cancellable = fpi_device_get_cancellable (dev);

      if (!self->idle_verified || self->session_failed)
        {
          g_clear_error (&error);
          error = fpi_device_error_new_msg (
            FP_DEVICE_ERROR_PROTO, "FTE3600 capture retry left no verified sensor state");
        }
      else if (cancellable && g_cancellable_is_cancelled (cancellable))
        {
          g_clear_error (&error);
          g_cancellable_set_error_if_cancelled (cancellable, &error);
        }
    }

  switch (fpi_device_get_current_action (dev))
    {
    case FPI_DEVICE_ACTION_ENROLL:
      if (error && error->domain == FP_DEVICE_RETRY)
        {
          fpi_device_enroll_progress (dev, self->enroll_stages_passed, NULL, error);
          fte3600_start_capture (self);
          return;
        }
      self->enroll_stages_passed = 0;
      self->enroll_needs_release = FALSE;
      g_clear_pointer (&self->enroll_template, fpi_fte3600_template_free);
      fpi_device_enroll_complete (dev, NULL, error);
      return;

    case FPI_DEVICE_ACTION_CAPTURE:
      fpi_device_capture_complete (dev, NULL, error);
      return;

    case FPI_DEVICE_ACTION_VERIFY:
      g_clear_pointer (&self->verify_template, fpi_fte3600_template_free);
      if (error && error->domain == FP_DEVICE_RETRY)
        {
          fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL, error);
          fpi_device_verify_complete (dev, NULL);
          return;
        }
      fpi_device_verify_complete (dev, error);
      return;

    case FPI_DEVICE_ACTION_NONE:
    case FPI_DEVICE_ACTION_PROBE:
    case FPI_DEVICE_ACTION_OPEN:
    case FPI_DEVICE_ACTION_CLOSE:
    case FPI_DEVICE_ACTION_IDENTIFY:
    case FPI_DEVICE_ACTION_LIST:
    case FPI_DEVICE_ACTION_DELETE:
    case FPI_DEVICE_ACTION_CLEAR_STORAGE:
      fpi_device_action_error (dev, error);
      return;
    }
}

static void
fte3600_capture_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);

  fpi_fte3600_clear_irq_source (self);
  self->capturing = FALSE;
  /* Enrollment may have successfully rearmed for the next sample. Otherwise
   * terminal cleanup must establish idle before this session is reused. */
  if (!self->idle_verified && !self->armed)
    {
      self->session_failed = TRUE;
      if (!error || error->domain == FP_DEVICE_RETRY)
        {
          g_clear_error (&error);
          error = fpi_device_error_new_msg (
            FP_DEVICE_ERROR_PROTO, "FTE3600 capture left no verified sensor state");
        }
    }
  if (error)
    {
      fpi_fte3600_secure_clear (self->capture_rx, self->capture_frame_size);
      fte3600_complete_action_error (self, error);
      return;
    }

  g_assert (self->captured_image != NULL);
  if (self->sensor->image_ppmm > 0.0)
    self->captured_image->ppmm = self->sensor->image_ppmm;
  switch (fpi_device_get_current_action (dev))
    {
    case FPI_DEVICE_ACTION_ENROLL:
      self->enroll_needs_release = TRUE;
      fte3600_enroll_capture_async (self);
      return;

    case FPI_DEVICE_ACTION_CAPTURE:
      self->armed = FALSE;
      fpi_device_capture_complete (
        dev, g_steal_pointer (&self->captured_image), NULL);
      return;

    case FPI_DEVICE_ACTION_VERIFY:
#if FTE3600_ENABLE_PERSONAL_AUTH
      fte3600_verify_capture_async (self);
      return;
#else
      g_assert_not_reached ();
#endif

    case FPI_DEVICE_ACTION_NONE:
    case FPI_DEVICE_ACTION_PROBE:
    case FPI_DEVICE_ACTION_OPEN:
    case FPI_DEVICE_ACTION_CLOSE:
    case FPI_DEVICE_ACTION_IDENTIFY:
    case FPI_DEVICE_ACTION_LIST:
    case FPI_DEVICE_ACTION_DELETE:
    case FPI_DEVICE_ACTION_CLEAR_STORAGE:
      fpi_fte3600_clear_captured_image (self);
      fpi_device_action_error (
        dev, fpi_device_error_new_msg (
          FP_DEVICE_ERROR_GENERAL,
          "Unexpected action completed an FTE3600 capture"));
      return;
    }
}

static void
fte3600_reset_data_free (Fte3600ResetData *data)
{
  g_clear_error (&data->operation_error);
  g_free (data);
}

static void
fte3600_reset_complete (FpiSsm *ssm, FpDevice *dev, GError *reset_error)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  Fte3600ResetData *data = fpi_ssm_get_data (ssm);
  GError *operation_error = g_steal_pointer (&data->operation_error);

  switch (data->purpose)
    {
    case FTE3600_RESET_FOR_OPEN_ERROR:
      if (reset_error)
        {
          if (!g_error_matches (reset_error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE))
            fp_warn ("Sensor reset after open failure also failed: %s",
                     reset_error->message);
          g_clear_error (&reset_error);
        }
      fte3600_finish_open_error (self, operation_error);
      return;

    case FTE3600_RESET_FOR_CLOSE:
      g_clear_error (&operation_error);
      {
        g_autoptr(GError) cleanup = NULL;
        if (!fpi_fte3600_transport_close (self, &cleanup) && !reset_error)
          reset_error = g_steal_pointer (&cleanup);
      }
      fpi_fte3600_release_transport (self);
      fpi_device_close_complete (dev, reset_error);
      return;

    case FTE3600_RESET_FOR_ACTION_ERROR:
      if (!self->idle_verified)
        self->session_failed = TRUE;
      if (reset_error)
        {
          if (!g_error_matches (reset_error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE))
            fp_warn ("Sensor reset after action failure also failed: %s",
                     reset_error->message);
          g_clear_error (&reset_error);
        }
      fte3600_complete_action_error (self, operation_error);
      return;
    }

  g_assert_not_reached ();
}

static void
fte3600_reset_wrapper (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  Fte3600ResetData *data = fpi_ssm_get_data (ssm);

  /* A backend owns its child's state/data. Completion metadata belongs to
   * this parent so a backend's private reset context cannot be overwritten. */
  if (data->purpose == FTE3600_RESET_FOR_CLOSE && self->backend->create_shutdown)
    fpi_ssm_start_subsm (ssm, self->backend->create_shutdown (self));
  else
    fpi_ssm_start_subsm (ssm, self->backend->create_reset (self));
}

static void
fte3600_start_reset (FpiDeviceFte3600 *self, Fte3600ResetPurpose purpose,
                     GError *operation_error)
{
  Fte3600ResetData *data;
  FpiSsm *ssm;

  self->armed = FALSE;
  fpi_fte3600_clear_irq_source (self);

  data = g_new0 (Fte3600ResetData, 1);
  data->purpose = purpose;
  data->operation_error = operation_error;

  ssm = fpi_ssm_new (FP_DEVICE (self), fte3600_reset_wrapper, 1);
  fpi_ssm_set_data (ssm, data, (GDestroyNotify) fte3600_reset_data_free);
  fpi_ssm_start (ssm, fte3600_reset_complete);
}

static gboolean
fte3600_select_backend (FpiDeviceFte3600 *self, GError **error)
{
  const Fte3600SensorDescriptor *sensor = fpi_fte3600_sensor_get (self->identity.sensor);
  const Fte3600Backend *backend = fpi_fte3600_backend_for_sensor (self->identity.sensor);
  gsize image_size, frame_size, transfer_size;

  if (!sensor || !backend || !(sensor->capabilities & FTE3600_SENSOR_CAP_CAPTURE))
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED,
                   "FTE3600 detected %s, which has no implemented capture protocol",
                   sensor ? sensor->name : "an unknown sensor");
      return FALSE;
    }
  if (self->probed_sensor != FTE3600_SENSOR_UNKNOWN &&
      self->probed_sensor != sensor->sensor)
    {
      g_set_error_literal (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED,
                           "FTE3600 sensor changed since enumeration; rediscover the device");
      return FALSE;
    }

  /* Dimensions are immutable protocol metadata, never allocation sizes supplied
   * by the device. Still validate the complete frame against the transport. */
  image_size = (gsize) sensor->width * sensor->height;
  if (!image_size || image_size > G_MAXUINT16 ||
      !backend->bytes_per_pixel || backend->bytes_per_pixel > 2 ||
      backend->frame_overhead > G_MAXUINT16 || !backend->prepare_capture)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Invalid FTE3600 backend frame description");
      return FALSE;
    }
  frame_size = image_size * backend->bytes_per_pixel + backend->frame_overhead;
  transfer_size = backend->required_transfer_size ? backend->required_transfer_size : frame_size;
  if (frame_size > G_MAXUINT16 || transfer_size > self->max_transfer)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "%s requires a %zu-byte SPI transfer; spidev buffer permits %u",
                   sensor->name, transfer_size, self->max_transfer);
      return FALSE;
    }

  if (self->backend && self->backend->destroy)
    self->backend->destroy (self);
  self->backend_data = NULL;
  fpi_fte3600_secure_clear (self->capture_tx, self->capture_frame_size);
  fpi_fte3600_secure_clear (self->capture_rx, self->capture_frame_size);
  g_clear_pointer (&self->capture_tx, g_free);
  g_clear_pointer (&self->capture_rx, g_free);
  self->sensor = sensor;
  self->backend = backend;
  self->image_size = image_size;
  self->capture_frame_size = frame_size;
  self->capture_tx = g_malloc0 (frame_size);
  self->capture_rx = g_malloc0 (frame_size);
  return backend->prepare_capture (self, error);
}

static void
fte3600_discovery_failed (FpDevice *dev, GError *error)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);

  g_autoptr(GError) cleanup = NULL;

  if (!fpi_fte3600_transport_close (self, &cleanup) &&
      !g_error_matches (cleanup, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE))
    fp_warn ("Failed to release FTE3600 transport after discovery failure: %s", cleanup->message);
  fpi_fte3600_release_transport (self);
  if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_PROBE)
    fpi_device_probe_complete (dev, NULL, NULL, error);
  else
    fpi_device_open_complete (dev, error);
}

static void
fte3600_discover_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);

  if (!error)
    fte3600_select_backend (self, &error);
  if (error)
    {
      fte3600_discovery_failed (dev, error);
      return;
    }

  if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_PROBE)
    {
      g_autofree gchar *name = g_strdup_printf ("FocalTech %s Fingerprint Sensor",
                                                self->sensor->name);
      gboolean auth = FALSE;

#if FTE3600_ENABLE_PERSONAL_AUTH
      auth = fte3600_device_match_profile (self) != NULL;
#endif
      self->probed_sensor = self->sensor->sensor;
      fpi_device_update_features (dev, FP_DEVICE_FEATURE_VERIFY,
                                  auth ? FP_DEVICE_FEATURE_VERIFY : 0);
      if (auth)
        fpi_device_set_nr_enroll_stages (dev, FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES);
      fpi_fte3600_transport_close (self, &error);
      fpi_fte3600_release_transport (self);
      fpi_device_probe_complete (dev, NULL, name, error);
      return;
    }

  fpi_ssm_start (self->backend->create_init (self), fte3600_init_complete);
}

static void
fte3600_begin_discovery (FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  GError *error = NULL;

  if (!fpi_fte3600_transport_open (self, &error))
    {
      fte3600_discovery_failed (dev, error);
      return;
    }

  self->idle_verified = FALSE;
  self->session_failed = FALSE;
  self->init_hardware_reset_attempted = FALSE;
  self->init_firmware_upload_attempted = FALSE;
  self->identity = (Fte3600Identity){ 0 };
  self->rom_identity = (Fte3600Identity){ 0 };
  g_clear_pointer (&self->firmware_bytes, g_bytes_unref);
  fpi_ssm_start (fpi_fte3600_discovery_new (self, FALSE), fte3600_discover_complete);
}

static void
fte3600_probe (FpDevice *dev)
{
  /* Identify before publishing per-device features. Enumeration never loads
  * firmware or initializes capture; release the transport after probing. */
  fte3600_begin_discovery (dev);
}

static void
fte3600_open (FpDevice *dev)
{
  /* Revalidate identity in every new session; cached geometry cannot authorize
  * firmware upload, and a different sensor cannot inherit old capabilities. */
  fte3600_begin_discovery (dev);
}

static void
fte3600_close (FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);

  g_clear_pointer (&self->firmware_bytes, g_bytes_unref);
  if (self->spi_fd < 0)
    {
      fpi_device_close_complete (dev, NULL);
      return;
    }

  /* A verified awake idle permits another action, not necessarily shutdown.
   * Run a chip-specific final-close sequence while the IRQ/reset lease and
   * selected CS are still held. Never send runtime commands to a failed
   * session, which may have lost its identity or transport generation. */
  if (self->session_failed ||
      (self->idle_verified && !self->backend->create_shutdown))
    {
      GError *error = NULL;

      fpi_fte3600_transport_close (self, &error);
      self->idle_verified = FALSE;
      fpi_fte3600_release_transport (self);
      fpi_device_close_complete (dev, error);
      return;
    }

  fte3600_start_reset (self, FTE3600_RESET_FOR_CLOSE, NULL);
}

static void
fte3600_release_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);

  fpi_fte3600_clear_irq_source (self);
  self->capturing = FALSE;
  self->waiting_for_release = FALSE;
  /* A release observer owns cleanup, just like capture. Never advance on an
   * unverified state, even if it accidentally reports successful completion. */
  if (!self->idle_verified || self->armed)
    {
      self->session_failed = TRUE;
      if (!error || error->domain == FP_DEVICE_RETRY)
        {
          g_clear_error (&error);
          error = fpi_device_error_new_msg (
            FP_DEVICE_ERROR_PROTO, "FTE3600 release wait left no verified sensor state");
        }
    }
  if (!error)
    {
      GCancellable *cancellable = fpi_device_get_cancellable (dev);

      if (cancellable)
        g_cancellable_set_error_if_cancelled (cancellable, &error);
    }
  if (error)
    {
      fte3600_complete_action_error (self, error);
      return;
    }
  self->enroll_needs_release = FALSE;
  fte3600_start_capture (self);
}

static void
fte3600_start_capture (FpiDeviceFte3600 *self)
{
  FpiSsm *ssm;

  if (self->capturing)
    return;

  if (self->session_failed)
    {
      fte3600_complete_action_error (
        self, fpi_device_error_new_msg (
          FP_DEVICE_ERROR_GENERAL,
          "FTE3600 cleanup did not establish idle; close and reopen the device"));
      return;
    }

  self->capturing = TRUE;
  self->idle_verified = FALSE;
  self->false_irq_count = 0;
  if (fpi_device_get_current_action (FP_DEVICE (self)) == FPI_DEVICE_ACTION_ENROLL &&
      self->enroll_needs_release && self->backend->create_wait_release)
    {
      self->waiting_for_release = TRUE;
      /* PRESENT without NEEDED asks the application to remove the finger. */
      fpi_device_report_finger_status (FP_DEVICE (self), FP_FINGER_STATUS_PRESENT);
      ssm = self->backend->create_wait_release (self);
      fpi_ssm_start (ssm, fte3600_release_complete);
      return;
    }
  fpi_device_report_finger_status (FP_DEVICE (self), FP_FINGER_STATUS_NEEDED);
  ssm = self->backend->create_capture (self);
  fpi_ssm_start (ssm, fte3600_capture_complete);
}

static void
fte3600_cancel (FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  FpiSsm *ssm = self->irq_wait_ssm;

  g_autoptr(GError) error = NULL;
  GCancellable *cancellable;

  /* Cancellable SPI transfers and BRISK jobs finish through their normal
   * callbacks.  The GPIO wait has no transfer callback, so wake that state
   * explicitly. */
  if (!ssm)
    return;

  fpi_fte3600_clear_irq_source (self);
  cancellable = fpi_device_get_cancellable (dev);
  if (!cancellable ||
      !g_cancellable_set_error_if_cancelled (cancellable, &error))
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                 "Fingerprint operation was cancelled");
  fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
}

static void
fte3600_enroll (FpDevice *dev)
{
#if FTE3600_ENABLE_PERSONAL_AUTH
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  const Fte3600MatchProfile *profile = fte3600_device_match_profile (self);

  if (!profile)
    {
      fpi_device_enroll_complete (dev, NULL, fpi_device_error_new_msg (
                                    FP_DEVICE_ERROR_NOT_SUPPORTED,
                                    "FTE3600 sensor has no compatible matching profile"));
      return;
    }

  g_clear_pointer (&self->verify_template, fpi_fte3600_template_free);
  self->enroll_stages_passed = 0;
  self->enroll_needs_release = FALSE;
  g_clear_pointer (&self->enroll_template, fpi_fte3600_template_free);
  self->enroll_template = fpi_fte3600_template_new_for_profile (profile);
  fte3600_start_capture (self);
#else
  /* The public enroll API dispatches the vfunc even with zero advertised
   * stages. Keep a rejecting handler in capture-only builds. */
  fpi_device_enroll_complete (dev, NULL, fpi_device_error_new_msg (
                                FP_DEVICE_ERROR_NOT_SUPPORTED, "FTE3600 authentication is disabled in this build"));
#endif
}

#if FTE3600_ENABLE_PERSONAL_AUTH
static void
fte3600_verify (FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  const Fte3600MatchProfile *profile = fte3600_device_match_profile (self);
  Fte3600VerifyLoad *load;

  g_autoptr(GTask) task = NULL;
  g_autoptr(GBytes) wire = NULL;
  GError *error;

  if (!profile)
    {
      fpi_device_verify_complete (dev, fpi_device_error_new_msg (
                                    FP_DEVICE_ERROR_NOT_SUPPORTED,
                                    "FTE3600 sensor has no compatible matching profile"));
      return;
    }

  g_clear_pointer (&self->verify_template, fpi_fte3600_template_free);
  error = fte3600_verify_get_wire (self, &wire);
  if (error != NULL)
    {
      fpi_device_verify_complete (dev, error);
      return;
    }

  task = g_task_new (self, fpi_device_get_cancellable (dev),
                     fte3600_verify_load_complete, NULL);
  load = g_new0 (Fte3600VerifyLoad, 1);
  load->wire = g_steal_pointer (&wire);
  load->profile = profile;
  g_task_set_task_data (task, load, (GDestroyNotify) fte3600_verify_load_free);
  g_task_set_return_on_cancel (task, TRUE);
  g_task_run_in_thread (task, fte3600_verify_load_worker);
}
#endif

static void
fte3600_capture (FpDevice *dev)
{
  gboolean wait_for_finger;

  fpi_device_get_capture_data (dev, &wait_for_finger);
  if (!wait_for_finger)
    {
      fpi_device_capture_complete (
        dev,
        NULL,
        fpi_device_error_new_msg (
          FP_DEVICE_ERROR_NOT_SUPPORTED,
          "FTE3600 only supports finger-triggered image capture"));
      return;
    }
  fte3600_start_capture (FPI_DEVICE_FTE3600 (dev));
}

static void
fpi_device_fte3600_init (FpiDeviceFte3600 *self)
{
  self->spi_fd = -1;
  self->reset_fd = -1;
  self->irq_fd = -1;
}

static void
fpi_device_fte3600_finalize (GObject *object)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (object);

  g_autoptr(GError) error = NULL;

  if (!fpi_fte3600_transport_close (self, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE))
    fp_warn ("Failed to release FTE3600 transport at finalization: %s", error->message);
  fpi_fte3600_release_transport (self);
  g_clear_pointer (&self->enroll_template, fpi_fte3600_template_free);
  g_clear_pointer (&self->verify_template, fpi_fte3600_template_free);
  fpi_fte3600_secure_clear (self->capture_tx, self->capture_frame_size);
  fpi_fte3600_secure_clear (self->capture_rx, self->capture_frame_size);
  g_clear_pointer (&self->capture_tx, g_free);
  g_clear_pointer (&self->capture_rx, g_free);
  g_clear_pointer (&self->firmware_bytes, g_bytes_unref);
  fpi_fte3600_clear_captured_image (self);

  G_OBJECT_CLASS (fpi_device_fte3600_parent_class)->finalize (object);
}

static void
fpi_device_fte3600_class_init (FpiDeviceFte3600Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = "fte3600";
  dev_class->full_name = "FocalTech FTE3600 Fingerprint Sensor Family";
  dev_class->type = FP_DEVICE_TYPE_UDEV;
  dev_class->id_table = fte3600_id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  /* Backend idle time and scan duty cycle differ. No measured family-wide
   * thermal model or sensor temperature is available; elapsed action time
   * alone must not be presented as a calibrated temperature estimate. */
  dev_class->temp_hot_seconds = -1;
  dev_class->probe = fte3600_probe;
  dev_class->open = fte3600_open;
  dev_class->close = fte3600_close;
  dev_class->enroll = fte3600_enroll;
#if FTE3600_ENABLE_PERSONAL_AUTH
  /* Enrollment and one-template verification are published together only in
   * an explicit personal-auth build.  A policy-zero build remains useful for
   * controlled image capture, but must not advertise enrollment of templates
   * which it can never authenticate.  This is a personal usability policy,
   * not population FAR calibration. */
  dev_class->verify = fte3600_verify;
  /* Probe publishes the stage count only for a supported matching geometry. */
#endif
  /* Identify remains unavailable: gallery ambiguity and population policy
   * have not been designed or calibrated. */
  dev_class->capture = fte3600_capture;
  dev_class->cancel = fte3600_cancel;

  G_OBJECT_CLASS (klass)->finalize = fpi_device_fte3600_finalize;
  fpi_device_class_auto_initialize_features (dev_class);
}
