/*
 * Independent FT9365/FT9769 hardware protocol definitions
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

#include <gio/gio.h>
#include "fte3600-sensor.h"

#define FT93XX_REGISTER_READ_SIZE 10
#define FT93XX_REGISTER_WRITE_SIZE 8
#define FT93XX_FIFO_PAYLOAD_MAX 1790
#define FT93XX_FIFO_OVERHEAD 8
#define FT93XX_TRANSFER_MAX (FT93XX_FIFO_PAYLOAD_MAX + FT93XX_FIFO_OVERHEAD)
#define FT93XX_REG_CHIP_ID 0x1a8b
#define FT93XX_REG_VARIANT 0x1816
#define FT93XX_REG_INTERRUPT_STATUS 0x1a82
#define FT93XX_REG_INTERRUPT_CLEAR 0x1a84
#define FT93XX_REG_FIFO 0x1a05
#define FT93XX_INTERRUPT_IMAGE_READY 0x0020
#define FT93XX_INTERRUPT_FAULT 0x0e00

typedef enum {
  FT93XX_COMMAND_IDLE,
  FT93XX_COMMAND_SCAN_IMAGE,
  FT93XX_COMMAND_WAKE,
  FT93XX_COMMAND_RELEASE,
} Fte3600Ft93xxCommand;

typedef struct
{
  Fte3600Sensor sensor;
  guint16       width;
  guint16       height;
  guint8        extra_rows;
  guint8        initial_dac;
  guint8        maximum_dac;
  guint8        gain;
  guint16       scan_window;
  guint16       scan_extension;
} Fte3600Ft93xxProfile;

const Fte3600Ft93xxProfile *fpi_fte3600_ft93xx_profile (Fte3600Sensor sensor);

gsize fpi_fte3600_ft93xx_read16 (guint8  *buffer,
                                 gsize    capacity,
                                 guint16  address,
                                 GError **error);
gboolean fpi_fte3600_ft93xx_read16_result (const guint8 *response,
                                           gsize         size,
                                           guint16      *value,
                                           GError      **error);
gsize fpi_fte3600_ft93xx_write16 (guint8  *buffer,
                                  gsize    capacity,
                                  guint16  address,
                                  guint16  value,
                                  GError **error);
gsize fpi_fte3600_ft93xx_read8 (guint8  *buffer,
                                gsize    capacity,
                                guint8   address,
                                GError **error);
gsize fpi_fte3600_ft93xx_write8 (guint8  *buffer,
                                 gsize    capacity,
                                 guint8   address,
                                 guint8   value,
                                 GError **error);
gsize fpi_fte3600_ft93xx_command (guint8              *buffer,
                                  gsize                capacity,
                                  Fte3600Ft93xxCommand command,
                                  GError             **error);
gsize fpi_fte3600_ft93xx_fifo_read (guint8  *buffer,
                                    gsize    capacity,
                                    gsize    payload_size,
                                    GError **error);
guint16 fpi_fte3600_ft93xx_crc16 (const guint8 *data,
                                  gsize         size);

/* A zero register trailer is the reference driver's accepted CRC sentinel.
 * FIFO trailers are opaque: the reference image reader does not validate them.
 * FT9392 reverses each group of four pixels; the extra rows follow the image. */
gboolean fpi_fte3600_ft93xx_decode (const Fte3600Ft93xxProfile *profile,
                                    guint16                     chip_id,
                                    const guint8               *raw,
                                    gsize                       raw_size,
                                    guint16                    *pixels,
                                    gsize                       pixel_count,
                                    GError                    **error);
gboolean fpi_fte3600_ft93xx_matches (Fte3600Sensor sensor,
                                     guint16       chip_id,
                                     guint16       variant);
