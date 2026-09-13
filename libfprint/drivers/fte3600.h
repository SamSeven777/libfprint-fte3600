/*
 * FocalTech FTE3600/FT9361 SPI fingerprint driver
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include <config.h>

#ifndef HAVE_UDEV
#error "fte3600 requires udev"
#endif

#ifndef HAVE_GPIOD
#error "fte3600 requires libgpiod"
#endif

#include <fp-device.h>
#include <fpi-device.h>

#define FTE3600_SPI_SPEED_HZ 1000000U

#define FT9361_IMAGE_WIDTH 64
#define FT9361_IMAGE_HEIGHT 80
#define FT9361_IMAGE_SIZE (FT9361_IMAGE_WIDTH * FT9361_IMAGE_HEIGHT)
#define FT9361_CAPTURE_FRAME_SIZE (FT9361_IMAGE_SIZE + 8)
#define FT9361_CAPTURE_DATA_OFFSET 8

#define FT9361_SENSOR_ID_HIGH 0x40
#define FT9361_SENSOR_ID_LOW 0x50
#define FT9361_FW_VERSION 0x30
#define FT9361_AGC_VERSION 0x31

#define FT9361_FIRMWARE_SIZE 10396
#define FT9361_FIRMWARE_PACKET_SIZE (6 + FT9361_FIRMWARE_SIZE + 1)
#define FT9361_FIRMWARE_SHA256 "027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f"

/* Internal firmware validation; shared with the hardware-independent tests. */
GBytes *fte3600_load_firmware (const gchar *path, GError **error);

#define FT9361_REG_SENSOR_ID_HIGH 0x14
#define FT9361_REG_SENSOR_ID_LOW 0x15
#define FT9361_REG_FW_VERSION 0x1a
#define FT9361_REG_FINGER_STATUS 0x1d
#define FT9361_REG_CAPTURE_START 0x1e
#define FT9361_REG_CAPTURE_ENABLE 0x1f
#define FT9361_REG_MCU_STATUS 0x20
#define FT9361_REG_CONFIG_22 0x22
#define FT9361_REG_CONFIG_23 0x23
#define FT9361_REG_CONFIG_MARKER 0x30
#define FT9361_REG_AGC_VERSION 0x3c
#define FT9361_REG_CONFIG_41 0x41
#define FT9361_REG_QUICK_TRIGGER 0x54
#define FT9361_REG_CAPTURE_MODE 0x76

static const FpIdEntry fte3600_id_table[] = {
  {
      .udev_types = FPI_DEVICE_UDEV_SUBTYPE_SPIDEV,
      .spi_acpi_id = "FTE3600",
  },
  { .udev_types = 0 },
};
