/*
 * Independently constructed FW9369 SPI packets
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "fte3600-fw9369-protocol.h"

#include <gio/gio.h>
#include <string.h>

static gboolean
check_output (guint8 *out, gsize capacity, gsize length, GError **error)
{
  if (!out || capacity < length)
    {
      g_set_error_literal (error, G_IO_ERROR,
                           out ? G_IO_ERROR_NO_SPACE : G_IO_ERROR_INVALID_ARGUMENT,
                           "FW9369 packet output buffer is missing or too small");
      return FALSE;
    }
  return TRUE;
}

static gboolean
check_address (guint16 address, GError **error)
{
  if (address > 0x7fff)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "FW9369 word address exceeds 15 bits");
      return FALSE;
    }
  return TRUE;
}

static void
write_header (guint8 *out, guint8 command, guint16 address, guint16 words)
{
  out[0] = command;
  out[1] = (guint8) ~command;
  out[2] = (address >> 8) | 0x80;
  out[3] = address & 0xff;
  out[4] = words >> 8;
  out[5] = words & 0xff;
}

gsize
fpi_fte3600_fw9369_build_sfr_read (guint8 *out, gsize capacity,
                                  guint8 reg, GError **error)
{
  if (!check_output (out, capacity, FTE3600_FW9369_SFR_READ_SIZE, error))
    return 0;
  out[0] = 0x08;
  out[1] = 0xf7;
  out[2] = reg;
  out[3] = 0;
  out[4] = 0;
  return FTE3600_FW9369_SFR_READ_SIZE;
}

gsize
fpi_fte3600_fw9369_build_sfr_write (guint8 *out, gsize capacity,
                                   guint8 reg, guint8 value, GError **error)
{
  if (!check_output (out, capacity, FTE3600_FW9369_SFR_WRITE_SIZE, error))
    return 0;
  out[0] = 0x09;
  out[1] = 0xf6;
  out[2] = reg;
  out[3] = value;
  return FTE3600_FW9369_SFR_WRITE_SIZE;
}

gsize
fpi_fte3600_fw9369_build_word_read (guint8 *out, gsize capacity,
                                   guint16 address, GError **error)
{
  if (!check_address (address, error) ||
      !check_output (out, capacity, FTE3600_FW9369_WORD_READ_SIZE, error))
    return 0;
  memset (out, 0, FTE3600_FW9369_WORD_READ_SIZE);
  write_header (out, 0x04, address, 1);
  return FTE3600_FW9369_WORD_READ_SIZE;
}

gsize
fpi_fte3600_fw9369_build_word_write (guint8 *out, gsize capacity,
                                    guint16 address, guint16 value, GError **error)
{
  if (!check_address (address, error) ||
      !check_output (out, capacity, FTE3600_FW9369_WORD_WRITE_SIZE, error))
    return 0;
  write_header (out, 0x05, address, 1);
  out[6] = value >> 8;
  out[7] = value & 0xff;
  return FTE3600_FW9369_WORD_WRITE_SIZE;
}

gsize
fpi_fte3600_fw9369_build_command (guint8 *out, gsize capacity,
                                 Fte3600Fw9369Command command, GError **error)
{
  switch (command)
    {
    case FTE3600_FW9369_CMD_IDLE_1:
    case FTE3600_FW9369_CMD_IDLE_2:
    case FTE3600_FW9369_CMD_FDT:
    case FTE3600_FW9369_CMD_IMAGE:
    case FTE3600_FW9369_CMD_WAKE:
    case FTE3600_FW9369_CMD_WAKE_END:
      break;
    default:
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Unestablished FW9369 command");
      return 0;
    }
  if (!check_output (out, capacity, FTE3600_FW9369_COMMAND_SIZE, error))
    return 0;
  out[0] = command;
  out[1] = (guint8) ~command;
  out[2] = 0;
  return FTE3600_FW9369_COMMAND_SIZE;
}

gsize
fpi_fte3600_fw9369_build_image_read (guint8 *out, gsize capacity, GError **error)
{
  if (!check_output (out, capacity, FTE3600_FW9369_FRAME_SIZE, error))
    return 0;
  memset (out, 0, FTE3600_FW9369_FRAME_SIZE);
  write_header (out, 0x06, FTE3600_FW9369_WORD_FIFO, FTE3600_FW9369_PIXELS);
  return FTE3600_FW9369_FRAME_SIZE;
}

gboolean
fpi_fte3600_fw9369_decode_frame (const guint8 *frame, gsize frame_len,
                                guint16 *pixels, gsize pixel_capacity,
                                GError **error)
{
  if (!frame || !pixels || frame_len != FTE3600_FW9369_FRAME_SIZE ||
      pixel_capacity < FTE3600_FW9369_PIXELS)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "FW9369 requires one complete 64 by 80, 16-bit frame");
      return FALSE;
    }
  for (gsize i = 0; i < FTE3600_FW9369_PIXELS; i++)
    {
      gsize offset = FTE3600_FW9369_IMAGE_OFFSET + 2 * i;
      pixels[i] = ((guint16) frame[offset] << 8) | frame[offset + 1];
    }
  return TRUE;
}

gsize
fpi_fte3600_fw9369_build_fdt_read (guint8 *out, gsize capacity,
                                  gboolean smic, GError **error)
{
  if (!check_output (out, capacity, FTE3600_FW9369_FDT_READ_SIZE, error))
    return 0;
  memset (out, 0, FTE3600_FW9369_FDT_READ_SIZE);
  write_header (out, 0x04, smic ? 0x00e8 : 0x00b8, 4);
  return FTE3600_FW9369_FDT_READ_SIZE;
}

gsize
fpi_fte3600_fw9369_build_fdt_base (guint8 *out, gsize capacity,
                                  gboolean smic, const guint16 *base,
                                  gsize channels, GError **error)
{
  if (!base || channels != FTE3600_FW9369_FDT_CHANNELS)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "FW9369 requires four FDT baseline channels");
      return 0;
    }
  if (!check_output (out, capacity, FTE3600_FW9369_FDT_WRITE_SIZE, error))
    return 0;
  memset (out, 0, FTE3600_FW9369_FDT_WRITE_SIZE);
  write_header (out, 0x05, smic ? 0x00e0 : 0x00b0, 8);
  for (gsize i = 0; i < channels; i++)
    {
      out[6 + 2 * i] = base[i] >> 8;
      out[7 + 2 * i] = base[i] & 0xff;
    }
  return FTE3600_FW9369_FDT_WRITE_SIZE;
}

guint
fpi_fte3600_fw9369_image_median (const guint16 *raw)
{
  guint histogram[1024] = { 0 };
  guint count = 0;
  guint cumulative = 0;

  /* The outer columns and rows are not used to tune the ADC. */
  for (guint y = 1; y < FTE3600_FW9369_HEIGHT - 1; y++)
    for (guint x = 2; x < FTE3600_FW9369_WIDTH - 2; x++)
      {
        histogram[MIN (raw[y * FTE3600_FW9369_WIDTH + x] / 4, 1023)]++;
        count++;
      }
  for (guint value = 0; value < G_N_ELEMENTS (histogram); value++)
    {
      cumulative += histogram[value];
      if (cumulative > count / 2)
        return value;
    }
  return 1023;
}

gboolean
fpi_fte3600_fw9369_make_image (const guint16 *base, const guint16 *raw,
                               gsize pixels, guint8 *out, gsize capacity,
                               GError **error)
{
  guint histogram[4096] = { 0 };
  guint cumulative = 0;
  guint high = 0;
  guint active = 0;

  if (!base || !raw || !out || pixels != FTE3600_FW9369_PIXELS || capacity < pixels)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "FW9369 image buffers have invalid dimensions");
      return FALSE;
    }
  for (gsize i = 0; i < pixels; i++)
    {
      guint delta = base[i] > raw[i] ? base[i] - raw[i] : 0;
      histogram[MIN (delta, 4095)]++;
      active += delta >= 16;
    }
  for (high = 0; high < G_N_ELEMENTS (histogram) - 1; high++)
    {
      cumulative += histogram[high];
      if (cumulative >= pixels * 99 / 100)
        break;
    }
  /* Do not turn an empty/noisy frame into high-contrast fingerprint data. */
  if (high < 16 || active < pixels / 20)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "FW9369 frame has insufficient fingerprint contrast");
      return FALSE;
    }
  for (gsize i = 0; i < pixels; i++)
    {
      guint delta = base[i] > raw[i] ? base[i] - raw[i] : 0;
      out[i] = 255 - MIN (delta, high) * 255 / high;
    }
  /* Boundary samples are not reliable imaging pixels. Extend the closest
   * interior sample; no synthetic ridge detail is introduced. */
  for (guint y = 0; y < FTE3600_FW9369_HEIGHT; y++)
    {
      guint row = y * FTE3600_FW9369_WIDTH;
      out[row] = out[row + 1] = out[row + 2];
      out[row + 63] = out[row + 62] = out[row + 61];
    }
  memcpy (out, out + FTE3600_FW9369_WIDTH, FTE3600_FW9369_WIDTH);
  memcpy (out + (FTE3600_FW9369_HEIGHT - 1) * FTE3600_FW9369_WIDTH,
          out + (FTE3600_FW9369_HEIGHT - 2) * FTE3600_FW9369_WIDTH,
          FTE3600_FW9369_WIDTH);
  return TRUE;
}
