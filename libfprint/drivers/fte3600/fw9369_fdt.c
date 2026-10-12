/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_fw9369_fdt.h"
#include "fte3600_calibration.h"

#include <string.h>

struct fdt_operation
{
  struct fte3600_fw9369_fdt_state              *state;
  const struct fte3600_fw9369_fdt_config       *config;
  const struct fte3600_capture_io              *io;
  struct fte3600_capture_diagnostics           *diagnostics;
  enum fte3600_fw9369_fdt_status sample_error;
  const struct fte3600_fw9369_fdt_image_config *image;
};

static int
valid (const struct fte3600_fw9369_fdt_config *config,
       const struct fte3600_capture_io        *io)
{
  return config != NULL && config->channel_mode <= 1 && config->rate != 0 &&
         config->fast_rate != 0 && io != NULL && io->spi != NULL &&
         io->wait_ms != NULL;
}

static size_t
channel_count (const struct fte3600_fw9369_fdt_config *config)
{
  return config->channel_mode == 1 ? 4 : 8;
}

static void
delay (const struct fdt_operation *op, uint32_t ms)
{
  op->io->wait_ms (op->io->context, ms);
}

static int
transfer (const struct fdt_operation *op, uint8_t *buffer, size_t size)
{
  for (unsigned int i = 0; i < 3; ++i)
    {
      uint32_t status = op->io->spi (op->io->context, buffer, size);
      if ((status & UINT32_C (0x80000000)) == 0)
        return 1;
      if (op->diagnostics != NULL)
        {
          ++op->diagnostics->failed_attempts;
          op->diagnostics->last_failure = status;
        }
      delay (op, 10);
    }
  if (op->diagnostics != NULL)
    ++op->diagnostics->exhausted_transactions;
  return 0;
}

static void
command (const struct fdt_operation *op, uint8_t a, uint8_t b)
{
  uint8_t buffer[3] = {a, b, 0};

  (void) transfer (op, buffer, sizeof (buffer));
}

static void
idle (const struct fdt_operation *op)
{
  uint8_t buffer[5] = {0x08, 0xf7, 0x80, 0, 0};

  command (op, 0x5a, 0xa5);
  delay (op, 1);
  (void) transfer (op, buffer, sizeof (buffer));
  if (buffer[4] != 0x50)
    {
      command (op, 0xc0, 0x3f);
      delay (op, 1);
    }
  command (op, 0xa5, 0x5a);
}

static int
read_word (const struct fdt_operation *op, uint16_t address,
           uint16_t *value)
{
  uint8_t buffer[12] = {0x04, 0xfb, (uint8_t) (0x80 | (address >> 8)),
                        (uint8_t) address, 0, 1, 0, 0, 0, 0, 0, 0};

  if (!transfer (op, buffer, sizeof (buffer)))
    return 0;
  *value = (uint16_t) ((uint16_t) buffer[6] << 8 | buffer[7]);
  return 1;
}

static int
write_word (const struct fdt_operation *op, uint16_t address,
            uint16_t value, int checked)
{
  uint16_t observed;
  uint8_t buffer[8] = {0x05, 0xfa, (uint8_t) (0x80 | (address >> 8)),
                       (uint8_t) address, 0, 1,
                       (uint8_t) (value >> 8), (uint8_t) value};

  (void) transfer (op, buffer, sizeof (buffer));
  if (checked && op->state->check_registers != 0)
    {
      if (!read_word (op, address, &observed))
        return 0;
      if (observed != value)
        op->state->check_registers = 2;
    }
  return 1;
}

static int
modify (const struct fdt_operation *op, uint16_t address,
        uint16_t keep, uint16_t set, int checked)
{
  uint16_t value;

  return read_word (op, address, &value) &&
         write_word (op, address, (uint16_t) ((value & keep) | set), checked);
}

static void
write_byte (const struct fdt_operation *op, uint8_t address,
            uint8_t value)
{
  uint8_t buffer[4] = {0x09, 0xf6, address, value};

  (void) transfer (op, buffer, sizeof (buffer));
}

static int
image_prefix (const struct fdt_operation *op)
{
  struct fte3600_fw9369_fdt_state *s = op->state;
  struct fte3600_capture_state image = {
    0, 2, op->config->channel_mode, s->mode, s->configured,
    s->check_registers, s->dac_cache, 200
  };
  struct fte3600_capture_diagnostics diagnostics;

  if (op->image != NULL)
    {
      image.dac = op->image->dac;
      image.shift = op->image->shift;
      image.period = op->image->period;
    }
  enum fte3600_capture_status status = fte3600_capture_configure (
    &image, op->io, &diagnostics);
  s->mode = image.mode;
  s->configured = image.configured;
  s->check_registers = image.check_registers;
  s->dac_cache = image.dac_cache;
  if (op->diagnostics != NULL)
    {
      op->diagnostics->failed_attempts += diagnostics.failed_attempts;
      op->diagnostics->exhausted_transactions += diagnostics.exhausted_transactions;
      if (diagnostics.last_failure != 0)
        op->diagnostics->last_failure = diagnostics.last_failure;
    }
  return status == FTE3600_CAPTURE_OK;
}

static int
configure (const struct fdt_operation *op)
{
  struct fte3600_fw9369_fdt_state *s = op->state;
  const struct fte3600_fw9369_fdt_config *c = op->config;
  uint16_t timing, value;
  int fast = c->detect_state == 6;

  idle (op);
  if (s->configured == 0 && !image_prefix (op))
    return 0;
  if (!write_word (op, 0x1801, (uint16_t) (0xfc80 | s->dac), 1))
    return 0;
  s->dac_cache = s->dac;
  if (s->calibrating_cache != s->calibrating)
    {
      if (!write_word (op, 0x180c, s->calibrating == 0 ? 0x0600 : 0, 1))
        return 0;
      s->calibrating_cache = s->calibrating;
    }
  if ((s->mode == 0xa1 && !fast) || (s->mode == 0xa2 && fast))
    return 1;
  timing = (uint16_t) (((1000u / (fast ? c->fast_rate : c->rate) - 1u) << 8) |
                       ((uint32_t) (fast ? 1 : (uint16_t) (c->repeat - 1u)) << 2));
  if (!write_word (op, 0x1881, timing, 1))
    return 0;
  if (s->mode == 0xa1 || s->mode == 0xa2)
    return 1;
  s->mode = fast ? 0xa2 : 0xa1;
  if (!write_word (op, 0x1800, c->channel_mode == 0 ? 0x0890 : 0x07fe, 1) ||
      !write_word (op, 0x1804, (uint16_t) (0x27c8 |
                                           (uint16_t) (c->integration_multiplier - 1u)), 1) ||
      (c->channel_mode == 1 && !write_word (op, 0x1807, 0x1671, 1)) ||
      !write_word (op, 0x1808, c->channel_mode == 1 ? 0x0801 :
                   (uint16_t) ((0x0800 | (uint16_t) (c->fdt_rows - 1u)) & 0xf8ff), 1) ||
      !write_word (op, 0x1887, c->channel_mode == 1 ? 5 : 2, 1) ||
      !modify (op, 0x1806, 0xc07f, 0x0980, 0) ||
      !modify (op, 0x180a, 0xc000, 0x0987, 0) ||
      !modify (op, 0x180b, 0xc000, 0x0491, 0))
    return 0;
  if (s->fdt_configured == 1)
    return 1;
  if (!read_word (op, 0x1805, &value))
    return 0;
  value &= 0xffef;
  if (!write_word (op, 0x1805, value, 1) ||
      !write_word (op, 0x180d, (uint16_t) ((value & 0xfc00) | 0x0384), 1) ||
      !modify (op, 0x1888, 0xfc03, 0, 1))
    return 0;
  write_byte (op, 0x9a, 0x5a);
  for (unsigned int group = 0; group < (c->channel_mode == 1 ? 1u : 2u); ++group)
    {
      uint16_t base = c->channel_mode == 1 ? 0xc0 : (uint16_t) (0xc9 + group * 9);
      if (!modify (op, base, 0xe000, c->channel_mode == 1 ? 0x0444 : 0x06cd, 1) ||
          !modify (op, (uint16_t) (base + 1), 0xffc0, 0x0021, 1) ||
          !modify (op, (uint16_t) (base + 2), 0xffff, 0x00c0, 1))
        return 0;
    }
  write_byte (op, 0x9a, 0);
  if (!modify (op, 0x1880, 0, (uint16_t) ((uint16_t) c->threshold_high << 8 |
                                          c->threshold_low), 1) ||
      !modify (op, 0x1881, 0xfffe, 0x0082, 1) ||
      !modify (op, 0x1884, 0xfff8, (uint16_t) (c->average - 1u), 1) ||
      !write_word (op, 0x1a8a, 0x00ff, 1) ||
      !modify (op, 0x1a83, 0xffff, 0x0008, 0))
    return 0;
  s->fdt_configured = 1;
  return 1;
}

static enum fte3600_fw9369_fdt_status
scan (const struct fdt_operation *op)
{
  uint16_t value;

  if (!configure (op) || !read_word (op, 0x1881, &value))
    return FTE3600_FW9369_FDT_WORD_READ_ERROR;
  if ((value & 2) == 0)
    (void) write_word (op, 0x1881, (uint16_t) (value | 2), 0);
  command (op, 0xc2, 0x3d);
  delay (op, 1);
  (void) write_word (op, 0x1885, 1, 0);
  for (unsigned int i = 0; i < 5; ++i)
    {
      if (!read_word (op, 0x1a82, &value))
        return FTE3600_FW9369_FDT_WORD_READ_ERROR;
      if ((value & 8) != 0)
        return FTE3600_FW9369_FDT_OK;
      delay (op, 1);
    }
  if (op->diagnostics != NULL)
    op->diagnostics->ready_timeout = 1;
  return FTE3600_FW9369_FDT_OK;
}

static enum fte3600_fw9369_fdt_status
read_channels (
  const struct fdt_operation *op, uint16_t *channels)
{
  size_t count = channel_count (op->config);
  uint8_t buffer[22] = {0x04, 0xfb, 0x80,
                        op->config->foundry == 0xaa ? 0xe8 : 0xb8, 0, (uint8_t) count};

  if (!transfer (op, buffer, 6 + 2 * count))
    return FTE3600_FW9369_FDT_ADC_READ_ERROR;
  for (size_t i = 0; i < count; ++i)
    channels[i] = (uint16_t) ((uint16_t) buffer[6 + 2 * i] << 8 | buffer[7 + 2 * i]);
  return FTE3600_FW9369_FDT_OK;
}

static void
reset_diagnostics (struct fte3600_capture_diagnostics *diagnostics)
{
  if (diagnostics != NULL)
    memset (diagnostics, 0, sizeof (*diagnostics));
}

enum fte3600_fw9369_fdt_status
fte3600_fw9369_fdt_scan (
  struct fte3600_fw9369_fdt_state        *state,
  const struct fte3600_fw9369_fdt_config *config,
  const struct fte3600_capture_io        *io,
  struct fte3600_capture_diagnostics     *diagnostics)
{
  struct fdt_operation op = {state, config, io, diagnostics, 0, NULL};

  if (state == NULL || !valid (config, io))
    return FTE3600_FW9369_FDT_INVALID_ARGUMENT;
  reset_diagnostics (diagnostics);
  return scan (&op);
}

enum fte3600_fw9369_fdt_status
fte3600_fw9369_fdt_read (
  const struct fte3600_fw9369_fdt_config *config,
  uint16_t *channels, size_t capacity,
  const struct fte3600_capture_io *io,
  struct fte3600_capture_diagnostics *diagnostics)
{
  struct fdt_operation op = {NULL, config, io, diagnostics, 0, NULL};

  if (!valid (config, io) || channels == NULL || capacity < channel_count (config))
    return FTE3600_FW9369_FDT_INVALID_ARGUMENT;
  reset_diagnostics (diagnostics);
  return read_channels (&op, channels);
}

enum fte3600_fw9369_fdt_status
fte3600_fw9369_fdt_measure (
  struct fte3600_fw9369_fdt_state *state,
  const struct fte3600_fw9369_fdt_config *config,
  uint16_t *channels, size_t capacity,
  const struct fte3600_capture_io *io,
  struct fte3600_capture_diagnostics *diagnostics)
{
  struct fdt_operation op = {state, config, io, diagnostics, 0, NULL};
  enum fte3600_fw9369_fdt_status status;

  if (state == NULL || !valid (config, io) || channels == NULL ||
      capacity < channel_count (config))
    return FTE3600_FW9369_FDT_INVALID_ARGUMENT;
  reset_diagnostics (diagnostics);
  status = scan (&op);
  return status == FTE3600_FW9369_FDT_OK ? read_channels (&op, channels) : status;
}

static int
physical_sample (void *context, uint8_t dac, uint16_t *channels)
{
  struct fdt_operation *op = context;

  (void) dac;  /* Controller writes the very same state DAC before calling. */
  op->sample_error = scan (op);
  if (op->sample_error == FTE3600_FW9369_FDT_OK)
    op->sample_error = read_channels (op, channels);
  return op->sample_error;
}

enum fte3600_fw9369_fdt_status
fte3600_fw9369_fdt_calibrate (
  struct fte3600_fw9369_fdt_state        *state,
  const struct fte3600_fw9369_fdt_config *config,
  const struct fte3600_capture_io        *io,
  struct fte3600_capture_diagnostics     *diagnostics)
{
  return fte3600_fw9369_fdt_run (FTE3600_FW9369_FDT_CALIBRATE,
                                 state, config, NULL, NULL, 0, io, diagnostics);
}

enum fte3600_fw9369_fdt_status
fte3600_fw9369_fdt_run (
  enum fte3600_fw9369_fdt_operation operation,
  struct fte3600_fw9369_fdt_state *state,
  const struct fte3600_fw9369_fdt_config *config,
  const struct fte3600_fw9369_fdt_image_config *image,
  uint16_t *channels, size_t capacity,
  const struct fte3600_capture_io *io,
  struct fte3600_capture_diagnostics *diagnostics)
{
  struct fdt_operation op = {state, config, io, diagnostics, 0, image};
  enum fte3600_calibration_status result;
  enum fte3600_fw9369_fdt_status status;

  if (state == NULL || !valid (config, io) ||
      operation < FTE3600_FW9369_FDT_CONFIGURE ||
      operation > FTE3600_FW9369_FDT_CALIBRATE ||
      (operation == FTE3600_FW9369_FDT_MEASURE &&
       (channels == NULL || capacity < channel_count (config))))
    return FTE3600_FW9369_FDT_INVALID_ARGUMENT;
  reset_diagnostics (diagnostics);
  if (operation == FTE3600_FW9369_FDT_CONFIGURE)
    return configure (&op) ? FTE3600_FW9369_FDT_OK :
           FTE3600_FW9369_FDT_WORD_READ_ERROR;
  if (operation == FTE3600_FW9369_FDT_MEASURE)
    {
      status = scan (&op);
      return status == FTE3600_FW9369_FDT_OK ? read_channels (&op, channels) : status;
    }
  state->calibrating = 1;
  result = fte3600_calibrate_channels (&state->dac, &state->valid,
                                       channel_count (config), physical_sample, &op);
  state->calibrating = 0;
  if (result == FTE3600_CALIBRATION_SAMPLE_ERROR)
    return op.sample_error;
  return result == FTE3600_CALIBRATION_OK ? FTE3600_FW9369_FDT_OK :
         FTE3600_FW9369_FDT_FAILED;
}
