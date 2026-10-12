/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_device_session.h"
#include "fte3600_fw9369_grayscale.h"
#include "fte3600_fw9369_release.h"
#include "fte3600_ft93xx_image_pipeline.h"
#include <stdlib.h>
#include <string.h>

struct fte3600_device_session
{
  struct fte3600_device_config              config;
  struct fte3600_device_status              status;
  struct fte3600_factory_integration_state  factory;
  struct fte3600_factory_startup_storage    startup_storage;
  struct fte3600_factory_integration_result startup_result;
  fte3600_factory_integration_io            factory_io;
  struct fte3600_capture_io                 fw_io;
  struct fte3600_ft93xx_io                  modern_io;
  struct fte3600_ft9368_io                  byte_io;
  struct fte3600_recovery_io                legacy_io;
  struct fte3600_legacy_boot_profile        legacy_profile;
  struct fte3600_legacy_lifecycle_state     legacy_request;
  struct fte3600_ft93xx_processing_state    processing;
  uint8_t                                  *candidate, *image, *raw;
  uint16_t                                 *processed;
  void                                     *workspace;
  size_t                                    image_bytes, raw_bytes, workspace_bytes;
  int                                       active, io_failed;
  int                                       fw_maintenance_pending, fw_down_armed;
  unsigned                                  fw_release_rejections;
};
static int
failed (uint32_t value)
{
  return (value >> 31) != 0;
}
static uint32_t
spi (void *context, uint8_t *buffer, size_t length)
{
  struct fte3600_device_session *s = context;

  ++s->status.spi_calls;
  uint32_t value = s->config.io.spi (s->config.io.context, buffer, length);
  if (failed (value))
    {
      s->io_failed = 1;
      if (s->status.failed_spi_calls != UINT32_MAX)
        ++s->status.failed_spi_calls;
      s->status.last_spi_status = value;
    }
  return value;
}
static void
wait_ms (void *context, uint32_t amount)
{
  struct fte3600_device_session *s = context;

  s->config.io.wait_ms (s->config.io.context, amount);
}
static void
wait_us (void *context, uint32_t amount)
{
  struct fte3600_device_session *s = context;

  s->config.io.wait_us (s->config.io.context, amount);
}
static uint32_t
gpio (void *context, int level)
{
  struct fte3600_device_session *s = context;
  uint32_t value = s->config.io.gpio (s->config.io.context, level);

  if (failed (value))
    {
      s->io_failed = 1;
      s->status.last_gpio_status = value;
    }
  return value;
}
static uint32_t
boundary (void *context, enum fte3600_factory_boundary which)
{
  struct fte3600_device_session *s = context;
  uint32_t value = s->config.io.boundary (s->config.io.context, which);

  if (failed (value))
    {
      s->io_failed = 1;
      s->status.compatibility_status = value;
    }
  return value;
}
static uint32_t
clock_ms (void *context)
{
  struct fte3600_device_session *s = context;

  return s->config.io.clock_ms (s->config.io.context);
}
static int
legacy_raw_status (uint32_t value)
{
  /* LOAD distinguishes terminal device absence from retryable I/O failure.
   * Preserve this callback's result, never an earlier failure diagnostic. */
  return !failed (value) ? 0 : value == UINT32_C (0xc000000e) ? FTE3600_LEGACY_BOOT_RAW_DEVICE_ABSENT : 1;
}
static int
legacy_gpio (void *context, int level)
{
  return legacy_raw_status (gpio (context, level));
}
static int
legacy_spi (void *context, const uint8_t *tx, uint8_t *rx, size_t length)
{
  memcpy (rx, tx, length);
  return legacy_raw_status (spi (context, rx, length));
}
static enum fte3600_sensor
legacy_sensor (enum fte3600_factory_model model)
{
  switch (model)
    {
    case FTE3600_MODEL_FT9338: return FTE3600_FT9338;

    case FTE3600_MODEL_FT9348: return FTE3600_FT9348;

    case FTE3600_MODEL_FT9361: return FTE3600_FT9361;

    case FTE3600_MODEL_FT9536: return FTE3600_FT9536;

    case FTE3600_MODEL_UNKNOWN:
    case FTE3600_MODEL_FT9368:
    case FTE3600_MODEL_FW9369:
    case FTE3600_MODEL_FT9769:
    case FTE3600_MODEL_FT9365:
    default: return FTE3600_SENSOR_UNKNOWN;
    }
}
static uint16_t
legacy_id (enum fte3600_sensor sensor)
{
  switch (sensor)
    {
    case FTE3600_FT9338: return 0x9338;

    case FTE3600_FT9348: return 0x9348;

    case FTE3600_FT9361: return 0x9361;

    case FTE3600_FT9536: return 0x9536;

    case FTE3600_SENSOR_UNKNOWN:
    default: return 0;
    }
}
static void
begin (struct fte3600_device_session *s)
{
  s->active = 1;
  s->io_failed = 0;
  s->status.component_error = 0;
  s->status.compatibility_status = 0;
  s->status.platform_error = 0;
  s->config.io.clear_error (s->config.io.context);
}
static enum fte3600_device_error
finish (struct fte3600_device_session *s,
        enum fte3600_device_error error, int fault)
{
  s->status.platform_error = s->config.io.error (s->config.io.context);
  if (s->io_failed || s->status.platform_error)
    {
      error = FTE3600_DEVICE_PLATFORM_ERROR;
      fault = 1;
    }
  s->active = 0;
  s->status.error = error;
  if (fault)
    {
      s->status.state = FTE3600_DEVICE_FAULT;
      s->legacy_request.request_pending = 0;
    }
  return error;
}
static void
free_buffers (struct fte3600_device_session *s)
{
  free (s->candidate);
  free (s->image);
  free (s->raw);
  free (s->processed);
  free (s->workspace);
  s->candidate = s->image = s->raw = NULL;
  s->processed = NULL;
  s->workspace = NULL;
  s->image_bytes = s->raw_bytes = s->workspace_bytes = 0;
}
static enum fte3600_device_error
allocate_buffers (struct fte3600_device_session *s)
{
  size_t n = (size_t) s->status.width * s->status.height;

  if (!n || n > FTE3600_FT93XX_STARTUP_SAMPLES)
    return FTE3600_DEVICE_UNSUPPORTED;
  free_buffers (s);
  s->image_bytes = n;
  s->candidate = calloc (n, 1);
  s->image = calloc (n, 1);
  if (s->status.model == FTE3600_MODEL_FW9369)
    {
      s->workspace_bytes = sizeof (struct fte3600_fw9369_grayscale_workspace);
    }
  else if (s->status.model == FTE3600_MODEL_FT9365 || s->status.model == FTE3600_MODEL_FT9769)
    {
      s->raw_bytes = (size_t) s->status.width * (s->status.height + s->status.extra_rows) * 2;
      s->raw = calloc (s->raw_bytes, 1);
      s->workspace_bytes = fte3600_ft93xx_pipeline_workspace_size (s->status.chip_id, FTE3600_FT93XX_PIPELINE_CAPTURE);
      if (!s->workspace_bytes)
        return FTE3600_DEVICE_UNSUPPORTED;
      if (!s->raw)
        return FTE3600_DEVICE_NO_MEMORY;
    }
  if (s->workspace_bytes)
    {
      s->processed = calloc (n, sizeof (uint16_t));
      s->workspace = calloc (s->workspace_bytes, 1);
    }
  if (!s->candidate || !s->image || (s->workspace_bytes && (!s->processed || !s->workspace)))
    return FTE3600_DEVICE_NO_MEMORY;
  return FTE3600_DEVICE_OK;
}
enum fte3600_device_error
fte3600_device_create (
  const struct fte3600_device_config *config, struct fte3600_device_session **output)
{
  if (!config || !output || !config->io.spi || !config->io.wait_ms || !config->io.wait_us ||
      !config->io.gpio || !config->io.boundary || !config->io.clock_ms ||
      !config->io.error || !config->io.clear_error || config->startup.scene != 0 ||
      config->startup.initial_model != FTE3600_MODEL_UNKNOWN || config->startup.resume_cleanup ||
      ((config->startup.firmware || config->startup.firmware_length) && !legacy_sensor (config->legacy_firmware_model)))
    return FTE3600_DEVICE_INVALID_ARGUMENT;
  struct fte3600_device_session *s = calloc (1, sizeof (*s));
  if (!s)
    return FTE3600_DEVICE_NO_MEMORY;
  s->config = *config;
  s->factory_io = (fte3600_factory_integration_io){s, spi, wait_ms, wait_us, gpio, boundary};
  s->fw_io = (struct fte3600_capture_io){s, spi, wait_ms};
  s->modern_io = (struct fte3600_ft93xx_io){s, spi, wait_ms};
  s->byte_io = (struct fte3600_ft9368_io){s, spi, wait_ms};
  s->legacy_io = (struct fte3600_recovery_io){s, legacy_gpio, legacy_spi, wait_ms, clock_ms};
  (void) fte3600_factory_integration_state_defaults (&s->factory);
  s->status.state = FTE3600_DEVICE_CREATED;
  *output = s;
  return FTE3600_DEVICE_OK;
}
static int
modern_transport_bad (const struct fte3600_ft93xx_diagnostics *d)
{
  return d->exhausted_transactions || d->idle_unacknowledged || d->mode_unacknowledged ||
         d->ready_unacknowledged || d->voltage_unacknowledged || d->spi_mode_unacknowledged;
}
static enum fte3600_device_error
identify_ready (struct fte3600_device_session *s)
{
  struct fte3600_factory_integration_result *r = &s->startup_result;

  s->status.model = s->factory.selected_model;
  if (!r->sensor_constructed || s->status.model == FTE3600_MODEL_UNKNOWN || failed (r->status))
    return FTE3600_DEVICE_STARTUP_ERROR;
  enum fte3600_sensor sensor = legacy_sensor (s->status.model);
  if (sensor)
    {
      struct fte3600_legacy_geometry geometry = fte3600_legacy_geometry (sensor);
      s->status.chip_id = legacy_id (sensor);
      /* Legacy discovery exposes classification, not a retained raw ID. */
      s->status.raw_id = 0;
      s->status.width = (uint16_t) geometry.width;
      s->status.height = (uint16_t) geometry.height;
      if (!s->config.startup.firmware_length)
        return FTE3600_DEVICE_MISSING_FIRMWARE;
      if (s->config.legacy_firmware_model != s->status.model)
        return FTE3600_DEVICE_FIRMWARE_MISMATCH;
      s->legacy_profile = fte3600_legacy_boot_defaults (sensor);
      s->legacy_profile.boot = s->factory.legacy.boot_edition ? FTE3600_RECOVERY_BOOT_B : FTE3600_RECOVERY_BOOT_A;
      s->status.component_error = fte3600_legacy_boot_run (&s->legacy_profile, FTE3600_LEGACY_BOOT_INITIALIZE,
                                                           s->config.startup.firmware, s->config.startup.firmware_length, &s->factory.legacy.irq_enabled, &s->legacy_io);
      if (s->status.component_error)
        return FTE3600_DEVICE_STARTUP_ERROR;
      memset (&s->legacy_request, 0, sizeof (s->legacy_request));
      s->legacy_request.irq_enabled = 1;
      s->legacy_request.request_kind = 2;
    }
  else if (s->status.model == FTE3600_MODEL_FT9368)
    {
      uint8_t information[32];
      s->status.compatibility_status = fte3600_ft9368_info (sizeof (information), information, sizeof (information), &s->byte_io);
      if (failed (s->status.compatibility_status) || information[19] != 0x93 || information[20] != 0x68 ||
          information[21] != s->factory.byte.selected.expected_version)
        return FTE3600_DEVICE_STARTUP_ERROR;
      s->status.chip_id = s->status.raw_id = 0x9368;
      s->status.width = 64;
      s->status.height = 80;
    }
  else if (s->status.model == FTE3600_MODEL_FW9369)
    {
      const struct fte3600_fw9369_startup_diagnostics *d = &r->word.fw;
      const struct fte3600_fw9369_startup_state *f = &s->factory.word.fw;
      s->status.raw_id = 0x9362;
      s->status.chip_id = 0x9369;
      s->status.width = 64;
      s->status.height = 80;
      s->status.baseline_crc = f->baseline_crc;
      s->status.calibrated_image_dac = f->image_dac;
      if (!r->fw_ran || r->child_status || d->completed != 7 || d->fdt_status || d->image_status ||
          d->warnings || d->transport.exhausted_transactions || d->transport.ready_timeout ||
          !f->baseline_flag_1 || !f->baseline_flag_2)
        return FTE3600_DEVICE_CALIBRATION_ERROR;
      s->fw_down_armed = 1;
    }
  else if (s->status.model == FTE3600_MODEL_FT9365 || s->status.model == FTE3600_MODEL_FT9769)
    {
      const struct fte3600_ft93xx_preparation *p = &s->factory.word.modern.preparation;
      const struct fte3600_ft93xx_startup_diagnostics *d = &r->word.modern;
      s->status.raw_id = p->identity.raw_id;
      s->status.chip_id = p->identity.chip_id;
      s->status.width = p->geometry.width;
      s->status.height = p->geometry.height;
      s->status.extra_rows = p->geometry.extra_rows;
      s->status.calibrated_image_dac = p->state.dac;
      if (!r->modern_ran || r->child_status || !d->completed || !d->fdt.baseline_valid ||
          d->fdt.search_failures || d->fdt.readiness_timeouts || d->image.search_failures ||
          d->image.mode_timeouts || d->image.readiness_timeouts || modern_transport_bad (&d->transport))
        return FTE3600_DEVICE_CALIBRATION_ERROR;
      s->status.component_error = fte3600_ft93xx_processing_defaults (p->identity.chip_id, p->version, &s->processing);
      if (s->status.component_error)
        return FTE3600_DEVICE_UNSUPPORTED;
    }
  else
    {
      return FTE3600_DEVICE_UNSUPPORTED;
    }
  s->status.startup_complete = 1;
  s->status.calibration_complete = 1;
  return allocate_buffers (s);
}
enum fte3600_device_error
fte3600_device_start (struct fte3600_device_session *s)
{
  if (!s)
    return FTE3600_DEVICE_INVALID_ARGUMENT;
  if (s->active || s->status.state == FTE3600_DEVICE_ARMED)
    return FTE3600_DEVICE_BUSY;
  begin (s);
  s->status.image_available = 0;
  s->status.startup_complete = 0;
  s->status.calibration_complete = 0;
  s->status.raw_id = s->status.chip_id = s->status.width = s->status.height = s->status.extra_rows = 0;
  s->status.baseline_crc = 0;
  s->status.calibrated_image_dac = 0;
  s->fw_maintenance_pending = s->fw_down_armed = 0;
  s->fw_release_rejections = 0;
  /* Recovery starts a real cold initialization; stale flags cannot qualify. */
  (void) fte3600_factory_integration_state_defaults (&s->factory);
  s->status.component_error = fte3600_factory_integration_run_optional_firmware (&s->config.startup, s->config.legacy_firmware_model,
                                                                                 &s->factory, &s->startup_storage, sizeof (s->startup_storage), &s->factory_io, NULL, &s->startup_result);
  s->status.model = s->factory.selected_model;
  s->status.compatibility_status = s->startup_result.status;
  enum fte3600_device_error error;
  if (s->status.component_error == FTE3600_FACTORY_INTEGRATION_MISSING_FIRMWARE)
    error = FTE3600_DEVICE_MISSING_FIRMWARE;
  else if (s->status.component_error == FTE3600_FACTORY_INTEGRATION_UNCONFIRMED_IDENTITY)
    error = FTE3600_DEVICE_UNSUPPORTED;
  else if (s->status.component_error == FTE3600_FACTORY_INTEGRATION_FIRMWARE_MODEL_MISMATCH)
    error = FTE3600_DEVICE_FIRMWARE_MISMATCH;
  else if (s->status.component_error)
    error = FTE3600_DEVICE_STARTUP_ERROR;
  else
    error = identify_ready (s);
  if (!error)
    s->status.state = FTE3600_DEVICE_IDLE;
  enum fte3600_device_error result = finish (s, error, error != FTE3600_DEVICE_OK);
  if (result)
    {
      s->status.startup_complete = 0;
      s->status.calibration_complete = 0;
    }
  return result;
}
static void
legacy_complete (void *context, int matched, enum fte3600_legacy_completion result,
                 const uint8_t *image, size_t length)
{
  (void) context;
  (void) matched;
  (void) result;
  (void) image;
  (void) length;
}
static struct fte3600_legacy_boot_lifecycle_result
legacy_run (struct fte3600_device_session          *s,
            enum fte3600_legacy_lifecycle_operation operation)
{
  const struct fte3600_legacy_lifecycle_options options = {.cancel_matches = 1};
  const struct fte3600_legacy_boot_lifecycle_io io = {s->legacy_io, s, legacy_complete};

  return fte3600_legacy_boot_lifecycle_run (&s->legacy_profile, operation, &s->legacy_request, &options,
                                            s->candidate, s->image_bytes, s->config.startup.firmware, s->config.startup.firmware_length, &io);
}
enum fte3600_device_error
fte3600_device_arm (struct fte3600_device_session *s)
{
  if (!s)
    return FTE3600_DEVICE_INVALID_ARGUMENT;
  if (s->active || s->status.state == FTE3600_DEVICE_ARMED)
    return FTE3600_DEVICE_BUSY;
  if (s->status.state != FTE3600_DEVICE_IDLE)
    return FTE3600_DEVICE_NOT_READY;
  if (s->status.model == FTE3600_MODEL_FW9369 && s->fw_maintenance_pending)
    return FTE3600_DEVICE_NOT_READY;
  begin (s);
  enum fte3600_device_error error = FTE3600_DEVICE_OK;
  if (legacy_sensor (s->status.model))
    {
      s->legacy_request.request_pending = s->legacy_request.request_active = 1;
      struct fte3600_legacy_boot_lifecycle_result r = legacy_run (s, FTE3600_LEGACY_START);
      if (r.lifecycle.status == FTE3600_LEGACY_LIFECYCLE_INVALID_INPUT)
        error = FTE3600_DEVICE_ACQUISITION_ERROR;
    }
  else if (s->status.model == FTE3600_MODEL_FW9369)
    {
      /* Startup and successful release already own a Down arm. Repeating
       * it would clear a touch which arrived before the next request. */
      if (!s->fw_down_armed)
        {
          struct fte3600_fw9369_release_diagnostics d;
          s->status.component_error = fte3600_fw9369_arm_detection (&s->factory.word.fw,
                                                                    FTE3600_FW9369_DOWN, &s->fw_io, &d);
          if (s->status.component_error)
            error = FTE3600_DEVICE_ACQUISITION_ERROR;
          else
            s->fw_down_armed = 1;
        }
    }
  else if (s->status.model == FTE3600_MODEL_FT9365 || s->status.model == FTE3600_MODEL_FT9769)
    {
      struct fte3600_ft93xx_device *m = &s->factory.word.modern;
      struct fte3600_ft93xx_fdt_diagnostics d;
      s->status.component_error = fte3600_ft93xx_fdt_run (m->preparation.identity.chip_id, m->preparation.foundry,
                                                          m->preparation.features, &m->preparation.state, &m->fdt, FTE3600_FT93XX_FDT_ARM, 1, NULL, 0, &s->modern_io, &d);
      if (s->status.component_error || modern_transport_bad (&d.transport))
        error = FTE3600_DEVICE_ACQUISITION_ERROR;
    }
  else if (s->status.model == FTE3600_MODEL_FT9368)
    {
      /* Reviewed application wake. Notification signal is not scan arm. */
      s->status.compatibility_status = fte3600_ft9368_wake (&s->byte_io);
      if (failed (s->status.compatibility_status))
        error = FTE3600_DEVICE_ACQUISITION_ERROR;
    }
  else
    {
      error = FTE3600_DEVICE_UNSUPPORTED;
    }
  if (!error)
    s->status.state = FTE3600_DEVICE_ARMED;
  return finish (s, error, error != FTE3600_DEVICE_OK);
}
static enum fte3600_device_error
acquire (struct fte3600_device_session *s)
{
  enum fte3600_device_error error = FTE3600_DEVICE_OK;
  struct fte3600_fw9369_startup_state fw_next;
  int fw_staged = 0;

  begin (s);
  if (legacy_sensor (s->status.model))
    {
      struct fte3600_legacy_boot_lifecycle_result r = legacy_run (s, FTE3600_LEGACY_IRQ);
      s->status.component_error = (unsigned) r.initialize_status;
      if (r.lifecycle.status == FTE3600_LEGACY_LIFECYCLE_INVALID_INPUT ||
          (r.initialize_called && r.initialize_status != FTE3600_LEGACY_BOOT_OK))
        error = FTE3600_DEVICE_ACQUISITION_ERROR;
      else if (r.lifecycle.image_length != s->image_bytes)
        error = FTE3600_DEVICE_NO_FRAME;
    }
  else if (s->status.model == FTE3600_MODEL_FT9368)
    {
      s->status.compatibility_status = fte3600_ft9368_capture (s->candidate, s->image_bytes, &s->byte_io);
      if (failed (s->status.compatibility_status))
        error = FTE3600_DEVICE_ACQUISITION_ERROR;
    }
  else if (s->status.model == FTE3600_MODEL_FW9369)
    {
      struct fte3600_fw9369_release_diagnostics rd;
      enum fte3600_fw9369_event event;
      fw_next = s->factory.word.fw;
      s->status.component_error = fte3600_fw9369_event_run (&fw_next, 0, &s->fw_release_rejections,
                                                            &s->startup_storage.fw, sizeof (s->startup_storage.fw), &event, &s->fw_io, &rd);
      if (s->status.component_error)
        {
          error = FTE3600_DEVICE_ACQUISITION_ERROR;
          goto complete;
        }
      if (event == FTE3600_FW9369_EVENT_RECOVERY)
        {
          error = FTE3600_DEVICE_RECOVERY_REQUIRED;
          goto complete;
        }
      if (event != FTE3600_FW9369_EVENT_TOUCH)
        {
          if (event == FTE3600_FW9369_EVENT_RELEASED)
            {
              s->status.component_error = fte3600_fw9369_arm_detection (&fw_next,
                                                                        FTE3600_FW9369_DOWN, &s->fw_io, &rd);
              if (s->status.component_error)
                {
                  error = FTE3600_DEVICE_ACQUISITION_ERROR;
                  goto complete;
                }
              s->fw_down_armed = 1;
            }
          fw_staged = 1;
          error = FTE3600_DEVICE_NO_FRAME;
          goto complete;
        }
      struct fte3600_fw9369_grayscale_buffers b = {NULL, 0, s->processed, s->image_bytes, s->candidate, s->image_bytes};
      struct fte3600_fw9369_grayscale_diagnostics d;
      s->status.component_error = fte3600_fw9369_grayscale_run (FTE3600_FW9369_GRAYSCALE_CAPTURE,
                                                                s->status.width, s->status.height, &fw_next, &b, s->workspace, s->workspace_bytes, &s->fw_io, &d, NULL);
      if (s->status.component_error || !d.completed || d.transport.exhausted_transactions || d.transport.ready_timeout)
        error = FTE3600_DEVICE_ACQUISITION_ERROR;
      if (!error)
        {
          /* Preserve an UP arriving while the caller processes the image. */
          s->status.component_error = fte3600_fw9369_arm_detection (&fw_next,
                                                                    FTE3600_FW9369_UP, &s->fw_io, &rd);
          if (s->status.component_error)
            error = FTE3600_DEVICE_ACQUISITION_ERROR;
        }
      if (!error)
        fw_staged = 1;
    }
  else
    {
      struct fte3600_ft93xx_preparation *p = &s->factory.word.modern.preparation;
      const struct fte3600_ft93xx_pipeline_buffers b = {s->raw, s->raw_bytes, s->processed, s->image_bytes, s->candidate, s->image_bytes};
      struct fte3600_ft93xx_pipeline_diagnostics d;
      s->status.component_error = fte3600_ft93xx_pipeline_run (p->identity.chip_id, p->identity.raw_id, &p->geometry,
                                                               &p->state, &s->processing, FTE3600_FT93XX_PIPELINE_CAPTURE, &b, s->workspace, s->workspace_bytes, &s->modern_io, &d);
      if (s->status.component_error || !d.completed || modern_transport_bad (&d.transport))
        error = FTE3600_DEVICE_ACQUISITION_ERROR;
    }
complete:
  error = finish (s, error, error != FTE3600_DEVICE_OK && error != FTE3600_DEVICE_NO_FRAME);
  if (fw_staged && (!error || error == FTE3600_DEVICE_NO_FRAME))
    {
      s->factory.word.fw = fw_next;
      s->fw_down_armed = fw_next.arm_mode == FTE3600_FW9369_DOWN;
      s->status.baseline_crc = fw_next.baseline_crc;
      s->status.calibrated_image_dac = fw_next.image_dac;
    }
  if (!error)
    {
      memcpy (s->image, s->candidate, s->image_bytes);
      ++s->status.image_sequence;
      s->status.image_available = 1;
      s->status.state = FTE3600_DEVICE_IDLE;
      s->legacy_request.request_pending = s->legacy_request.request_active = 0;
      if (s->status.model == FTE3600_MODEL_FW9369)
        {
          s->fw_maintenance_pending = 1;
          s->fw_down_armed = 0;
        }
    }
  return error;
}
enum fte3600_device_error
fte3600_device_release_step (struct fte3600_device_session *s,
                             int                           *released)
{
  if (!s || !released)
    return FTE3600_DEVICE_INVALID_ARGUMENT;
  if (s->active || s->status.state == FTE3600_DEVICE_ARMED)
    return FTE3600_DEVICE_BUSY;
  if (s->status.state != FTE3600_DEVICE_IDLE)
    return FTE3600_DEVICE_NOT_READY;
  if (s->status.model != FTE3600_MODEL_FW9369)
    return FTE3600_DEVICE_UNSUPPORTED;
  if (!s->fw_maintenance_pending)
    {
      *released = 1;
      return FTE3600_DEVICE_OK;
    }
  begin (s);
  struct fte3600_fw9369_startup_state next = s->factory.word.fw;
  struct fte3600_fw9369_release_diagnostics d;
  enum fte3600_fw9369_event outcome = FTE3600_FW9369_EVENT_PENDING;
  enum fte3600_device_error error = FTE3600_DEVICE_OK;
  s->status.component_error = fte3600_fw9369_event_run (&next, 1, &s->fw_release_rejections,
                                                        &s->startup_storage.fw, sizeof (s->startup_storage.fw), &outcome, &s->fw_io, &d);
  if (s->status.component_error)
    {
      error = FTE3600_DEVICE_CALIBRATION_ERROR;
      goto done;
    }
  if (outcome == FTE3600_FW9369_EVENT_RECOVERY)
    {
      error = FTE3600_DEVICE_RECOVERY_REQUIRED;
      goto done;
    }
  if (outcome != FTE3600_FW9369_EVENT_PENDING)
    {
      s->status.component_error = fte3600_fw9369_arm_detection (&next,
                                                                outcome == FTE3600_FW9369_EVENT_RELEASED ? FTE3600_FW9369_DOWN : FTE3600_FW9369_UP,
                                                                &s->fw_io, &d);
      if (s->status.component_error)
        error = FTE3600_DEVICE_ACQUISITION_ERROR;
    }
done:
  error = finish (s, error, error != FTE3600_DEVICE_OK);
  if (!error)
    {
      s->factory.word.fw = next;
      s->status.baseline_crc = next.baseline_crc;
      s->status.calibrated_image_dac = next.image_dac;
      *released = outcome == FTE3600_FW9369_EVENT_RELEASED;
      s->fw_maintenance_pending = !*released;
      s->fw_down_armed = *released;
    }
  return error;
}
enum fte3600_device_error
fte3600_device_recover_fw9369 (struct fte3600_device_session *s,
                               int                            waiting_release)
{
  if (!s)
    return FTE3600_DEVICE_INVALID_ARGUMENT;
  if (s->active)
    return FTE3600_DEVICE_BUSY;
  if (s->status.model != FTE3600_MODEL_FW9369)
    return FTE3600_DEVICE_UNSUPPORTED;
  if (s->status.state != FTE3600_DEVICE_FAULT || s->status.error != FTE3600_DEVICE_RECOVERY_REQUIRED)
    return FTE3600_DEVICE_NOT_READY;
  begin (s);
  struct fte3600_fw9369_startup_state next = s->factory.word.fw;
  struct fte3600_fw9369_startup_diagnostics d;
  enum fte3600_device_error error = FTE3600_DEVICE_PLATFORM_ERROR;
  if (failed (gpio (s, 1)) || s->config.io.error (s->config.io.context))
    goto done;
  wait_ms (s, 10);
  if (s->config.io.error (s->config.io.context))
    goto done;
  if (failed (gpio (s, 0)) || s->config.io.error (s->config.io.context))
    goto done;
  wait_ms (s, 20);
  if (s->config.io.error (s->config.io.context))
    goto done;
  if (failed (gpio (s, 1)) || s->config.io.error (s->config.io.context))
    goto done;
  wait_ms (s, 5);
  if (s->config.io.error (s->config.io.context))
    goto done;
  s->status.component_error = fte3600_fw9369_startup_checked (&next, &s->startup_storage.fw,
                                                              sizeof (s->startup_storage.fw), &s->fw_io, NULL, &d);
  if (s->status.component_error || d.completed != 7 || d.fdt_status || d.image_status || d.warnings ||
      d.transport.exhausted_transactions || d.transport.ready_timeout ||
      !next.baseline_flag_1 || !next.baseline_flag_2)
    {
      error = FTE3600_DEVICE_CALIBRATION_ERROR;
      goto done;
    }
  if (waiting_release)
    {
      struct fte3600_fw9369_release_diagnostics rd;
      s->status.component_error = fte3600_fw9369_arm_detection (&next, FTE3600_FW9369_UP, &s->fw_io, &rd);
      if (s->status.component_error)
        {
          error = FTE3600_DEVICE_ACQUISITION_ERROR;
          goto done;
        }
    }
  error = FTE3600_DEVICE_OK;
done:
  error = finish (s, error, error != FTE3600_DEVICE_OK);
  if (!error)
    {
      s->factory.word.fw = next;
      s->status.baseline_crc = next.baseline_crc;
      s->status.calibrated_image_dac = next.image_dac;
      s->status.state = waiting_release ? FTE3600_DEVICE_IDLE : FTE3600_DEVICE_ARMED;
      s->fw_maintenance_pending = waiting_release != 0;
      s->fw_down_armed = !waiting_release;
    }
  return error;
}
enum fte3600_device_error
fte3600_device_irq (struct fte3600_device_session *s)
{
  if (!s)
    return FTE3600_DEVICE_INVALID_ARGUMENT;
  if (s->active)
    return FTE3600_DEVICE_BUSY;
  if (s->status.state != FTE3600_DEVICE_ARMED)
    return FTE3600_DEVICE_NOT_READY;
  return acquire (s);
}
enum fte3600_device_error
fte3600_device_capture (struct fte3600_device_session *s)
{
  if (!s)
    return FTE3600_DEVICE_INVALID_ARGUMENT;
  if (s->active)
    return FTE3600_DEVICE_BUSY;
  if (s->status.state == FTE3600_DEVICE_IDLE)
    {
      enum fte3600_device_error e = fte3600_device_arm (s);
      if (e)
        return e;
    }
  return fte3600_device_irq (s);
}
enum fte3600_device_error
fte3600_device_copy_image (const struct fte3600_device_session *s,
                           uint64_t sequence, uint8_t *pixels, size_t capacity)
{
  if (!s || !pixels || capacity < s->image_bytes)
    return FTE3600_DEVICE_INVALID_ARGUMENT;
  if (s->active)
    return FTE3600_DEVICE_BUSY;
  if (!s->status.image_available || sequence != s->status.image_sequence)
    return FTE3600_DEVICE_NO_FRAME;
  memcpy (pixels, s->image, s->image_bytes);
  return FTE3600_DEVICE_OK;
}
enum fte3600_device_error
fte3600_device_cancel (struct fte3600_device_session *s)
{
  if (!s)
    return FTE3600_DEVICE_INVALID_ARGUMENT;
  if (s->active)
    return FTE3600_DEVICE_BUSY;
  if (s->status.state != FTE3600_DEVICE_ARMED)
    return FTE3600_DEVICE_OK;
  /* Cancellation consumes only our pending ownership. Platform IRQ disable
   * belongs to the caller; retain calibrated sensor state for the next arm. */
  if (legacy_sensor (s->status.model))
    (void) legacy_run (s, FTE3600_LEGACY_CANCEL);
  s->legacy_request.request_pending = s->legacy_request.request_active = 0;
  s->status.state = FTE3600_DEVICE_IDLE;
  s->status.error = FTE3600_DEVICE_CANCELLED;
  return FTE3600_DEVICE_CANCELLED;
}
enum fte3600_device_error
fte3600_device_get_status (const struct fte3600_device_session *s,
                           struct fte3600_device_status        *status)
{
  if (!s || !status)
    return FTE3600_DEVICE_INVALID_ARGUMENT;
  *status = s->status;
  return FTE3600_DEVICE_OK;
}
const char *
fte3600_device_error_string (enum fte3600_device_error error)
{
  static const char *const names[] = {"ok", "invalid argument", "out of memory", "not ready", "busy",
                                      "platform/transport error", "startup failed", "calibration incomplete", "explicit firmware required",
                                      "unsupported device/path", "acquisition failed", "no fresh frame", "cancelled", "firmware model mismatch", "sensor recovery required"};

  return error >= FTE3600_DEVICE_OK && error <= FTE3600_DEVICE_RECOVERY_REQUIRED ? names[error] : "unknown session error";
}
void
fte3600_device_close (struct fte3600_device_session *s)
{
  if (s)
    {
      free_buffers (s);
      free (s);
    }
}
