/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Native identification callback boundaries; no device or GPIO is opened.
 */
#include <stdarg.h>

int medion_cli_main (int    argc,
                     char **argv);
#define main medion_cli_main
#include "../examples/fte3600-medion.c"
#undef main

enum { MOCK_SPI_FD = 19 };

typedef struct
{
  const guint8 *tx;
  gsize         length;
  guint         calls;
  gint          result;
  gint          failure_errno;
  guint         guard_calls;
  guint         guard_fail_at;
  gboolean      reset_sequence;
  gchar         events[16];
  guint         events_length;
  guint         reset_calls;
  guint         reset_fail_at;
  guint         reset_fail_mask;
  gint64        reset_times[3];
  GCancellable *cancel_on_assert;
  guint         pending_at_release;
  guint         pending_calls;
} ExchangeMock;

static ExchangeMock exchange_mock;

static void
record_event (gchar event)
{
  if (!exchange_mock.reset_sequence)
    return;
  g_assert_cmpuint (exchange_mock.events_length + 1, <, sizeof exchange_mock.events);
  exchange_mock.events[exchange_mock.events_length++] = event;
}

static gboolean
pending_after_release (gpointer user_data)
{
  exchange_mock.pending_calls++;
  return G_SOURCE_REMOVE;
}

static void
unexpected_reset_print (const gchar *message)
{
  record_event ('P');
}

static gboolean
set_reset (FpiDeviceFte3600 *self, gboolean asserted, GError **error)
{
  guint call = ++exchange_mock.reset_calls;

  record_event (asserted ? 'L' : 'H');
  g_assert_cmpuint (call, <=, G_N_ELEMENTS (exchange_mock.reset_times));
  exchange_mock.reset_times[call - 1] = g_get_monotonic_time ();
  if (asserted && exchange_mock.cancel_on_assert)
    g_cancellable_cancel (exchange_mock.cancel_on_assert);
  if (!asserted && call > 1 && exchange_mock.pending_at_release)
    exchange_mock.pending_at_release = g_idle_add (pending_after_release, NULL);
  if (call == exchange_mock.reset_fail_at || (exchange_mock.reset_fail_mask & (1U << call)))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Injected GPIO failure %u", call);
      return FALSE;
    }
  return TRUE;
}

static gboolean
check_session (FpiDeviceFte3600 *self, GError **error)
{
  record_event ('G');
  if (++exchange_mock.guard_calls == exchange_mock.guard_fail_at)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
                           "Injected session invalidation");
      return FALSE;
    }
  return TRUE;
}

static gboolean
close_session (FpiDeviceFte3600 *self, GError **error)
{
  self->spi_fd = -1;
  return TRUE;
}

static FpiDeviceFte3600 *
new_device (guint32 max_transfer)
{
  static const Fte3600TransportOps ops = {
    .check = check_session,
    .close = close_session,
    .set_reset = set_reset,
  };
  FpiDeviceFte3600 *device = g_object_new (fpi_device_fte3600_get_type (), NULL);

  device->spi_fd = MOCK_SPI_FD;
  device->max_transfer = max_transfer;
  device->transport_ops = &ops;
  return device;
}

int __wrap_ioctl (int           fd,
                  unsigned long request,
                  ...);

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
  record_event ('S');
  g_assert_cmpint (fd, ==, MOCK_SPI_FD);
  g_assert_cmpuint (request, ==, SPI_IOC_MESSAGE (1));
  g_assert_cmpuint (transfer->len, ==, exchange_mock.length);
  if (exchange_mock.reset_sequence)
    {
      g_assert_cmpmem ((const guint8 *) (uintptr_t) transfer->tx_buf, transfer->len,
                       exchange_mock.tx, exchange_mock.length);
      g_assert_cmpuint (exchange_mock.guard_calls, ==, 1);
      g_assert_cmpuint (exchange_mock.pending_calls, ==, 0);
      g_assert_cmpstr (exchange_mock.events, ==, "GHLHS");
    }
  else
    {
      g_assert_true ((const guint8 *) (uintptr_t) transfer->tx_buf == exchange_mock.tx);
    }
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

  g_autoptr(FpiDeviceFte3600) device = new_device (64);
  IdentifyIo io = { .device = device };
  const guint8 tx[] = { 0x10, 0xef, 0x20, 0, 0, 0 };
  guint8 rx[sizeof tx];

  g_autoptr(GError) error = NULL;

  memset (rx, 0xff, sizeof rx);
  exchange_mock = (ExchangeMock){ .tx = tx, .length = sizeof tx, .result = sizeof tx };
  g_assert_true (identify_exchange (&io, tx, discard_reply ? NULL : rx, sizeof tx, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (exchange_mock.calls, ==, 1);
  g_assert_cmpuint (exchange_mock.guard_calls, ==, 2);
  for (gsize i = 0; i < sizeof rx; i++)
    g_assert_cmpuint (rx[i], ==, discard_reply ? 0xff : 0xa0 + i);
}

static void
test_transfer_failure (gconstpointer data)
{
  gint syscall_errno = GPOINTER_TO_INT (data);

  g_autoptr(FpiDeviceFte3600) device = new_device (64);
  IdentifyIo io = { .device = device };
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
  g_autoptr(FpiDeviceFte3600) device = new_device (64);
  IdentifyIo io = { .device = device };
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
test_wake_transfer (void)
{
  g_autoptr(FpiDeviceFte3600) device = new_device (64);
  IdentifyIo io = { .device = device };
  const guint8 tx[] = { 0x70 };

  for (gint result = -1; result <= 1; result++)
    {
      guint8 rx[sizeof tx];
      g_autoptr(GError) error = NULL;

      exchange_mock = (ExchangeMock){
        .tx = tx, .length = sizeof tx, .result = result, .failure_errno = EINTR,
      };
      if (result == 1)
        {
          g_assert_true (identify_exchange (&io, tx, rx, sizeof tx, &error));
          g_assert_no_error (error);
        }
      else
        {
          g_assert_false (identify_exchange (&io, tx, rx, sizeof tx, &error));
          g_assert_error (error, G_IO_ERROR,
                          (result == 0 ? G_IO_ERROR_PARTIAL_INPUT : g_io_error_from_errno (EINTR)));
        }
      /* A single-byte wake is full duplex too: zero is a short transfer, and
       * EINTR never replays a command that may already have reached hardware. */
      g_assert_cmpuint (exchange_mock.calls, ==, 1);
    }
}

static void
test_invalid_request (void)
{
  g_autoptr(FpiDeviceFte3600) device = new_device (4);
  IdentifyIo io = { .device = device };
  const guint8 tx[5] = { 0 };

  const struct { const guint8 *tx;
                 gsize         length;
  } cases[] = {
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
  guint         calls;
} PendingCancel;

static void
test_exchange_guard (gconstpointer data)
{
  g_autoptr(FpiDeviceFte3600) device = new_device (64);
  IdentifyIo io = { .device = device };
  const guint8 tx[] = { 0x70 };
  guint fail_at = GPOINTER_TO_UINT (data);
  g_autoptr(GError) error = NULL;

  exchange_mock = (ExchangeMock){
    .tx = tx, .length = sizeof tx, .result = sizeof tx, .guard_fail_at = fail_at,
  };
  g_assert_false (identify_exchange (&io, tx, NULL, sizeof tx, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE);
  g_assert_cmpuint (exchange_mock.guard_calls, ==, fail_at);
  g_assert_cmpuint (exchange_mock.calls, ==, fail_at - 1);
}

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

static void
test_reset_and_sync (gconstpointer data)
{
  gboolean cancelled = GPOINTER_TO_INT (data);

  g_autoptr(FpiDeviceFte3600) device = new_device (64);
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  IdentifyIo io = { .device = device, .cancellable = cancellable };
  const guint8 tx[] = { 0x55, 0xaa };
  g_autoptr(GError) error = NULL;

  exchange_mock = (ExchangeMock){
    .tx = tx, .length = sizeof tx, .result = sizeof tx,
    .reset_sequence = TRUE, .pending_at_release = 1,
    .cancel_on_assert = cancelled ? cancellable : NULL,
  };
  GPrintFunc previous_print = g_set_print_handler (unexpected_reset_print);
  gboolean success = identify_reset_and_sync (&io, &error);
  g_set_print_handler (previous_print);
  g_assert_true (success);
  g_assert_no_error (error);
  g_assert_cmpstr (exchange_mock.events, ==, "GHLHSG");
  g_assert_cmpuint (exchange_mock.calls, ==, 1);
  g_assert_cmpuint (exchange_mock.reset_calls, ==, 3);
  g_assert_cmpint (exchange_mock.reset_times[1] - exchange_mock.reset_times[0], >=, 10000);
  g_assert_cmpint (exchange_mock.reset_times[2] - exchange_mock.reset_times[1], >=, 20000);
  /* This source was queued by final release. Only an explicit subsequent
   * dispatch may run it; no guard or dispatcher can intervene before SPI. */
  g_assert_cmpuint (exchange_mock.pending_calls, ==, 0);
  dispatch_signals ();
  g_assert_cmpuint (exchange_mock.pending_calls, ==, 1);
  g_assert_cmpint (g_cancellable_is_cancelled (cancellable), ==, cancelled);
  if (cancelled)
    {
      g_assert_true (identify_cancelled (&io, &error));
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    }
}

static void
test_reset_gpio_failure (gconstpointer data)
{
  guint fail_at = GPOINTER_TO_UINT (data);

  g_autoptr(FpiDeviceFte3600) device = new_device (64);
  IdentifyIo io = { .device = device };
  g_autoptr(GError) error = NULL;

  exchange_mock = (ExchangeMock){ .reset_sequence = TRUE, .reset_fail_at = fail_at };
  g_assert_false (identify_reset_and_sync (&io, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpuint (exchange_mock.calls, ==, 0);
  g_assert_cmpuint (exchange_mock.guard_calls, ==, 1);
  g_assert_cmpstr (exchange_mock.events, ==, fail_at == 1 ? "GHH" : "GHLH");
  if (fail_at != 1)
    g_assert_cmpint (exchange_mock.reset_times[2] - exchange_mock.reset_times[1], >=, 20000);
}

static void
test_reset_guard_failure (gconstpointer data)
{
  guint fail_at = GPOINTER_TO_UINT (data);

  g_autoptr(FpiDeviceFte3600) device = new_device (64);
  IdentifyIo io = { .device = device };
  const guint8 tx[] = { 0x55, 0xaa };
  g_autoptr(GError) error = NULL;

  exchange_mock = (ExchangeMock){
    .tx = tx, .length = sizeof tx, .result = sizeof tx,
    .reset_sequence = TRUE, .guard_fail_at = fail_at,
  };
  g_assert_false (identify_reset_and_sync (&io, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE);
  g_assert_cmpstr (exchange_mock.events, ==, fail_at == 1 ? "G" : "GHLHSG");
  g_assert_cmpuint (exchange_mock.calls, ==, fail_at - 1);
}

static void
test_reset_cleanup_failure (void)
{
  g_autoptr(FpiDeviceFte3600) device = new_device (64);
  IdentifyIo io = { .device = device };
  g_autoptr(GError) error = NULL;

  exchange_mock = (ExchangeMock){ .reset_sequence = TRUE, .reset_fail_mask = (1U << 2) | (1U << 3) };
  g_assert_false (identify_reset_and_sync (&io, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull (strstr (error->message, "GPIO failure 2"));
  g_assert_nonnull (strstr (error->message, "GPIO failure 3"));
  g_assert_cmpuint (exchange_mock.calls, ==, 0);
  g_assert_cmpstr (exchange_mock.events, ==, "GHLH");
  g_assert_cmpint (exchange_mock.reset_times[2] - exchange_mock.reset_times[1], >=, 20000);
}

static void
test_reset_spi_failure (gconstpointer data)
{
  gint result = GPOINTER_TO_INT (data);

  g_autoptr(FpiDeviceFte3600) device = new_device (64);
  IdentifyIo io = { .device = device };
  const guint8 tx[] = { 0x55, 0xaa };
  g_autoptr(GError) error = NULL;

  exchange_mock = (ExchangeMock){
    .tx = tx, .length = sizeof tx, .result = result,
    .reset_sequence = TRUE, .failure_errno = EINTR,
  };
  g_assert_false (identify_reset_and_sync (&io, &error));
  g_assert_error (error, G_IO_ERROR,
                  (result < 0 ? g_io_error_from_errno (EINTR) : G_IO_ERROR_PARTIAL_INPUT));
  g_assert_cmpstr (exchange_mock.events, ==, "GHLHS");
  g_assert_cmpuint (exchange_mock.calls, ==, 1);
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
  g_test_add_func ("/medion-identify-io/wake-transfer", test_wake_transfer);
  g_test_add_func ("/medion-identify-io/invalid-request", test_invalid_request);
  g_test_add_data_func ("/medion-identify-io/guard/before", GUINT_TO_POINTER (1), test_exchange_guard);
  g_test_add_data_func ("/medion-identify-io/guard/after", GUINT_TO_POINTER (2), test_exchange_guard);
  g_test_add_func ("/medion-identify-io/cancel-dispatch", test_cancel_dispatch);
  g_test_add_func ("/medion-identify-io/wait-finishes-pulse", test_wait_finishes_pulse);
  g_test_add_data_func ("/medion-identify-io/reset-sync/success", GINT_TO_POINTER (FALSE), test_reset_and_sync);
  g_test_add_data_func ("/medion-identify-io/reset-sync/cancel", GINT_TO_POINTER (TRUE), test_reset_and_sync);
  for (guint i = 1; i <= 3; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/medion-identify-io/reset-sync/gpio-failure/%u", i);
      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_reset_gpio_failure);
    }
  g_test_add_data_func ("/medion-identify-io/reset-sync/guard-before", GUINT_TO_POINTER (1), test_reset_guard_failure);
  g_test_add_data_func ("/medion-identify-io/reset-sync/guard-after", GUINT_TO_POINTER (2), test_reset_guard_failure);
  g_test_add_func ("/medion-identify-io/reset-sync/cleanup-failure", test_reset_cleanup_failure);
  g_test_add_data_func ("/medion-identify-io/reset-sync/interrupted", GINT_TO_POINTER (-1), test_reset_spi_failure);
  g_test_add_data_func ("/medion-identify-io/reset-sync/short", GINT_TO_POINTER (1), test_reset_spi_failure);
  g_test_add_data_func ("/medion-identify-io/reset-sync/long", GINT_TO_POINTER (3), test_reset_spi_failure);
  return g_test_run ();
}
