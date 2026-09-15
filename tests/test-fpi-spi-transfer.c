/*
 * Unit tests for libfprint SPI transfer handling
 * Copyright (C) 2026 The libfprint authors
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

#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

#include "fpi-spi-transfer.h"
#include "test-device-fake.h"

#define MOCK_SPI_FD 42

typedef enum {
  MOCK_IOCTL_NONE,
  MOCK_IOCTL_FULL_DUPLEX,
  MOCK_IOCTL_SEQUENTIAL,
} MockIoctlMode;

typedef struct
{
  MockIoctlMode mode;
  const guint8 *buffer_wr;
  guint8       *buffer_rd;
  gsize         length_wr;
  gsize         length_rd;
  const guint8 *reply;
  guint         call_count;
  gboolean      has_result_override;
  int           result_override;
} MockIoctlData;

static MockIoctlData mock_ioctl;

int
ioctl (int fd, unsigned long request, ...)
{
  struct spi_ioc_transfer *xfers;
  va_list args;

  g_assert_cmpint (fd, ==, MOCK_SPI_FD);

  va_start (args, request);
  xfers = va_arg (args, struct spi_ioc_transfer *);
  va_end (args);

  g_assert_nonnull (xfers);
  mock_ioctl.call_count++;

  switch (mock_ioctl.mode)
    {
    case MOCK_IOCTL_FULL_DUPLEX:
      g_assert_cmpuint (request, ==, SPI_IOC_MESSAGE (1));
      g_assert_true ((guint8 *) (guintptr) xfers[0].tx_buf == mock_ioctl.buffer_wr);
      g_assert_true ((guint8 *) (guintptr) xfers[0].rx_buf == mock_ioctl.buffer_rd);
      g_assert_cmpuint (xfers[0].len, ==, mock_ioctl.length_wr);
      g_assert_cmpuint (xfers[0].len, ==, mock_ioctl.length_rd);
      g_assert_cmpuint (xfers[0].cs_change, ==, 0);
      memcpy (mock_ioctl.buffer_rd, mock_ioctl.reply, mock_ioctl.length_rd);
      if (mock_ioctl.has_result_override)
        return mock_ioctl.result_override;
      return xfers[0].len;

    case MOCK_IOCTL_SEQUENTIAL:
      g_assert_cmpuint (request, ==, SPI_IOC_MESSAGE (2));
      g_assert_true ((guint8 *) (guintptr) xfers[0].tx_buf == mock_ioctl.buffer_wr);
      g_assert_cmpuint (xfers[0].rx_buf, ==, 0);
      g_assert_cmpuint (xfers[0].len, ==, mock_ioctl.length_wr);
      g_assert_cmpuint (xfers[0].cs_change, ==, 0);
      g_assert_cmpuint (xfers[1].tx_buf, ==, 0);
      g_assert_true ((guint8 *) (guintptr) xfers[1].rx_buf == mock_ioctl.buffer_rd);
      g_assert_cmpuint (xfers[1].len, ==, mock_ioctl.length_rd);
      g_assert_cmpuint (xfers[1].cs_change, ==, 0);
      memcpy (mock_ioctl.buffer_rd, mock_ioctl.reply, mock_ioctl.length_rd);
      return xfers[0].len + xfers[1].len;

    case MOCK_IOCTL_NONE:
    default:
      g_assert_not_reached ();
    }
}

static void
mock_ioctl_reset (MockIoctlMode mode,
                  const guint8 *buffer_wr,
                  gsize         length_wr,
                  guint8       *buffer_rd,
                  gsize         length_rd,
                  const guint8 *reply)
{
  mock_ioctl = (MockIoctlData) {
    .mode = mode,
    .buffer_wr = buffer_wr,
    .buffer_rd = buffer_rd,
    .length_wr = length_wr,
    .length_rd = length_rd,
    .reply = reply,
  };
}

static void
test_full_duplex_sync (void)
{
  guint8 request[] = { 0x04, 0xfb, 0x34, 0x00 };
  const guint8 reply[] = { 0x00, 0x00, 0xa5, 0x5a };
  guint8 response[G_N_ELEMENTS (reply)] = { 0 };
  g_autoptr(FpDevice) device = g_object_new (FPI_TYPE_DEVICE_FAKE, NULL);
  g_autoptr(FpiSpiTransfer) transfer = NULL;
  g_autoptr(GError) error = NULL;

  mock_ioctl_reset (MOCK_IOCTL_FULL_DUPLEX,
                    request, sizeof (request),
                    response, sizeof (response),
                    reply);

  transfer = fpi_spi_transfer_new (device, MOCK_SPI_FD);
  fpi_spi_transfer_write_full (transfer, request, sizeof (request), NULL);
  fpi_spi_transfer_read_full (transfer, response, sizeof (response), NULL);
  fpi_spi_transfer_set_full_duplex (transfer, TRUE);

  g_assert_true (fpi_spi_transfer_submit_sync (transfer, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (mock_ioctl.call_count, ==, 1);
  g_assert_cmpmem (response, sizeof (response), reply, sizeof (reply));
}

typedef struct
{
  GMainLoop *loop;
  gboolean   called;
} AsyncTransferData;

static void
full_duplex_async_cb (FpiSpiTransfer *transfer,
                      FpDevice       *device,
                      gpointer        user_data,
                      GError         *error)
{
  AsyncTransferData *data = user_data;

  g_assert_nonnull (transfer);
  g_assert_true (FP_IS_DEVICE (device));
  g_assert_no_error (error);

  data->called = TRUE;
  g_main_loop_quit (data->loop);
}

static void
test_full_duplex_async (void)
{
  guint8 request[] = { 0x10, 0xef, 0x20, 0x00 };
  const guint8 reply[] = { 0x00, 0x00, 0xa5, 0x5a };
  guint8 response[G_N_ELEMENTS (reply)] = { 0 };
  g_autoptr(FpDevice) device = g_object_new (FPI_TYPE_DEVICE_FAKE, NULL);
  g_autoptr(FpiSpiTransfer) transfer = NULL;
  g_autoptr(GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  AsyncTransferData data = { .loop = loop };

  mock_ioctl_reset (MOCK_IOCTL_FULL_DUPLEX,
                    request, sizeof (request),
                    response, sizeof (response),
                    reply);

  transfer = fpi_spi_transfer_new (device, MOCK_SPI_FD);
  fpi_spi_transfer_write_full (transfer, request, sizeof (request), NULL);
  fpi_spi_transfer_read_full (transfer, response, sizeof (response), NULL);
  fpi_spi_transfer_set_full_duplex (transfer, TRUE);
  fpi_spi_transfer_submit (g_steal_pointer (&transfer),
                           NULL,
                           full_duplex_async_cb,
                           &data);

  g_main_loop_run (loop);
  g_assert_true (data.called);
  g_assert_cmpuint (mock_ioctl.call_count, ==, 1);
  g_assert_cmpmem (response, sizeof (response), reply, sizeof (reply));
}

static void
test_full_duplex_short_transfer (void)
{
  guint8 request[] = { 0x04, 0xfb, 0x34, 0x00 };
  const guint8 reply[] = { 0x00, 0x00, 0xa5, 0x5a };
  guint8 response[G_N_ELEMENTS (reply)] = { 0 };
  g_autoptr(FpDevice) device = g_object_new (FPI_TYPE_DEVICE_FAKE, NULL);
  g_autoptr(FpiSpiTransfer) transfer = NULL;
  g_autoptr(GError) error = NULL;

  mock_ioctl_reset (MOCK_IOCTL_FULL_DUPLEX,
                    request, sizeof (request),
                    response, sizeof (response),
                    reply);
  mock_ioctl.has_result_override = TRUE;
  mock_ioctl.result_override = sizeof (request) - 1;

  transfer = fpi_spi_transfer_new (device, MOCK_SPI_FD);
  fpi_spi_transfer_write_full (transfer, request, sizeof (request), NULL);
  fpi_spi_transfer_read_full (transfer, response, sizeof (response), NULL);
  fpi_spi_transfer_set_full_duplex (transfer, TRUE);

  g_assert_false (fpi_spi_transfer_submit_sync (transfer, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT);
  g_assert_cmpuint (mock_ioctl.call_count, ==, 1);
}

static void
test_full_duplex_unequal_lengths (void)
{
  guint8 request[2] = { 0 };
  guint8 response[3] = { 0 };
  g_autoptr(FpDevice) device = g_object_new (FPI_TYPE_DEVICE_FAKE, NULL);
  g_autoptr(FpiSpiTransfer) transfer = NULL;
  g_autoptr(GError) error = NULL;

  mock_ioctl_reset (MOCK_IOCTL_NONE, NULL, 0, NULL, 0, NULL);

  transfer = fpi_spi_transfer_new (device, MOCK_SPI_FD);
  fpi_spi_transfer_write_full (transfer, request, sizeof (request), NULL);
  fpi_spi_transfer_read_full (transfer, response, sizeof (response), NULL);
  fpi_spi_transfer_set_full_duplex (transfer, TRUE);

  g_assert_false (fpi_spi_transfer_submit_sync (transfer, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpuint (mock_ioctl.call_count, ==, 0);
}

static void
test_full_duplex_too_large (void)
{
  const gsize oversize = G_MAXUINT16 + 1;
  g_autofree guint8 *request = g_malloc0 (oversize);
  g_autofree guint8 *response = g_malloc0 (oversize);
  g_autoptr(FpDevice) device = g_object_new (FPI_TYPE_DEVICE_FAKE, NULL);
  g_autoptr(FpiSpiTransfer) transfer = NULL;
  g_autoptr(GError) error = NULL;

  mock_ioctl_reset (MOCK_IOCTL_NONE, NULL, 0, NULL, 0, NULL);

  transfer = fpi_spi_transfer_new (device, MOCK_SPI_FD);
  fpi_spi_transfer_write_full (transfer, request, oversize, NULL);
  fpi_spi_transfer_read_full (transfer, response, oversize, NULL);
  fpi_spi_transfer_set_full_duplex (transfer, TRUE);

  g_assert_false (fpi_spi_transfer_submit_sync (transfer, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
  g_assert_cmpuint (mock_ioctl.call_count, ==, 0);
}

static void
test_sequential_unchanged (void)
{
  guint8 request[] = { 0x10, 0xef };
  const guint8 reply[] = { 0xa5, 0x5a, 0x00 };
  guint8 response[G_N_ELEMENTS (reply)] = { 0 };
  g_autoptr(FpDevice) device = g_object_new (FPI_TYPE_DEVICE_FAKE, NULL);
  g_autoptr(FpiSpiTransfer) transfer = NULL;
  g_autoptr(GError) error = NULL;

  mock_ioctl_reset (MOCK_IOCTL_SEQUENTIAL,
                    request, sizeof (request),
                    response, sizeof (response),
                    reply);

  transfer = fpi_spi_transfer_new (device, MOCK_SPI_FD);
  fpi_spi_transfer_write_full (transfer, request, sizeof (request), NULL);
  fpi_spi_transfer_read_full (transfer, response, sizeof (response), NULL);

  g_assert_true (fpi_spi_transfer_submit_sync (transfer, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (mock_ioctl.call_count, ==, 1);
  g_assert_cmpmem (response, sizeof (response), reply, sizeof (reply));
}

static void
test_sensitive_log_redaction (void)
{
  if (g_test_subprocess ())
    {
      guint8 request[] = { 0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe };
      const guint8 reply[] = { 0xba, 0xad, 0xf0, 0x0d, 0x12, 0x34 };
      guint8 response[G_N_ELEMENTS (reply)] = { 0 };
      g_autoptr(FpDevice) device =
          g_object_new (FPI_TYPE_DEVICE_FAKE, NULL);
      g_autoptr(FpiSpiTransfer) transfer = NULL;
      g_autoptr(GError) error = NULL;

      g_setenv ("FP_DEBUG_TRANSFER", "1", TRUE);
      g_setenv ("G_MESSAGES_DEBUG", "all", TRUE);
      mock_ioctl_reset (MOCK_IOCTL_FULL_DUPLEX,
                        request, sizeof (request),
                        response, sizeof (response),
                        reply);

      transfer = fpi_spi_transfer_new (device, MOCK_SPI_FD);
      fpi_spi_transfer_write_full (transfer, request, sizeof (request), NULL);
      fpi_spi_transfer_read_full (transfer, response, sizeof (response), NULL);
      fpi_spi_transfer_set_full_duplex (transfer, TRUE);
      fpi_spi_transfer_set_sensitive (transfer, TRUE);

      g_assert_true (fpi_spi_transfer_submit_sync (transfer, &error));
      g_assert_no_error (error);
      return;
    }

  g_test_trap_subprocess (NULL, 0, (GTestSubprocessFlags) 0);
  g_test_trap_assert_passed ();
  g_test_trap_assert_stdout ("*buffer contents redacted (sensitive)*");
  g_test_trap_assert_stdout_unmatched ("*deadbeefcafe*");
  g_test_trap_assert_stdout_unmatched ("*baadf00d1234*");
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/spi-transfer/full-duplex/sync", test_full_duplex_sync);
  g_test_add_func ("/spi-transfer/full-duplex/async", test_full_duplex_async);
  g_test_add_func ("/spi-transfer/full-duplex/short-transfer", test_full_duplex_short_transfer);
  g_test_add_func ("/spi-transfer/full-duplex/unequal-lengths", test_full_duplex_unequal_lengths);
  g_test_add_func ("/spi-transfer/full-duplex/too-large", test_full_duplex_too_large);
  g_test_add_func ("/spi-transfer/sequential/unchanged", test_sequential_unchanged);
  g_test_add_func ("/spi-transfer/log/sensitive-redaction", test_sensitive_log_redaction);

  return g_test_run ();
}
