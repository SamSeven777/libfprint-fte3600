/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_capture.h"

#include <string.h>

struct operation
{
  const struct fte3600_capture_io    *io;
  struct fte3600_capture_diagnostics *diagnostics;
};

size_t
fte3600_capture_frame_count (uint8_t requested)
{
  if (requested == 0)
    return 1;
  return requested > FTE3600_CAPTURE_MAX_FRAMES ?
         FTE3600_CAPTURE_MAX_FRAMES : requested;
}

static int
valid_io (const struct fte3600_capture_io *io)
{
  return io != NULL && io->spi != NULL && io->wait_ms != NULL;
}

static int
valid_output (uint8_t count, const uint8_t *payload, size_t capacity,
              const struct fte3600_capture_io *io)
{
  return valid_io (io) && payload != NULL &&
         capacity >= fte3600_capture_frame_count (count) *
         FTE3600_CAPTURE_FRAME_BYTES;
}

static void
wait_ms (const struct operation *op, uint32_t milliseconds)
{
  op->io->wait_ms (op->io->context, milliseconds);
}

/* The same buffer is reused, including bytes written by failed callbacks. */
static int
transfer (const struct operation *op, uint8_t *buffer, size_t length)
{
  for (unsigned int attempt = 0; attempt < 3; ++attempt)
    {
      uint32_t status = op->io->spi (op->io->context, buffer, length);
      if ((status & UINT32_C (0x80000000)) == 0)
        return 1;
      if (op->diagnostics != NULL)
        {
          ++op->diagnostics->failed_attempts;
          op->diagnostics->last_failure = status;
        }
      wait_ms (op, 10);
    }
  if (op->diagnostics != NULL)
    ++op->diagnostics->exhausted_transactions;
  return 0;
}

static void
command (const struct operation *op, uint8_t first, uint8_t second)
{
  uint8_t buffer[3] = {first, second, 0};

  (void) transfer (op, buffer, sizeof (buffer));
}

static uint8_t
read_byte (const struct operation *op, uint8_t address)
{
  uint8_t buffer[5] = {0x08, 0xf7, address, 0, 0};

  (void) transfer (op, buffer, sizeof (buffer));
  return buffer[4];
}

static void
write_byte (const struct operation *op, uint8_t address, uint8_t value)
{
  uint8_t buffer[4] = {0x09, 0xf6, address, value};

  (void) transfer (op, buffer, sizeof (buffer));
}

static int
read_word (const struct operation *op, uint16_t address, uint16_t *value)
{
  uint8_t buffer[12] = {
    0x04, 0xfb, (uint8_t) (0x80 | (address >> 8)), (uint8_t) address, 0, 1,
    0, 0, 0, 0, 0, 0
  };

  if (!transfer (op, buffer, sizeof (buffer)))
    return 0;
  *value = (uint16_t) ((uint16_t) buffer[6] << 8 | buffer[7]);
  return 1;
}

static void
write_word (const struct operation *op, uint16_t address, uint16_t value)
{
  uint8_t buffer[8] = {
    0x05, 0xfa, (uint8_t) (0x80 | (address >> 8)), (uint8_t) address, 0, 1,
    (uint8_t) (value >> 8), (uint8_t) value
  };

  (void) transfer (op, buffer, sizeof (buffer));
}

static int
checked_write (const struct operation *op,
               struct fte3600_capture_state *state,
               uint16_t address, uint16_t value)
{
  uint16_t observed;

  write_word (op, address, value);
  if (state->check_registers != 0)
    {
      if (!read_word (op, address, &observed))
        return 0;
      if (observed != value)
        state->check_registers = 2;
    }
  return 1;
}

static int
modify_word (const struct operation *op,
             struct fte3600_capture_state *state,
             uint16_t address, uint16_t keep, uint16_t set,
             int checked)
{
  uint16_t value;

  if (!read_word (op, address, &value))
    return 0;
  value = (uint16_t) ((value & keep) | set);
  if (checked)
    return checked_write (op, state, address, value);
  write_word (op, address, value);
  return 1;
}

static int
configure (const struct operation       *op,
           struct fte3600_capture_state *state)
{
  uint16_t shift_value = (uint16_t) (0x27c0 | state->shift |
                                     (state->shift >= 2 ? 8 : 0));
  /* Use unsigned arithmetic: period zero wraps before the shift. */
  uint16_t period_value = (uint16_t) (
    ((uint32_t) (uint16_t) (state->period - 1u) << 5) | 1u);

  state->mode = 0xa3;
  if (!checked_write (op, state, 0x1804, shift_value) ||
      !modify_word (op, state, 0x1806, 0xc07f, 0x0480, 0) ||
      !modify_word (op, state, 0x180a, 0xc000, 0x0483, 0) ||
      !modify_word (op, state, 0x180b, 0xc000, 0x0208, 0) ||
      !checked_write (op, state, 0x1807, period_value) ||
      !checked_write (op, state, 0x1887, state->channel_mode != 0 ? 2 : 0))
    return 0;
  if (state->configured == 1)
    return 1;
  if (!modify_word (op, state, 0x1805, 0xff0f, 0, 1) ||
      !modify_word (op, state, 0x1811, 0xfc00, 0x01fe, 1) ||
      !modify_word (op, state, 0x1a83, 0xffff, 0x0020, 0) ||
      !modify_word (op, state, 0x1a83, 0xffff, 0x0040, 0))
    return 0;
  write_byte (op, 0x8e, 0xf4);
  write_byte (op, 0x90, 0x00);
  write_byte (op, 0x91, 0x07);
  write_byte (op, 0x92, 0xd0);
  write_byte (op, 0x90, 0x01);
  state->configured = 1;
  return 1;
}

static int
fifo (const struct operation *op, uint8_t count, uint8_t *payload,
      int checked)
{
  static const uint8_t prefix[6] = {0x06, 0xf9, 0x9a, 0x05, 0x14, 0x00};
  uint8_t buffer[FTE3600_CAPTURE_TRANSFER_BYTES];

  for (size_t frame = 0; frame < fte3600_capture_frame_count (count); ++frame)
    {
      memset (buffer, 0, sizeof (buffer));
      memcpy (buffer, prefix, sizeof (prefix));
      if (!transfer (op, buffer, sizeof (buffer)) && checked)
        return 0;
      memcpy (payload + frame * FTE3600_CAPTURE_FRAME_BYTES,
              buffer + sizeof (prefix), FTE3600_CAPTURE_FRAME_BYTES);
    }
  return 1;
}

enum fte3600_capture_status
fte3600_capture_fifo (
  uint8_t count, uint8_t *payload, size_t capacity,
  const struct fte3600_capture_io *io,
  struct fte3600_capture_diagnostics *diagnostics)
{
  struct operation op = {io, diagnostics};

  if (!valid_output (count, payload, capacity, io))
    return FTE3600_CAPTURE_INVALID_ARGUMENT;
  if (diagnostics != NULL)
    memset (diagnostics, 0, sizeof (*diagnostics));
  (void) fifo (&op, count, payload, 0);
  return FTE3600_CAPTURE_OK;
}

static int
configuration_prefix (const struct operation       *op,
                      struct fte3600_capture_state *state)
{
  command (op, 0x5a, 0xa5);
  wait_ms (op, 1);
  if (read_byte (op, 0x80) != 0x50)
    {
      command (op, 0xc0, 0x3f);
      wait_ms (op, 1);
    }
  command (op, 0xa5, 0x5a);
  if (!checked_write (op, state, 0x1801, (uint16_t) (0xfc80 | state->dac)))
    return 0;
  state->dac_cache = state->dac;
  return checked_write (op, state, 0x1800, 0x4ffe) &&
         (state->mode == 0xa3 || configure (op, state));
}

enum fte3600_capture_status
fte3600_capture_configure (
  struct fte3600_capture_state *state, const struct fte3600_capture_io *io,
  struct fte3600_capture_diagnostics *diagnostics)
{
  struct operation op = {io, diagnostics};

  if (state == NULL || !valid_io (io))
    return FTE3600_CAPTURE_INVALID_ARGUMENT;
  if (diagnostics != NULL)
    memset (diagnostics, 0, sizeof (*diagnostics));
  return configuration_prefix (&op, state) ? FTE3600_CAPTURE_OK :
         FTE3600_CAPTURE_WORD_READ_ERROR;
}

static enum fte3600_capture_status
frames (
  struct fte3600_capture_state *state, uint8_t count,
  uint8_t *payload, size_t capacity, const struct fte3600_capture_io *io,
  struct fte3600_capture_diagnostics *diagnostics, int checked)
{
  struct operation op = {io, diagnostics};
  uint16_t value;
  unsigned int poll;

  if (state == NULL || !valid_output (count, payload, capacity, io))
    return FTE3600_CAPTURE_INVALID_ARGUMENT;
  if (diagnostics != NULL)
    memset (diagnostics, 0, sizeof (*diagnostics));
  if (!configuration_prefix (&op, state))
    return FTE3600_CAPTURE_WORD_READ_ERROR;
  command (&op, 0xc4, 0x3b);
  wait_ms (&op, 1);
  for (poll = 0; poll < 10; ++poll)
    if (read_byte (&op, 0x80) == 0x54)
      break;
  if (poll == 10 && diagnostics != NULL)
    diagnostics->ready_timeout = 1;
  if (!read_word (&op, 0x1800, &value))
    return FTE3600_CAPTURE_WORD_READ_ERROR;
  write_word (&op, 0x1800, (uint16_t) (value | 1));
  wait_ms (&op, 1);
  if (!fifo (&op, count, payload, checked))
    return FTE3600_CAPTURE_FIFO_READ_ERROR;
  write_word (&op, 0x1a84, 0x0020);
  return FTE3600_CAPTURE_OK;
}

enum fte3600_capture_status
fte3600_capture_frames (
  struct fte3600_capture_state *state, uint8_t count,
  uint8_t *payload, size_t capacity, const struct fte3600_capture_io *io,
  struct fte3600_capture_diagnostics *diagnostics)
{
  return frames (state, count, payload, capacity, io, diagnostics, 0);
}

enum fte3600_capture_status
fte3600_capture_frames_checked (
  struct fte3600_capture_state *state, uint8_t count,
  uint8_t *payload, size_t capacity, const struct fte3600_capture_io *io,
  struct fte3600_capture_diagnostics *diagnostics)
{
  return frames (state, count, payload, capacity, io, diagnostics, 1);
}

static uint32_t
recovery_spi (void *context, uint8_t *buffer, size_t length)
{
  const struct fte3600_recovery_io *io = context;
  uint8_t transmit[FTE3600_CAPTURE_TRANSFER_BYTES];

  if (length > sizeof (transmit))
    return UINT32_C (0x80000000);
  memcpy (transmit, buffer, length);
  return io->spi (io->context, transmit, buffer, length) == 0 ?
         0 : UINT32_C (0x80000000);
}

static void
recovery_wait (void *context, uint32_t milliseconds)
{
  const struct fte3600_recovery_io *io = context;

  io->wait_ms (io->context, milliseconds);
}

enum fte3600_capture_status
fte3600_capture_io_from_recovery (
  const struct fte3600_recovery_io *recovery,
  struct fte3600_capture_io        *output)
{
  if (recovery == NULL || recovery->spi == NULL ||
      recovery->wait_ms == NULL || output == NULL)
    return FTE3600_CAPTURE_INVALID_ARGUMENT;
  output->context = (void *) recovery;
  output->spi = recovery_spi;
  output->wait_ms = recovery_wait;
  return FTE3600_CAPTURE_OK;
}

enum fte3600_capture_status
fte3600_capture_bridge_reset_cumulative (
  struct fte3600_capture_bridge *bridge)
{
  if (bridge == NULL)
    return FTE3600_CAPTURE_INVALID_ARGUMENT;
  memset (&bridge->cumulative_diagnostics, 0,
          sizeof (bridge->cumulative_diagnostics));
  return FTE3600_CAPTURE_OK;
}

static uint32_t
saturated_add (uint32_t total, uint32_t value)
{
  return value > UINT32_MAX - total ? UINT32_MAX : total + value;
}

static void
accumulate_diagnostics (struct fte3600_capture_bridge *bridge)
{
  struct fte3600_capture_diagnostics *total = &bridge->cumulative_diagnostics;
  const struct fte3600_capture_diagnostics *latest = &bridge->diagnostics;

  total->failed_attempts = saturated_add (total->failed_attempts,
                                          latest->failed_attempts);
  total->exhausted_transactions = saturated_add (total->exhausted_transactions,
                                                 latest->exhausted_transactions);
  if (latest->last_failure != 0)
    total->last_failure = latest->last_failure;
  total->ready_timeout |= latest->ready_timeout;
}

int
fte3600_capture_image (void *context, uint8_t requested_dac,
                       uint16_t pixels[FTE3600_IMAGE_PIXELS])
{
  struct fte3600_capture_bridge *bridge = context;
  uint8_t payload[FTE3600_CAPTURE_FRAME_BYTES];
  enum fte3600_capture_status status;

  if (bridge == NULL || pixels == NULL || !valid_io (&bridge->io))
    return FTE3600_CAPTURE_INVALID_ARGUMENT;
  bridge->state.dac = requested_dac;
  status = fte3600_capture_frames (&bridge->state, 1, payload, sizeof (payload),
                                   &bridge->io, &bridge->diagnostics);
  accumulate_diagnostics (bridge);
  if (status != FTE3600_CAPTURE_OK)
    return status;
  for (size_t i = 0; i < FTE3600_IMAGE_PIXELS; ++i)
    pixels[i] = (uint16_t) ((uint16_t) payload[2 * i] << 8 | payload[2 * i + 1]);
  return FTE3600_CAPTURE_OK;
}
