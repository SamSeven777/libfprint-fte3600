/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Independently expressed FT9368 wire formats; no firmware payloads. */
#pragma once

#include <glib.h>

#define FTE3600_FT9368_WIDTH 64
#define FTE3600_FT9368_HEIGHT 80
#define FTE3600_FT9368_PIXELS (FTE3600_FT9368_WIDTH * FTE3600_FT9368_HEIGHT)
#define FTE3600_FT9368_HEADER 7
#define FTE3600_FT9368_INFO_SIZE 32
#define FTE3600_FT9368_WAKE_CHECK_SIZE 4
#define FTE3600_FT9368_APP_SIZE 27120
#define FTE3600_FT9368_PRAM_SIZE 6096
#define FTE3600_FT9368_APP_CHUNK 256
#define FTE3600_FT9368_PRAM_CHUNK 128
#define FTE3600_FT9368_MAX_PACKET (FTE3600_FT9368_APP_CHUNK + FTE3600_FT9368_HEADER)
#define FTE3600_FT9368_CLEAR_SIZE 6
#define FTE3600_FT9368_PROGRAMMED_VERSION 0x13
#define FTE3600_FT9368_BOOT_ID 0x56a2
#define FTE3600_FT9368_ERASE_ACK 0xf0aa
#define FTE3600_FT9368_PRAM_READ 0x71
#define FTE3600_FT9368_MODE_LOAD 0x55
#define FTE3600_FT9368_MODE_EXECUTE 0x0a
#define FTE3600_FT9368_SFR_UNLOCK 0x0004
#define FTE3600_FT9368_SFR_UNLOCK_VALUE 0xaa55
#define FTE3600_FT9368_SFR_CONFIG 0x0048
#define FTE3600_FT9368_SFR_CONFIG_VALUE 0x0110
#define FTE3600_FT9368_SFR_START 0x0007
#define FTE3600_FT9368_SFR_START_VALUE 0x5a5a

enum {
  FTE3600_FT9368_WAKE = 0xff00,
  FTE3600_FT9368_INFO = 0x9180,
  FTE3600_FT9368_IMAGE = 0x9080,
  FTE3600_FT9368_START = 0xf680,
  FTE3600_FT9368_FLASH_HANDSHAKE = 0x5500,
  FTE3600_FT9368_FLASH_STATUS = 0x6a80,
  FTE3600_FT9368_CHECKSUM_START = 0x6400,
  FTE3600_FT9368_CHECKSUM_READ = 0x6680,
  FTE3600_FT9368_REBOOT = 0x0700,
};

/* Write opcodes have a separate byte field from the read command words. */
enum {
  FTE3600_FT9368_FLASH_CONFIG1 = 0x09,
  FTE3600_FT9368_FLASH_CONFIG2 = 0x10,
  FTE3600_FT9368_FLASH_ERASE = 0x61,
  FTE3600_FT9368_FLASH_ADDRESS = 0xab,
  FTE3600_FT9368_FLASH_DATA = 0xbf,
  FTE3600_FT9368_CHECKSUM_RANGE = 0x65,
};

typedef struct
{
  guint8   version;
  guint8   manufacturer;
  guint8   width;
  guint8   height;
  gboolean finger_present;
} Fte3600Ft9368Info;

/* Zero return means invalid arguments; failed builders do not write output. */
gsize fpi_fte3600_ft9368_read (guint8 *out,
                               gsize   capacity,
                               guint16 command,
                               gsize   length);
gsize fpi_fte3600_ft9368_write (guint8       *out,
                                gsize         capacity,
                                guint8        command,
                                const guint8 *data,
                                gsize         length);
gsize fpi_fte3600_ft9368_mode (guint8 *out,
                               gsize   capacity,
                               guint8  mode);
gsize fpi_fte3600_ft9368_sfr (guint8 *out,
                              gsize   capacity,
                              guint16 address,
                              guint16 value);
gsize fpi_fte3600_ft9368_pram_write (guint8       *out,
                                     gsize         capacity,
                                     gsize         offset,
                                     const guint8 *data,
                                     gsize         length);
gsize fpi_fte3600_ft9368_pram_select (guint8 *out,
                                      gsize   capacity,
                                      gsize   offset,
                                      gsize   length);
gboolean fpi_fte3600_ft9368_parse_info (const guint8      *data,
                                        gsize              length,
                                        Fte3600Ft9368Info *info);
/* Factory wake readiness predicate only; not sensor identity. */
gboolean fpi_fte3600_ft9368_wake_ready (const guint8 *data,
                                        gsize         length);
/* The firmware's verification polynomial consumes big-endian 16-bit words.
 * This is not the ordinary byte-wise CRC-16/X25 recurrence. */
gboolean fpi_fte3600_ft9368_checksum (const guint8 *data,
                                      gsize         length,
                                      guint16      *checksum);
guint16 fpi_fte3600_ft9368_program_ack (gsize offset,
                                        gsize length);
