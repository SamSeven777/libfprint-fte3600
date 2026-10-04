/* Synthetic FT93xx protocol and image-layout tests.
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <string.h>
#include "drivers/fte3600-ft93xx-protocol.h"

static void
test_packets (void)
{
  guint8 buffer[20];
  const guint8 read_id[] = { 0x04, 0xfb, 0x9a, 0x8b, 0, 0, 0, 0, 0, 0 };
  const guint8 write[] = { 0x05, 0xfa, 0x98, 0x07, 0, 0, 0x0f, 0xef };
  const guint8 read_sfr[] = { 0x08, 0xf7, 0xc6, 0, 0 };
  const guint8 write_sfr[] = { 0x09, 0xf6, 0xc6, 1 };

  g_autoptr(GError) error = NULL;
  memset (buffer, 0xaa, sizeof buffer);
  g_assert_cmpuint (fpi_fte3600_ft93xx_read16 (buffer, 10, 0x1a8b, &error), ==, 10);
  g_assert_no_error (error);
  g_assert_cmpmem (buffer, 10, read_id, sizeof read_id);
  g_assert_cmphex (buffer[10], ==, 0xaa);
  g_assert_cmpuint (fpi_fte3600_ft93xx_write16 (buffer, 8, 0x1807, 0x0fef, &error), ==, 8);
  g_assert_cmpmem (buffer, 8, write, sizeof write);
  g_assert_cmpuint (fpi_fte3600_ft93xx_read8 (buffer, 5, 0xc6, &error), ==, 5);
  g_assert_cmpmem (buffer, 5, read_sfr, sizeof read_sfr);
  g_assert_cmpuint (fpi_fte3600_ft93xx_write8 (buffer, 4, 0xc6, 1, &error), ==, 4);
  g_assert_cmpmem (buffer, 4, write_sfr, sizeof write_sfr);
  g_assert_no_error (error);
}

static void
test_commands (void)
{
  const guint8 expected[][3] = {
    { 0xc0, 0x3f, 0 }, { 0xc4, 0x3b, 0 }, { 0x5a, 0xa5, 0 }, { 0xa5, 0x5a, 0 },
  };
  guint8 buffer[4];

  for (guint i = 0; i < G_N_ELEMENTS (expected); i++)
    {
      memset (buffer, 0xcc, sizeof buffer);
      g_assert_cmpuint (fpi_fte3600_ft93xx_command (buffer, 3, i, NULL), ==, 3);
      g_assert_cmpmem (buffer, 3, expected[i], 3);
      g_assert_cmphex (buffer[3], ==, 0xcc);
    }
  g_autoptr(GError) error = NULL;
  memset (buffer, 0xcc, sizeof buffer);
  g_assert_cmpuint (fpi_fte3600_ft93xx_command (buffer, sizeof buffer, 99, &error), ==, 0);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  for (guint i = 0; i < sizeof buffer; i++)
    g_assert_cmphex (buffer[i], ==, 0xcc);
}

static void
test_register_crc (void)
{
  guint8 response[10] = { 0, 0, 0, 0, 0, 0, 0x93, 0x65, 0x6c, 0xb4 };
  guint16 value = 0;

  g_autoptr(GError) error = NULL;
  /* Standard check vector, and an independent binascii.crc_hqx vector. */
  g_assert_cmphex (fpi_fte3600_ft93xx_crc16 ((const guint8 *) "123456789", 9), ==, 0x29b1);
  g_assert_cmphex (fpi_fte3600_ft93xx_crc16 (NULL, 0), ==, 0xffff);
  g_assert_true (fpi_fte3600_ft93xx_read16_result (response, 10, &value, &error));
  g_assert_cmphex (value, ==, 0x9365);
  response[8] = response[9] = 0; /* Explicitly accepted reference sentinel. */
  g_assert_true (fpi_fte3600_ft93xx_read16_result (response, 10, &value, &error));
  response[9] = 1;
  value = 0x5a5a;
  g_assert_false (fpi_fte3600_ft93xx_read16_result (response, 10, &value, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_cmphex (value, ==, 0x5a5a);
  g_clear_error (&error);
  g_assert_false (fpi_fte3600_ft93xx_read16_result (response, 9, &value, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_fifo_boundaries (void)
{
  guint8 buffer[1800];
  const guint8 header[] = { 0x06, 0xf9, 0x9a, 0x05, 0x03, 0x7e };
  const gsize bad_sizes[] = { 0, 1, 1791, 1792, G_MAXSIZE };

  for (gsize size = 2; size <= 1790; size += 2)
    {
      memset (buffer, 0xcc, sizeof buffer);
      g_assert_cmpuint (fpi_fte3600_ft93xx_fifo_read (buffer, size + 8, size, NULL), ==, size + 8);
      g_assert_cmpuint (((guint) buffer[4] << 8) | buffer[5], ==, size / 2 - 1);
      for (gsize i = 6; i < size + 8; i++)
        g_assert_cmpuint (buffer[i], ==, 0);
      g_assert_cmphex (buffer[size + 8], ==, 0xcc);
    }
  g_assert_cmpmem (buffer, 6, header, sizeof header);
  for (guint i = 0; i < G_N_ELEMENTS (bad_sizes); i++)
    {
      g_autoptr(GError) error = NULL;
      memset (buffer, 0xcc, sizeof buffer);
      g_assert_cmpuint (fpi_fte3600_ft93xx_fifo_read (buffer, sizeof buffer, bad_sizes[i], &error), ==, 0);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
      for (guint j = 0; j < sizeof buffer; j++)
        g_assert_cmphex (buffer[j], ==, 0xcc);
    }
  g_autoptr(GError) error = NULL;
  g_assert_cmpuint (fpi_fte3600_ft93xx_fifo_read (buffer, 1797, 1790, &error), ==, 0);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_image_layout (void)
{
  const Fte3600Sensor models[] = { FTE3600_SENSOR_FT9365, FTE3600_SENSOR_FT9769,
                                   FTE3600_SENSOR_FT9769 };
  const guint16 ids[] = { 0x9365, 0x9391, 0x9392 };
  const gsize sizes[] = { 10240, 16000, 16000 };
  const gsize counts[] = { 5120, 7840, 7840 };

  for (guint model = 0; model < G_N_ELEMENTS (models); model++)
    {
      const Fte3600Ft93xxProfile *profile = fpi_fte3600_ft93xx_profile (models[model]);
      g_autofree guint8 *raw = g_malloc (sizes[model]);
      g_autofree guint16 *pixels = g_new (guint16, counts[model] + 1);
      g_autoptr(GError) error = NULL;
      for (gsize i = 0; i < sizes[model] / 2; i++)
        {
          guint16 sample = (i * 13 + 7) & 0xfff;
          /* High nibble and low two bits are not pixel signal. */
          raw[i * 2] = 0xa0 | (sample >> 8);
          raw[i * 2 + 1] = sample;
        }
      pixels[counts[model]] = 0xdead;
      g_assert_true (fpi_fte3600_ft93xx_decode (profile, ids[model], raw, sizes[model],
                                                pixels, counts[model], &error));
      g_assert_no_error (error);
      for (gsize i = 0; i < counts[model]; i++)
        g_assert_cmphex (pixels[i], ==, (((model == 2 ? i ^ 3 : i) * 13 + 7) & 0xffc));
      g_assert_cmphex (pixels[counts[model]], ==, 0xdead);
      g_assert_false (fpi_fte3600_ft93xx_decode (profile, ids[model], raw, sizes[model] - 1,
                                                 pixels, counts[model], &error));
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
      g_clear_error (&error);
      g_assert_false (fpi_fte3600_ft93xx_decode (profile, 0x9395, raw, sizes[model],
                                                 pixels, counts[model], &error));
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    }
}

static void
test_identity (void)
{
  g_assert_true (fpi_fte3600_ft93xx_matches (FTE3600_SENSOR_FT9365, 0x9365, 0));
  g_assert_true (fpi_fte3600_ft93xx_matches (FTE3600_SENSOR_FT9769, 0x9391, 0x7fff));
  g_assert_true (fpi_fte3600_ft93xx_matches (FTE3600_SENSOR_FT9769, 0x9392, 0x7fff));
  g_assert_false (fpi_fte3600_ft93xx_matches (FTE3600_SENSOR_FT9769, 0x9391, 0x0fff));
  g_assert_false (fpi_fte3600_ft93xx_matches (FTE3600_SENSOR_FT9365, 0x9363, 0));
  g_assert_false (fpi_fte3600_ft93xx_matches (FTE3600_SENSOR_FT9769, 0x9395, 0));
  g_assert_false (fpi_fte3600_ft93xx_matches (FTE3600_SENSOR_FT9361, 0x9365, 0));
  g_assert_null (fpi_fte3600_ft93xx_profile (FTE3600_SENSOR_UNKNOWN));
  g_assert_null (fpi_fte3600_ft93xx_profile ((Fte3600Sensor) - 1));
}

static void
test_short_buffers (void)
{
  guint8 buffer[12];

  for (guint operation = 0; operation < 5; operation++)
    {
      g_autoptr(GError) error = NULL;
      gsize length = 1;
      memset (buffer, 0x72, sizeof buffer);
      switch (operation)
        {
        case 0: length = fpi_fte3600_ft93xx_read16 (buffer, 9, 0x1a8b, &error);
          break;

        case 1: length = fpi_fte3600_ft93xx_write16 (buffer, 7, 0x1800, 0, &error);
          break;

        case 2: length = fpi_fte3600_ft93xx_read8 (buffer, 4, 0xc6, &error);
          break;

        case 3: length = fpi_fte3600_ft93xx_write8 (buffer, 3, 0xc6, 1, &error);
          break;

        case 4: length = fpi_fte3600_ft93xx_command (buffer, 2, FT93XX_COMMAND_IDLE, &error);
          break;

        default: g_assert_not_reached ();
        }
      g_assert_cmpuint (length, ==, 0);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
      for (guint i = 0; i < sizeof buffer; i++)
        g_assert_cmphex (buffer[i], ==, 0x72);
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/fte3600/ft93xx/packets", test_packets);
  g_test_add_func ("/fte3600/ft93xx/commands", test_commands);
  g_test_add_func ("/fte3600/ft93xx/register-crc", test_register_crc);
  g_test_add_func ("/fte3600/ft93xx/fifo-boundaries", test_fifo_boundaries);
  g_test_add_func ("/fte3600/ft93xx/image-layout", test_image_layout);
  g_test_add_func ("/fte3600/ft93xx/identity", test_identity);
  g_test_add_func ("/fte3600/ft93xx/short-buffers", test_short_buffers);
  return g_test_run ();
}
