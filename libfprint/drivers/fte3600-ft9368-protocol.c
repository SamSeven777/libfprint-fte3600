/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600-ft9368-protocol.h"

gboolean
fpi_fte3600_ft9368_wake_ready (const guint8 *data, gsize length)
{
  if (!data || length != FTE3600_FT9368_WAKE_CHECK_SIZE)
    return FALSE;
  return data[0] == 0 || data[0] != data[1] ||
         data[0] != data[2] || data[0] != data[3];
}

#include <string.h>

static void
put_be16 (guint8 *out, guint16 value)
{
  out[0] = value >> 8;
  out[1] = value;
}

gsize
fpi_fte3600_ft9368_read (guint8 *out, gsize capacity,
                         guint16 command, gsize length)
{
  gsize size;

  if (!out || length > G_MAXUINT16)
    return 0;
  size = length ? length + FTE3600_FT9368_HEADER : 4;
  if (capacity < size)
    return 0;
  memset (out, 0, size);
  put_be16 (out, command);
  put_be16 (out + 2, length);
  return size;
}

gsize
fpi_fte3600_ft9368_write (guint8 *out, gsize capacity,
                          guint8 command, const guint8 *data, gsize length)
{
  if (!out || !data || !length || length > FTE3600_FT9368_APP_CHUNK ||
      capacity < length + FTE3600_FT9368_HEADER)
    return 0;
  memset (out, 0, FTE3600_FT9368_HEADER);
  out[0] = command;
  put_be16 (out + 2, length);
  memcpy (out + FTE3600_FT9368_HEADER, data, length);
  return length + FTE3600_FT9368_HEADER;
}

gsize
fpi_fte3600_ft9368_mode (guint8 *out, gsize capacity, guint8 mode)
{
  if (!out || capacity < 3)
    return 0;
  out[0] = 0x70;
  out[1] = mode;
  out[2] = ~mode;
  return 3;
}

gsize
fpi_fte3600_ft9368_sfr (guint8 *out, gsize capacity,
                        guint16 address, guint16 value)
{
  if (!out || capacity < 11)
    return 0;
  memset (out, 0, 11);
  out[0] = 0x70;
  out[1] = 7;
  out[2] = 0xf8;
  put_be16 (out + 3, address);
  put_be16 (out + 7, value);
  return 11;
}

static gboolean
pram_range_valid (gsize offset, gsize length, gsize maximum)
{
  return length > 0 && length <= maximum && !(offset % 4) && !(length % 4) &&
         offset < FTE3600_FT9368_PRAM_SIZE &&
         length <= FTE3600_FT9368_PRAM_SIZE - offset;
}

gsize
fpi_fte3600_ft9368_pram_write (guint8 *out, gsize capacity, gsize offset,
                               const guint8 *data, gsize length)
{
  if (!out || !data || !pram_range_valid (offset, length, FTE3600_FT9368_PRAM_CHUNK) ||
      capacity < length + 7)
    return 0;
  out[0] = 0x70;
  out[1] = 5;
  out[2] = 0xfa;
  put_be16 (out + 3, 0x2000 + offset / 4);
  put_be16 (out + 5, length / 4 - 1);
  memcpy (out + 7, data, length);
  return length + 7;
}

gsize
fpi_fte3600_ft9368_pram_select (guint8 *out, gsize capacity,
                                gsize offset, gsize length)
{
  if (!out || !pram_range_valid (offset, length, 256) || capacity < 7)
    return 0;
  out[0] = 0x70;
  out[1] = 4;
  out[2] = 0xfb;
  put_be16 (out + 3, 0x2000 + offset / 4);
  put_be16 (out + 5, length / 4 - 1);
  return 7;
}

gboolean
fpi_fte3600_ft9368_parse_info (const guint8 *data, gsize length,
                               Fte3600Ft9368Info *info)
{
  if (!data || !info || length != FTE3600_FT9368_INFO_SIZE ||
      data[19] != 0x93 || data[20] != 0x68 ||
      data[23] != FTE3600_FT9368_WIDTH || data[24] != FTE3600_FT9368_HEIGHT ||
      (data[0] == 0xff && data[1] == 0xff) || data[2] == 0x22)
    return FALSE;
  *info = (Fte3600Ft9368Info){
    .version = data[21], .manufacturer = data[22],
    .width = data[23], .height = data[24],
    .finger_present = data[1] == 0x11 && data[2] == 0x11,
  };
  return TRUE;
}

gboolean
fpi_fte3600_ft9368_checksum (const guint8 *data, gsize length, guint16 *checksum)
{
  guint16 value = 0;

  if (!data || !checksum || !length || (length & 1) ||
      length > FTE3600_FT9368_APP_SIZE)
    return FALSE;
  for (gsize i = 0; i < length; i += 2)
    {
      value ^= ((guint16) data[i] << 8) | data[i + 1];
      for (guint bit = 0; bit < 16; bit++)
        value = (value >> 1) ^ ((value & 1) ? 0x8408 : 0);
    }
  *checksum = value;
  return TRUE;
}

guint16
fpi_fte3600_ft9368_program_ack (gsize offset, gsize length)
{
  if (!length || length > FTE3600_FT9368_APP_CHUNK ||
      offset >= FTE3600_FT9368_APP_SIZE ||
      length > FTE3600_FT9368_APP_SIZE - offset || offset % 256)
    return 0;
  /* The observed final partial packet divides its offset by that packet's
   * length. Preserve this wire acknowledgment, not an assumed page index. */
  return 0x1000 + offset / length;
}
