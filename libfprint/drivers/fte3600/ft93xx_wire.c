/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "ft93xx_wire.h"

#include <string.h>

uint16_t
ft93xx_crc16 (const uint8_t *bytes, size_t length)
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

void
ft93xx_wait_ms (const struct ft93xx_operation *op, uint32_t duration)
{
  op->io->wait_ms (op->io->context, duration);
}

static int
transfer (const struct ft93xx_operation *op, uint8_t *buffer, size_t length)
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
      ft93xx_wait_ms (op, 10);
    }
  if (op->diagnostics != NULL)
    ++op->diagnostics->exhausted_transactions;
  return 0;
}

int
ft93xx_command (const struct ft93xx_operation *op, uint8_t command_byte)
{
  uint8_t buffer[3] = {command_byte, (uint8_t) ~command_byte, 0};

  return transfer (op, buffer, sizeof (buffer));
}

int
ft93xx_read_byte (const struct ft93xx_operation *op, uint8_t address, uint8_t *value)
{
  uint8_t buffer[5] = {0x08, 0xf7, address, 0, 0};

  if (!transfer (op, buffer, sizeof (buffer)))
    return 0;
  *value = buffer[4];
  return 1;
}

int
ft93xx_write_byte (const struct ft93xx_operation *op, uint8_t address, uint8_t value)
{
  uint8_t buffer[4] = {0x09, 0xf6, address, value};

  return transfer (op, buffer, sizeof (buffer));
}

int
ft93xx_read_word (const struct ft93xx_operation *op, uint16_t address, uint16_t *value)
{
  uint8_t buffer[10] = {0x04, 0xfb, (uint8_t) (0x80 | (address >> 8)),
                        (uint8_t) address, 0, 0, 0, 0, 0, 0};

  if (!transfer (op, buffer, sizeof (buffer)))
    return 0;
  if (buffer[8] != 0 || buffer[9] != 0)
    {
      uint16_t crc = ft93xx_crc16 (buffer + 6, 2);
      if (crc != (uint16_t) ((uint16_t) buffer[8] << 8 | buffer[9]))
        {
          *op->error = FTE3600_FT93XX_BAD_CRC;
          return 0;
        }
    }
  *value = (uint16_t) ((uint16_t) buffer[6] << 8 | buffer[7]);
  return 1;
}

int
ft93xx_write_word (const struct ft93xx_operation *op, uint16_t address, uint16_t value)
{
  uint8_t buffer[8] = {0x05, 0xfa, (uint8_t) (0x80 | (address >> 8)),
                       (uint8_t) address, 0, 0,
                       (uint8_t) (value >> 8), (uint8_t) value};

  return transfer (op, buffer, sizeof (buffer));
}

int
ft93xx_read_fifo (const struct ft93xx_operation *op, uint8_t *payload, size_t length)
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

int
ft93xx_read_fdt_values (const struct ft93xx_operation *op, uint8_t address, uint16_t values[4])
{
  uint8_t buffer[16] = {0x04, 0xfb, 0x80, address, 0, 3};

  if (!transfer (op, buffer, sizeof (buffer)))
    return 0;
  uint16_t crc = (uint16_t) ((uint16_t) buffer[14] << 8 | buffer[15]);
  if (crc != 0 && crc != ft93xx_crc16 (buffer + 6, 8))
    {
      *op->error = FTE3600_FT93XX_BAD_CRC;
      return 0;
    }
  for (size_t i = 0; i < 4; ++i)
    values[i] = (uint16_t) ((uint16_t) buffer[6 + 2 * i] << 8 | buffer[7 + 2 * i]);
  return 1;
}

int
ft93xx_write_fdt_baseline (const struct ft93xx_operation *op, uint8_t address, const uint16_t values[8])
{
  uint8_t buffer[22] = {0x05, 0xfa, 0x80, address, 0, 7};

  for (size_t i = 0; i < 8; ++i)
    {
      buffer[6 + 2 * i] = (uint8_t) (values[i] >> 8);
      buffer[7 + 2 * i] = (uint8_t) values[i];
    }
  return transfer (op, buffer, sizeof (buffer));
}
