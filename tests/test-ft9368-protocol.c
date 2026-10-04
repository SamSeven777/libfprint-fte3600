/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <glib.h>
#include <string.h>

#include "drivers/fte3600-ft9368-protocol.h"

static void
test_commands (void)
{
  guint8 out[FTE3600_FT9368_PIXELS + 7];
  const guint8 wake[] = { 0xff, 0, 0, 0 };
  const guint8 info[] = { 0x91, 0x80, 0, 0x20, 0, 0, 0 };
  const guint8 image[] = { 0x90, 0x80, 0x14, 0, 0, 0, 0 };
  const guint8 mode[] = { 0x70, 0x55, 0xaa };
  const guint8 sfr[] = { 0x70, 7, 0xf8, 0, 4, 0, 0, 0xaa, 0x55, 0, 0 };
  const guint8 value[] = { 0x0a };
  const guint8 write[] = { 9, 0, 0, 1, 0, 0, 0, 0x0a };

  g_assert_cmpuint (fpi_fte3600_ft9368_read (out, sizeof out, FTE3600_FT9368_WAKE, 0), ==, 4);
  g_assert_cmpmem (out, 4, wake, sizeof wake);
  memset (out, 0xa5, sizeof out);
  g_assert_cmpuint (fpi_fte3600_ft9368_read (out, sizeof out, FTE3600_FT9368_INFO, 32), ==, 39);
  g_assert_cmpmem (out, 7, info, sizeof info);
  for (guint i = 7; i < 39; i++)
    g_assert_cmpuint (out[i], ==, 0);
  g_assert_cmpuint (out[39], ==, 0xa5);
  g_assert_cmpuint (fpi_fte3600_ft9368_read (out, sizeof out, FTE3600_FT9368_IMAGE,
                                             FTE3600_FT9368_PIXELS), ==, sizeof out);
  g_assert_cmpmem (out, 7, image, sizeof image);
  g_assert_cmpuint (fpi_fte3600_ft9368_mode (out, sizeof out, 0x55), ==, sizeof mode);
  g_assert_cmpmem (out, sizeof mode, mode, sizeof mode);
  g_assert_cmpuint (fpi_fte3600_ft9368_sfr (out, sizeof out, 4, 0xaa55), ==, sizeof sfr);
  g_assert_cmpmem (out, sizeof sfr, sfr, sizeof sfr);
  g_assert_cmpuint (fpi_fte3600_ft9368_write (out, sizeof out, 9, value, sizeof value), ==, sizeof write);
  g_assert_cmpmem (out, sizeof write, write, sizeof write);
}

static void
test_pram (void)
{
  guint8 data[128], out[135];
  const guint8 header[] = { 0x70, 5, 0xfa, 0x20, 0x20, 0, 0x1f };
  const guint8 tail[] = { 0x70, 5, 0xfa, 0x25, 0xe0, 0, 0x13 };
  const guint8 read[] = { 0x70, 4, 0xfb, 0x20, 0x40, 0, 0x3f };

  for (guint i = 0; i < sizeof data; i++)
    data[i] = i;
  g_assert_cmpuint (fpi_fte3600_ft9368_pram_write (out, sizeof out, 128, data, 128), ==, 135);
  g_assert_cmpmem (out, 7, header, sizeof header);
  g_assert_cmpmem (out + 7, 128, data, 128);
  g_assert_cmpuint (fpi_fte3600_ft9368_pram_write (out, sizeof out, 6016, data, 80), ==, 87);
  g_assert_cmpmem (out, 7, tail, sizeof tail);
  g_assert_cmpuint (fpi_fte3600_ft9368_pram_select (out, sizeof out, 256, 256), ==, 7);
  g_assert_cmpmem (out, 7, read, sizeof read);
  g_assert_cmpuint (fpi_fte3600_ft9368_pram_write (out, sizeof out, 6016, data, 128), ==, 0);
  g_assert_cmpuint (fpi_fte3600_ft9368_pram_write (out, sizeof out, G_MAXSIZE, data, 128), ==, 0);
  g_assert_cmpuint (fpi_fte3600_ft9368_pram_select (out, sizeof out, 0, 258), ==, 0);
  g_assert_cmpuint (fpi_fte3600_ft9368_pram_select (out, sizeof out, 2, 128), ==, 0);
  g_assert_cmpuint (fpi_fte3600_ft9368_program_ack (0, 256), ==, 0x1000);
  g_assert_cmpuint (fpi_fte3600_ft9368_program_ack (256, 256), ==, 0x1001);
  g_assert_cmpuint (fpi_fte3600_ft9368_program_ack (26880, 240), ==, 0x1070);
  g_assert_cmpuint (fpi_fte3600_ft9368_program_ack (26880, 256), ==, 0);
  g_assert_cmpuint (fpi_fte3600_ft9368_program_ack (1, 256), ==, 0);
}

static void
test_info (void)
{
  guint8 info[32] = { 0 };
  Fte3600Ft9368Info decoded;

  info[19] = 0x93;
  info[20] = 0x68;
  info[21] = 0x13;
  info[23] = 64;
  info[24] = 80;
  g_assert_true (fpi_fte3600_ft9368_parse_info (info, sizeof info, &decoded));
  g_assert_false (decoded.finger_present);
  info[1] = info[2] = 0x11;
  g_assert_true (fpi_fte3600_ft9368_parse_info (info, sizeof info, &decoded));
  g_assert_true (decoded.finger_present);
  info[21] = 0x12;
  g_assert_true (fpi_fte3600_ft9368_parse_info (info, sizeof info, &decoded));
  info[2] = 0x22;
  g_assert_false (fpi_fte3600_ft9368_parse_info (info, sizeof info, &decoded));
  info[2] = 0;
  info[23] = 80;
  g_assert_false (fpi_fte3600_ft9368_parse_info (info, sizeof info, &decoded));
  info[23] = 64;
  info[20] = 0x62;
  g_assert_false (fpi_fte3600_ft9368_parse_info (info, sizeof info, &decoded));
  info[20] = 0x68;
  g_assert_false (fpi_fte3600_ft9368_parse_info (info, 31, &decoded));
  g_assert_false (fpi_fte3600_ft9368_parse_info (NULL, 32, &decoded));
}

static void
test_bounds_checksum (void)
{
  guint8 data[16], out[16], saved[16];
  guint16 checksum;

  memset (out, 0x42, sizeof out);
  memcpy (saved, out, sizeof out);
  for (guint i = 0; i < sizeof data; i++)
    data[i] = i;
  g_assert_cmpuint (fpi_fte3600_ft9368_read (out, sizeof out, 0x9180, G_MAXSIZE), ==, 0);
  g_assert_cmpuint (fpi_fte3600_ft9368_read (out, 3, 0xff00, 0), ==, 0);
  g_assert_cmpuint (fpi_fte3600_ft9368_write (out, sizeof out, 0xbf, data, sizeof data), ==, 0);
  g_assert_cmpuint (fpi_fte3600_ft9368_mode (out, 2, 0x55), ==, 0);
  g_assert_cmpuint (fpi_fte3600_ft9368_sfr (out, 10, 4, 0xaa55), ==, 0);
  g_assert_cmpmem (out, sizeof out, saved, sizeof saved);
  g_assert_true (fpi_fte3600_ft9368_checksum (data, sizeof data, &checksum));
  g_assert_cmpuint (checksum, ==, 0xef10);
  g_assert_false (fpi_fte3600_ft9368_checksum (data, 15, &checksum));
  g_assert_false (fpi_fte3600_ft9368_checksum (NULL, 16, &checksum));
  g_assert_false (fpi_fte3600_ft9368_checksum (data, G_MAXSIZE - 1, &checksum));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/ft9368/commands", test_commands);
  g_test_add_func ("/ft9368/pram", test_pram);
  g_test_add_func ("/ft9368/info", test_info);
  g_test_add_func ("/ft9368/bounds-checksum", test_bounds_checksum);
  return g_test_run ();
}
