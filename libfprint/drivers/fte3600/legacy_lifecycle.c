/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_legacy_lifecycle.h"
#include <string.h>

static int
boolean (int value)
{
  return value == 0 || value == 1;
}

static int
ready (const struct fte3600_recovery_io *io, int *is_ready)
{
  const uint8_t tx[] = {0x10, 0xef, 0x20, 0, 0, 0};
  uint8_t rx[sizeof tx] = {0};
  int error = io->spi (io->context, tx, rx, sizeof tx);

  *is_ready = !error && rx[4] == 0xa5 && rx[5] == 0x5a;
  return error;
}

static int
read_frame_flag (const struct fte3600_recovery_io *io, uint8_t *value)
{
  const uint8_t tx[] = {0x10, 0xef, 0x1d, 0, 0};
  uint8_t rx[sizeof tx] = {0};
  int error = io->spi (io->context, tx, rx, sizeof tx);

  *value = !error ? rx[4] : 0;
  return error;
}

static int
write_byte (const struct fte3600_recovery_io *io, uint8_t reg, uint8_t value)
{
  const uint8_t tx[] = {0x11, 0xee, reg, value, 0};
  uint8_t rx[sizeof tx] = {0};

  return io->spi (io->context, tx, rx, sizeof tx);
}

static int
frame_available (const struct fte3600_recovery_io *io)
{
  int is_ready, error = ready (io, &is_ready);
  uint8_t flag;

  if (!error && !is_ready)
    return 0;
  /* A failed MCU read still requires the frame-flag transaction. */
  if (read_frame_flag (io, &flag))
    return 0;
  return !error && is_ready && (flag == 1 || flag == 0xa0);
}

static int
resume_probe (enum fte3600_sensor               sensor,
              const struct fte3600_recovery_io *io)
{
  int is_ready;

  if (frame_available (io))
    return 1;
  if (!ready (io, &is_ready) && is_ready)
    (void) fte3600_legacy_arm (sensor, fte3600_legacy_geometry (sensor).rearm_mode, io);
  return 0;
}

static void
strict_suspend (enum fte3600_sensor               sensor,
                const struct fte3600_recovery_io *io)
{
  int is_ready;
  uint8_t ignored;

  if (ready (io, &is_ready) || !is_ready)
    return;
  if (write_byte (io, sensor == FTE3600_FT9536 ? 0x47 : 0x76, 1) ||
      write_byte (io, 0x1f, 1) || write_byte (io, 0x1e, 1))
    return;
  io->wait_ms (io->context, 10);
  if (read_frame_flag (io, &ignored) || ready (io, &is_ready))
    return;
  if (is_ready)
    (void) write_byte (io, 0, 0x20);
}

static size_t
fetch (enum fte3600_sensor sensor,
       struct fte3600_legacy_lifecycle_state *state,
       const struct fte3600_legacy_lifecycle_options *options,
       uint8_t *image, size_t capacity,
       const struct fte3600_legacy_lifecycle_io *io)
{
  struct fte3600_legacy_frame_result frame =
    fte3600_legacy_frame_fetch (sensor, image, capacity, &io->protocol);

  if (state->request_pending && !options->completion_cancelled)
    {
      io->complete (io->request_context, 1, FTE3600_LEGACY_COMPLETE_OK,
                    frame.image_length ? image : NULL, frame.image_length);
      state->request_pending = 0;
    }
  return frame.image_length;
}

struct fte3600_legacy_lifecycle_result
fte3600_legacy_lifecycle_run (
  enum fte3600_sensor sensor, enum fte3600_legacy_lifecycle_operation operation,
  struct fte3600_legacy_lifecycle_state *state,
  const struct fte3600_legacy_lifecycle_options *options,
  uint8_t *image, size_t capacity,
  const struct fte3600_legacy_lifecycle_io *io)
{
  struct fte3600_legacy_lifecycle_result result = {
    FTE3600_LEGACY_LIFECYCLE_INVALID_INPUT, 0
  };
  struct fte3600_legacy_geometry geometry = fte3600_legacy_geometry (sensor);
  int is_ready;

  if (!geometry.image_bytes || operation < FTE3600_LEGACY_START ||
      operation > FTE3600_LEGACY_CANCEL || state == NULL || options == NULL ||
      io == NULL || image == NULL || capacity < geometry.image_bytes ||
      io->protocol.spi == NULL || io->protocol.wait_ms == NULL ||
      io->initialize == NULL || io->complete == NULL ||
      !boolean (state->request_pending) || !boolean (options->completion_cancelled) ||
      !boolean (options->cancel_matches) || !boolean (options->cancel_context_guard))
    return result;
  memset (image, 0, geometry.image_bytes);
  result.status = operation == FTE3600_LEGACY_IRQ ?
                  FTE3600_LEGACY_LIFECYCLE_HANDLED : FTE3600_LEGACY_LIFECYCLE_OK;
  switch (operation)
    {
    case FTE3600_LEGACY_START:
      if (state->request_kind != 2)
        break;
      if (frame_available (&io->protocol) && state->cached_frame == 1)
        {
          state->cached_frame = 0;
          if (state->request_pending)
            result.image_length = fetch (sensor, state, options, image, capacity, io);
        }
      else if (!ready (&io->protocol, &is_ready) && is_ready)
        {
          (void) fte3600_legacy_arm (sensor, 1, &io->protocol);
        }
      break;

    case FTE3600_LEGACY_IRQ:
      state->irq_count = (uint8_t) (state->irq_count + 1u);
      if (!state->irq_enabled || state->suppress_irq == 1 || !state->request_pending)
        break;
      if (frame_available (&io->protocol))
        {
          result.image_length = fetch (sensor, state, options, image, capacity, io);
        }
      else if (!ready (&io->protocol, &is_ready) && is_ready)
        {
          (void) fte3600_legacy_arm (sensor, geometry.rearm_mode, &io->protocol);
        }
      else if (!state->health_running)
        {
          state->health_running = 1;
          (void) io->initialize (io->request_context);
          state->health_running = 0;
        }
      break;

    case FTE3600_LEGACY_RESUME:
      state->cached_frame = 0;
      if (options->power_policy == 2 && resume_probe (sensor, &io->protocol))
        state->cached_frame = 1;
      else
        (void) io->initialize (io->request_context);
      break;

    case FTE3600_LEGACY_SUSPEND:
      if (options->power_policy == 2)
        {
          if (state->power_flag == 1)
            {
              if (resume_probe (sensor, &io->protocol))
                break;
            }
          else
            {
              state->power_flag = 1;
            }
          (void) fte3600_legacy_idle (sensor, &io->protocol);
          io->protocol.wait_ms (io->protocol.context, 5);
          (void) fte3600_legacy_arm (sensor, 3, &io->protocol);
        }
      else
        {
          strict_suspend (sensor, &io->protocol);
        }
      break;

    case FTE3600_LEGACY_CANCEL: {
        int matched = state->request_pending && options->cancel_matches;
        if (matched)
          {
            state->request_pending = 0;
            state->request_active = 0;
          }
        io->complete (io->request_context, matched,
                      matched ? FTE3600_LEGACY_COMPLETE_CANCELLED : FTE3600_LEGACY_COMPLETE_OK,
                      NULL, 0);
        break;
      }
    }
  return result;
}
