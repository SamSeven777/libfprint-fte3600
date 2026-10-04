/*
 * Synthetic FTE3600 wire-format and bounds tests
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <gio/gio.h>
#include <string.h>

#include "drivers/fte3600-protocol.h"

typedef gsize (*RegisterRead) (guint8 *, gsize, guint8, gsize, GError **);
typedef gsize (*RegisterWrite) (guint8 *, gsize, guint8, guint8, GError **);

static void
assert_filled (const guint8 *buffer, gsize length, guint8 value)
{
  for (gsize i = 0; i < length; i++)
    g_assert_cmphex (buffer[i], ==, value);
}

static void
test_register_packets (void)
{
  const struct {
    RegisterRead read;
    RegisterWrite write;
    guint8 read_header[4];
    guint8 write_packet[5];
  } cases[] = {
    { fpi_fte3600_build_app_read, fpi_fte3600_build_app_write,
      { 0x10, 0xef, 0x20, 0x00 }, { 0x11, 0xee, 0x20, 0xa5, 0x00 } },
    { fpi_fte3600_build_boot_read, fpi_fte3600_build_boot_write,
      { 0x08, 0xf7, 0x20, 0x00 }, { 0x09, 0xf6, 0x20, 0xa5, 0x00 } },
  };

  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      guint8 buffer[32];
      g_autoptr(GError) error = NULL;

      for (gsize result_len = 1; result_len <= 17; result_len++)
        {
          memset (buffer, 0xcc, sizeof buffer);
          g_assert_cmpuint (cases[i].read (buffer, 4 + result_len, 0x20,
                                           result_len, &error), ==, 4 + result_len);
          g_assert_no_error (error);
          g_assert_cmpmem (buffer, 4, cases[i].read_header, 4);
          assert_filled (buffer + 4, result_len, 0);
          assert_filled (buffer + 4 + result_len, sizeof buffer - 4 - result_len, 0xcc);
        }

      memset (buffer, 0xcc, sizeof buffer);
      g_assert_cmpuint (cases[i].write (buffer, 5, 0x20, 0xa5, &error), ==, 5);
      g_assert_no_error (error);
      g_assert_cmpmem (buffer, 5, cases[i].write_packet, 5);
      assert_filled (buffer + 5, sizeof buffer - 5, 0xcc);

      /* Every register/value byte must survive without sign extension,
       * truncation, or an accidental interpretation as a length. */
      for (guint reg = 0; reg <= G_MAXUINT8; reg++)
        for (guint value = 0; value <= G_MAXUINT8; value++)
          {
            g_assert_cmpuint (cases[i].write (buffer, 5, reg, value, &error), ==, 5);
            g_assert_no_error (error);
            g_assert_cmpuint (buffer[2], ==, reg);
            g_assert_cmpuint (buffer[3], ==, value);
            g_assert_cmpuint (buffer[4], ==, 0);
            g_assert_cmpuint (buffer[5], ==, 0xcc);
          }
    }
}

static void
test_register_bounds (void)
{
  const RegisterRead reads[] = { fpi_fte3600_build_app_read, fpi_fte3600_build_boot_read };
  const RegisterWrite writes[] = { fpi_fte3600_build_app_write, fpi_fte3600_build_boot_write };
  guint8 buffer[16];

  memset (buffer, 0xcc, sizeof buffer);
  for (gsize i = 0; i < G_N_ELEMENTS (reads); i++)
    {
      g_autoptr(GError) error = NULL;
      const gsize overflowing[] = { G_MAXSIZE - 3, G_MAXSIZE - 1, G_MAXSIZE };

      g_assert_cmpuint (reads[i] (buffer, sizeof buffer, 0, 0, &error), ==, 0);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
      g_clear_error (&error);
      g_assert_cmpuint (reads[i] (NULL, sizeof buffer, 0, 1, &error), ==, 0);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
      g_clear_error (&error);

      for (gsize j = 0; j < G_N_ELEMENTS (overflowing); j++)
        {
          g_assert_cmpuint (reads[i] (buffer, sizeof buffer, 0, overflowing[j], &error), ==, 0);
          g_assert_error (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
          g_clear_error (&error);
        }

      /* This addition fits gsize exactly, but cannot fit the output. */
      g_assert_cmpuint (reads[i] (buffer, sizeof buffer, 0, G_MAXSIZE - 4, &error), ==, 0);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
      g_clear_error (&error);
      for (gsize capacity = 0; capacity < 6; capacity++)
        {
          g_assert_cmpuint (reads[i] (buffer, capacity, 0, 2, &error), ==, 0);
          g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
          g_clear_error (&error);
        }

      g_assert_cmpuint (reads[i] (buffer, 0, 0, 1, NULL), ==, 0);
      assert_filled (buffer, sizeof buffer, 0xcc);
    }

  for (gsize i = 0; i < G_N_ELEMENTS (writes); i++)
    {
      g_autoptr(GError) error = NULL;

      g_assert_cmpuint (writes[i] (NULL, sizeof buffer, 0, 0, &error), ==, 0);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
      g_clear_error (&error);
      for (gsize capacity = 0; capacity < 5; capacity++)
        {
          g_assert_cmpuint (writes[i] (buffer, capacity, 0, 0, &error), ==, 0);
          g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
          g_clear_error (&error);
        }

      g_assert_cmpuint (writes[i] (buffer, 0, 0, 0, NULL), ==, 0);
      assert_filled (buffer, sizeof buffer, 0xcc);
    }
}

static void
test_image_packets (void)
{
  const struct {
    gsize pixels;
    guint8 header[6];
  } cases[] = {
    { 64 * 80,  { 0x04, 0xfb, 0x34, 0x00, 0x14, 0x08 } },
    { 96 * 96,  { 0x04, 0xfb, 0x34, 0x00, 0x24, 0x08 } },
    { 88 * 88,  { 0x04, 0xfb, 0x34, 0x00, 0x1e, 0x48 } },
    { 64 * 128, { 0x04, 0xfb, 0x34, 0x00, 0x20, 0x08 } },
    { 1,        { 0x04, 0xfb, 0x34, 0x00, 0x00, 0x09 } },
    { 65527,    { 0x04, 0xfb, 0x34, 0x00, 0xff, 0xff } },
  };

  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      gsize length = cases[i].pixels + 8;
      g_autofree guint8 *buffer = g_malloc (length + 1);
      g_autoptr(GError) error = NULL;

      memset (buffer, 0xcc, length + 1);
      g_assert_cmpuint (fpi_fte3600_build_image_read (buffer, length - 1,
                                                    cases[i].pixels, &error), ==, 0);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
      assert_filled (buffer, length + 1, 0xcc);
      g_clear_error (&error);

      g_assert_cmpuint (fpi_fte3600_build_image_read (buffer, length,
                                                    cases[i].pixels, &error), ==, length);
      g_assert_no_error (error);
      g_assert_cmpmem (buffer, 6, cases[i].header, 6);
      assert_filled (buffer + 6, length - 6, 0);
      g_assert_cmphex (buffer[length], ==, 0xcc);
    }
}

static void
test_image_bounds (void)
{
  guint8 buffer[16];
  const gsize invalid[] = { 65528, 65535, G_MAXSIZE - 7, G_MAXSIZE };
  g_autoptr(GError) error = NULL;

  memset (buffer, 0xcc, sizeof buffer);
  g_assert_cmpuint (fpi_fte3600_build_image_read (buffer, sizeof buffer, 0, &error), ==, 0);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);
  g_assert_cmpuint (fpi_fte3600_build_image_read (NULL, sizeof buffer, 1, &error), ==, 0);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  for (gsize i = 0; i < G_N_ELEMENTS (invalid); i++)
    {
      g_assert_cmpuint (fpi_fte3600_build_image_read (buffer, sizeof buffer,
                                                    invalid[i], &error), ==, 0);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
      g_clear_error (&error);
    }

  g_assert_cmpuint (fpi_fte3600_build_image_read (buffer, 0, 1, NULL), ==, 0);
  assert_filled (buffer, sizeof buffer, 0xcc);
}

static void
test_firmware_packets (void)
{
  const guint8 payload[] = { 0x12, 0x34, 0xab, 0xcd };
  const guint8 expected[] = { 0x05, 0xfa, 0x00, 0x00, 0x00, 0x04,
                              0x12, 0x34, 0xab, 0xcd, 0x00 };
  guint8 buffer[32];
  g_autoptr(GError) error = NULL;

  memset (buffer, 0xcc, sizeof buffer);
  g_assert_cmpuint (fpi_fte3600_build_firmware (buffer, sizeof expected, payload,
                                              sizeof payload, &error), ==, sizeof expected);
  g_assert_no_error (error);
  g_assert_cmpmem (buffer, sizeof expected, expected, sizeof expected);
  assert_filled (buffer + sizeof expected, sizeof buffer - sizeof expected, 0xcc);

  /* Input may overlap the header, output payload, or trailer. */
  for (gsize offset = 0; offset <= sizeof expected; offset++)
    {
      memset (buffer, 0xcc, sizeof buffer);
      memcpy (buffer + offset, payload, sizeof payload);
      g_assert_cmpuint (fpi_fte3600_build_firmware (buffer, sizeof expected,
                                                  buffer + offset, sizeof payload,
                                                  &error), ==, sizeof expected);
      g_assert_no_error (error);
      g_assert_cmpmem (buffer, sizeof expected, expected, sizeof expected);
    }

  /* Maximum wire payload differs from maximum image size: firmware's encoded
   * length excludes its six-byte header and one-byte trailer. */
  {
    g_autofree guint8 *maximum = g_malloc (65535);
    g_autofree guint8 *packet = g_malloc (65543);
    const guint8 header[] = { 0x05, 0xfa, 0x00, 0x00, 0xff, 0xff };

    for (gsize i = 0; i < 65535; i++)
      maximum[i] = (i * 37 + 11) & 0xff;
    memset (packet, 0xcc, 65543);
    g_assert_cmpuint (fpi_fte3600_build_firmware (packet, 65542, maximum,
                                                65535, &error), ==, 65542);
    g_assert_no_error (error);
    g_assert_cmpmem (packet, 6, header, 6);
    g_assert_cmpmem (packet + 6, 65535, maximum, 65535);
    g_assert_cmpuint (packet[65541], ==, 0);
    g_assert_cmphex (packet[65542], ==, 0xcc);
  }
}

static void
test_firmware_bounds (void)
{
  const guint8 payload[] = { 0x12, 0x34, 0xab, 0xcd };
  guint8 buffer[16];
  const gsize invalid[] = { 65536, G_MAXSIZE - 6, G_MAXSIZE };
  g_autoptr(GError) error = NULL;

  memset (buffer, 0xcc, sizeof buffer);
  g_assert_cmpuint (fpi_fte3600_build_firmware (buffer, sizeof buffer, NULL, 4, &error), ==, 0);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);
  g_assert_cmpuint (fpi_fte3600_build_firmware (buffer, sizeof buffer, payload, 0, &error), ==, 0);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);
  g_assert_cmpuint (fpi_fte3600_build_firmware (NULL, sizeof buffer, payload, 4, &error), ==, 0);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  for (gsize i = 0; i < G_N_ELEMENTS (invalid); i++)
    {
      g_assert_cmpuint (fpi_fte3600_build_firmware (buffer, sizeof buffer,
                                                  payload, invalid[i], &error), ==, 0);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
      g_clear_error (&error);
    }

  for (gsize capacity = 0; capacity < 11; capacity++)
    {
      g_assert_cmpuint (fpi_fte3600_build_firmware (buffer, capacity, payload, 4, &error), ==, 0);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
      g_clear_error (&error);
    }

  g_assert_cmpuint (fpi_fte3600_build_firmware (buffer, 0, payload, 4, NULL), ==, 0);
  assert_filled (buffer, sizeof buffer, 0xcc);
}

static void
test_command_packets (void)
{
  const struct {
    Fte3600Command command;
    gsize length;
    guint8 packet[11];
  } cases[] = {
    { FTE3600_COMMAND_SOFT_RESET, 1, { 0x70 } },
    { FTE3600_COMMAND_BOOT_PROBE, 3, { 0x90, 0x00, 0x00 } },
    { FTE3600_COMMAND_BOOT_ENTER, 3, { 0x06, 0xf9, 0x00 } },
    { FTE3600_COMMAND_BOOT_SYNC, 2, { 0x55, 0xaa } },
    { FTE3600_COMMAND_FAMILY_QUERY, 11,
      { 0x05, 0xfa, 0x85, 0xc0, 0x00, 0x04, 0x11, 0xee, 0x02, 0x00, 0x00 } },
    { FTE3600_COMMAND_FAMILY_TRIGGER, 4, { 0x09, 0xf6, 0xa4, 0x01 } },
    { FTE3600_COMMAND_FAMILY_READ, 8, { 0x04, 0xfb, 0x85, 0xc0, 0x00, 0x00, 0x00, 0x00 } },
  };

  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      guint8 buffer[16];
      g_autoptr(GError) error = NULL;

      memset (buffer, 0xcc, sizeof buffer);
      for (gsize capacity = 0; capacity < cases[i].length; capacity++)
        {
          g_assert_cmpuint (fpi_fte3600_build_command (buffer, capacity,
                                                     cases[i].command, &error), ==, 0);
          g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
          g_clear_error (&error);
          assert_filled (buffer, sizeof buffer, 0xcc);
        }

      g_assert_cmpuint (fpi_fte3600_build_command (NULL, sizeof buffer,
                                                 cases[i].command, &error), ==, 0);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
      g_clear_error (&error);
      g_assert_cmpuint (fpi_fte3600_build_command (buffer, cases[i].length,
                                                 cases[i].command, &error), ==, cases[i].length);
      g_assert_no_error (error);
      g_assert_cmpmem (buffer, cases[i].length, cases[i].packet, cases[i].length);
      assert_filled (buffer + cases[i].length, sizeof buffer - cases[i].length, 0xcc);
    }
}

static void
test_invalid_commands (void)
{
  const Fte3600Command commands[] = {
    (Fte3600Command) -1, (Fte3600Command) 7, (Fte3600Command) G_MAXINT,
  };
  guint8 buffer[16];
  g_autoptr(GError) error = NULL;

  memset (buffer, 0xcc, sizeof buffer);
  for (gsize i = 0; i < G_N_ELEMENTS (commands); i++)
    {
      g_assert_cmpuint (fpi_fte3600_build_command (buffer, sizeof buffer,
                                                 commands[i], &error), ==, 0);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
      g_clear_error (&error);
    }

  g_assert_cmpuint (fpi_fte3600_build_command (buffer, 0, FTE3600_COMMAND_SOFT_RESET, NULL), ==, 0);
  assert_filled (buffer, sizeof buffer, 0xcc);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/fte3600/protocol/register-packets", test_register_packets);
  g_test_add_func ("/fte3600/protocol/register-bounds", test_register_bounds);
  g_test_add_func ("/fte3600/protocol/image-packets", test_image_packets);
  g_test_add_func ("/fte3600/protocol/image-bounds", test_image_bounds);
  g_test_add_func ("/fte3600/protocol/firmware-packets", test_firmware_packets);
  g_test_add_func ("/fte3600/protocol/firmware-bounds", test_firmware_bounds);
  g_test_add_func ("/fte3600/protocol/command-packets", test_command_packets);
  g_test_add_func ("/fte3600/protocol/invalid-commands", test_invalid_commands);
  return g_test_run ();
}
