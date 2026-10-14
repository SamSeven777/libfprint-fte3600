/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_legacy_boot.h"
#include "fte3600_reset_sync.h"
#include <stdlib.h>
#include <string.h>

/* Existing components collapse raw errors to IO_ERROR only for immediately
 * terminal callbacks. Record the LAST callback, including success, and consult
 * it only when the component returns IO_ERROR. A swallowed readback error,
 * not-ready read or successful diagnostic must never poison a later result. */
struct observed_io
{
  const struct fte3600_recovery_io *source;
  int                               last_result;
};

static int
observed_gpio (void *context, int level)
{
  struct observed_io *o = context;

  o->last_result = o->source->gpio (o->source->context, level);
  return o->last_result;
}

static int
observed_spi (void *context, const uint8_t *tx, uint8_t *rx, size_t n)
{
  struct observed_io *o = context;

  o->last_result = o->source->spi (o->source->context, tx, rx, n);
  return o->last_result;
}

static int
observed_release_and_sync (void *context, int *gpio_status)
{
  struct observed_io *o = context;

  o->last_result = o->source->release_and_sync (o->source->context, gpio_status);
  return o->last_result;
}

static void
observed_wait (void *context, uint32_t ms)
{
  struct observed_io *o = context;

  o->source->wait_ms (o->source->context, ms);
}

static uint32_t
observed_clock (void *context)
{
  struct observed_io *o = context;

  return o->source->clock_ms (o->source->context);
}

static enum fte3600_legacy_boot_status
raw_status (int error)
{
  return error == 0 ? FTE3600_LEGACY_BOOT_OK :
         error == FTE3600_LEGACY_BOOT_RAW_DEVICE_ABSENT ?
         FTE3600_LEGACY_BOOT_DEVICE_ABSENT : FTE3600_LEGACY_BOOT_IO_ERROR;
}

static void
phase (const struct fte3600_legacy_boot_observer *o,
       enum fte3600_legacy_boot_phase p, int entering, enum fte3600_legacy_boot_status status)
{
  if (o)
    o->phase (o->context, p, entering, status);
}
static void
reset_observation (void *context, int entering, int raw)
{
  const struct fte3600_legacy_boot_observer *o = context;

  phase (o, FTE3600_LEGACY_PHASE_RESET, entering,
         entering ? FTE3600_LEGACY_BOOT_NOT_RUN : raw_status (raw));
}

static enum fte3600_legacy_boot_status
runtime_status (
  enum fte3600_legacy_status status, const struct observed_io *o)
{
  if (status == FTE3600_LEGACY_OK)
    return FTE3600_LEGACY_BOOT_OK;
  if (status == FTE3600_LEGACY_IO_ERROR)
    return raw_status (o->last_result);
  return FTE3600_LEGACY_BOOT_INVALID_INPUT;
}

struct fte3600_legacy_boot_profile
fte3600_legacy_boot_defaults (enum fte3600_sensor sensor)
{
  struct fte3600_legacy_boot_profile p = {0};

  switch (sensor)
    {
    case FTE3600_FT9338:
      p.expected_firmware = 0x40;
      p.expected_agc = 0x10;
      p.startup_wait_ms = 80;
      break;

    case FTE3600_FT9348:
    case FTE3600_FT9361:
      p.expected_firmware = 0x30;
      p.expected_agc = 0x31;
      p.startup_wait_ms = 160;
      break;

    case FTE3600_FT9536:
      p.expected_firmware = 0x23;
      p.expected_agc = 0x13;
      p.startup_wait_ms = 180;
      break;

    case FTE3600_SENSOR_UNKNOWN:
    default: return p;
    }
  p.sensor = sensor;
  p.boot = FTE3600_RECOVERY_BOOT_A;
  return p;
}

static int
read_bytes (const struct fte3600_recovery_io *io, uint8_t reg,
            uint8_t *value, size_t n)
{
  const uint8_t tx[6] = {0x10, 0xef, reg, 0, 0, 0};
  uint8_t rx[6] = {0};
  int error = io->spi (io->context, tx, rx, n + 4);

  if (error == 0)
    memcpy (value, rx + 4, n);
  return error;
}

static enum fte3600_legacy_boot_status
version (
  const struct fte3600_legacy_boot_profile *p, const struct fte3600_recovery_io *io)
{
  uint8_t firmware, agc;
  int error = read_bytes (io, 0x1a, &firmware, 1);

  if (error)
    return raw_status (error);
  error = read_bytes (io, 0x3c, &agc, 1);
  if (error)
    return raw_status (error);
  return firmware == p->expected_firmware && agc == p->expected_agc ?
         FTE3600_LEGACY_BOOT_OK : FTE3600_LEGACY_BOOT_VERSION_MISMATCH;
}

static int
reset_raw (const struct fte3600_recovery_io *io)
{
  int error = io->gpio (io->context, 1);

  if (error)
    return error;
  io->wait_ms (io->context, 10);
  error = io->gpio (io->context, 0);
  if (error)
    return error;
  io->wait_ms (io->context, 20);
  return io->gpio (io->context, 1);
}

static int
reset (const struct fte3600_recovery_io          *io,
       const struct fte3600_legacy_boot_observer *o)
{
  phase (o, FTE3600_LEGACY_PHASE_RESET, 1, FTE3600_LEGACY_BOOT_NOT_RUN);
  int error = reset_raw (io);
  phase (o, FTE3600_LEGACY_PHASE_RESET, 0, raw_status (error));
  return error;
}

static enum fte3600_legacy_boot_status
upload_a8 (
  const struct fte3600_legacy_boot_profile *p, const uint8_t *firmware,
  size_t n, const struct fte3600_recovery_io *io,
  const struct fte3600_legacy_boot_observer *o)
{
  const uint8_t idle = 0x70;
  const struct fte3600_reset_observer ro = {(void *) o, reset_observation};
  uint8_t small_rx[2] = {0}, reply[2] = {0};

  uint8_t *tx = calloc (n + 7, 1), *rx = calloc (n + 7, 1);
  unsigned attempt;
  int error = 0;
  enum fte3600_legacy_boot_status status = FTE3600_LEGACY_BOOT_NO_MEMORY;
  if (!tx || !rx)
    goto done;
  tx[0] = 0x05;
  tx[1] = 0xfa;
  tx[4] = (uint8_t) (n >> 8);
  tx[5] = (uint8_t) n;
  memcpy (tx + 6, firmware, n);
  error = fte3600_reset_and_sync (io, o ? &ro : NULL);
  if (error)
    goto transport_error;
  error = io->spi (io->context, tx, rx, n + 7);
  if (error)
    goto transport_error;
  io->wait_ms (io->context, 2);
  error = reset (io, o);
  if (error)
    goto transport_error;
  io->wait_ms (io->context, 10);
  error = reset (io, o);
  if (error)
    goto transport_error;
  io->wait_ms (io->context, p->startup_wait_ms);
  error = io->spi (io->context, &idle, small_rx, 1);
  if (error)
    goto transport_error;
  io->wait_ms (io->context, 5);
  error = io->spi (io->context, &idle, small_rx, 1);
  if (error)
    goto transport_error;
  io->wait_ms (io->context, 2);
  for (attempt = 0; attempt < 20; ++attempt)
    {
      if (read_bytes (io, 0x20, reply, 2) == 0 &&
          reply[0] == 0xa5 && reply[1] == 0x5a)
        {
          (void) read_bytes (io, 0x14, reply, 1);
          (void) read_bytes (io, 0x15, reply, 1);
          status = FTE3600_LEGACY_BOOT_OK;
          goto done;
        }
      io->wait_ms (io->context, 2);
    }
  status = FTE3600_LEGACY_BOOT_FAILED;
  goto done;
transport_error:
  status = raw_status (error);
done:
  free (rx);
  free (tx);
  return status;
}

static enum fte3600_legacy_boot_status
load (
  const struct fte3600_legacy_boot_profile *p, const uint8_t *firmware,
  size_t n, uint8_t *irq, const struct fte3600_recovery_io *io,
  const struct observed_io *observed,
  const struct fte3600_legacy_boot_observer *o)
{
  enum fte3600_legacy_boot_status status = FTE3600_LEGACY_BOOT_FAILED;
  unsigned attempt;

  phase (o, FTE3600_LEGACY_PHASE_LOAD, 1, FTE3600_LEGACY_BOOT_NOT_RUN);
  *irq = 0;
  for (attempt = 0; attempt < 5; ++attempt)
    {
      enum fte3600_legacy_boot_phase child = p->sensor == FTE3600_FT9348 || p->sensor == FTE3600_FT9361 ?
                                             FTE3600_LEGACY_PHASE_UPLOAD_A8 : p->boot == FTE3600_RECOVERY_BOOT_A ?
                                             FTE3600_LEGACY_PHASE_BOOT_A : FTE3600_LEGACY_PHASE_BOOT_B;
      phase (o, child, 1, FTE3600_LEGACY_BOOT_NOT_RUN);
      if (p->sensor == FTE3600_FT9348 || p->sensor == FTE3600_FT9361)
        {
          status = upload_a8 (p, firmware, n, io, o);
        }
      else
        {
          const struct fte3600_reset_observer ro = {(void *) o, reset_observation};
          enum fte3600_recovery_status recovered = fte3600_recover_observed (
            p->boot, firmware, n, p->startup_wait_ms, io, o ? &ro : NULL);
          switch (recovered)
            {
            case FTE3600_RECOVERY_OK: status = FTE3600_LEGACY_BOOT_OK;
              break;

            case FTE3600_RECOVERY_IO_ERROR: status = raw_status (observed->last_result);
              break;

            case FTE3600_RECOVERY_FAILED: status = FTE3600_LEGACY_BOOT_FAILED;
              break;

            case FTE3600_RECOVERY_NO_MEMORY: status = FTE3600_LEGACY_BOOT_NO_MEMORY;
              break;

            case FTE3600_RECOVERY_INVALID_INPUT:
            default: status = FTE3600_LEGACY_BOOT_INVALID_INPUT;
              break;
            }
        }
      phase (o, child, 0, status);
      if (status == FTE3600_LEGACY_BOOT_OK ||
          status == FTE3600_LEGACY_BOOT_DEVICE_ABSENT)
        break;
    }
  *irq = 1;
  phase (o, FTE3600_LEGACY_PHASE_LOAD, 0, status);
  return status;
}

struct arm_observation
{
  const struct fte3600_legacy_boot_observer *observer;
  const struct observed_io                  *io;
};
static void
arm_idle_observation (void *c, int entry, enum fte3600_legacy_status status)
{
  const struct arm_observation *a = c;

  phase (a->observer, FTE3600_LEGACY_PHASE_IDLE, entry,
         entry ? FTE3600_LEGACY_BOOT_NOT_RUN : runtime_status (status, a->io));
}

static int
valid_input (
  const struct fte3600_legacy_boot_profile *p,
  enum fte3600_legacy_boot_operation operation,
  const uint8_t *firmware, size_t n, const uint8_t *irq,
  const struct fte3600_recovery_io *source)
{
  int a8;

  if (!p || !source || !source->gpio || !source->spi || !source->wait_ms ||
      !source->clock_ms || !firmware || n == 0 ||
      n > FTE3600_RECOVERY_MAX_FIRMWARE || !irq ||
      !fte3600_legacy_geometry (p->sensor).image_bytes ||
      (p->boot != FTE3600_RECOVERY_BOOT_A && p->boot != FTE3600_RECOVERY_BOOT_B) ||
      operation < FTE3600_LEGACY_BOOT_VERSION || operation > FTE3600_LEGACY_BOOT_UPLOAD_A8)
    return 0;
  a8 = p->sensor == FTE3600_FT9348 || p->sensor == FTE3600_FT9361;
  return !(a8 && p->boot != FTE3600_RECOVERY_BOOT_A) &&
         !(!a8 && operation == FTE3600_LEGACY_BOOT_UPLOAD_A8);
}

enum fte3600_legacy_boot_status
fte3600_legacy_boot_run_observed (
  const struct fte3600_legacy_boot_profile *p,
  enum fte3600_legacy_boot_operation operation,
  const uint8_t *firmware, size_t n, uint8_t *irq,
  const struct fte3600_recovery_io *source,
  const struct fte3600_legacy_boot_observer *o)
{
  struct observed_io observed = {source, 0};
  const struct fte3600_recovery_io io = {
    &observed, observed_gpio, observed_spi, observed_wait, observed_clock,
    source && source->release_and_sync ? observed_release_and_sync : NULL
  };
  enum fte3600_legacy_boot_status status;
  enum fte3600_legacy_status runtime;

  if (!valid_input (p, operation, firmware, n, irq, source) || (o && !o->phase))
    return FTE3600_LEGACY_BOOT_INVALID_INPUT;
  if (operation == FTE3600_LEGACY_BOOT_VERSION)
    return version (p, &io);
  if (operation == FTE3600_LEGACY_BOOT_UPLOAD_A8)
    return upload_a8 (p, firmware, n, &io, o);
  if (operation == FTE3600_LEGACY_BOOT_LOAD)
    return load (p, firmware, n, irq, &io, &observed, o);
  phase (o, FTE3600_LEGACY_PHASE_IDLE, 1, FTE3600_LEGACY_BOOT_NOT_RUN);
  runtime = fte3600_legacy_idle (p->sensor, &io);
  phase (o, FTE3600_LEGACY_PHASE_IDLE, 0, runtime_status (runtime, &observed));
  if (runtime != FTE3600_LEGACY_OK)
    return runtime_status (runtime, &observed);
  phase (o, FTE3600_LEGACY_PHASE_VERSION, 1, FTE3600_LEGACY_BOOT_NOT_RUN);
  status = version (p, &io);
  phase (o, FTE3600_LEGACY_PHASE_VERSION, 0, status);
  if (status != FTE3600_LEGACY_BOOT_OK)
    {
      status = load (p, firmware, n, irq, &io, &observed, o);
      if (status != FTE3600_LEGACY_BOOT_OK)
        return status;
    }
  phase (o, FTE3600_LEGACY_PHASE_CONFIG, 1, FTE3600_LEGACY_BOOT_NOT_RUN);
  runtime = fte3600_legacy_config (p->sensor, &io);
  phase (o, FTE3600_LEGACY_PHASE_CONFIG, 0, runtime_status (runtime, &observed));
  if (runtime != FTE3600_LEGACY_OK)
    return runtime_status (runtime, &observed);
  phase (o, FTE3600_LEGACY_PHASE_ARM, 1, FTE3600_LEGACY_BOOT_NOT_RUN);
  const struct arm_observation arm = {o, &observed};
  const struct fte3600_legacy_arm_observer arm_observer = {(void *) &arm, arm_idle_observation};
  runtime = fte3600_legacy_arm_observed (p->sensor, 1, &io, o ? &arm_observer : NULL);
  phase (o, FTE3600_LEGACY_PHASE_ARM, 0, runtime_status (runtime, &observed));
  return FTE3600_LEGACY_BOOT_OK;
}

enum fte3600_legacy_boot_status
fte3600_legacy_boot_run (
  const struct fte3600_legacy_boot_profile *p, enum fte3600_legacy_boot_operation operation,
  const uint8_t *firmware, size_t n, uint8_t *irq, const struct fte3600_recovery_io *source)
{
  return fte3600_legacy_boot_run_observed (p, operation, firmware, n, irq, source, NULL);
}

struct lifecycle_bridge
{
  const struct fte3600_legacy_boot_profile      *profile;
  const uint8_t                                 *firmware;
  size_t                                         firmware_length;
  const struct fte3600_legacy_boot_lifecycle_io *io;
  uint8_t                                        irq;
  struct fte3600_legacy_boot_lifecycle_result   *result;
};

static int
bridge_initialize (void *context)
{
  struct lifecycle_bridge *bridge = context;

  bridge->result->initialize_called = 1;
  bridge->result->initialize_status = fte3600_legacy_boot_run (
    bridge->profile, FTE3600_LEGACY_BOOT_INITIALIZE, bridge->firmware,
    bridge->firmware_length, &bridge->irq, &bridge->io->protocol);
  return bridge->result->initialize_status;
}

static void
bridge_complete (void *context, int matched_pending,
                 enum fte3600_legacy_completion status, const uint8_t *image, size_t image_length)
{
  struct lifecycle_bridge *bridge = context;

  bridge->io->complete (bridge->io->completion_context, matched_pending,
                        status, image, image_length);
}

static int
boolean (int value)
{
  return value == 0 || value == 1;
}

struct fte3600_legacy_boot_lifecycle_result
fte3600_legacy_boot_lifecycle_run (
  const struct fte3600_legacy_boot_profile *profile,
  enum fte3600_legacy_lifecycle_operation operation,
  struct fte3600_legacy_lifecycle_state *state,
  const struct fte3600_legacy_lifecycle_options *options,
  uint8_t *image, size_t capacity, const uint8_t *firmware, size_t firmware_length,
  const struct fte3600_legacy_boot_lifecycle_io *io)
{
  struct fte3600_legacy_boot_lifecycle_result result = {
    {FTE3600_LEGACY_LIFECYCLE_INVALID_INPUT, 0}, 0, FTE3600_LEGACY_BOOT_NOT_RUN
  };
  struct lifecycle_bridge bridge;
  struct fte3600_legacy_lifecycle_io lifecycle_io;

  if (!state || !options || !image || !io || !io->complete ||
      !valid_input (profile, FTE3600_LEGACY_BOOT_INITIALIZE, firmware,
                    firmware_length, &state->irq_enabled, &io->protocol) ||
      capacity < fte3600_legacy_geometry (profile->sensor).image_bytes ||
      operation < FTE3600_LEGACY_START || operation > FTE3600_LEGACY_CANCEL ||
      !boolean (state->request_pending) || !boolean (options->completion_cancelled) ||
      !boolean (options->cancel_matches) || !boolean (options->cancel_context_guard))
    return result;
  bridge.profile = profile;
  bridge.firmware = firmware;
  bridge.firmware_length = firmware_length;
  bridge.io = io;
  bridge.irq = state->irq_enabled;
  bridge.result = &result;
  lifecycle_io.protocol = io->protocol;
  lifecycle_io.request_context = &bridge;
  lifecycle_io.initialize = bridge_initialize;
  lifecycle_io.complete = bridge_complete;
  result.lifecycle = fte3600_legacy_lifecycle_run (profile->sensor, operation,
                                                   state, options, image, capacity, &lifecycle_io);
  if (result.initialize_called)
    state->irq_enabled = bridge.irq;
  return result;
}
