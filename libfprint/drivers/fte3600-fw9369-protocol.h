/*
 * FW9369 (silicon response 0x9362) SPI protocol
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

#include <glib.h>

#define FTE3600_FW9369_CHIP_ID 0x9362u
#define FTE3600_FW9369_WIDTH 64u
#define FTE3600_FW9369_HEIGHT 80u
#define FTE3600_FW9369_PIXELS (FTE3600_FW9369_WIDTH * FTE3600_FW9369_HEIGHT)
#define FTE3600_FW9369_BYTES_PER_PIXEL 2u
#define FTE3600_FW9369_IMAGE_OFFSET 6u
#define FTE3600_FW9369_FRAME_SIZE (FTE3600_FW9369_PIXELS * 2u + FTE3600_FW9369_IMAGE_OFFSET)
#define FTE3600_FW9369_SFR_READ_SIZE 5u
#define FTE3600_FW9369_SFR_WRITE_SIZE 4u
#define FTE3600_FW9369_SFR_RESULT_OFFSET 4u
#define FTE3600_FW9369_WORD_READ_SIZE 12u
#define FTE3600_FW9369_WORD_WRITE_SIZE 8u
#define FTE3600_FW9369_WORD_RESULT_OFFSET 6u
#define FTE3600_FW9369_COMMAND_SIZE 3u

typedef enum {
  FTE3600_FW9369_SFR_STATE = 0x80,
  FTE3600_FW9369_SFR_CLOCK = 0x8e,
  FTE3600_FW9369_SFR_TIMER_ENABLE = 0x90,
  FTE3600_FW9369_SFR_TIMER_HIGH = 0x91,
  FTE3600_FW9369_SFR_TIMER_LOW = 0x92,
  FTE3600_FW9369_SFR_BANK_UNLOCK = 0x9a,
  FTE3600_FW9369_SFR_PROCESS = 0x9b,
  FTE3600_FW9369_SFR_SPI_MODE = 0xc6,
} Fte3600Fw9369Sfr;

typedef enum {
  FTE3600_FW9369_WORD_SCAN_CONTROL = 0x1800,
  FTE3600_FW9369_WORD_DAC = 0x1801,
  FTE3600_FW9369_WORD_SAMPLE = 0x1804,
  FTE3600_FW9369_WORD_ANALOG = 0x1805,
  FTE3600_FW9369_WORD_RATE = 0x1806,
  FTE3600_FW9369_WORD_INTEGRATION = 0x1807,
  FTE3600_FW9369_WORD_FDT_INTEGRATION = 0x1808,
  FTE3600_FW9369_WORD_RATE_A = 0x180a,
  FTE3600_FW9369_WORD_RATE_B = 0x180b,
  FTE3600_FW9369_WORD_CALIBRATION = 0x180c,
  FTE3600_FW9369_WORD_FDT_ANALOG = 0x180d,
  FTE3600_FW9369_WORD_PIXEL_CONTROL = 0x1811,
  FTE3600_FW9369_WORD_FDT_THRESHOLDS = 0x1880,
  FTE3600_FW9369_WORD_FDT_CONTROL = 0x1881,
  FTE3600_FW9369_WORD_FDT_COUNT = 0x1884,
  FTE3600_FW9369_WORD_FDT_TRIGGER = 0x1885,
  FTE3600_FW9369_WORD_CHANNEL = 0x1887,
  FTE3600_FW9369_WORD_FDT_FILTER = 0x1888,
  FTE3600_FW9369_WORD_FIFO = 0x1a05,
  FTE3600_FW9369_WORD_EVENTS = 0x1a82,
  FTE3600_FW9369_WORD_EVENT_MASK = 0x1a83,
  FTE3600_FW9369_WORD_EVENT_CLEAR = 0x1a84,
  FTE3600_FW9369_WORD_CHIP_ID = 0x1a8b,
  FTE3600_FW9369_WORD_FDT_ENABLE = 0x1a8a,
} Fte3600Fw9369Word;

typedef enum {
  FTE3600_FW9369_CMD_IDLE_1 = 0xc0,
  FTE3600_FW9369_CMD_IDLE_2 = 0xc1,
  FTE3600_FW9369_CMD_FDT = 0xc2,
  FTE3600_FW9369_CMD_IMAGE = 0xc4,
  FTE3600_FW9369_CMD_WAKE = 0x5a,
  FTE3600_FW9369_CMD_WAKE_END = 0xa5,
} Fte3600Fw9369Command;

#define FTE3600_FW9369_EVENT_IDLE       0x0001u
#define FTE3600_FW9369_EVENT_DOWN       0x0002u
#define FTE3600_FW9369_EVENT_UP         0x0004u
#define FTE3600_FW9369_EVENT_MANUAL     0x0008u
#define FTE3600_FW9369_EVENT_INVALID    0x0010u
#define FTE3600_FW9369_EVENT_DATA       0x0020u
#define FTE3600_FW9369_EVENT_AFE        0x0040u
#define FTE3600_FW9369_EVENT_HALF       0x0080u
#define FTE3600_FW9369_EVENT_FULL       0x0100u
#define FTE3600_FW9369_EVENT_RESET      0x0200u
#define FTE3600_FW9369_EVENT_ESD        0x0400u
#define FTE3600_FW9369_STATE_IDLE       0x50u
#define FTE3600_FW9369_STATE_IMAGE      0x54u
#define FTE3600_FW9369_FDT_CHANNELS     4u
#define FTE3600_FW9369_FDT_READ_SIZE    14u
#define FTE3600_FW9369_FDT_WRITE_SIZE   22u
#define FTE3600_FW9369_DAC_MIN          1u
#define FTE3600_FW9369_DAC_MAX          125u
#define FTE3600_FW9369_IMAGE_DAC        54u
#define FTE3600_FW9369_FDT_DAC          27u

/* No I/O. Return complete TX length, or zero with G_IO_ERROR. Failure does
 * not alter the caller's output. Word addresses are 15-bit hardware addresses;
 * bit 15 is set by the builder as the wire address flag. */
gsize fpi_fte3600_fw9369_build_sfr_read (guint8 *out, gsize capacity,
                                       guint8 reg, GError **error);
gsize fpi_fte3600_fw9369_build_sfr_write (guint8 *out, gsize capacity,
                                        guint8 reg, guint8 value, GError **error);
gsize fpi_fte3600_fw9369_build_word_read (guint8 *out, gsize capacity,
                                        guint16 address, GError **error);
gsize fpi_fte3600_fw9369_build_word_write (guint8 *out, gsize capacity,
                                         guint16 address, guint16 value, GError **error);
gsize fpi_fte3600_fw9369_build_command (guint8 *out, gsize capacity,
                                      Fte3600Fw9369Command command, GError **error);
gsize fpi_fte3600_fw9369_build_image_read (guint8 *out, gsize capacity, GError **error);
gboolean fpi_fte3600_fw9369_decode_frame (const guint8 *frame, gsize frame_len,
                                         guint16 *pixels, gsize pixel_capacity,
                                         GError **error);
gsize fpi_fte3600_fw9369_build_fdt_read (guint8 *out, gsize capacity,
                                       gboolean smic, GError **error);
gsize fpi_fte3600_fw9369_build_fdt_base (guint8 *out, gsize capacity,
                                       gboolean smic, const guint16 *base,
                                       gsize channels, GError **error);
/* Independent host image processing: saturated baseline subtraction followed
 * by a robust linear stretch. This is not the proprietary Windows algorithm. */
gboolean fpi_fte3600_fw9369_make_image (const guint16 *base,
                                      const guint16 *raw, gsize pixels,
                                      guint8 *out, gsize capacity,
                                      GError **error);
guint fpi_fte3600_fw9369_image_median (const guint16 *raw);
