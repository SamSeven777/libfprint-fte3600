/*
 * Independent FT9365/FT9769 hardware protocol definitions
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "fte3600-ft93xx-protocol.h"
#include <string.h>

static const Fte3600Ft93xxProfile ft9365 = {
  .sensor = FTE3600_SENSOR_FT9365,
  .width = 64, .height = 80,
  .initial_dac = 72, .maximum_dac = 127, .gain = 7,
  .scan_window = 0x4ffe,
};
static const Fte3600Ft93xxProfile ft9769 = {
  .sensor = FTE3600_SENSOR_FT9769,
  .width = 40, .height = 196, .extra_rows = 4,
  .initial_dac = 124, .maximum_dac = 255, .gain = 5,
  .scan_window = 0xc7fe, .scan_extension = 0x7fff,
};

const Fte3600Ft93xxProfile *
fpi_fte3600_ft93xx_profile (Fte3600Sensor sensor)
{
  switch (sensor)
    {
    case FTE3600_SENSOR_FT9365: return &ft9365;

    case FTE3600_SENSOR_FT9769: return &ft9769;

    case FTE3600_SENSOR_UNKNOWN:
    case FTE3600_SENSOR_FT9338:
    case FTE3600_SENSOR_FT9348:
    case FTE3600_SENSOR_FT9361:
    case FTE3600_SENSOR_FT9536:
    case FTE3600_SENSOR_FT9368:
    case FTE3600_SENSOR_FT9369:
    case FTE3600_SENSOR_COUNT:
    default: return NULL;
    }
}

static gboolean
check_buffer (guint8 *buffer, gsize capacity, gsize required, GError **error)
{
  if (!buffer || capacity < required)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "FT93xx command buffer is too small");
      return FALSE;
    }
  return TRUE;
}

static void
encode_address (guint8 *buffer, guint16 address)
{
  buffer[2] = (address >> 8) | 0x80;
  buffer[3] = address;
}

gsize
fpi_fte3600_ft93xx_read16 (guint8 *buffer, gsize capacity,
                           guint16 address, GError **error)
{
  if (!check_buffer (buffer, capacity, FT93XX_REGISTER_READ_SIZE, error))
    return 0;
  memset (buffer, 0, FT93XX_REGISTER_READ_SIZE);
  buffer[0] = 0x04;
  buffer[1] = 0xfb;
  encode_address (buffer, address);
  return FT93XX_REGISTER_READ_SIZE;
}

guint16
fpi_fte3600_ft93xx_crc16 (const guint8 *data, gsize size)
{
  guint16 crc = 0xffff;

  for (gsize i = 0; i < size; i++)
    {
      crc ^= (guint16) data[i] << 8;
      for (guint bit = 0; bit < 8; bit++)
        crc = (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0);
    }
  return crc;
}

gboolean
fpi_fte3600_ft93xx_read16_result (const guint8 *response, gsize size,
                                  guint16 *value, GError **error)
{
  guint16 trailer;

  if (!response || !value || size != FT93XX_REGISTER_READ_SIZE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Invalid FT93xx register response length");
      return FALSE;
    }
  trailer = ((guint16) response[8] << 8) | response[9];
  if (trailer && trailer != fpi_fte3600_ft93xx_crc16 (response + 6, 2))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Invalid FT93xx register checksum");
      return FALSE;
    }
  *value = ((guint16) response[6] << 8) | response[7];
  return TRUE;
}

gsize
fpi_fte3600_ft93xx_write16 (guint8 *buffer, gsize capacity,
                            guint16 address, guint16 value, GError **error)
{
  if (!check_buffer (buffer, capacity, FT93XX_REGISTER_WRITE_SIZE, error))
    return 0;
  memset (buffer, 0, FT93XX_REGISTER_WRITE_SIZE);
  buffer[0] = 0x05;
  buffer[1] = 0xfa;
  encode_address (buffer, address);
  buffer[6] = value >> 8;
  buffer[7] = value;
  return FT93XX_REGISTER_WRITE_SIZE;
}

gsize
fpi_fte3600_ft93xx_read8 (guint8 *buffer, gsize capacity,
                          guint8 address, GError **error)
{
  if (!check_buffer (buffer, capacity, 5, error))
    return 0;
  memset (buffer, 0, 5);
  buffer[0] = 0x08;
  buffer[1] = 0xf7;
  buffer[2] = address;
  return 5;
}

gsize
fpi_fte3600_ft93xx_write8 (guint8 *buffer, gsize capacity,
                           guint8 address, guint8 value, GError **error)
{
  if (!check_buffer (buffer, capacity, 4, error))
    return 0;
  buffer[0] = 0x09;
  buffer[1] = 0xf6;
  buffer[2] = address;
  buffer[3] = value;
  return 4;
}

gsize
fpi_fte3600_ft93xx_command (guint8 *buffer, gsize capacity,
                            Fte3600Ft93xxCommand command, GError **error)
{
  guint8 opcode;

  switch (command)
    {
    case FT93XX_COMMAND_IDLE: opcode = 0xc0;
      break;

    case FT93XX_COMMAND_SCAN_IMAGE: opcode = 0xc4;
      break;

    case FT93XX_COMMAND_WAKE: opcode = 0x5a;
      break;

    case FT93XX_COMMAND_RELEASE: opcode = 0xa5;
      break;

    default:
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Unknown FT93xx command");
      return 0;
    }
  if (!check_buffer (buffer, capacity, 3, error))
    return 0;
  buffer[0] = opcode;
  buffer[1] = opcode ^ 0xff;
  buffer[2] = 0;
  return 3;
}

gsize
fpi_fte3600_ft93xx_fifo_read (guint8 *buffer, gsize capacity,
                              gsize payload_size, GError **error)
{
  guint16 words;

  if (!payload_size || payload_size > FT93XX_FIFO_PAYLOAD_MAX || payload_size % 2)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "FT93xx FIFO payload must contain 1 to 895 words");
      return 0;
    }
  if (!check_buffer (buffer, capacity, payload_size + FT93XX_FIFO_OVERHEAD, error))
    return 0;
  memset (buffer, 0, payload_size + FT93XX_FIFO_OVERHEAD);
  buffer[0] = 0x06;
  buffer[1] = 0xf9;
  encode_address (buffer, FT93XX_REG_FIFO);
  words = payload_size / 2 - 1;
  buffer[4] = words >> 8;
  buffer[5] = words;
  return payload_size + FT93XX_FIFO_OVERHEAD;
}

gboolean
fpi_fte3600_ft93xx_matches (Fte3600Sensor sensor, guint16 chip_id, guint16 variant)
{
  if (sensor == FTE3600_SENSOR_FT9365)
    return chip_id == 0x9365;
  if (sensor == FTE3600_SENSOR_FT9769)
    return chip_id == 0x9392 || (chip_id == 0x9391 && variant != 0x0fff);
  return FALSE;
}

gboolean
fpi_fte3600_ft93xx_decode (const Fte3600Ft93xxProfile *profile,
                           guint16 chip_id, const guint8 *raw, gsize raw_size,
                           guint16 *pixels, gsize pixel_count, GError **error)
{
  const Fte3600Ft93xxProfile *known = profile ? fpi_fte3600_ft93xx_profile (profile->sensor) : NULL;
  gsize count;
  gsize required;

  if (!known || profile != known || !raw || !pixels ||
      !fpi_fte3600_ft93xx_matches (profile->sensor, chip_id, 0))
    goto invalid;
  count = (gsize) profile->width * profile->height;
  required = 2 * (gsize) profile->width * (profile->height + profile->extra_rows);
  if (raw_size != required || pixel_count != count)
    goto invalid;
  for (gsize i = 0; i < count; i++)
    {
      gsize source = chip_id == 0x9392 ? (i ^ 3) : i;
      pixels[i] = (((guint16) raw[source * 2] << 8) | raw[source * 2 + 1]) & 0x0ffc;
    }
  return TRUE;

invalid:
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       "FT93xx pixel identity, geometry or buffer size mismatch");
  return FALSE;
}
