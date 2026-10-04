/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Native identification callback boundaries; no device or GPIO is opened.
 */
#include <stdarg.h>

int medion_cli_main (int argc, char **argv);
#define main medion_cli_main
#include "../examples/fte3600-medion.c"
#undef main

enum { MOCK_SPI_FD = 19 };

typedef struct
{
  const guint8 *tx;
  gsize length;
  guint calls;
  gint result;
  gint failure_errno;
} ExchangeMock;

static ExchangeMock exchange_mock;

int __wrap_ioctl (int fd, unsigned long request, ...);

int
__wrap_ioctl (int fd, unsigned long request, ...)
{
  va_list args;
  struct spi_ioc_transfer *transfer;
  guint8 *rx;

  va_start (args, request);
  transfer = va_arg (args, struct spi_ioc_transfer *);
  va_end (args);

  exchange_mock.calls++;
  g_assert_cmpint (fd, ==, MOCK_SPI_FD);
  g_assert_cmpuint (request, ==, SPI_IOC_MESSAGE (1));
  g_assert_cmpuint (transfer->len, ==, exchange_mock.length);
  g_assert_true ((const guint8 *) (uintptr_t) transfer->tx_buf == exchange_mock.tx);
  g_assert_cmpuint (transfer->rx_buf, !=, 0);
  g_assert_cmpuint (transfer->cs_change, ==, 0);
  /* Zero means use the adapter's validated per-device defaults. A callback
   * must not silently replace a lower configured SPI speed with 1 MHz. */
  g_assert_cmpuint (transfer->speed_hz, ==, 0);
  g_assert_cmpuint (transfer->bits_per_word, ==, 0);
  g_assert_cmpuint (transfer->delay_usecs, ==, 0);
  g_assert_cmpuint (transfer->tx_nbits, ==, 0);
  g_assert_cmpuint (transfer->rx_nbits, ==, 0);

  rx = (guint8 *) (uintptr_t) transfer->rx_buf;
  for (gsize i = 0; i < exchange_mock.length; i++)
    {
      g_assert_cmpuint (rx[i], ==, 0);
      rx[i] = 0xa0 + i;
    }
  if (exchange_mock.result < 0)
    errno = exchange_mock.failure_errno;
  return exchange_mock.result;
}

static void
test_exchange (gconstpointer data)
{
  gboolean discard_reply = GPOINTER_TO_INT (data);
  FpiDeviceFte3600 device = { .spi_fd = MOCK_SPI_FD, .max_transfer = 64 };
  IdentifyIo io = { .device = &device };
  const guint8 tx[] = { 0x10, 0xef, 0x20, 0, 0, 0 };
  guint8 rx[sizeof tx];
  g_autoptr(GError) error = NULL;

  memset (rx, 0xff, sizeof rx);
  exchange_mock = (ExchangeMock){ .tx = tx, .length = sizeof tx, .result = sizeof tx };
  g_assert_true (identify_exchange (&io, tx, discard_reply ? NULL : rx, sizeof tx, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (exchange_mock.calls, ==, 1);
  for (gsize i = 0; i < sizeof rx; i++)
    g_assert_cmpuint (rx[i], ==, discard_reply ? 0xff : 0xa0 + i);
}

static void
test_transfer_failure (gconstpointer data)
{
  gint syscall_errno = GPOINTER_TO_INT (data);
  FpiDeviceFte3600 device = { .spi_fd = MOCK_SPI_FD, .max_transfer = 64 };
  IdentifyIo io = { .device = &device };
  const guint8 tx[] = { 0x09, 0xf6, 0xf4, 0 };
  g_autoptr(GError) error = NULL;

  exchange_mock = (ExchangeMock){
    .tx = tx, .length = sizeof tx, .result = -1, .failure_errno = syscall_errno,
  };
  g_assert_false (identify_exchange (&io, tx, NULL, sizeof tx, &error));
  g_assert_error (error, G_IO_ERROR, g_io_error_from_errno (syscall_errno));
  /* A failure, including EINTR, must never replay a potentially delivered write. */
  g_assert_cmpuint (exchange_mock.calls, ==, 1);
}

static void
test_transfer_length_mismatch (gconstpointer data)
{
  FpiDeviceFte3600 device = { .spi_fd = MOCK_SPI_FD, .max_transfer = 64 };
  IdentifyIo io = { .device = &device };
  const guint8 tx[] = { 0x08, 0xf7, 0xf3, 0 };
  g_autoptr(GError) error = NULL;

  exchange_mock = (ExchangeMock){
    .tx = tx, .length = sizeof tx, .result = GPOINTER_TO_INT (data),
  };
  g_assert_false (identify_exchange (&io, tx, NULL, sizeof tx, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT);
  g_assert_cmpuint (exchange_mock.calls, ==, 1);
}

static void
test_invalid_request (void)
{
  FpiDeviceFte3600 device = { .spi_fd = MOCK_SPI_FD, .max_transfer = 4 };
  IdentifyIo io = { .device = &device };
  const guint8 tx[5] = { 0 };
  const struct { const guint8 *tx; gsize length; } cases[] = {
    { NULL, 4 }, { tx, 0 }, { tx, 5 }, { tx, G_MAXSIZE },
  };

  exchange_mock = (ExchangeMock){ 0 };
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autoptr(GError) error = NULL;

      g_assert_false (identify_exchange (&io, cases[i].tx, NULL, cases[i].length, &error));
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
      g_assert_cmpuint (exchange_mock.calls, ==, 0);
    }
}

typedef struct
{
  GCancellable *cancellable;
  guint calls;
} PendingCancel;

static gboolean
cancel_once (gpointer user_data)
{
  PendingCancel *pending = user_data;

  pending->calls++;
  g_cancellable_cancel (pending->cancellable);
  return G_SOURCE_REMOVE;
}

static void
test_cancel_dispatch (void)
{
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  IdentifyIo io = { .cancellable = cancellable };
  PendingCancel pending = { .cancellable = cancellable };
  g_autoptr(GError) error = NULL;

  g_assert_false (identify_cancelled (&io, &error));
  g_assert_no_error (error);
  g_idle_add (cancel_once, &pending);
  g_assert_false (g_cancellable_is_cancelled (cancellable));
  g_assert_true (identify_cancelled (&io, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint (pending.calls, ==, 1);
}

static void
test_wait_finishes_pulse (void)
{
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  IdentifyIo io = { .cancellable = cancellable };
  PendingCancel pending = { .cancellable = cancellable };
  g_autoptr(GError) error = NULL;
  gint64 started;

  g_idle_add (cancel_once, &pending);
  started = g_get_monotonic_time ();
  g_assert_true (identify_wait (&io, 20, &error));
  /* Cancellation must be delivered, but cannot truncate an active reset pulse. */
  g_assert_cmpint (g_get_monotonic_time () - started, >=, 20000);
  g_assert_no_error (error);
  g_assert_cmpuint (pending.calls, ==, 1);
  g_assert_true (g_cancellable_is_cancelled (cancellable));
  g_assert_true (identify_cancelled (&io, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_data_func ("/medion-identify-io/exchange/read", GINT_TO_POINTER (FALSE), test_exchange);
  g_test_add_data_func ("/medion-identify-io/exchange/discard", GINT_TO_POINTER (TRUE), test_exchange);
  g_test_add_data_func ("/medion-identify-io/error/interrupted", GINT_TO_POINTER (EINTR), test_transfer_failure);
  g_test_add_data_func ("/medion-identify-io/error/oversized", GINT_TO_POINTER (EMSGSIZE), test_transfer_failure);
  g_test_add_data_func ("/medion-identify-io/error/io", GINT_TO_POINTER (EIO), test_transfer_failure);
  g_test_add_data_func ("/medion-identify-io/length/zero", GINT_TO_POINTER (0), test_transfer_length_mismatch);
  g_test_add_data_func ("/medion-identify-io/length/short", GINT_TO_POINTER (3), test_transfer_length_mismatch);
  g_test_add_data_func ("/medion-identify-io/length/long", GINT_TO_POINTER (5), test_transfer_length_mismatch);
  g_test_add_func ("/medion-identify-io/invalid-request", test_invalid_request);
  g_test_add_func ("/medion-identify-io/cancel-dispatch", test_cancel_dispatch);
  g_test_add_func ("/medion-identify-io/wait-finishes-pulse", test_wait_finishes_pulse);
  return g_test_run ();
}
