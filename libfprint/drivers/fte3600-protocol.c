/*
 * FocalTech FTE3600 SPI packet construction
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "fte3600-protocol.h"

#include <gio/gio.h>
#include <string.h>

static gboolean
check_output (guint8 *buffer, gsize capacity, gsize required, GError **error)
{
  if (!buffer)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "FTE3600 packet requires an output buffer");
      return FALSE;
    }

  if (capacity < required)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                   "FTE3600 packet requires %zu bytes; buffer has %zu",
                   required, capacity);
      return FALSE;
    }

  return TRUE;
}

static void
write_opcode (guint8 *buffer, Fte3600Opcode opcode)
{
  buffer[0] = opcode;
  buffer[1] = (guint8) ~opcode;
}

static void
write_be16 (guint8 *buffer, guint16 value)
{
  buffer[0] = value >> 8;
  buffer[1] = value & 0xff;
}

static void
write_memory_header (guint8 *buffer, Fte3600Opcode opcode,
                     guint16 address, guint16 length)
{
  write_opcode (buffer, opcode);
  write_be16 (buffer + 2, address);
  write_be16 (buffer + 4, length);
}

static gsize
build_register_read (guint8 *buffer, gsize capacity, Fte3600Opcode opcode,
                     guint8 reg, gsize result_len, GError **error)
{
  gsize length;

  if (!result_len)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "FTE3600 register read requires response bytes");
      return 0;
    }

  if (result_len > G_MAXSIZE - FTE3600_REG_READ_HEADER_SIZE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                           "FTE3600 register-read length overflows");
      return 0;
    }

  length = FTE3600_REG_READ_HEADER_SIZE + result_len;
  if (!check_output (buffer, capacity, length, error))
    return 0;

  memset (buffer, 0, length);
  write_opcode (buffer, opcode);
  buffer[2] = reg;
  return length;
}

static gsize
build_register_write (guint8 *buffer, gsize capacity, Fte3600Opcode opcode,
                      guint8 reg, guint8 value, GError **error)
{
  if (!check_output (buffer, capacity, FTE3600_REG_WRITE_SIZE, error))
    return 0;

  write_opcode (buffer, opcode);
  buffer[2] = reg;
  buffer[3] = value;
  buffer[4] = 0;
  return FTE3600_REG_WRITE_SIZE;
}

gsize
fpi_fte3600_build_app_read (guint8 *buffer, gsize capacity,
                            guint8 reg, gsize result_len, GError **error)
{
  return build_register_read (buffer, capacity, FTE3600_OPCODE_APP_READ,
                              reg, result_len, error);
}

gsize
fpi_fte3600_build_app_write (guint8 *buffer, gsize capacity,
                             guint8 reg, guint8 value, GError **error)
{
  return build_register_write (buffer, capacity, FTE3600_OPCODE_APP_WRITE,
                               reg, value, error);
}

gsize
fpi_fte3600_build_boot_read (guint8 *buffer, gsize capacity,
                             guint8 reg, gsize result_len, GError **error)
{
  return build_register_read (buffer, capacity, FTE3600_OPCODE_BOOT_READ,
                              reg, result_len, error);
}

gsize
fpi_fte3600_build_boot_write (guint8 *buffer, gsize capacity,
                              guint8 reg, guint8 value, GError **error)
{
  return build_register_write (buffer, capacity, FTE3600_OPCODE_BOOT_WRITE,
                               reg, value, error);
}

gsize
fpi_fte3600_build_image_read (guint8 *buffer, gsize capacity,
                              gsize pixel_count, GError **error)
{
  gsize length;

  if (!pixel_count)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "FTE3600 image requires at least one pixel");
      return 0;
    }

  if (pixel_count > G_MAXUINT16 - FTE3600_IMAGE_DATA_OFFSET)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                           "FTE3600 image frame exceeds its 16-bit wire length");
      return 0;
    }

  length = pixel_count + FTE3600_IMAGE_DATA_OFFSET;
  if (!check_output (buffer, capacity, length, error))
    return 0;

  memset (buffer, 0, length);
  write_memory_header (buffer, FTE3600_OPCODE_MEMORY_READ,
                       FTE3600_IMAGE_ADDRESS, length);
  return length;
}

gsize
fpi_fte3600_build_firmware (guint8 *buffer, gsize capacity,
                            const guint8 *payload, gsize payload_len,
                            GError **error)
{
  gsize length;

  if (!payload || !payload_len)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "FTE3600 firmware requires a nonempty payload");
      return 0;
    }

  if (payload_len > G_MAXUINT16)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                           "FTE3600 firmware exceeds its 16-bit wire length");
      return 0;
    }

  length = FTE3600_FIRMWARE_HEADER_SIZE + payload_len + FTE3600_FIRMWARE_TRAILER_SIZE;
  if (!check_output (buffer, capacity, length, error))
    return 0;

  /* Move the payload before writing the header, allowing in-place wrapping. */
  memmove (buffer + FTE3600_FIRMWARE_HEADER_SIZE, payload, payload_len);
  write_memory_header (buffer, FTE3600_OPCODE_MEMORY_WRITE,
                       FTE3600_FIRMWARE_ADDRESS, payload_len);
  buffer[length - 1] = 0;
  return length;
}

gsize
fpi_fte3600_build_command (guint8 *buffer, gsize capacity,
                           Fte3600Command command, GError **error)
{
  guint8 packet[FTE3600_COMMAND_MAX_SIZE] = { 0 };
  gsize length;

  switch (command)
    {
    case FTE3600_COMMAND_SOFT_RESET:
      packet[0] = FTE3600_OPCODE_SOFT_RESET;
      length = FTE3600_SOFT_RESET_SIZE;
      break;

    case FTE3600_COMMAND_BOOT_PROBE:
      packet[0] = FTE3600_OPCODE_BOOT_PROBE;
      length = FTE3600_BOOT_PROBE_SIZE;
      break;

    case FTE3600_COMMAND_BOOT_ENTER:
      write_opcode (packet, FTE3600_OPCODE_BOOT_ENTER);
      length = FTE3600_BOOT_ENTER_SIZE;
      break;

    case FTE3600_COMMAND_BOOT_SYNC:
      write_opcode (packet, FTE3600_OPCODE_BOOT_SYNC);
      length = FTE3600_BOOT_SYNC_SIZE;
      break;

    case FTE3600_COMMAND_FAMILY_QUERY:
      write_memory_header (packet, FTE3600_OPCODE_MEMORY_WRITE,
                           FTE3600_FAMILY_QUERY_ADDRESS, 4);
      write_opcode (packet + 6, FTE3600_OPCODE_APP_WRITE);
      packet[8] = 2;
      length = FTE3600_FAMILY_QUERY_SIZE;
      break;

    case FTE3600_COMMAND_FAMILY_TRIGGER:
      write_opcode (packet, FTE3600_OPCODE_BOOT_WRITE);
      packet[2] = FTE3600_BOOT_REG_QUERY_TRIGGER;
      packet[3] = 1;
      length = FTE3600_FAMILY_TRIGGER_SIZE;
      break;

    case FTE3600_COMMAND_FAMILY_READ:
      write_memory_header (packet, FTE3600_OPCODE_MEMORY_READ,
                           FTE3600_FAMILY_QUERY_ADDRESS, 0);
      length = FTE3600_FAMILY_READ_SIZE;
      break;

    default:
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Unknown FTE3600 protocol command");
      return 0;
    }

  if (!check_output (buffer, capacity, length, error))
    return 0;

  memcpy (buffer, packet, length);
  return length;
}
