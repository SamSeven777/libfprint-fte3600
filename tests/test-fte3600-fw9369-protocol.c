/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gio/gio.h>
#include <string.h>

#include "drivers/fte3600-fw9369-protocol.h"

static void
test_registers (void)
{
  const guint8 sfr_read[] = { 0x08, 0xf7, 0x80, 0, 0 };
  const guint8 sfr_write[] = { 0x09, 0xf6, 0xc6, 1 };
  const guint8 id_read[] = { 0x04, 0xfb, 0x9a, 0x8b, 0, 1, 0, 0, 0, 0, 0, 0 };
  const guint8 dac_write[] = { 0x05, 0xfa, 0x98, 1, 0, 1, 0xfc, 0xb6 };
  guint8 buffer[32];

  memset (buffer, 0xcc, sizeof buffer);
  g_assert_cmpuint (fpi_fte3600_fw9369_build_sfr_read (buffer, sizeof buffer, 0x80, NULL), ==, 5);
  g_assert_cmpmem (buffer, 5, sfr_read, sizeof sfr_read);
  g_assert_cmpuint (buffer[5], ==, 0xcc);
  g_assert_cmpuint (fpi_fte3600_fw9369_build_sfr_write (buffer, sizeof buffer, 0xc6, 1, NULL), ==, 4);
  g_assert_cmpmem (buffer, 4, sfr_write, sizeof sfr_write);
  g_assert_cmpuint (fpi_fte3600_fw9369_build_word_read (buffer, sizeof buffer, 0x1a8b, NULL), ==, 12);
  g_assert_cmpmem (buffer, 12, id_read, sizeof id_read);
  g_assert_cmpuint (fpi_fte3600_fw9369_build_word_write (buffer, sizeof buffer, 0x1801, 0xfcb6, NULL), ==, 8);
  g_assert_cmpmem (buffer, 8, dac_write, sizeof dac_write);
}

static void
test_boundaries (void)
{
  guint8 buffer[32];
  guint8 original[32];
  g_autoptr(GError) error = NULL;

  memset (buffer, 0x97, sizeof buffer);
  memcpy (original, buffer, sizeof buffer);
  for (gsize size = 0; size < 12; size++)
    {
      g_assert_cmpuint (fpi_fte3600_fw9369_build_word_read (buffer, size, 0x1a8b, &error), ==, 0);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
      g_clear_error (&error);
      g_assert_cmpmem (buffer, sizeof buffer, original, sizeof original);
    }
  g_assert_cmpuint (fpi_fte3600_fw9369_build_word_write (buffer, sizeof buffer, 0x8000, 1, &error), ==, 0);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);
  g_assert_cmpuint (fpi_fte3600_fw9369_build_word_read (NULL, G_MAXSIZE, 0, &error), ==, 0);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);
  g_assert_cmpuint (fpi_fte3600_fw9369_build_sfr_write (buffer, 3, 0, 0, &error), ==, 0);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
  g_clear_error (&error);
  g_assert_cmpuint (fpi_fte3600_fw9369_build_command (buffer, sizeof buffer, 0x55, &error), ==, 0);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);
  g_assert_cmpuint (fpi_fte3600_fw9369_build_fdt_base (buffer, sizeof buffer, FALSE, NULL, 4, &error), ==, 0);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpmem (buffer, sizeof buffer, original, sizeof original);
}

static void
test_commands (void)
{
  const guint8 opcodes[] = { 0xc0, 0xc1, 0xc2, 0xc4, 0x5a, 0xa5 };
  guint8 buffer[4] = { 0, 0, 0, 0xcc };

  for (guint i = 0; i < G_N_ELEMENTS (opcodes); i++)
    {
      g_assert_cmpuint (fpi_fte3600_fw9369_build_command (buffer, 3, opcodes[i], NULL), ==, 3);
      g_assert_cmpuint (buffer[0], ==, opcodes[i]);
      g_assert_cmpuint (buffer[0] ^ buffer[1], ==, 0xff);
      g_assert_cmpuint (buffer[2], ==, 0);
      g_assert_cmpuint (buffer[3], ==, 0xcc);
    }
}

static void
test_fdt (void)
{
  const guint16 base[] = { 500, 510, 520, 530 };
  const guint8 db_read[] = { 0x04, 0xfb, 0x80, 0xb8, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0 };
  const guint8 smic_write[] = {
    0x05, 0xfa, 0x80, 0xe0, 0, 8, 1, 0xf4, 1, 0xfe, 2, 8, 2, 0x12,
    0, 0, 0, 0, 0, 0, 0, 0,
  };
  guint8 buffer[22];

  g_assert_cmpuint (fpi_fte3600_fw9369_build_fdt_read (buffer, sizeof buffer, FALSE, NULL), ==, 14);
  g_assert_cmpmem (buffer, 14, db_read, sizeof db_read);
  g_assert_cmpuint (fpi_fte3600_fw9369_build_fdt_base (buffer, sizeof buffer, TRUE, base, 4, NULL), ==, 22);
  g_assert_cmpmem (buffer, 22, smic_write, sizeof smic_write);
}

static void
test_frame (void)
{
  const guint8 header[] = { 0x06, 0xf9, 0x9a, 0x05, 0x14, 0 };
  g_autofree guint8 *frame = g_malloc (10247);
  g_autofree guint16 *raw = g_new (guint16, 5120);
  g_autoptr(GError) error = NULL;

  memset (frame, 0xab, 10247);
  g_assert_cmpuint (fpi_fte3600_fw9369_build_image_read (frame, 10246, NULL), ==, 10246);
  g_assert_cmpmem (frame, 6, header, sizeof header);
  g_assert_cmpuint (frame[10246], ==, 0xab);
  for (guint i = 0; i < 5120; i++)
    {
      guint16 value = (i * 37) & 0xffff;
      g_assert_cmpuint (frame[6 + 2 * i], ==, 0);
      g_assert_cmpuint (frame[7 + 2 * i], ==, 0);
      frame[6 + 2 * i] = value >> 8;
      frame[7 + 2 * i] = value & 0xff;
    }
  g_assert_true (fpi_fte3600_fw9369_decode_frame (frame, 10246, raw, 5120, NULL));
  for (guint i = 0; i < 5120; i++)
    g_assert_cmpuint (raw[i], ==, (i * 37) & 0xffff);
  g_assert_false (fpi_fte3600_fw9369_decode_frame (frame, 10245, raw, 5120, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);
  g_assert_false (fpi_fte3600_fw9369_decode_frame (frame, 10247, raw, 5120, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);
  g_assert_false (fpi_fte3600_fw9369_decode_frame (frame, 10246, raw, 5119, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_pixels (void)
{
  g_autofree guint16 *base = g_new (guint16, 5120);
  g_autofree guint16 *raw = g_new (guint16, 5120);
  g_autofree guint8 *image = g_malloc (5120);
  g_autoptr(GError) error = NULL;

  for (guint i = 0; i < 5120; i++)
    base[i] = raw[i] = 2048;
  memset (image, 0xcc, 5120);
  g_assert_cmpuint (fpi_fte3600_fw9369_image_median (raw), ==, 512);
  g_assert_false (fpi_fte3600_fw9369_make_image (base, raw, 5120, image, 5120, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);
  for (guint i = 0; i < 5120; i++)
    {
      g_assert_cmpuint (image[i], ==, 0xcc);
      raw[i] = i % 2 ? 1848 : 2148;
    }
  g_assert_true (fpi_fte3600_fw9369_make_image (base, raw, 5120, image, 5120, NULL));
  /* Negative subtraction saturates to white; a known positive ridge is dark. */
  g_assert_cmpuint (image[64 + 2], ==, 255);
  g_assert_cmpuint (image[64 + 3], ==, 0);
  g_assert_cmpuint (image[64], ==, image[64 + 2]);
  g_assert_cmpmem (image, 64, image + 64, 64);
  g_assert_false (fpi_fte3600_fw9369_make_image (base, raw, 5120, image, 5119, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/fte3600/fw9369/registers", test_registers);
  g_test_add_func ("/fte3600/fw9369/boundaries", test_boundaries);
  g_test_add_func ("/fte3600/fw9369/commands", test_commands);
  g_test_add_func ("/fte3600/fw9369/fdt", test_fdt);
  g_test_add_func ("/fte3600/fw9369/frame", test_frame);
  g_test_add_func ("/fte3600/fw9369/pixels", test_pixels);
  return g_test_run ();
}
