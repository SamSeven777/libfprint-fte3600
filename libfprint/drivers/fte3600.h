/*
 * FocalTech FTE3600 SPI fingerprint driver integration
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

#include <fp-device.h>
#include <fpi-device.h>
#include "fte3600-protocol.h"

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

#include "fte3600-firmware.h"

#include "fte3600-sensor.h"

/* Compatibility names for FT9361 consumers. The protocol module owns the
 * register map; driver state machines use its family-wide names directly. */
#define FT9361_REG_SENSOR_ID_HIGH FTE3600_REG_SENSOR_ID_HIGH
#define FT9361_REG_SENSOR_ID_LOW FTE3600_REG_SENSOR_ID_LOW
#define FT9361_REG_FW_VERSION FTE3600_REG_FW_VERSION
#define FT9361_REG_FINGER_STATUS FTE3600_REG_FINGER_STATUS
#define FT9361_REG_CAPTURE_START FTE3600_REG_START
#define FT9361_REG_CAPTURE_ENABLE FTE3600_REG_ENABLE
#define FT9361_REG_MCU_STATUS FTE3600_REG_MCU_STATUS
#define FT9361_REG_CONFIG_22 FTE3600_REG_CONFIG_22
#define FT9361_REG_CONFIG_23 FTE3600_REG_CONFIG_23
#define FT9361_REG_CONFIG_MARKER FTE3600_REG_CONFIG_MARKER
#define FT9361_REG_AGC_VERSION FTE3600_REG_AGC_VERSION
#define FT9361_REG_CONFIG_41 FTE3600_REG_CONFIG_41
#define FT9361_REG_QUICK_TRIGGER FTE3600_REG_QUICK_TRIGGER
#define FT9361_REG_CAPTURE_MODE FTE3600_REG_MODE

static const FpIdEntry fte3600_id_table[] = {
  {
    .udev_types = FPI_DEVICE_UDEV_SUBTYPE_FTE3600 | FPI_DEVICE_UDEV_SUBTYPE_GPIO |
                  FPI_DEVICE_UDEV_SUBTYPE_UIO,
    .spi_acpi_id = "FTE3600",
  },
  { .udev_types = 0 },
};
