/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include <glib.h>

/* FT9338/FT9536 boot commands 68/69 have no extra turnaround byte. */
#define FTE3600_BOOT38_REGISTER_SIZE 4
#define FTE3600_BOOT38_RESULT_OFFSET 3
#define FTE3600_BOOT38_READBACK_OFFSET 6
#define FTE3600_BOOT38_READBACK_OVERHEAD 8
#define FTE3600_BOOT38_CONFIG_C8 0xc8
#define FTE3600_BOOT38_CONFIG_CA 0xca
#define FTE3600_BOOT38_CONFIG_CB 0xcb
#define FTE3600_BOOT38_CONFIG_B9 0xb9
#define FTE3600_BOOT38_CONFIG_FD 0xfd
#define FTE3600_BOOT38_ID_FE 0xfe
#define FTE3600_BOOT38_CONFIG_ALL 0xff
#define FTE3600_BOOT38_CONFIG_PREPARE 0xbf
#define FTE3600_BOOT38_IDENTIFY_ENABLE 0x20
#define FTE3600_BOOT38_IDENTIFY_VALUE 0x11
#define FTE3600_BOOT38_OTP_ENABLE 1
#define FTE3600_BOOT38_OTP_ADDRESS 0x1d

/* Return zero without writing output on invalid arguments. */
gsize fpi_fte3600_build_boot38_read (guint8 *out,
                                     gsize   capacity,
                                     guint8  reg);
gsize fpi_fte3600_build_boot38_write (guint8 *out,
                                      gsize   capacity,
                                      guint8  reg,
                                      guint8  value);
gsize fpi_fte3600_build_boot38_readback (guint8 *out,
                                         gsize   capacity,
                                         gsize   firmware_size);
