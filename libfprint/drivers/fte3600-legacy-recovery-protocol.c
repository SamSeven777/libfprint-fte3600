/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600-legacy-recovery-protocol.h"
#include <string.h>

gsize
fpi_fte3600_build_boot38_read (guint8 *out, gsize capacity, guint8 reg)
{
  if (!out || capacity < FTE3600_BOOT38_REGISTER_SIZE)
    return 0;
  out[0] = 0x08;
  out[1] = 0xf7;
  out[2] = reg;
  out[3] = 0;
  return FTE3600_BOOT38_REGISTER_SIZE;
}

gsize
fpi_fte3600_build_boot38_write (guint8 *out, gsize capacity,
                                guint8 reg, guint8 value)
{
  if (!out || capacity < FTE3600_BOOT38_REGISTER_SIZE)
    return 0;
  out[0] = 0x09;
  out[1] = 0xf6;
  out[2] = reg;
  out[3] = value;
  return FTE3600_BOOT38_REGISTER_SIZE;
}

gsize
fpi_fte3600_build_boot38_readback (guint8 *out, gsize capacity,
                                   gsize firmware_size)
{
  gsize size;

  if (!out || !firmware_size ||
      firmware_size > G_MAXUINT16 - FTE3600_BOOT38_READBACK_OVERHEAD)
    return 0;
  size = firmware_size + FTE3600_BOOT38_READBACK_OVERHEAD;
  if (capacity < size)
    return 0;
  memset (out, 0, size);
  out[0] = 0x04;
  out[1] = 0xfb;
  out[4] = size >> 8;
  out[5] = size;
  return size;
}
