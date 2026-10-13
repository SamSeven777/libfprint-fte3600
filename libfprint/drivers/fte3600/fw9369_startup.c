/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_fw9369_startup.h"
#include <string.h>

struct startup
{
  struct fte3600_fw9369_startup_state          *s;
  struct fte3600_fw9369_startup_workspace      *w;
  const struct fte3600_capture_io              *io;
  struct fte3600_fw9369_startup_diagnostics    *d;
  struct fte3600_image_state                   *search;
  enum fte3600_fw9369_startup_status error;
  const struct fte3600_fw9369_startup_observer *observer;
  int                                           checked, linux_detection;
};
static void
observe (struct startup *op, enum fte3600_fw9369_startup_phase phase,
         int entering, enum fte3600_fw9369_startup_status status)
{
  if (op->observer && op->observer->phase)
    op->observer->phase (op->observer->context, phase, entering, status);
}
static uint32_t
add (uint32_t a, uint32_t b)
{
  return UINT32_MAX - a < b ? UINT32_MAX : a + b;
}
static void
accumulate (struct startup                           *op,
            const struct fte3600_capture_diagnostics *d)
{
  struct fte3600_capture_diagnostics *out = &op->d->transport;

  out->failed_attempts = add (out->failed_attempts, d->failed_attempts);
  out->exhausted_transactions = add (out->exhausted_transactions,
                                     d->exhausted_transactions);
  if (d->last_failure != 0)
    out->last_failure = d->last_failure;
  out->ready_timeout |= d->ready_timeout;
}
static void
wait_ms (struct startup *op, uint32_t ms)
{
  op->io->wait_ms (op->io->context, ms);
}
static int
transfer (struct startup *op, uint8_t *bytes, size_t count)
{
  for (unsigned int i = 0; i < 3; ++i)
    {
      uint32_t status = op->io->spi (op->io->context, bytes, count);
      if ((status & UINT32_C (0x80000000)) == 0)
        return 1;
      op->d->transport.failed_attempts = add (op->d->transport.failed_attempts, 1);
      op->d->transport.last_failure = status;
      wait_ms (op, 10);
    }
  op->d->transport.exhausted_transactions = add (
    op->d->transport.exhausted_transactions, 1);
  return 0;
}
static void
command (struct startup *op, uint8_t a, uint8_t b)
{
  uint8_t bytes[3] = {a, b, 0};

  (void) transfer (op, bytes, sizeof (bytes));
}
static void
write_byte (struct startup *op, uint8_t address, uint8_t value)
{
  uint8_t bytes[4] = {9, 0xf6, address, value};

  (void) transfer (op, bytes, sizeof (bytes));
}
static uint8_t
read_byte (struct startup *op, uint8_t address)
{
  uint8_t bytes[5] = {8, 0xf7, address, 0, 0};

  (void) transfer (op, bytes, sizeof (bytes));
  return bytes[4];
}
static int
write_word (struct startup *op, uint16_t address, uint16_t value)
{
  uint8_t bytes[8] = {5, 0xfa, (uint8_t) (0x80 | (address >> 8)),
                      (uint8_t) address, 0, 1, (uint8_t) (value >> 8), (uint8_t) value};

  return transfer (op, bytes, sizeof (bytes));
}
static int
read_word (struct startup *op, uint16_t address, uint16_t *value)
{
  uint8_t bytes[12] = {4, 0xfb, (uint8_t) (0x80 | (address >> 8)),
                       (uint8_t) address, 0, 1};

  if (!transfer (op, bytes, sizeof (bytes)))
    return 0;
  *value = (uint16_t) ((uint16_t) bytes[6] << 8 | bytes[7]);
  return 1;
}
static void
idle (struct startup *op)
{
  command (op, 0x5a, 0xa5);
  wait_ms (op, 1);
  if (read_byte (op, 0x80) != 0x50)
    {
      command (op, 0xc0, 0x3f);
      wait_ms (op, 1);
    }
  command (op, 0xa5, 0x5a);
}
static uint16_t
crc (const uint16_t *words, size_t count)
{
  uint16_t result = 0xffff;

  for (size_t i = 0; i < count; ++i)
    for (unsigned int byte = 0; byte < 2; ++byte)
      {
        result ^= (uint16_t) ((uint16_t) (uint8_t) (words[i] >> (8 * byte)) << 8);
        for (unsigned int bit = 0; bit < 8; ++bit)
          result = (uint16_t) ((result << 1) ^ ((result & 0x8000) ? 0x1021 : 0));
      }
  return result;
}
static enum fte3600_fw9369_startup_status
fdt_call (struct startup *op,
          enum fte3600_fw9369_fdt_operation operation, uint16_t *channels)
{
  struct fte3600_fw9369_fdt_image_config image = {
    op->s->image_dac, op->s->shift, op->s->period
  };
  struct fte3600_capture_diagnostics diagnostics;
  enum fte3600_fw9369_fdt_status status = fte3600_fw9369_fdt_run (operation,
                                                                  &op->s->fdt, &op->s->config, &image, channels, 8, op->io, &diagnostics);

  accumulate (op, &diagnostics);
  return (enum fte3600_fw9369_startup_status) status;
}
static enum fte3600_fw9369_startup_status
fdt_baseline (struct startup *op)
{
  uint16_t minima[4], previous[4], channels[8], stable = 0;
  int have_minimum = 0;
  enum fte3600_fw9369_startup_status result = FTE3600_FW9369_STARTUP_FAILED;

  for (unsigned int attempt = 0; attempt < 3; ++attempt)
    {
      enum fte3600_fw9369_startup_status status;
      ++op->d->calibration_attempts;
      status = fdt_call (op, FTE3600_FW9369_FDT_CALIBRATE, NULL);
      if (status > FTE3600_FW9369_STARTUP_FAILED)
        return status;
      if (status != FTE3600_FW9369_STARTUP_OK)
        {
          ++op->d->calibration_failures;
          continue;
        }
      have_minimum = 1;
      for (size_t c = 0; c < 4; ++c)
        minima[c] = 1023;
      for (unsigned int sample = 0; sample < 10; ++sample)
        {
          status = fdt_call (op, FTE3600_FW9369_FDT_MEASURE, channels);
          if (status != FTE3600_FW9369_STARTUP_OK)
            return status;
          ++stable;
          for (size_t c = 0; c < 4; ++c)
            {
              if (channels[c] < minima[c])
                minima[c] = channels[c];
              if (sample && ((int) channels[c] - previous[c] > 5 ||
                             (int) previous[c] - channels[c] > 5))
                stable = 1;
              previous[c] = channels[c];
            }
          if (sample && stable >= 3)
            break;
        }
      result = FTE3600_FW9369_STARTUP_OK;
      for (size_t c = 0; c < 4; ++c)
        if (minima[c] < 300 || minima[c] > 700)
          result = FTE3600_FW9369_STARTUP_FAILED;
      if (result == FTE3600_FW9369_STARTUP_OK)
        break;
    }
  if (!have_minimum)
    return FTE3600_FW9369_STARTUP_UNDEFINED_BASELINE;
  for (size_t c = 0; c < 4; ++c)
    {
      uint16_t value = minima[c] < 31 ? 0 : (uint16_t) (minima[c] - 30);
      op->s->fdt_reference[c] = op->s->fdt_down[c] = op->s->fdt_up[c] = value;
    }
  op->s->fdt_down_crc = op->s->fdt_up_crc = crc (op->s->fdt_up, 4);
  return result;
}
static int
capture (void *context, uint8_t dac, uint16_t *pixels)
{
  struct startup *op = context;
  struct fte3600_fw9369_startup_state *s = op->s;
  struct fte3600_capture_state image;
  struct fte3600_capture_diagnostics diagnostics;
  enum fte3600_capture_status status;

  if (op->search != NULL)
    s->fdt.configured = op->search->auxiliary_state;
  s->image_dac = dac;
  image = (struct fte3600_capture_state){dac, s->shift, s->config.channel_mode,
                                         s->fdt.mode, s->fdt.configured, s->fdt.check_registers, s->fdt.dac_cache,
                                         s->period};
  status = (op->checked ? fte3600_capture_frames_checked : fte3600_capture_frames)(
    &image, 1, op->w->raw, sizeof (op->w->raw), op->io, &diagnostics);
  s->fdt.mode = image.mode;
  s->fdt.configured = image.configured;
  s->fdt.check_registers = image.check_registers;
  s->fdt.dac_cache = image.dac_cache;
  if (op->search != NULL)
    op->search->auxiliary_state = image.configured;
  accumulate (op, &diagnostics);
  if (status != FTE3600_CAPTURE_OK)
    {
      op->error = status == FTE3600_CAPTURE_FIFO_READ_ERROR ?
                  FTE3600_FW9369_STARTUP_FIFO_READ_ERROR : FTE3600_FW9369_STARTUP_WORD_READ_ERROR;
      return 1;
    }
  for (size_t i = 0; i < FTE3600_IMAGE_PIXELS; ++i)
    pixels[i] = (uint16_t) ((uint16_t) op->w->raw[i * 2] << 8 | op->w->raw[i * 2 + 1]);
  return 0;
}
static enum fte3600_fw9369_startup_status
warm_baseline (
  struct fte3600_fw9369_startup_state *s)
{
  /* Validate before any retained baseline mutation. The final row is not an
  * observed edge; its neighbor-dependent branch has no portable meaning. */
  for (size_t i = 79 * 64 + 1; i < FTE3600_IMAGE_PIXELS; ++i)
    if ((int) s->work_image[i] - (int) s->baseline[i] > 300)
      return FTE3600_FW9369_STARTUP_UNDEFINED_NEIGHBORHOOD;
  for (size_t i = 0; i < FTE3600_IMAGE_PIXELS; ++i)
    {
      int difference = (int) s->work_image[i] - (int) s->baseline[i];
      if (difference <= 0)
        continue;
      if (difference > 300 && i / 64 != 0 && i / 64 != 63 && i % 64 != 0)
        {
          const size_t neighbors[4] = {i - 64, i + 64, i - 1, i + 1};
          int supported = 0;
          for (size_t n = 0; n < 4; ++n)
            if ((int) s->work_image[neighbors[n]] -
                (int) s->baseline[neighbors[n]] >= 100)
              supported = 1;
          if (!supported)
            continue;
        }
      s->baseline[i] = s->work_image[i];
    }
  return FTE3600_FW9369_STARTUP_OK;
}
static enum fte3600_fw9369_startup_status
image_baseline (struct startup *op,
                uint8_t         reset)
{
  struct fte3600_fw9369_startup_state *s = op->s;

  if (reset == 1)
    {
      struct fte3600_image_state search = {s->image_dac, s->image_valid != 0,
                                           s->image_median, s->fdt.configured};
      enum fte3600_image_status result;
      op->search = &search;
      if (s->image_valid == 0)
        s->image_valid = 1;
      result = fte3600_image_search (&search, op->w->image, capture, op);
      op->search = NULL;
      s->image_dac = search.dac;
      s->image_median = search.stored_median;
      s->fdt.configured = search.auxiliary_state;
      if (result == FTE3600_IMAGE_CAPTURE_ERROR)
        return op->error;
      if (result != FTE3600_IMAGE_OK)
        return FTE3600_FW9369_STARTUP_FAILED;
    }
  if (capture (op, s->image_dac, s->work_image))
    return op->error;
  for (unsigned int i = 1; i < s->image_count; ++i)
    {
      if (capture (op, s->image_dac, op->w->image))
        return op->error;
      for (size_t p = 0; p < FTE3600_IMAGE_PIXELS; ++p)
        s->work_image[p] = (uint16_t) (s->work_image[p] + op->w->image[p]);
    }
  if (reset == 1)
    {
      memcpy (s->baseline, s->work_image, sizeof (s->baseline));
    }
  else
    {
      enum fte3600_fw9369_startup_status result = warm_baseline (s);
      if (result != FTE3600_FW9369_STARTUP_OK)
        return result;
    }
  s->baseline_crc = crc (s->baseline, 128);
  s->baseline_flag_1 = s->baseline_flag_2 = 1;
  return FTE3600_FW9369_STARTUP_OK;
}
static void
reset_cached (struct fte3600_fw9369_startup_state *s)
{
  s->image_dac = 54;
  s->fdt.dac = s->config.channel_mode ? 27 : 56;
  s->image_median = 512;
  s->fdt.mode = 0xa0;
  s->fdt.configured = s->fdt.fdt_configured = 0;
  s->baseline_flag_1 = s->baseline_flag_2 = s->arm_mode = 0;
}
static enum fte3600_fw9369_startup_status
prefix (struct startup *op)
{
  uint16_t id;

  for (unsigned int negotiation = 0; negotiation < 2; ++negotiation)
    {
      unsigned int i;
      for (i = 0; i < 31; ++i)
        {
          write_byte (op, 0xc6, 1);
          wait_ms (op, 4);
          if (read_byte (op, 0xc6) == 1)
            break;
        }
      if (i == 31)
        op->d->warnings |= FTE3600_FW9369_SPI_MODE_EXHAUSTED;
    }
  if (!read_word (op, 0x1a8b, &id))
    return FTE3600_FW9369_STARTUP_WORD_READ_ERROR;
  if (id != 0x9362)
    op->d->warnings |= FTE3600_FW9369_UNRECOGNIZED_ID;
  reset_cached (op->s);
  write_word (op, 0x1a84, 0xffff);
  observe (op, FTE3600_FW9369_PHASE_WAKE, 1, FTE3600_FW9369_STARTUP_OK);
  idle (op);
  observe (op, FTE3600_FW9369_PHASE_WAKE, 0, FTE3600_FW9369_STARTUP_OK);
  for (unsigned int i = 0; i < 2; ++i)
    {
      write_byte (op, 0xf1, i ? 0x13 : 3);
      write_byte (op, 0xf4, 0xc0);
      write_byte (op, 0xf4, 0xc1);
      write_byte (op, 0xf4, 0xc0);
      write_byte (op, 0xf4, 0xc0);
      (void) read_byte (op, 0xf3);
    }
  op->s->config.foundry = 0;
  for (unsigned int i = 0; i < 10; ++i)
    {
      uint8_t code = (uint8_t) (read_byte (op, 0x9b) >> 2);
      if (code == 0x13)
        {
          op->s->config.foundry = 0xaa;
          break;
        }
      if (code == 0)
        break;
      if (i == 9)
        op->d->warnings |= FTE3600_FW9369_UNRECOGNIZED_FOUNDRY;
    }
  reset_cached (op->s);
  write_word (op, 0x1a84, 0xffff);
  {
    uint8_t foundry = op->s->config.foundry;
    uint8_t detect_state = op->s->config.detect_state;
    op->s->config = (struct fte3600_fw9369_fdt_config)
                    FTE3600_FW9369_FDT_CONFIG_INITIALIZER;
    op->s->config.foundry = foundry;
    op->s->config.detect_state = detect_state;
    op->s->period = foundry == 0xaa ? 150 : 200;
  }
  op->s->image_count = 1;
  op->s->shift = 2;
  return FTE3600_FW9369_STARTUP_OK;
}
static enum fte3600_fw9369_startup_status
arm (struct startup *op)
{
  uint8_t bytes[22] = {5, 0xfa, 0x80, 0xb0, 0, 8};
  uint16_t value;
  enum fte3600_fw9369_startup_status result =
    fdt_call (op, FTE3600_FW9369_FDT_CONFIGURE, NULL);

  if (result != FTE3600_FW9369_STARTUP_OK)
    return result;
  op->s->arm_mode = 1;
  write_byte (op, 0x9a, 0x5a);
  if (op->s->config.foundry == 0xaa)
    bytes[3] = 0xe0;
  for (size_t c = 0; c < 8; ++c)
    {
      bytes[6 + c * 2] = (uint8_t) (op->s->fdt_down[c] >> 8);
      bytes[7 + c * 2] = (uint8_t) op->s->fdt_down[c];
    }
  (void) transfer (op, bytes, sizeof (bytes));
  write_byte (op, 0x9a, 0);
  if (!read_word (op, 0x1881, &value))
    return FTE3600_FW9369_STARTUP_WORD_READ_ERROR;
  write_word (op, 0x1881, (uint16_t) ((value & 0xff7c) | 0x81));
  if (op->linux_detection)
    {
      if (!read_word (op, 0x1a83, &value))
        return FTE3600_FW9369_STARTUP_WORD_READ_ERROR;
      if (!write_word (op, 0x1a83, (uint16_t) ((value & 0xff80) | 0x16)))
        return FTE3600_FW9369_STARTUP_TRANSPORT_ERROR;
    }
  write_word (op, 0x1a84, 0x2f);
  command (op, 0xc2, 0x3d);
  wait_ms (op, 1);
  op->d->completed |= 4;
  return FTE3600_FW9369_STARTUP_OK;
}
static enum fte3600_fw9369_startup_status
run (
  enum fte3600_fw9369_startup_operation operation,
  struct fte3600_fw9369_startup_state *state, uint8_t sample_limit, uint8_t reset,
  struct fte3600_fw9369_startup_workspace *workspace, size_t capacity,
  const struct fte3600_capture_io *io,
  struct fte3600_fw9369_startup_diagnostics *diagnostics,
  const struct fte3600_fw9369_startup_observer *observer, int checked, int linux_detection)
{
  struct fte3600_fw9369_startup_diagnostics local;
  struct startup op = {state, workspace, io, diagnostics ? diagnostics : &local,
                       NULL, FTE3600_FW9369_STARTUP_OK, observer, checked, linux_detection};
  enum fte3600_fw9369_startup_status result = FTE3600_FW9369_STARTUP_OK;
  int failed = 0;

  if (state == NULL || workspace == NULL || capacity < sizeof (*workspace) ||
      io == NULL || io->spi == NULL || io->wait_ms == NULL ||
      operation < FTE3600_FW9369_STARTUP || operation > FTE3600_FW9369_IMAGE_BASELINE ||
      sample_limit != 10 || state->config.channel_mode > 1 ||
      (operation != FTE3600_FW9369_STARTUP &&
       (state->config.channel_mode != 1 || state->config.rate == 0 ||
        state->config.fast_rate == 0)))
    return FTE3600_FW9369_STARTUP_INVALID_ARGUMENT;
  memset (op.d, 0, sizeof (*op.d));
  if (operation == FTE3600_FW9369_STARTUP)
    {
      result = prefix (&op);
      if (result != FTE3600_FW9369_STARTUP_OK)
        goto done;
    }
  if (operation != FTE3600_FW9369_IMAGE_BASELINE)
    {
      observe (&op, FTE3600_FW9369_PHASE_FDT, 1, result);
      result = fdt_baseline (&op);
      observe (&op, FTE3600_FW9369_PHASE_FDT, 0, result);
      op.d->fdt_status = result;
      op.d->completed |= 1;
      if (result != FTE3600_FW9369_STARTUP_OK)
        op.d->warnings |= FTE3600_FW9369_FDT_BASELINE_FAILED;
      if (result > FTE3600_FW9369_STARTUP_FAILED)
        goto done;
      failed |= result == FTE3600_FW9369_STARTUP_FAILED;
    }
  if (operation != FTE3600_FW9369_FDT_BASELINE)
    {
      observe (&op, FTE3600_FW9369_PHASE_IMAGE, 1, result);
      result = image_baseline (&op, reset);
      observe (&op, FTE3600_FW9369_PHASE_IMAGE, 0, result);
      op.d->image_status = result;
      op.d->completed |= 2;
      if (result != FTE3600_FW9369_STARTUP_OK)
        op.d->warnings |= FTE3600_FW9369_IMAGE_BASELINE_FAILED;
      if (result > FTE3600_FW9369_STARTUP_FAILED)
        goto done;
      failed |= result == FTE3600_FW9369_STARTUP_FAILED;
    }
  result = failed ? FTE3600_FW9369_STARTUP_FAILED : FTE3600_FW9369_STARTUP_OK;
  if (operation == FTE3600_FW9369_STARTUP)
    {
      observe (&op, FTE3600_FW9369_PHASE_ARM, 1, result);
      result = arm (&op);
      observe (&op, FTE3600_FW9369_PHASE_ARM, 0, result);
      if (result != FTE3600_FW9369_STARTUP_OK)
        goto done;
      state->image_valid = state->fdt.valid = 0;
    }
done:
  if (result > FTE3600_FW9369_STARTUP_FAILED)
    op.d->warnings |= FTE3600_FW9369_LOCAL_ERROR;
  return result;
}

enum fte3600_fw9369_startup_status
fte3600_fw9369_startup_run (
  enum fte3600_fw9369_startup_operation operation,
  struct fte3600_fw9369_startup_state *state, uint8_t sample_limit, uint8_t reset,
  struct fte3600_fw9369_startup_workspace *workspace, size_t capacity,
  const struct fte3600_capture_io *io,
  struct fte3600_fw9369_startup_diagnostics *diagnostics)
{
  if (reset != 1)
    return FTE3600_FW9369_STARTUP_INVALID_ARGUMENT;
  return run (operation, state, sample_limit, reset, workspace, capacity, io, diagnostics, NULL, 0, 0);
}

enum fte3600_fw9369_startup_status
fte3600_fw9369_image_baseline_run (
  struct fte3600_fw9369_startup_state *state, uint8_t reset,
  struct fte3600_fw9369_startup_workspace *workspace, size_t capacity,
  const struct fte3600_capture_io *io,
  struct fte3600_fw9369_startup_diagnostics *diagnostics)
{
  return run (FTE3600_FW9369_IMAGE_BASELINE, state, 10, reset,
              workspace, capacity, io, diagnostics, NULL, 0, 0);
}

enum fte3600_fw9369_startup_status
fte3600_fw9369_startup_observed (
  struct fte3600_fw9369_startup_state *state,
  struct fte3600_fw9369_startup_workspace *workspace, size_t capacity,
  const struct fte3600_capture_io *io,
  const struct fte3600_fw9369_startup_observer *observer,
  struct fte3600_fw9369_startup_diagnostics *diagnostics)
{
  return run (FTE3600_FW9369_STARTUP, state, 10, 1, workspace, capacity,
              io, diagnostics, observer, 0, 0);
}

enum fte3600_fw9369_startup_status
fte3600_fw9369_image_baseline_checked (
  struct fte3600_fw9369_startup_state *state, uint8_t reset,
  struct fte3600_fw9369_startup_workspace *workspace, size_t capacity,
  const struct fte3600_capture_io *io,
  struct fte3600_fw9369_startup_diagnostics *diagnostics)
{
  return run (FTE3600_FW9369_IMAGE_BASELINE, state, 10, reset,
              workspace, capacity, io, diagnostics, NULL, 1, 0);
}

enum fte3600_fw9369_startup_status
fte3600_fw9369_startup_checked (
  struct fte3600_fw9369_startup_state *state,
  struct fte3600_fw9369_startup_workspace *workspace, size_t capacity,
  const struct fte3600_capture_io *io,
  const struct fte3600_fw9369_startup_observer *observer,
  struct fte3600_fw9369_startup_diagnostics *diagnostics)
{
  return run (FTE3600_FW9369_STARTUP, state, 10, 1, workspace, capacity,
              io, diagnostics, observer, 1, 0);
}

enum fte3600_fw9369_startup_status
fte3600_fw9369_startup_linux (
  struct fte3600_fw9369_startup_state *state,
  struct fte3600_fw9369_startup_workspace *workspace, size_t capacity,
  const struct fte3600_capture_io *io,
  const struct fte3600_fw9369_startup_observer *observer,
  struct fte3600_fw9369_startup_diagnostics *diagnostics)
{
  return run (FTE3600_FW9369_STARTUP, state, 10, 1, workspace, capacity,
              io, diagnostics, observer, 1, 1);
}

/* Retained automatic detection after image acquisition; no calibration. */
enum fte3600_fw9369_startup_status
fte3600_fw9369_startup_arm (
  struct fte3600_fw9369_startup_state       *state,
  const struct fte3600_capture_io           *io,
  struct fte3600_fw9369_startup_diagnostics *diagnostics)
{
  struct fte3600_fw9369_startup_diagnostics local;

  if (!state || !io || !io->spi || !io->wait_ms ||
      state->config.channel_mode != 1 || !state->config.rate ||
      !state->config.fast_rate)
    return FTE3600_FW9369_STARTUP_INVALID_ARGUMENT;
  struct startup op = {state, NULL, io, diagnostics ? diagnostics : &local,
                       NULL, FTE3600_FW9369_STARTUP_OK, NULL, 1, 0};
  memset (op.d, 0, sizeof (*op.d));
  enum fte3600_fw9369_startup_status status = arm (&op);
  if (status > FTE3600_FW9369_STARTUP_FAILED)
    op.d->warnings |= FTE3600_FW9369_LOCAL_ERROR;
  return status;
}
