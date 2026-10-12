/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_ft93xx.h"

#include <string.h>

struct profile
{
  uint16_t id, width, height, window, extension;
  uint8_t  dac, gain, polarity;
};

static const struct profile profiles[] = {
  {0x9349, 80, 128, 0x7ffe, 0x003f, 122, 8, 0},
  {0x9363, 64, 80, 0x4ffe, 0, 60, 7, 0},
  {0x9365, 64, 80, 0x4ffe, 0, 72, 7, 0},
  {0x9372, 56, 56, 0x06fe, 0, 62, 7, 0},
  {0x9391, 40, 196, 0xc7fe, 0x7fff, 124, 5, 3},
  {0x9392, 40, 196, 0xc7fe, 0x7fff, 124, 5, 3},
  {0x9395, 36, 176, 0xaffe, 0x0fff, 120, 8, 0},
  {0x9396, 36, 96, 0x5ffe, 0x0003, 128, 8, 0},
  {0x9397, 32, 176, 0xaffe, 0x0fff, 97, 11, 3},
  {0x9398, 36, 144, 0x8ffe, 0x00ff, 128, 8, 0}
};

static const struct profile *
lookup (uint16_t id)
{
  for (size_t i = 0; i < sizeof (profiles) / sizeof (profiles[0]); ++i)
    if (profiles[i].id == id)
      return &profiles[i];
  return NULL;
}

static void
apply_area (uint16_t chip_id, uint16_t raw_id, uint8_t byte,
            struct fte3600_ft93xx_geometry *geometry, uint8_t *version)
{
  const struct profile *p = lookup (chip_id);

  if (p != NULL)
    {
      geometry->width = p->width;
      geometry->height = p->height;
      if (chip_id == 0x9391 || chip_id == 0x9392)
        geometry->extra_rows = 4;
    }
  *version = byte == 255 && raw_id == 0x9391 ? 5 : (uint8_t) (byte & 15);
}

enum fte3600_ft93xx_status
fte3600_ft93xx_area (
  uint16_t chip_id, uint16_t raw_id, uint8_t foundry,
  struct fte3600_ft93xx_geometry *geometry, uint8_t *version,
  fte3600_ft93xx_efuse_fn efuse, void *context)
{
  uint8_t byte = 0;

  if (geometry == NULL || version == NULL || efuse == NULL)
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  if (efuse (context, foundry, 19, &byte) != 0)
    return FTE3600_FT93XX_EFUSE_ERROR;
  apply_area (chip_id, raw_id, byte, geometry, version);
  return FTE3600_FT93XX_OK;
}

uint8_t
fte3600_ft93xx_features (uint16_t chip_id, uint16_t raw_id,
                         uint8_t version, uint8_t foundry)
{
  switch (raw_id)
    {
    case 0x9365:
    case 0x9396:
    case 0x9397:
    case 0x9398:
      return 3;

    case 0x9392:
      return 2;

    case 0x9395:
      return foundry == 3 ? 3 : 2;

    case 0x9391:
      return version > 4 && chip_id == 0x9395 ? 3 : 0;

    default:
      return 0;
    }
}

enum fte3600_ft93xx_status
fte3600_ft93xx_configuration (
  uint16_t chip_id, uint8_t version, struct fte3600_ft93xx_state *state)
{
  const struct profile *p = lookup (chip_id);

  if (p == NULL || state == NULL)
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  state->dac = chip_id == 0x9397 && version == 0 ? 110 : p->dac;
  state->gain = chip_id == 0x9397 && version == 0 ? 12 : p->gain;
  state->dac_polarity = p->polarity;
  state->integration = 128;
  state->sample_time = 15;
  state->sample_mode = 2;
  state->subtract_baseline = 1;
  return FTE3600_FT93XX_OK;
}

size_t
fte3600_ft93xx_frame_count (uint8_t count)
{
  return count == 0 ? 1 : count > 5 ? 5 : count;
}

enum fte3600_ft93xx_status
fte3600_ft93xx_payload_size (
  const struct fte3600_ft93xx_geometry *geometry, uint8_t count,
  size_t *length)
{
  size_t rows, bytes, frames = fte3600_ft93xx_frame_count (count);

  if (geometry == NULL || length == NULL || geometry->width == 0 ||
      geometry->height == 0)
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  rows = (size_t) geometry->height + geometry->extra_rows;
  if (rows > SIZE_MAX / geometry->width)
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  bytes = rows * geometry->width;
  if (bytes > SIZE_MAX / 2 || bytes * 2 > SIZE_MAX / frames)
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  *length = bytes * 2 * frames;
  return FTE3600_FT93XX_OK;
}

struct operation
{
  const struct fte3600_ft93xx_io    *io;
  struct fte3600_ft93xx_diagnostics *diagnostics;
  enum fte3600_ft93xx_status *error;
};

static void
wait_ms (const struct operation *op, uint32_t duration)
{
  op->io->wait_ms (op->io->context, duration);
}

static int
transfer (const struct operation *op, uint8_t *buffer, size_t length)
{
  uint8_t original[FTE3600_FT93XX_MAX_TRANSFER];

  memcpy (original, buffer, length);
  for (unsigned int attempt = 0; attempt < 3; ++attempt)
    {
      uint32_t status;
      memcpy (buffer, original, length);
      status = op->io->spi (op->io->context, buffer, length);
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

static int
command (const struct operation *op, uint8_t command_byte)
{
  uint8_t buffer[3] = {command_byte, (uint8_t) ~command_byte, 0};

  return transfer (op, buffer, sizeof (buffer));
}

static int
read_byte (const struct operation *op, uint8_t address, uint8_t *value)
{
  uint8_t buffer[5] = {0x08, 0xf7, address, 0, 0};

  if (!transfer (op, buffer, sizeof (buffer)))
    return 0;
  *value = buffer[4];
  return 1;
}

static int
write_byte (const struct operation *op, uint8_t address, uint8_t value)
{
  uint8_t buffer[4] = {0x09, 0xf6, address, value};

  return transfer (op, buffer, sizeof (buffer));
}

static int
read_word (const struct operation *op, uint16_t address, uint16_t *value)
{
  uint8_t buffer[10] = {0x04, 0xfb, (uint8_t) (0x80 | (address >> 8)),
                        (uint8_t) address, 0, 0, 0, 0, 0, 0};

  if (!transfer (op, buffer, sizeof (buffer)))
    return 0;
  if (buffer[8] != 0 || buffer[9] != 0)
    {
      uint16_t crc = 0xffff;
      for (size_t i = 6; i < 8; ++i)
        {
          crc = (uint16_t) (crc ^ ((unsigned int) buffer[i] << 8));
          for (unsigned int bit = 0; bit < 8; ++bit)
            crc = (uint16_t) (((unsigned int) crc << 1) ^
                              ((crc & 0x8000) != 0 ? 0x1021u : 0u));
        }
      if (crc != (uint16_t) ((uint16_t) buffer[8] << 8 | buffer[9]))
        {
          *op->error = FTE3600_FT93XX_BAD_CRC;
          return 0;
        }
    }
  *value = (uint16_t) ((uint16_t) buffer[6] << 8 | buffer[7]);
  return 1;
}

static int
write_word (const struct operation *op, uint16_t address, uint16_t value)
{
  uint8_t buffer[8] = {0x05, 0xfa, (uint8_t) (0x80 | (address >> 8)),
                       (uint8_t) address, 0, 0,
                       (uint8_t) (value >> 8), (uint8_t) value};

  return transfer (op, buffer, sizeof (buffer));
}

static int
checked_write (const struct operation *op,
               struct fte3600_ft93xx_state *state,
               uint16_t address, uint16_t value)
{
  uint16_t observed;

  if (!write_word (op, address, value))
    return 0;
  if (state->check_registers == 1)
    {
      if (!read_word (op, address, &observed))
        return 0;
      if (observed != value)
        state->check_registers = 2;
    }
  return 1;
}

static int
idle (const struct operation *op)
{
  uint8_t value;

  if (!command (op, 0x5a))
    return 0;
  wait_ms (op, 1);
  if (!read_byte (op, 0x80, &value))
    return 0;
  for (unsigned int i = 0; value != 0x50 && i < 5; ++i)
    {
      if (!command (op, 0xc0))
        return 0;
      wait_ms (op, 1);
      if (!read_byte (op, 0x80, &value))
        return 0;
      if (value != 0x50)
        wait_ms (op, 1);
    }
  if (op->diagnostics != NULL)
    op->diagnostics->idle_unacknowledged |= value != 0x50;
  return command (op, 0xa5);
}

static int
configure (const struct operation *op, const struct profile *p,
           struct fte3600_ft93xx_state *state, uint8_t mode)
{
  static const uint8_t byte_settings[][2] = {
    {0x8e, 0xf4}, {0x8d, 0x61}, {0x8f, 0xff}, {0x90, 0},
    {0x91, 7}, {0x92, 0xd0}, {0x90, 1}
  };
  uint16_t value;
  uint16_t dac = (uint16_t) (0x3000u | ((unsigned int) state->dac_polarity << 14) |
                             ((unsigned int) state->gain << 8) | state->dac);
  uint16_t timing = (uint16_t) (((unsigned int) state->integration - 1u) << 5 |
                                state->sample_time);
  uint8_t prior_mode = state->mode;

  if (!idle (op) || !checked_write (op, state, 0x1801, dac) ||
      !checked_write (op, state, 0x1800, p->window) ||
      !checked_write (op, state, 0x1816, p->extension) ||
      !checked_write (op, state, 0x1807, timing) ||
      !checked_write (op, state, 0x1806, 0x023b) ||
      !checked_write (op, state, 0x180a, 1) ||
      !checked_write (op, state, 0x180b, 0x0104) ||
      !read_word (op, 0x1812, &value))
    return 0;
  value = (uint16_t) ((value & 0x8000) |
                      ((unsigned int) state->subtract_baseline << 14) | 0x0100);
  if (!checked_write (op, state, 0x1812, value))
    return 0;
  state->mode = mode;
  if (prior_mode == 0xa3 || prior_mode == 0xa4)
    return 1;
  value = (uint16_t) (0x27c0 | state->sample_mode |
                      (state->sample_mode > 1 ? 8 : 0));
  if (!checked_write (op, state, 0x1804, value))
    return 0;
  if (state->configured == 1)
    return 1;
  if (!checked_write (op, state, 0x1811, 0x01fe))
    return 0;
  for (unsigned int bit = 0x20; bit <= 0x40; bit <<= 1)
    {
      if (!read_word (op, 0x1a83, &value))
        return 0;
      if ((value & bit) == 0 &&
          !write_word (op, 0x1a83, (uint16_t) (value | bit)))
        return 0;
    }
  state->configured = 1;
  for (size_t i = 0; i < sizeof (byte_settings) / sizeof (byte_settings[0]); ++i)
    if (!write_byte (op, byte_settings[i][0], byte_settings[i][1]))
      return 0;
  return write_word (op, 0x1a06, 1);
}

static int
fifo (const struct operation *op, uint8_t *payload, size_t length)
{
  uint8_t buffer[FTE3600_FT93XX_MAX_TRANSFER];
  size_t position = 0;

  while (position < length)
    {
      size_t block = length - position;
      if (block > 16384)
        block = 16384;
      while (block != 0)
        {
          size_t chunk = block > 1790 ? 1790 : block;
          size_t word_count = chunk / 2 - 1;
          memset (buffer, 0, chunk + 8);
          buffer[0] = 0x06;
          buffer[1] = 0xf9;
          buffer[2] = 0x9a;
          buffer[3] = 0x05;
          buffer[4] = (uint8_t) (word_count >> 8);
          buffer[5] = (uint8_t) word_count;
          if (!transfer (op, buffer, chunk + 8))
            return 0;
          memcpy (payload + position, buffer + 6, chunk);
          position += chunk;
          block -= chunk;
          if (op->diagnostics != NULL)
            op->diagnostics->raw_bytes_completed = position;
        }
    }
  return 1;
}

static int
valid_output (const struct fte3600_ft93xx_geometry *geometry,
              uint8_t count, const uint8_t *payload, size_t capacity,
              const struct fte3600_ft93xx_io *io, size_t *length)
{
  return payload != NULL && io != NULL && io->spi != NULL && io->wait_ms != NULL &&
         fte3600_ft93xx_payload_size (geometry, count, length) == FTE3600_FT93XX_OK &&
         capacity >= *length;
}

static int
valid_retained_state (const struct fte3600_ft93xx_state *s)
{
  return s != NULL && s->gain <= 15 && s->dac_polarity <= 3 &&
         s->sample_time <= 15 && s->sample_mode <= 3 && s->subtract_baseline <= 1 &&
         s->configured <= 1 && s->check_registers <= 2;
}

static int
valid_state (const struct fte3600_ft93xx_state *s)
{
  return valid_retained_state (s) && s->integration >= 1 && s->integration <= 512;
}

enum fte3600_ft93xx_status
fte3600_ft93xx_image_read (
  const struct fte3600_ft93xx_geometry *geometry, uint8_t count,
  uint8_t *payload, size_t capacity, const struct fte3600_ft93xx_io *io,
  struct fte3600_ft93xx_diagnostics *diagnostics)
{
  enum fte3600_ft93xx_status error = FTE3600_FT93XX_TRANSPORT_ERROR;
  struct operation op = {io, diagnostics, &error};
  size_t length;

  if (!valid_output (geometry, count, payload, capacity, io, &length))
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  if (diagnostics != NULL)
    memset (diagnostics, 0, sizeof (*diagnostics));
  return fifo (&op, payload, length) ? FTE3600_FT93XX_OK : error;
}

static void
decode_samples (uint8_t *payload, size_t samples, uint16_t raw_id)
{
  for (size_t i = 0; i < samples; i += 4)
    {
      uint16_t group[4];
      for (size_t j = 0; j < 4; ++j)
        group[j] = (uint16_t) (((unsigned int) payload[2 * (i + j)] << 8 |
                                payload[2 * (i + j) + 1]) & 0x0ffc);
      for (size_t j = 0; j < 4; ++j)
        {
          uint16_t value = group[raw_id == 0x9392 ? 3 - j : j];
          memcpy (payload + 2 * (i + j), &value, sizeof (value));
        }
    }
}

enum fte3600_ft93xx_status
fte3600_ft93xx_capture (
  uint16_t chip_id, uint16_t raw_id,
  const struct fte3600_ft93xx_geometry *geometry,
  struct fte3600_ft93xx_state *state, uint8_t count,
  uint8_t *payload, size_t capacity, const struct fte3600_ft93xx_io *io,
  struct fte3600_ft93xx_diagnostics *diagnostics)
{
  if (!valid_state (state))
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  return fte3600_ft93xx_capture_retained (chip_id, raw_id, geometry, state,
                                          count, payload, capacity, io, diagnostics);
}

enum fte3600_ft93xx_status
fte3600_ft93xx_capture_retained (
  uint16_t chip_id, uint16_t raw_id,
  const struct fte3600_ft93xx_geometry *geometry,
  struct fte3600_ft93xx_state *state, uint8_t count,
  uint8_t *payload, size_t capacity, const struct fte3600_ft93xx_io *io,
  struct fte3600_ft93xx_diagnostics *diagnostics)
{
  const struct profile *p = lookup (chip_id);
  enum fte3600_ft93xx_status error = FTE3600_FT93XX_TRANSPORT_ERROR;
  struct operation op = {io, diagnostics, &error};
  size_t length, samples;
  uint8_t byte = 0;
  uint16_t word = 0;

  if (p == NULL || !valid_retained_state (state) ||
      !valid_output (geometry, count, payload, capacity, io, &length) ||
      geometry->width != p->width || geometry->height != p->height ||
      ((chip_id == 0x9391 || chip_id == 0x9392) && geometry->extra_rows != 4))
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  if (diagnostics != NULL)
    memset (diagnostics, 0, sizeof (*diagnostics));
  if (!configure (&op, p, state, 0xa3) || !command (&op, 0xc4))
    return error;
  wait_ms (&op, 1);
  for (unsigned int i = 0; i < 10; ++i)
    {
      if (!read_byte (&op, 0x80, &byte))
        return error;
      if (byte == 0x54)
        break;
    }
  if (diagnostics != NULL)
    diagnostics->mode_unacknowledged = byte != 0x54;
  if (!write_word (&op, 0x1800, (uint16_t) (p->window | 1)))
    return error;
  for (unsigned int i = 0; i < 5; ++i)
    {
      if (!read_word (&op, 0x1a82, &word))
        return error;
      if ((word & 0x20) != 0)
        {
          if (!write_word (&op, 0x1a84, word))
            return error;
          break;
        }
      wait_ms (&op, 1);
    }
  if (diagnostics != NULL)
    diagnostics->ready_unacknowledged = (word & 0x20) == 0;
  if (!fifo (&op, payload, length) || !write_word (&op, 0x1a84, 0x20))
    return error;
  samples = (size_t) geometry->width * geometry->height;
  decode_samples (payload, samples, raw_id);
  return FTE3600_FT93XX_OK;
}

static int
spi_mode (const struct operation *op)
{
  uint8_t byte = 0;

  for (unsigned int attempt = 0; attempt < 4; ++attempt)
    {
      if (!write_byte (op, 0xc6, 1))
        return 0;
      wait_ms (op, 4);
      if (!read_byte (op, 0xc6, &byte))
        return 0;
      if (byte == 1)
        break;
    }
  if (op->diagnostics != NULL)
    op->diagnostics->spi_mode_unacknowledged |= byte != 1;
  return 1;
}

static int
probe_inner (const struct operation         *op,
             struct fte3600_ft93xx_identity *identity)
{
  uint8_t byte = 0;
  uint16_t word;

  for (unsigned int attempt = 0; attempt < 5; ++attempt)
    {
      if (!write_byte (op, 0xfd, 0x0a) || !write_byte (op, 0xfe, 0x7f))
        return 0;
      wait_ms (op, 1);
      if (!read_byte (op, 0xfe, &byte))
        return 0;
      if (byte == 0x7f)
        break;
    }
  if (op->diagnostics != NULL)
    op->diagnostics->voltage_unacknowledged = byte != 0x7f;
  if (!spi_mode (op))
    return 0;
  for (unsigned int attempt = 0; attempt < 3; ++attempt)
    {
      if (!read_word (op, 0x1a8b, &word))
        return 0;
      identity->chip_id = word;
      identity->raw_id = word;
      if (!write_word (op, 0x1a84, 0xffff))
        return 0;
      if (lookup (word) != NULL)
        {
          if (word == 0x9391)
            {
              if (!read_word (op, 0x1816, &word))
                return 0;
              if (word == 0x0fff)
                identity->chip_id = 0x9395;
            }
          return 1;
        }
      if (!idle (op))
        return 0;
      identity->chip_id = 0;
    }
  return 1;
}

static int
valid_io (const struct fte3600_ft93xx_io *io)
{
  return io != NULL && io->spi != NULL && io->wait_ms != NULL;
}

static void
clear_diagnostics (struct fte3600_ft93xx_diagnostics *diagnostics)
{
  if (diagnostics != NULL)
    memset (diagnostics, 0, sizeof (*diagnostics));
}

enum fte3600_ft93xx_status
fte3600_ft93xx_probe (
  struct fte3600_ft93xx_identity    *identity,
  const struct fte3600_ft93xx_io    *io,
  struct fte3600_ft93xx_diagnostics *diagnostics)
{
  enum fte3600_ft93xx_status error = FTE3600_FT93XX_TRANSPORT_ERROR;
  struct operation op = {io, diagnostics, &error};

  if (identity == NULL || !valid_io (io))
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  memset (identity, 0, sizeof (*identity));
  clear_diagnostics (diagnostics);
  return probe_inner (&op, identity) ? FTE3600_FT93XX_OK : error;
}

static int
efuse_byte (const struct operation *op, uint16_t chip_id,
            uint8_t foundry, uint8_t address, uint8_t *value)
{
  if (foundry == 1)
    return write_byte (op, 0xf6, (uint8_t) (address + 0x20u)) &&
           read_byte (op, 0xf7, value);
  if (foundry == 2)
    {
      static const uint8_t control[] = {0xc0, 0xc1, 0xc0, 0x80};
      if (!write_byte (op, 0xf1, address))
        return 0;
      for (size_t i = 0; i < sizeof (control); ++i)
        if (!write_byte (op, 0xf4, control[i]))
          return 0;
      return read_byte (op, 0xf3, value);
    }
  if (foundry == 3)
    {
      if (chip_id < 0x9398)
        return write_byte (op, 0xf1, address) &&
               write_byte (op, 0xf4, 0x81) &&
               write_byte (op, 0xf4, 0x80) && read_byte (op, 0xf3, value);
      uint8_t encoded = (uint8_t) (0x20u | (address & 0x1fu) |
                                   ((address & 0x20u) << 2));
      if (!write_byte (op, 0xf6, encoded))
        return 0;
      wait_ms (op, 1);
      return read_byte (op, 0xf7, value);
    }
  *value = 0;
  return 1;
}

static int
select_foundry (const struct operation *op, uint16_t chip_id,
                uint16_t raw_id, uint8_t *foundry)
{
  uint16_t word;
  uint8_t selected = 0;

  if (!read_word (op, 0x1a8d, &word))
    return 0;
  unsigned int marker = (word >> 2) & 0x3f;
  switch (chip_id)
    {
    case 0x9391: selected = 1;
      break;

    case 0x9392:
    case 0x9363:
      if (marker == 0x13)
        selected = 2;
      break;

    case 0x9395:
      if (raw_id != 0x9395)
        selected = 1;
      else if (marker == 0)
        selected = 2;
      else if (marker == 0x17)
        selected = 3;
      break;

    case 0x9372:
      if (marker == 0x13)
        selected = 2;
      else if (marker == 0x17)
        selected = 3;
      break;

    case 0x9365:
    case 0x9396:
    case 0x9398:
      if (marker == 0x17)
        selected = 3;
      break;

    case 0x9349:
    case 0x9397:
      if (marker == 0x13)
        selected = 1;
      break;

    default: break;
    }
  *foundry = selected;
  if (selected != 0)
    return 1;
  for (uint8_t trial = 1; trial <= 3; ++trial)
    {
      uint8_t value;
      if (!efuse_byte (op, chip_id, trial, 2, &value))
        return 0;
      if (value != 0)
        {
          *foundry = trial;
          break;
        }
    }
  return 1;
}

static int
serial_bytes (const struct operation *op, uint16_t chip_id,
              uint8_t foundry, uint8_t *serial)
{
  static const uint8_t addresses[17] = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
    0x02, 0x03, 0x04, 0x05, 0x06, 0x08, 0x09
  };

  for (size_t i = 0; i < sizeof (addresses); ++i)
    if (!efuse_byte (op, chip_id, foundry, addresses[i], serial + i))
      return 0;
  return 1;
}

enum fte3600_ft93xx_status
fte3600_ft93xx_efuse_read (
  uint16_t chip_id, uint8_t foundry, uint8_t address, uint8_t *value,
  const struct fte3600_ft93xx_io *io,
  struct fte3600_ft93xx_diagnostics *diagnostics)
{
  enum fte3600_ft93xx_status error = FTE3600_FT93XX_TRANSPORT_ERROR;
  struct operation op = {io, diagnostics, &error};

  if (value == NULL || !valid_io (io))
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  clear_diagnostics (diagnostics);
  return efuse_byte (&op, chip_id, foundry, address, value) ? FTE3600_FT93XX_OK : error;
}

enum fte3600_ft93xx_status
fte3600_ft93xx_foundry (
  uint16_t chip_id, uint16_t raw_id, uint8_t *foundry,
  const struct fte3600_ft93xx_io *io,
  struct fte3600_ft93xx_diagnostics *diagnostics)
{
  enum fte3600_ft93xx_status error = FTE3600_FT93XX_TRANSPORT_ERROR;
  struct operation op = {io, diagnostics, &error};

  if (foundry == NULL || !valid_io (io))
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  clear_diagnostics (diagnostics);
  return select_foundry (&op, chip_id, raw_id, foundry) ? FTE3600_FT93XX_OK : error;
}

enum fte3600_ft93xx_status
fte3600_ft93xx_serial_read (
  uint16_t chip_id, uint8_t foundry, uint8_t *serial, size_t capacity,
  const struct fte3600_ft93xx_io *io,
  struct fte3600_ft93xx_diagnostics *diagnostics)
{
  enum fte3600_ft93xx_status error = FTE3600_FT93XX_TRANSPORT_ERROR;
  struct operation op = {io, diagnostics, &error};

  if (serial == NULL || capacity < FTE3600_FT93XX_SERIAL_SIZE || !valid_io (io))
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  clear_diagnostics (diagnostics);
  return serial_bytes (&op, chip_id, foundry, serial) ? FTE3600_FT93XX_OK : error;
}

static enum fte3600_ft93xx_status
prepare_startup (
  struct fte3600_ft93xx_preparation *output,
  const struct fte3600_ft93xx_io    *io,
  struct fte3600_ft93xx_diagnostics *diagnostics,
  struct fte3600_ft93xx_device      *device)
{
  enum fte3600_ft93xx_status error = FTE3600_FT93XX_TRANSPORT_ERROR;
  struct operation op = {io, diagnostics, &error};
  uint8_t version_byte;
  uint16_t word;

  if (!idle (&op) || !probe_inner (&op, &output->identity))
    return error;
  uint16_t chip = output->identity.chip_id, raw = output->identity.raw_id;
  if (lookup (chip) == NULL)
    return FTE3600_FT93XX_UNSUPPORTED_DEVICE;
  if (!spi_mode (&op) || !select_foundry (&op, chip, raw, &output->foundry) ||
      !efuse_byte (&op, chip, output->foundry, 0x13, &version_byte))
    return error;
  apply_area (chip, raw, version_byte, &output->geometry, &output->version);
  output->features = fte3600_ft93xx_features (chip, raw, output->version, output->foundry);
  if ((output->features & 1) != 0)
    {
      if (!read_word (&op, 0x1a8e, &word) ||
          !write_word (&op, 0x1a8e, (uint16_t) ((word & 0xfff8) | 2)))
        return error;
    }
  if (!serial_bytes (&op, chip, output->foundry, output->serial))
    return error;
  output->state.mode = 0xa0;
  output->state.configured = 0;
  if (device != NULL)
    {
      device->fdt.analog.fdt_configured = 0;
      device->fdt.armed_direction = 0;
      device->image.nav_ready = 0;
    }
  if (!write_word (&op, 0x1a84, 0xffff))
    return error;
  (void) fte3600_ft93xx_configuration (chip, output->version, &output->state);
  output->calibration_pending = 1;
  return FTE3600_FT93XX_OK;
}

enum fte3600_ft93xx_status
fte3600_ft93xx_startup (
  struct fte3600_ft93xx_preparation *output,
  const struct fte3600_ft93xx_io    *io,
  struct fte3600_ft93xx_diagnostics *diagnostics)
{
  if (output == NULL || !valid_io (io))
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  memset (output, 0, sizeof (*output));
  clear_diagnostics (diagnostics);
  return prepare_startup (output, io, diagnostics, NULL);
}

enum fte3600_ft93xx_status
fte3600_ft93xx_fdt_defaults (
  uint16_t chip_id, uint8_t foundry, uint8_t version,
  struct fte3600_ft93xx_fdt_state *fdt)
{
  uint8_t dac;

  if (fdt == NULL || foundry > 3)
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  switch (chip_id)
    {
    case 0x9349: dac = 110;
      break;

    case 0x9363:
    case 0x9365: dac = 68;
      break;

    case 0x9372: dac = 65;
      break;

    case 0x9391:
    case 0x9392: dac = 122;
      break;

    case 0x9395: dac = foundry == 3 ? 120 : 112;
      break;

    case 0x9396: dac = 102;
      break;

    case 0x9397: dac = version == 0 ? 107 : 125;
      break;

    case 0x9398: dac = 106;
      break;

    default: return FTE3600_FT93XX_UNSUPPORTED_DEVICE;
    }
  *fdt = (struct fte3600_ft93xx_fdt_state){dac, 0, 0, 0xaa};
  return FTE3600_FT93XX_OK;
}

static int
masked_write (const struct operation *op,
              struct fte3600_ft93xx_state *state,
              uint16_t address, uint16_t mask, uint16_t bits)
{
  uint16_t value;

  return read_word (op, address, &value) &&
         checked_write (op, state, address, (uint16_t) ((value & mask) | bits));
}

static int
analog_channels (const struct operation *op, uint16_t chip,
                 struct fte3600_ft93xx_state *state)
{
  if (chip == 0x9349)
    return checked_write (op, state, 0xc0, 0x0440) &&
           checked_write (op, state, 0xc1, 0x00c0) &&
           checked_write (op, state, 0xc2, 0x0044);
  if (chip == 0x9363 || chip == 0x9365)
    return masked_write (op, state, 0xc0, 0xe000, 0x0444) &&
           masked_write (op, state, 0xc1, 0xffc0, 0x0021) &&
           masked_write (op, state, 0xc2, 0xffff, 0x00c0);
  if (chip == 0x9372)
    return checked_write (op, state, 0xc0, 0x0920) &&
           checked_write (op, state, 0xc1, 0x0c8e);
  static const uint8_t channels[][4] = {
    {3, 9, 15, 20}, {2, 8, 13, 19}, {0, 4, 7, 11}, {2, 6, 11, 15}
  };
  unsigned int row = chip == 0x9391 || chip == 0x9392 ? 0 :
                     chip == 0x9396 ? 2 : chip == 0x9398 ? 3 : 1;
  uint16_t bits = row == 0 ? 0x0103 : chip == 0x9395 ? 0x0083 : 0x0080;
  for (size_t i = 0; i < 4; ++i)
    {
      uint16_t address = (uint16_t) (0xc0 + 2 * channels[row][i]);
      if (!masked_write (op, state, address, 0xe000, bits) ||
          !masked_write (op, state, (uint16_t) (address + 1), 0xe000, 0))
        return 0;
    }
  return 1;
}

static int
configure_fdt (const struct operation *operation,
               uint16_t chip_id, uint8_t foundry,
               struct fte3600_ft93xx_state *state,
               struct fte3600_ft93xx_fdt_state *fdt, uint16_t replacement)
{
  const struct operation op = *operation;
  uint16_t word;
  unsigned int gain = chip_id == 0x9391 || chip_id == 0x9392 ? 7 :
                      chip_id == 0x9397 ? 11 : 10;
  uint16_t dac = (uint16_t) (0x3000u | ((unsigned int) state->dac_polarity << 14) |
                             (gain << 8) | fdt->fdt_dac);

  if (!idle (&op) || !checked_write (&op, state, 0x1801, dac))
    return 0;
  if (fdt->threshold_cached != fdt->threshold_mode)
    {
      if (!checked_write (&op, state, 0x180c, fdt->threshold_mode ? 0 : (uint16_t) (0x0400 | replacement)))
        return 0;
      fdt->threshold_cached = fdt->threshold_mode;
    }
  int newer = chip_id >= 0x9391;
  if (!masked_write (&op, state, 0x1807, 0xfff0, 1) ||
      !checked_write (&op, state, 0x1806, newer ? 0x09bb : 0x04bb) ||
      !checked_write (&op, state, 0x180a, newer ? 0x0987 : 0x0482) ||
      !checked_write (&op, state, 0x180b, newer ? 0x0491 : 0x0207) ||
      !masked_write (&op, state, 0x1812, 0x8000,
                     (uint16_t) (((unsigned int) state->subtract_baseline << 14) |
                                 (newer ? 0x0493 : 0x0209))))
    return 0;
  if (!newer && !masked_write (&op, state, 0x1810, 0xffff, 0x0380))
    return 0;
  if (state->mode == 0xa1)
    return 1;
  int wide = chip_id == 0x9391 || chip_id == 0x9392 || chip_id == 0x9395;
  if (!checked_write (&op, state, 0x1881, wide ? 0x20ce : 0x0fce))
    return 0;
  state->mode = 0xa1;
  uint16_t window = 2, extension = 0;
  switch (chip_id)
    {
    case 0x9391:
    case 0x9392: window = 0x1c10;
      extension = 0x0420;
      break;

    case 0x9395:
    case 0x9397: window = 0x1a08;
      extension = 0x0208;
      break;

    case 0x9396: window = 0x1922;
      extension = 2;
      break;

    case 0x9398: window = 0x1888;
      extension = 0x0022;
      break;

    default: break;
    }
  if (!checked_write (&op, state, 0x1800, window) ||
      (newer && !checked_write (&op, state, 0x1816, extension)) ||
      !checked_write (&op, state, 0x1804, 0x07c0))
    return 0;
  if (fdt->fdt_configured)
    return 1;
  uint16_t mode = wide ? 0x084b : chip_id == 0x9349 ? 0x0803 :
                  chip_id == 0x9372 ? 0x080b : 0x0807;
  uint16_t bias = chip_id != 0x9372 || foundry == 2 ? 0x5f64 :
                  foundry == 3 ? 0x5055 : 0;
  uint16_t routing = wide ? 0 : chip_id >= 0x9396 ? 4 : 1;
  if (!checked_write (&op, state, 0x1808, mode) ||
      !checked_write (&op, state, 0x1815, 0) ||
      !checked_write (&op, state, 0x180d, 0x03e8) ||
      !checked_write (&op, state, 0x1880, bias) ||
      !checked_write (&op, state, 0x1884, 3) ||
      !checked_write (&op, state, 0x1887, routing) ||
      !checked_write (&op, state, 0x1888, 0) ||
      !write_byte (&op, 0x9a, 0x5a) || !analog_channels (&op, chip_id, state) ||
      !write_byte (&op, 0x9a, 0) || !read_word (&op, 0x1a83, &word))
    return 0;
  if ((word & 8) == 0 && !write_word (&op, 0x1a83, (uint16_t) (word | 8)))
    return 0;
  fdt->fdt_configured = 1;
  return 1;
}

enum fte3600_ft93xx_status
fte3600_ft93xx_fdt_configure (
  uint16_t chip_id, uint8_t foundry,
  struct fte3600_ft93xx_state *state,
  struct fte3600_ft93xx_fdt_state *fdt,
  const struct fte3600_ft93xx_io *io,
  struct fte3600_ft93xx_diagnostics *diagnostics)
{
  enum fte3600_ft93xx_status error = FTE3600_FT93XX_TRANSPORT_ERROR;
  struct operation op = {io, diagnostics, &error};

  if (state == NULL || fdt == NULL || !valid_io (io) || foundry > 3 ||
      state->dac_polarity > 3 || state->subtract_baseline > 1 ||
      state->check_registers > 2 || fdt->fdt_configured > 1 || fdt->threshold_mode > 1)
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  if (lookup (chip_id) == NULL)
    return FTE3600_FT93XX_UNSUPPORTED_DEVICE;
  clear_diagnostics (diagnostics);
  return configure_fdt (&op, chip_id, foundry, state, fdt, 700) ? FTE3600_FT93XX_OK : error;
}

enum fte3600_ft93xx_status
fte3600_ft93xx_fdt_controller_defaults (
  uint16_t chip_id, uint8_t foundry, uint8_t version,
  struct fte3600_ft93xx_fdt_controller *controller)
{
  struct fte3600_ft93xx_fdt_controller next = {0};

  if (controller == NULL)
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  enum fte3600_ft93xx_status status =
    fte3600_ft93xx_fdt_defaults (chip_id, foundry, version, &next.analog);
  if (status != FTE3600_FT93XX_OK)
    return status;
  next.default_dac = next.analog.fdt_dac;
  next.replacement = 700;
  next.max_dac = chip_id == 0x9363 || chip_id == 0x9365 || chip_id == 0x9372 ? 127 : 255;
  next.step = 6;
  next.margin = 20;
  *controller = next;
  return FTE3600_FT93XX_OK;
}

struct fdt_operation
{
  struct operation                       wire;
  uint16_t                               chip;
  uint8_t                                foundry;
  struct fte3600_ft93xx_state           *state;
  struct fte3600_ft93xx_fdt_controller  *control;
  struct fte3600_ft93xx_fdt_diagnostics *diagnostics;
};

static uint16_t
fdt_crc (const uint8_t *bytes, size_t length)
{
  uint16_t crc = 0xffff;

  for (size_t i = 0; i < length; ++i)
    {
      crc = (uint16_t) (crc ^ ((unsigned int) bytes[i] << 8));
      for (unsigned int bit = 0; bit < 8; ++bit)
        crc = (uint16_t) (((unsigned int) crc << 1) ^
                          ((crc & 0x8000) ? 0x1021u : 0));
    }
  return crc;
}

static int
fdt_prepare (const struct fdt_operation *op)
{
  uint8_t previous_idle_timeout = op->diagnostics == NULL ? 0 :
                                  op->diagnostics->transport.idle_unacknowledged;
  int result = configure_fdt (&op->wire, op->chip, op->foundry, op->state,
                              &op->control->analog, op->control->replacement);

  if (op->diagnostics != NULL)
    op->diagnostics->transport.idle_unacknowledged |= previous_idle_timeout;
  return result;
}

static int
fdt_measure (const struct fdt_operation *op)
{
  uint16_t word;

  if (!fdt_prepare (op))
    return 0;
  if (op->diagnostics != NULL)
    ++op->diagnostics->measurements;
  for (unsigned int attempt = 0; attempt < 5; ++attempt)
    {
      if (!read_word (&op->wire, 0x1881, &word) ||
          (!(word & 2) && !write_word (&op->wire, 0x1881, (uint16_t) (word | 2))) ||
          !command (&op->wire, 0xc2))
        return 0;
      wait_ms (&op->wire, 1);
      if (!write_word (&op->wire, 0x1885, 1))
        return 0;
      for (unsigned int poll = 0; poll < 5; ++poll)
        {
          if (!read_word (&op->wire, 0x1a82, &word))
            return 0;
          if (word & 8)
            return write_word (&op->wire, 0x1a84, 8);
          wait_ms (&op->wire, 1);
        }
    }
  if (op->diagnostics != NULL)
    {
      ++op->diagnostics->readiness_timeouts;
      op->diagnostics->transport.ready_unacknowledged = 1;
    }
  return 1;
}

static int
fdt_read (const struct fdt_operation *op, uint16_t values[4])
{
  uint8_t buffer[16] = {0x04, 0xfb, 0x80, op->chip == 0x9363 ? 0xe8 : 0xb8, 0, 3};

  if (!transfer (&op->wire, buffer, sizeof (buffer)))
    return 0;
  uint16_t crc = (uint16_t) ((uint16_t) buffer[14] << 8 | buffer[15]);
  if (crc != 0 && crc != fdt_crc (buffer + 6, 8))
    {
      *op->wire.error = FTE3600_FT93XX_BAD_CRC;
      return 0;
    }
  for (size_t i = 0; i < 4; ++i)
    values[i] = (uint16_t) ((uint16_t) buffer[6 + 2 * i] << 8 | buffer[7 + 2 * i]);
  return 1;
}

static int
fdt_sample (const struct fdt_operation *op, uint16_t values[4])
{
  return fdt_measure (op) && fdt_read (op, values);
}

static int
fdt_valid_base (const uint16_t values[4])
{
  for (size_t i = 0; i < 4; ++i)
    if (values[i] < 500 || values[i] > 900)
      return 0;
  return 1;
}

/* Returns 1 for a crossing, 0 for a completed search failure, -1 for I/O. */
static int
fdt_search (const struct fdt_operation *op)
{
  struct fte3600_ft93xx_fdt_controller *c = op->control;
  uint16_t values[4], previous_average = 0;
  unsigned int lower = 0, upper = c->max_dac, previous_dac = 0;

  c->analog.threshold_mode = 1;
  c->damaged = 0;
  if (!c->dac_calibrated)
    c->analog.fdt_dac = c->default_dac;
  if (op->diagnostics != NULL)
    ++op->diagnostics->search_attempts;
  for (unsigned int iteration = 0; iteration < 51; ++iteration)
    {
      if (!fdt_sample (op, values))
        goto io_error;
      uint16_t sum = 0;
      for (size_t i = 0; i < 4; ++i)
        sum = (uint16_t) (sum + values[i]);
      unsigned int average = sum >> 2, current = c->analog.fdt_dac;
      if ((average <= 700 && previous_average > 700 && current + 1 == previous_dac) ||
          (average > 700 && previous_average <= 700 && current == previous_dac + 1))
        {
          c->analog.fdt_dac = (uint8_t) (average <= 700 ? previous_dac : current);
          c->dac_calibrated = 1;
          c->replacement = 0;
          if (!fdt_sample (op, values))
            goto io_error;
          sum = 0;
          int valid = 1;
          for (size_t i = 0; i < 4; ++i)
            {
              if (values[i] <= c->margin || values[i] > 1022)
                valid = 0;
              sum = (uint16_t) (sum + (uint16_t) (values[i] - c->margin));
            }
          c->replacement = valid ? (uint16_t) (sum >> 2) : 700;
          int difference = (int) c->analog.fdt_dac - c->default_dac;
          if (!op->state->subtract_baseline && (difference > 40 || difference < -40))
            c->damaged = 1;
          c->analog.threshold_mode = 0;
          return 1;
        }
      if (current == 0 || current > (unsigned int) c->max_dac - 1)
        break;
      previous_average = (uint16_t) average;
      previous_dac = current;
      if (average < 500)
        {
          if (current == upper)
            break;
          lower = current;
          c->analog.fdt_dac = (uint8_t) ((upper + current) / 2);
        }
      else if (average >= 901)
        {
          if (current == lower)
            break;
          upper = current;
          c->analog.fdt_dac = (uint8_t) ((lower + current) / 2);
        }
      else if (average > 700u + 2u * c->step)
        {
          c->analog.fdt_dac = (uint8_t) (current - (average - 700) / c->step);
        }
      else if (average + 2u * c->step < 700)
        {
          unsigned int next = current + (700 - average) / c->step;
          if (next >= c->max_dac)
            {
              c->analog.fdt_dac = (uint8_t) (c->max_dac - 1);
              if (!fdt_sample (op, values))
                goto io_error;
              break;
            }
          c->analog.fdt_dac = (uint8_t) next;
        }
      else if (average <= 700)
        {
          if (current + 1 >= c->max_dac)
            break;
          c->analog.fdt_dac = (uint8_t) (current + 1);
        }
      else
        {
          c->analog.fdt_dac = (uint8_t) (current - 1);
        }
    }
  c->analog.fdt_dac = c->default_dac;
  c->replacement = 700;
  if (!op->state->subtract_baseline)
    c->damaged = 1;
  c->analog.threshold_mode = 0;
  if (op->diagnostics != NULL)
    ++op->diagnostics->search_failures;
  return 0;
io_error:
  c->analog.threshold_mode = 0;
  return -1;
}

static int
fdt_adjust (const struct fdt_operation *op, uint16_t values[4])
{
  uint16_t off[4], on[4];
  unsigned int correction = 0;

  op->state->subtract_baseline = 0;
  if (!fdt_sample (op, off))
    return 0;
  op->state->subtract_baseline = 1;
  if (!fdt_sample (op, on))
    return 0;
  for (size_t i = 0; i < 4; ++i)
    {
      uint16_t wrapped = (uint16_t) (off[i] - on[i]);
      int difference = wrapped > 32767 ? (int) wrapped - 65536 : wrapped;
      if ((int) values[i] <= difference + op->control->margin)
        {
          if (!fdt_sample (op, values))
            return 0;
          correction = 30;
          if (op->diagnostics != NULL)
            op->diagnostics->adjustment_fallback = 1;
          break;
        }
      values[i] = (uint16_t) ((int) values[i] - op->control->margin - difference);
    }
  uint8_t bytes[8];
  for (size_t i = 0; i < 4; ++i)
    {
      uint16_t value = (uint16_t) (values[i] - correction);
      op->control->base_up[i] = op->control->base_down[i] = value;
      bytes[2 * i] = (uint8_t) value;
      bytes[2 * i + 1] = (uint8_t) (value >> 8);
    }
  op->control->base_up_checksum = op->control->base_down_checksum = fdt_crc (bytes, 8);
  return 1;
}

static int
fdt_baseline (const struct fdt_operation *op, unsigned int count)
{
  uint16_t minima[4] = {0}, previous[4] = {0}, values[4];
  uint8_t stable = 0;

  for (unsigned int round = 0; round < 5; ++round)
    {
      if (op->diagnostics != NULL)
        ++op->diagnostics->baseline_rounds;
      op->state->subtract_baseline = 0;
      int result = fdt_search (op);
      if (result < 0)
        return 0;
      if (!result)
        continue;
      for (size_t i = 0; i < 4; ++i)
        minima[i] = 1023;
      for (unsigned int sample = 0; sample < count; ++sample)
        {
          if (!fdt_sample (op, values))
            return 0;
          stable = (uint8_t) (stable + 1);
          for (size_t i = 0; i < 4; ++i)
            {
              if (values[i] < minima[i])
                minima[i] = values[i];
              int difference = (int) values[i] - previous[i];
              if (sample != 0 && (difference > 5 || difference < -5))
                stable = 1;
              previous[i] = values[i];
            }
          if (stable > 2)
            break;
        }
      if (fdt_valid_base (minima))
        {
          if (op->diagnostics != NULL)
            op->diagnostics->baseline_valid = 1;
          break;
        }
    }
  return fdt_adjust (op, minima);
}

static int
fdt_write_base (const struct fdt_operation *op, const uint16_t values[8])
{
  uint8_t buffer[22] = {0x05, 0xfa, 0x80, op->chip == 0x9363 ? 0xe0 : 0xb0, 0, 7};

  for (size_t i = 0; i < 8; ++i)
    {
      buffer[6 + 2 * i] = (uint8_t) (values[i] >> 8);
      buffer[7 + 2 * i] = (uint8_t) values[i];
    }
  return write_byte (&op->wire, 0x9a, 0x5a) && transfer (&op->wire, buffer, sizeof (buffer)) &&
         write_byte (&op->wire, 0x9a, 0);
}

static int
fdt_arm (const struct fdt_operation *op, uint32_t features, unsigned int direction)
{
  uint16_t word;

  if (!fdt_prepare (op))
    return 0;
  if ((features & 2) && (!read_word (&op->wire, 0x1817, &word) ||
                         !write_word (&op->wire, 0x1817, (uint16_t) (word & 0xfffc)) ||
                         !write_word (&op->wire, 0x1889, 4)))
    return 0;
  if (!write_word (&op->wire, 0x1a84, 0x002f))
    return 0;
  op->control->armed_direction = (uint8_t) direction;
  if (!fdt_write_base (op, direction ? op->control->base_down : op->control->base_up) ||
      !read_word (&op->wire, 0x1881, &word) ||
      !write_word (&op->wire, 0x1881, (uint16_t) ((word & 0xff7c) | (direction ? 0x81 : 0))) ||
      !command (&op->wire, 0xc2))
    return 0;
  wait_ms (&op->wire, 1);
  return 1;
}

enum fte3600_ft93xx_status
fte3600_ft93xx_fdt_run (
  uint16_t chip_id, uint8_t foundry, uint32_t features,
  struct fte3600_ft93xx_state *state,
  struct fte3600_ft93xx_fdt_controller *controller,
  enum fte3600_ft93xx_fdt_action action, unsigned int argument,
  uint16_t *values, size_t value_count,
  const struct fte3600_ft93xx_io *io,
  struct fte3600_ft93xx_fdt_diagnostics *diagnostics)
{
  if (state == NULL || controller == NULL || !valid_io (io) || foundry > 3 ||
      action < FTE3600_FT93XX_FDT_MEASURE || action > FTE3600_FT93XX_FDT_ARM ||
      state->dac_polarity > 3 || state->subtract_baseline > 1 || state->configured > 1 ||
      state->check_registers > 2 || controller->analog.fdt_configured > 1 ||
      controller->analog.threshold_mode > 1 || controller->dac_calibrated > 1 ||
      controller->damaged > 1 || controller->armed_direction > 1 ||
      controller->step == 0 || controller->max_dac < 2 ||
      argument > (action == FTE3600_FT93XX_FDT_BASELINE ? 255u :
                  action == FTE3600_FT93XX_FDT_ARM ? 1u : 0u))
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  size_t required = action == FTE3600_FT93XX_FDT_WRITE_BASE ? 8 :
                    action == FTE3600_FT93XX_FDT_READ || action == FTE3600_FT93XX_FDT_SAMPLE ||
                    action == FTE3600_FT93XX_FDT_VALIDATE_BASE || action == FTE3600_FT93XX_FDT_ADJUST_BASE ? 4 : 0;
  if (required != 0 && (values == NULL || value_count < required))
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  if (lookup (chip_id) == NULL)
    return FTE3600_FT93XX_UNSUPPORTED_DEVICE;
  if (diagnostics != NULL)
    memset (diagnostics, 0, sizeof (*diagnostics));
  enum fte3600_ft93xx_status error = FTE3600_FT93XX_TRANSPORT_ERROR;
  struct fdt_operation op = {{io, diagnostics == NULL ? NULL : &diagnostics->transport, &error},
                             chip_id, foundry, state, controller, diagnostics};
  int result = 0;
  switch (action)
    {
    case FTE3600_FT93XX_FDT_MEASURE: result = fdt_measure (&op);
      break;

    case FTE3600_FT93XX_FDT_READ: result = fdt_read (&op, values);
      break;

    case FTE3600_FT93XX_FDT_SAMPLE: result = fdt_sample (&op, values);
      break;

    case FTE3600_FT93XX_FDT_SEARCH:
      result = fdt_search (&op);
      return result > 0 ? FTE3600_FT93XX_OK : result == 0 ? FTE3600_FT93XX_FDT_SEARCH_FAILED : error;

    case FTE3600_FT93XX_FDT_VALIDATE_BASE:
      return fdt_valid_base (values) ? FTE3600_FT93XX_OK : FTE3600_FT93XX_FDT_INVALID_BASE;

    case FTE3600_FT93XX_FDT_ADJUST_BASE: result = fdt_adjust (&op, values);
      break;

    case FTE3600_FT93XX_FDT_BASELINE: result = fdt_baseline (&op, argument);
      break;

    case FTE3600_FT93XX_FDT_WRITE_BASE: result = fdt_write_base (&op, values);
      break;

    case FTE3600_FT93XX_FDT_ARM: result = fdt_arm (&op, features, argument);
      break;
    }
  return result ? FTE3600_FT93XX_OK : error;
}

static uint16_t
navigation_rows (uint16_t chip)
{
  switch (chip)
    {
    case 0x9349: return 16;

    case 0x9363:
    case 0x9365: return 10;

    case 0x9372: return 7;

    case 0x9391:
    case 0x9392: return 25;

    case 0x9395:
    case 0x9397: return 22;

    case 0x9396: return 12;

    case 0x9398: return 18;

    default: return 0;
    }
}

enum fte3600_ft93xx_status
fte3600_ft93xx_navigation_geometry (
  uint16_t chip_id, struct fte3600_ft93xx_geometry *geometry)
{
  const struct profile *p = lookup (chip_id);

  if (geometry == NULL)
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  if (p == NULL)
    return FTE3600_FT93XX_UNSUPPORTED_DEVICE;
  *geometry = (struct fte3600_ft93xx_geometry){p->width, navigation_rows (chip_id), 0};
  return FTE3600_FT93XX_OK;
}

enum fte3600_ft93xx_status
fte3600_ft93xx_image_controller_defaults (
  uint16_t chip_id, uint8_t version,
  struct fte3600_ft93xx_image_controller *controller)
{
  struct fte3600_ft93xx_state initial = {0};

  if (controller == NULL)
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  if (lookup (chip_id) == NULL)
    return FTE3600_FT93XX_UNSUPPORTED_DEVICE;
  (void) fte3600_ft93xx_configuration (chip_id, version, &initial);
  *controller = (struct fte3600_ft93xx_image_controller){
    0, initial.dac, 0, 868,
    chip_id == 0x9363 || chip_id == 0x9365 || chip_id == 0x9372 ? 127 : 255,
    chip_id == 0x9363 || chip_id == 0x9372 ? 15 : 6
  };
  return FTE3600_FT93XX_OK;
}

struct navigation_operation
{
  struct operation                         wire;
  const struct profile                    *profile;
  uint16_t                                 raw, rows;
  size_t                                   samples;
  struct fte3600_ft93xx_state             *state;
  struct fte3600_ft93xx_image_controller  *control;
  uint16_t                                *pixels;
  struct fte3600_ft93xx_image_diagnostics *diagnostics;
};

static int
navigation_prepare (const struct navigation_operation *op, int configure_image)
{
  uint8_t previous_idle_timeout = op->diagnostics == NULL ? 0 :
                                  op->diagnostics->transport.idle_unacknowledged;
  int result = configure_image ? configure (&op->wire, op->profile, op->state, 0xa4) : idle (&op->wire);

  if (op->diagnostics != NULL)
    op->diagnostics->transport.idle_unacknowledged |= previous_idle_timeout;
  return result;
}

static int
navigation_capture (const struct navigation_operation *op)
{
  uint16_t word;
  uint8_t byte = 0;

  if (op->diagnostics != NULL)
    {
      ++op->diagnostics->captures;
      op->diagnostics->transport.raw_bytes_completed = 0;
    }
  if (!op->control->nav_ready)
    {
      if (!navigation_prepare (op, 0) || !read_word (&op->wire, 0x1808, &word) ||
          !write_word (&op->wire, 0x1808, (uint16_t) ((word & 0xc7ff) | 0x0800)) ||
          !write_byte (&op->wire, 0x9a, 0x5a))
        return 0;
      for (unsigned int row = 0; row < op->rows; ++row)
        {
          int narrow = op->profile->id == 0x9363 || op->profile->id == 0x9365;
          uint16_t address = (uint16_t) (narrow ? 0xc2 + 3 * row : 0xc1 + 2 * row);
          uint16_t bits = row == 0 ? 7 : op->profile->id > 0x9390 ? 3 : 4;
          if (!read_word (&op->wire, address, &word) ||
              !write_word (&op->wire, address, (uint16_t) ((word & 0xffc0) | bits)))
            return 0;
        }
      op->control->nav_ready = 1;
      if (!write_byte (&op->wire, 0x9a, 0))
        return 0;
    }
  if (!navigation_prepare (op, 1) || !command (&op->wire, 0xc8))
    return 0;
  wait_ms (&op->wire, 1);
  for (unsigned int attempt = 0; attempt < 10; ++attempt)
    {
      if (!read_byte (&op->wire, 0x80, &byte))
        return 0;
      if (byte == 0x58)
        break;
    }
  if (byte != 0x58 && op->diagnostics != NULL)
    {
      ++op->diagnostics->mode_timeouts;
      op->diagnostics->transport.mode_unacknowledged = 1;
    }
  if (!write_word (&op->wire, 0x1800, (uint16_t) (op->profile->window | 1)))
    return 0;
  for (unsigned int poll = 0; poll < 5; ++poll)
    {
      if (!read_word (&op->wire, 0x1a82, &word))
        return 0;
      if (word & 0x20)
        {
          if (!write_word (&op->wire, 0x1a84, word))
            return 0;
          break;
        }
      wait_ms (&op->wire, 1);
    }
  if (!(word & 0x20) && op->diagnostics != NULL)
    {
      ++op->diagnostics->readiness_timeouts;
      op->diagnostics->transport.ready_unacknowledged = 1;
    }
  if (!fifo (&op->wire, (uint8_t *) op->pixels, 2 * op->samples))
    return 0;
  decode_samples ((uint8_t *) op->pixels, op->samples, op->raw);
  return write_word (&op->wire, 0x1a84, 0x20);
}

static uint16_t
navigation_statistic (const struct navigation_operation *op, unsigned int mode)
{
  uint16_t histogram[1024] = {0};
  size_t count = 0;
  unsigned int border = op->profile->id == 0x9349 ? 2 : 1;
  size_t rows = mode ? op->rows : op->profile->height;
  size_t begin = mode ? 0 : border;

  for (size_t row = begin; row < rows - begin; ++row)
    {
      for (size_t column = begin; column < op->profile->width - begin; ++column)
        {
          unsigned int bin = op->pixels[row * op->profile->width + column] >> 2;
          if (bin > 1023)
            bin = 1023;
          ++histogram[bin];
          ++count;
        }
    }
  size_t cumulative = 0;
  for (unsigned int bin = 0; bin < 1024; ++bin)
    {
      cumulative += histogram[bin];
      if (cumulative > count / 2)
        return (uint16_t) bin;
    }
  return 1023;
}

static void
navigation_tails (const struct navigation_operation *op, uint16_t *left, uint16_t *right)
{
  *left = *right = 0;
  for (size_t i = 0; i < op->samples; ++i)
    {
      if (op->pixels[i] < 1200)
        ++*left;
      if (op->pixels[i] > 4012)
        ++*right;
    }
}

static int
navigation_refine (const struct navigation_operation *op)
{
  uint16_t left, right;

  navigation_tails (op, &left, &right);
  if (!((left < 20 && right > 20) || (left > 20 && right < 20)))
    return 1;
  unsigned int previous_total = (unsigned int) left + right;
  uint8_t previous_dac = op->state->dac;
  op->state->dac = (uint8_t) (previous_dac + (left > 20 ? 1 : -1));
  if (op->diagnostics != NULL)
    ++op->diagnostics->refinement_captures;
  if (!navigation_capture (op))
    return 0;
  navigation_tails (op, &left, &right);
  if ((unsigned int) left + right < previous_total)
    op->control->statistic = navigation_statistic (op, 1);
  else
    op->state->dac = previous_dac;
  return 1;
}

/* Return 1 crossing success, 0 completed failure, -1 local transport/CRC error. */
static int
navigation_search (const struct navigation_operation *op)
{
  struct fte3600_ft93xx_image_controller *c = op->control;

  if (op->diagnostics != NULL)
    ++op->diagnostics->search_attempts;
  if (!c->initialized)
    {
      c->initialized = 1;
      op->state->dac = c->default_dac;
    }
  unsigned int previous_dac = op->state->dac, previous_statistic = 0;
  unsigned int lower = 0, upper = c->max_dac;
  if (op->state->dac < 5 || op->state->dac > c->max_dac - 5)
    op->state->dac = c->default_dac;
  for (unsigned int iteration = 0; iteration < 50; ++iteration)
    {
      if (!navigation_capture (op))
        return -1;
      unsigned int median = navigation_statistic (op, 1), current = op->state->dac;
      if (median <= 868 && previous_statistic > 868 && current + 1 == previous_dac)
        {
          op->state->dac = (uint8_t) previous_dac;
          c->statistic = (uint16_t) previous_statistic;
          if (!navigation_capture (op) || !navigation_refine (op))
            return -1;
          return 1;
        }
      if (median > 868 && previous_statistic <= 868 && current == previous_dac + 1)
        {
          c->statistic = (uint16_t) median;
          return navigation_refine (op) ? 1 : -1;
        }
      if (iteration == 49)
        break;
      previous_dac = current;
      uint8_t next;
      int increasing = 0;
      if (median < 500)
        {
          if (current == upper)
            break;
          lower = current;
          next = (uint8_t) ((upper + current) / 2);
        }
      else if (median > 1000)
        {
          if (current == lower)
            break;
          upper = current;
          next = (uint8_t) ((lower + current) / 2);
        }
      else if (median > 868u + 2u * c->step)
        {
          next = (uint8_t) (current - (median - 868) / c->step);
        }
      else if (median + 2u * c->step < 868)
        {
          next = (uint8_t) (current + (868 - median) / c->step);
          increasing = 1;
        }
      else if (median <= 868)
        {
          next = (uint8_t) (current + 1);
          increasing = 1;
        }
      else
        {
          next = (uint8_t) (current - 1);
        }
      if (increasing && next >= c->max_dac)
        {
          op->state->dac = (uint8_t) (c->max_dac - 5);
          if (!navigation_capture (op))
            return -1;
          c->statistic = navigation_statistic (op, 1);
          if (!navigation_refine (op))
            return -1;
          break;
        }
      op->state->dac = next;
      previous_statistic = median;
    }
  op->state->dac = c->default_dac;
  if (op->diagnostics != NULL)
    ++op->diagnostics->search_failures;
  return 0;
}

enum fte3600_ft93xx_status
fte3600_ft93xx_image_cal_run (
  uint16_t chip_id, uint16_t raw_id,
  struct fte3600_ft93xx_state *state,
  struct fte3600_ft93xx_image_controller *controller,
  enum fte3600_ft93xx_image_action action, unsigned int mode,
  uint16_t *pixels, size_t capacity,
  struct fte3600_ft93xx_image_result *result,
  const struct fte3600_ft93xx_io *io,
  struct fte3600_ft93xx_image_diagnostics *diagnostics)
{
  const struct profile *p = lookup (chip_id);

  if (!valid_state (state) || controller == NULL || !valid_io (io) || pixels == NULL || result == NULL ||
      controller->nav_ready > 1 || controller->initialized > 1 || controller->max_dac < 5 ||
      controller->step == 0 || action < FTE3600_FT93XX_IMAGE_CAPTURE || action > FTE3600_FT93XX_IMAGE_BASELINE ||
      mode > 1 || (action != FTE3600_FT93XX_IMAGE_STATISTIC && mode != 1))
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  if (p == NULL)
    return FTE3600_FT93XX_UNSUPPORTED_DEVICE;
  uint16_t rows = navigation_rows (chip_id);
  size_t samples = (size_t) p->width * rows;
  size_t needed = action == FTE3600_FT93XX_IMAGE_STATISTIC && mode == 0 ? (size_t) p->width * p->height : samples;
  if (capacity < needed)
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  if (diagnostics != NULL)
    memset (diagnostics, 0, sizeof (*diagnostics));
  enum fte3600_ft93xx_status error = FTE3600_FT93XX_TRANSPORT_ERROR;
  struct navigation_operation op = {{io, diagnostics == NULL ? NULL : &diagnostics->transport, &error},
                                    p, raw_id, rows, samples, state, controller, pixels, diagnostics};
  int completed;
  switch (action)
    {
    case FTE3600_FT93XX_IMAGE_CAPTURE:
      return navigation_capture (&op) ? FTE3600_FT93XX_OK : error;

    case FTE3600_FT93XX_IMAGE_STATISTIC:
      result->statistic = navigation_statistic (&op, mode);
      return FTE3600_FT93XX_OK;

    case FTE3600_FT93XX_IMAGE_TAILS:
      navigation_tails (&op, &result->left, &result->right);
      return FTE3600_FT93XX_OK;

    case FTE3600_FT93XX_IMAGE_REFINE:
      return navigation_refine (&op) ? FTE3600_FT93XX_OK : error;

    case FTE3600_FT93XX_IMAGE_SEARCH:
      completed = navigation_search (&op);
      return completed < 0 ? error : completed ? FTE3600_FT93XX_OK : FTE3600_FT93XX_IMAGE_SEARCH_FAILED;

    case FTE3600_FT93XX_IMAGE_BASELINE:
      for (unsigned int attempt = 0; attempt < 5; ++attempt)
        {
          state->subtract_baseline = 0;
          completed = navigation_search (&op);
          if (completed < 0)
            return error;
          if (completed)
            {
              state->dac = (uint8_t) (state->dac + 5);
              break;
            }
        }
      state->subtract_baseline = 1;
      return FTE3600_FT93XX_OK;
    }
  return FTE3600_FT93XX_INVALID_ARGUMENT;
}

enum fte3600_ft93xx_status
fte3600_ft93xx_device_defaults (
  struct fte3600_ft93xx_device *device)
{
  if (device == NULL)
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  memset (device, 0, sizeof (*device));
  device->fdt.analog.threshold_cached = 0xaa;
  return FTE3600_FT93XX_OK;
}

static void
accumulate_transport (struct fte3600_ft93xx_diagnostics       *total,
                      const struct fte3600_ft93xx_diagnostics *part)
{
  total->failed_attempts += part->failed_attempts;
  total->exhausted_transactions += part->exhausted_transactions;
  if (part->failed_attempts)
    total->last_failure = part->last_failure;
  total->idle_unacknowledged |= part->idle_unacknowledged;
  total->mode_unacknowledged |= part->mode_unacknowledged;
  total->ready_unacknowledged |= part->ready_unacknowledged;
  total->voltage_unacknowledged |= part->voltage_unacknowledged;
  total->spi_mode_unacknowledged |= part->spi_mode_unacknowledged;
}

static void
startup_checkpoint (const struct fte3600_ft93xx_startup_observer *observer,
                    enum fte3600_ft93xx_startup_phase phase, const struct fte3600_ft93xx_device *device)
{
  if (observer != NULL && observer->phase != NULL)
    observer->phase (observer->context, phase, device);
}

enum fte3600_ft93xx_status
fte3600_ft93xx_full_startup (
  struct fte3600_ft93xx_device *device, uint16_t *pixels, size_t capacity,
  const struct fte3600_ft93xx_io *io,
  const struct fte3600_ft93xx_startup_observer *observer,
  struct fte3600_ft93xx_startup_diagnostics *diagnostics)
{
  if (device == NULL || pixels == NULL || capacity < FTE3600_FT93XX_STARTUP_SAMPLES ||
      !valid_io (io) || device->preparation.state.check_registers > 2 ||
      device->fdt.analog.threshold_mode > 1 || device->fdt.dac_calibrated > 1 ||
      device->fdt.damaged > 1 || device->image.initialized > 1)
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  struct fte3600_ft93xx_startup_diagnostics local = {0};
  struct fte3600_ft93xx_startup_diagnostics *d = diagnostics == NULL ? &local : diagnostics;
  memset (d, 0, sizeof (*d));
  struct fte3600_ft93xx_preparation *p = &device->preparation;
  struct fte3600_ft93xx_state retained = p->state;
  memset (p, 0, sizeof (*p));
  p->state = retained;
  enum fte3600_ft93xx_status status = prepare_startup (p, io, &d->preparation, device);
  accumulate_transport (&d->transport, &d->preparation);
  if (status != FTE3600_FT93XX_OK)
    return status;

  /* Assign only fields owned by the per-profile defaults. In particular,
   * preserve threshold cache, calibration flags and complete baseline arrays. */
  struct fte3600_ft93xx_fdt_controller fdt_defaults;
  struct fte3600_ft93xx_image_controller image_defaults;
  uint16_t chip = p->identity.chip_id, raw = p->identity.raw_id;
  (void) fte3600_ft93xx_fdt_controller_defaults (chip, p->foundry, p->version, &fdt_defaults);
  (void) fte3600_ft93xx_image_controller_defaults (chip, p->version, &image_defaults);
  device->fdt.analog.fdt_dac = fdt_defaults.analog.fdt_dac;
  device->fdt.default_dac = fdt_defaults.default_dac;
  device->fdt.replacement = fdt_defaults.replacement;
  device->fdt.max_dac = fdt_defaults.max_dac;
  device->fdt.step = fdt_defaults.step;
  device->fdt.margin = fdt_defaults.margin;
  device->image.default_dac = image_defaults.default_dac;
  device->image.statistic = image_defaults.statistic;
  device->image.max_dac = image_defaults.max_dac;
  device->image.step = image_defaults.step;

  startup_checkpoint (observer, FTE3600_FT93XX_STARTUP_FDT_BASELINE, device);
  status = fte3600_ft93xx_fdt_run (chip, p->foundry, p->features, &p->state, &device->fdt,
                                   FTE3600_FT93XX_FDT_BASELINE, 10, NULL, 0, io, &d->fdt);
  accumulate_transport (&d->transport, &d->fdt.transport);
  if (status != FTE3600_FT93XX_OK)
    return status;

  startup_checkpoint (observer, FTE3600_FT93XX_STARTUP_IMAGE_BASELINE, device);
  struct fte3600_ft93xx_image_result result = {0};
  status = fte3600_ft93xx_image_cal_run (chip, raw, &p->state, &device->image,
                                         FTE3600_FT93XX_IMAGE_BASELINE, 1, pixels, capacity, &result, io, &d->image);
  accumulate_transport (&d->transport, &d->image.transport);
  d->transport.raw_bytes_completed = d->image.transport.raw_bytes_completed;
  if (status != FTE3600_FT93XX_OK)
    return status;

  startup_checkpoint (observer, FTE3600_FT93XX_STARTUP_AUTOMATIC_ARM, device);
  status = fte3600_ft93xx_fdt_run (chip, p->foundry, p->features, &p->state, &device->fdt,
                                   FTE3600_FT93XX_FDT_ARM, 1, NULL, 0, io, &d->arm);
  accumulate_transport (&d->transport, &d->arm.transport);
  if (status != FTE3600_FT93XX_OK)
    return status;
  device->final.startup_fdt_dac = device->fdt.analog.fdt_dac;
  device->final.saved_chip_id = chip;
  device->final.saved_width = p->geometry.width;
  device->final.saved_height = p->geometry.height;
  device->image.initialized = 0;
  device->fdt.dac_calibrated = 0;
  p->calibration_pending = 0;
  d->completed = 1;
  startup_checkpoint (observer, FTE3600_FT93XX_STARTUP_FINALIZE, device);
  return FTE3600_FT93XX_OK;
}

static uint32_t
recovery_spi (void *context, uint8_t *buffer, size_t length)
{
  const struct fte3600_recovery_io *io = context;
  uint8_t tx[FTE3600_FT93XX_MAX_TRANSFER];

  if (length > sizeof (tx))
    return UINT32_C (0x80000000);
  memcpy (tx, buffer, length);
  return io->spi (io->context, tx, buffer, length) == 0 ? 0 : UINT32_C (0x80000000);
}

static void
recovery_wait (void *context, uint32_t milliseconds)
{
  const struct fte3600_recovery_io *io = context;

  io->wait_ms (io->context, milliseconds);
}

enum fte3600_ft93xx_status
fte3600_ft93xx_io_from_recovery (
  const struct fte3600_recovery_io *recovery,
  struct fte3600_ft93xx_io         *output)
{
  if (recovery == NULL || recovery->spi == NULL || recovery->wait_ms == NULL ||
      output == NULL)
    return FTE3600_FT93XX_INVALID_ARGUMENT;
  output->context = (void *) recovery;
  output->spi = recovery_spi;
  output->wait_ms = recovery_wait;
  return FTE3600_FT93XX_OK;
}
