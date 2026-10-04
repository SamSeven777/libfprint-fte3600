/*
 * FocalTech FTE3600 SPI wire protocol
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

/* Register names describe observed use; they do not imply that an arbitrary
 * sensor or firmware implements this register map. */
typedef enum {
  FTE3600_REG_CONFIG_01        = 0x01,
  FTE3600_REG_SENSOR_ID_HIGH   = 0x14,
  FTE3600_REG_SENSOR_ID_LOW    = 0x15,
  FTE3600_REG_FW_VERSION       = 0x1a,
  FTE3600_REG_FINGER_STATUS    = 0x1d,
  FTE3600_REG_START            = 0x1e,
  FTE3600_REG_ENABLE           = 0x1f,
  FTE3600_REG_MCU_STATUS       = 0x20,
  FTE3600_REG_CONFIG_22        = 0x22,
  FTE3600_REG_CONFIG_23        = 0x23,
  FTE3600_REG_CONFIG_MARKER    = 0x30,
  FTE3600_REG_AGC_VERSION      = 0x3c,
  FTE3600_REG_CONFIG_41        = 0x41,
  FTE3600_REG_MODE_FT9536      = 0x47,
  FTE3600_REG_QUICK_TRIGGER    = 0x54,
  FTE3600_REG_MODE             = 0x76,
} Fte3600AppRegister;

typedef enum {
  FTE3600_BOOT_REG_QUERY_TRIGGER = 0xa4,
  FTE3600_BOOT_REG_OTP_CONFIG    = 0xc8,
  FTE3600_BOOT_REG_OTP_ADDRESS   = 0xf1,
  FTE3600_BOOT_REG_OTP_DATA      = 0xf3,
  FTE3600_BOOT_REG_OTP_CONTROL   = 0xf4,
} Fte3600BootRegister;

typedef enum {
  FTE3600_OPCODE_MEMORY_READ  = 0x04,
  FTE3600_OPCODE_MEMORY_WRITE = 0x05,
  FTE3600_OPCODE_BOOT_ENTER   = 0x06,
  FTE3600_OPCODE_BOOT_READ    = 0x08,
  FTE3600_OPCODE_BOOT_WRITE   = 0x09,
  FTE3600_OPCODE_APP_READ     = 0x10,
  FTE3600_OPCODE_APP_WRITE    = 0x11,
  FTE3600_OPCODE_BOOT_SYNC    = 0x55,
  FTE3600_OPCODE_SOFT_RESET   = 0x70,
  FTE3600_OPCODE_BOOT_PROBE   = 0x90,
} Fte3600Opcode;

typedef enum {
  FTE3600_COMMAND_SOFT_RESET,
  FTE3600_COMMAND_BOOT_PROBE,
  FTE3600_COMMAND_BOOT_ENTER,
  FTE3600_COMMAND_BOOT_SYNC,
  FTE3600_COMMAND_FAMILY_QUERY,
  FTE3600_COMMAND_FAMILY_TRIGGER,
  FTE3600_COMMAND_FAMILY_READ,
} Fte3600Command;

#define FTE3600_REG_READ_HEADER_SIZE 4u
#define FTE3600_REG_WRITE_SIZE 5u
#define FTE3600_REG_RESULT_OFFSET 4u
#define FTE3600_SMALL_FRAME_SIZE (FTE3600_REG_READ_HEADER_SIZE + 2u)
#define FTE3600_IMAGE_HEADER_SIZE 6u
#define FTE3600_IMAGE_DATA_OFFSET 8u
#define FTE3600_FIRMWARE_HEADER_SIZE 6u
#define FTE3600_FIRMWARE_TRAILER_SIZE 1u
#define FTE3600_SOFT_RESET_SIZE 1u
#define FTE3600_BOOT_PROBE_SIZE 3u
#define FTE3600_BOOT_PROBE_RESULT_OFFSET 2u
#define FTE3600_BOOT_ENTER_SIZE 3u
#define FTE3600_BOOT_SYNC_SIZE 2u
#define FTE3600_FAMILY_QUERY_SIZE 11u
#define FTE3600_FAMILY_TRIGGER_SIZE 4u
#define FTE3600_FAMILY_READ_SIZE 8u
#define FTE3600_FAMILY_RESULT_OFFSET 6u
#define FTE3600_COMMAND_MAX_SIZE FTE3600_FAMILY_QUERY_SIZE

#define FTE3600_IMAGE_ADDRESS 0x3400u
#define FTE3600_FIRMWARE_ADDRESS 0x0000u
#define FTE3600_FAMILY_QUERY_ADDRESS 0x85c0u

#define FTE3600_MCU_IDLE_HIGH 0xa5u
#define FTE3600_MCU_IDLE_LOW 0x5au
#define FTE3600_FINGER_PRESENT 0x01u
#define FTE3600_FINGER_PRESENT_ALT 0xa0u
#define FTE3600_CONFIGURED_MARKER 0xbbu
#define FTE3600_CONFIG_01_ENABLE 0x01u
#define FTE3600_CONFIG_41_VALUE 0x0fu
#define FTE3600_CONFIG_22_VALUE 0x00u
#define FTE3600_CONFIG_23_VALUE 0x0eu
#define FTE3600_CAPTURE_ENABLE 0x01u
#define FTE3600_CAPTURE_DISABLE 0x00u
#define FTE3600_QUICK_TRIGGER 0x01u
#define FTE3600_A8_FW_VERSION 0x30u
#define FTE3600_A8_AGC_VERSION 0x31u
#define FTE3600_FT9338_FW_VERSION 0x40u
#define FTE3600_FT9338_AGC_VERSION 0x10u
#define FTE3600_FT9536_FW_VERSION 0x23u
#define FTE3600_FT9536_AGC_VERSION 0x13u
#define FTE3600_BOOT_A_MARKER 0xefu
#define FTE3600_MODE_WAIT_FINGER 0x01u
#define FTE3600_MODE_QUICK_CAPTURE 0x02u
/* Return-idle skips capture-stop writes in modes 2, 3, and 4. The latter two
 * modes are not selected by this implementation. */
#define FTE3600_MODE_3 0x03u
#define FTE3600_MODE_4 0x04u
#define FTE3600_BOOT_OTP_CONFIG 0xdfu
#define FTE3600_BOOT_OTP_ADDRESS 0x1du
#define FTE3600_BOOT_OTP_ENABLE 0x01u

/* Builders perform no I/O, packet allocation, identity selection, or authorization.
 * The caller must select an established protocol and provide a live buffer of
 * at least capacity bytes. Return the complete TX size, or zero with a G_IO_ERROR
 * on invalid arguments, unrepresentable lengths, or insufficient capacity.
 * A failure leaves the output unchanged. All transmitted dummy bytes are zero.
 * GError may be NULL. The firmware builder permits overlapping input/output.
 */
gsize fpi_fte3600_build_app_read (guint8  *buffer,
                                  gsize    capacity,
                                  guint8   reg,
                                  gsize    result_len,
                                  GError **error);
gsize fpi_fte3600_build_app_write (guint8  *buffer,
                                   gsize    capacity,
                                   guint8   reg,
                                   guint8   value,
                                   GError **error);
gsize fpi_fte3600_build_boot_read (guint8  *buffer,
                                   gsize    capacity,
                                   guint8   reg,
                                   gsize    result_len,
                                   GError **error);
gsize fpi_fte3600_build_boot_write (guint8  *buffer,
                                    gsize    capacity,
                                    guint8   reg,
                                    guint8   value,
                                    GError **error);

/* Image requests encode the complete frame size (pixel_count + 8), not the
 * pixel count or the internal data length (pixel_count + 2). Pixels begin at
 * RX offset 8. Firmware requests instead encode only their payload length. */
gsize fpi_fte3600_build_image_read (guint8  *buffer,
                                    gsize    capacity,
                                    gsize    pixel_count,
                                    GError **error);
gsize fpi_fte3600_build_firmware (guint8       *buffer,
                                  gsize         capacity,
                                  const guint8 *payload,
                                  gsize         payload_len,
                                  GError      **error);

/* The family mailbox trigger is exactly four bytes, without the trailing
 * dummy byte of a normal boot-register write. */
gsize fpi_fte3600_build_command (guint8        *buffer,
                                 gsize          capacity,
                                 Fte3600Command command,
                                 GError       **error);
