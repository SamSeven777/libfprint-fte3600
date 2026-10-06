/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Synthetic protocol fixture, with no vendor firmware or biometric input.
 */
#include "examples/fte3600-medion-ft9338.h"

#include <string.h>

#define FIRMWARE_SIZE 14184u
#define TRANSFER_SIZE 14192u

typedef struct
{
  Fte3600MedionIdentifyIo io;
  GBytes                 *firmware;
  GString                *events;
  GString                *reports;
  guint16                 family;
  guint16                 geometry;
  guint8                  marker;
  guint8                  otp;
  guint8                  fe;
  guint8                  config_marker;
  guint8                  firmware_version;
  guint8                  agc_version;
  guint                   exchanges;
  guint                   uploads;
  guint                   readbacks;
  guint                   syncs;
  guint                   assertions;
  guint                   releases;
  guint                   disables;
  guint                   status_reads;
  guint                   config_writes;
  guint                   busy_polls;
  guint                   failed_polls;
  guint                   waits_2;
  GIOErrorEnum            poll_error;
  gboolean                asserted;
  gboolean                otp_enabled;
  gboolean                cancelled;
  gboolean                cancel_enable;
  gboolean                cancel_start;
  gboolean                cancel_readback;
  gboolean                fail_enable;
  gboolean                fail_assertion;
  gboolean                fail_release;
  gboolean                fail_sync;
  gboolean                corrupt_readback;
  gboolean                omit_callback_error;
} Fixture;

static gboolean
injected (Fixture *f, GIOErrorEnum code, GError **error)
{
  if (!f->omit_callback_error)
    g_set_error_literal (error, G_IO_ERROR, code, "injected failure");
  return FALSE;
}

static gboolean
mock_exchange (gpointer user_data, const guint8 *tx, guint8 *rx,
               gsize length, GError **error)
{
  Fixture *f = user_data;
  const guint8 *firmware = g_bytes_get_data (f->firmware, NULL);

  f->exchanges++;
  g_assert_false (f->asserted);
  g_assert_cmpuint (length, <=, TRANSFER_SIZE);
  for (gsize i = 0; i < length; i++)
    g_assert_cmphex (rx[i], ==, 0);

  if (length > 100)
    {
      if (tx[0] == 0x05)
        {
          const guint8 header[] = { 0x05, 0xfa, 0, 0, 0x37, 0x68 };

          g_assert_cmpuint (length, ==, FIRMWARE_SIZE + 7);
          g_assert_cmpmem (tx, sizeof header, header, sizeof header);
          g_assert_cmpmem (tx + 6, FIRMWARE_SIZE, firmware, FIRMWARE_SIZE);
          g_assert_cmphex (tx[length - 1], ==, 0);
          g_assert_false (f->otp_enabled);
          f->uploads++;
          g_string_append (f->events, "UPLOAD;");
          return TRUE;
        }
      else
        {
          const guint8 header[] = { 0x04, 0xfb, 0, 0, 0x37, 0x70 };

          g_assert_cmpuint (length, ==, TRANSFER_SIZE);
          g_assert_cmpmem (tx, sizeof header, header, sizeof header);
          for (gsize i = sizeof header; i < length; i++)
            g_assert_cmphex (tx[i], ==, 0);
          g_assert_cmpuint (f->uploads, ==, 1);
          memcpy (rx + 6, firmware, FIRMWARE_SIZE);
          if (f->corrupt_readback)
            rx[6 + FIRMWARE_SIZE - 1] ^= 1;
          /* Trailing response bytes are outside the firmware payload. */
          rx[length - 2] = 0x12;
          rx[length - 1] = 0x34;
          f->readbacks++;
          f->cancelled |= f->cancel_readback;
          g_string_append (f->events, "READBACK;");
          return TRUE;
        }
    }

  g_string_append_c (f->events, '[');
  for (gsize i = 0; i < length; i++)
    g_string_append_printf (f->events, "%02x", tx[i]);
  g_string_append (f->events, "];");

  switch (tx[0])
    {
    case 0x90:
      {
        const guint8 packet[] = { 0x90, 0, 0 };

        g_assert_cmpmem (tx, length, packet, sizeof packet);
        rx[2] = f->marker;
        return TRUE;
      }

    case 0x06:
      {
        const guint8 packet[] = { 0x06, 0xf9, 0 };

        g_assert_cmpmem (tx, length, packet, sizeof packet);
        return TRUE;
      }

    case 0x05:
      {
        const guint8 packet[] = { 0x05, 0xfa, 0x85, 0xc0, 0, 4, 0x11, 0xee, 2, 0, 0 };

        g_assert_cmpmem (tx, length, packet, sizeof packet);
        return TRUE;
      }

    case 0x04:
      {
        const guint8 packet[] = { 0x04, 0xfb, 0x85, 0xc0, 0, 0, 0, 0 };

        g_assert_cmpmem (tx, length, packet, sizeof packet);
        rx[6] = f->family >> 8;
        rx[7] = f->family & 0xff;
        return TRUE;
      }

    case 0x08:
      g_assert_cmpuint (length, ==, 4);
      g_assert_cmphex (tx[1], ==, 0xf7);
      g_assert_cmphex (tx[3], ==, 0);
      /* Nonzero response header makes an offset error observable. */
      rx[0] = 0x91;
      rx[1] = 0x82;
      rx[2] = 0x73;
      switch (tx[2])
        {
        case 0xc8: rx[3] = 0xa0;
          break;

        case 0xcb: rx[3] = 0x40;
          break;

        case 0xfe: rx[3] = f->fe;
          break;

        case 0xf4: rx[3] = 0x40;
          break;

        case 0xf3:
          g_assert_true (f->otp_enabled);
          rx[3] = f->otp;
          break;

        default: g_assert_not_reached ();
        }
      return TRUE;

    case 0x09:
      g_assert_cmpuint (length, ==, 4);
      g_assert_cmphex (tx[1], ==, 0xf6);
      if (tx[2] == 0xf4)
        {
          f->otp_enabled = tx[3] != 0;
          if (f->otp_enabled)
            {
              g_assert_cmphex (tx[3], ==, 0x41);
              f->cancelled |= f->cancel_enable;
              if (f->fail_enable)
                return injected (f, G_IO_ERROR_FAILED, error);
            }
          else
            {
              f->disables++;
            }
        }
      return TRUE;

    case 0x10:
      g_assert_cmphex (tx[1], ==, 0xef);
      g_assert_cmphex (tx[3], ==, 0);
      for (gsize i = 4; i < length; i++)
        g_assert_cmphex (tx[i], ==, 0);
      g_assert_cmpuint (f->assertions, ==, 2);
      if (tx[2] == 0x20)
        {
          g_assert_cmpuint (length, ==, 6);
          f->status_reads++;
          if (f->status_reads <= f->failed_polls)
            return injected (f, f->poll_error, error);
          if (f->status_reads > f->busy_polls)
            {
              rx[4] = 0xa5;
              rx[5] = 0x5a;
            }
          return TRUE;
        }
      g_assert_cmpuint (length, ==, 5);
      switch (tx[2])
        {
        case 0x30: rx[4] = f->config_marker;
          break;

        case 0x14: rx[4] = f->geometry >> 8;
          break;

        case 0x15: rx[4] = f->geometry & 0xff;
          break;

        case 0x1a: rx[4] = f->firmware_version;
          break;

        case 0x3c: rx[4] = f->agc_version;
          break;

        default: g_assert_not_reached ();
        }
      return TRUE;

    case 0x11:
      g_assert_cmpuint (length, ==, 5);
      g_assert_cmphex (tx[1], ==, 0xee);
      g_assert_cmphex (tx[4], ==, 0);
      g_assert_cmpuint (f->status_reads, >, 0);
      f->config_writes++;
      return TRUE;

    default:
      /* This includes any accidental software-reset 70 command. */
      g_assert_not_reached ();
    }
}

static gboolean
mock_set_reset (gpointer user_data, gboolean asserted, GError **error)
{
  Fixture *f = user_data;

  f->asserted = asserted;
  g_string_append (f->events, asserted ? "L;" : "H;");
  if (asserted)
    {
      f->assertions++;
      f->cancelled |= f->cancel_start;
      if (f->fail_assertion)
        return injected (f, G_IO_ERROR_FAILED, error);
    }
  else
    {
      f->releases++;
      if (f->fail_release)
        return injected (f, G_IO_ERROR_FAILED, error);
    }
  return TRUE;
}

static gboolean
mock_reset_sync (gpointer user_data, GError **error)
{
  Fixture *f = user_data;

  /* The native callback has its own pulse/transaction-order tests. This
   * fixture tests the engine's ordering around that indivisible operation. */
  f->syncs++;
  f->asserted = FALSE;
  g_string_append (f->events, "SYNC;");
  return f->fail_sync ? injected (f, G_IO_ERROR_FAILED, error) : TRUE;
}

static gboolean
mock_wait (gpointer user_data, guint milliseconds, GError **error)
{
  Fixture *f = user_data;

  (void) error;
  g_string_append_printf (f->events, "W%u;", milliseconds);
  f->waits_2 += milliseconds == 2;
  return TRUE;
}

static gboolean
mock_cancelled (gpointer user_data, GError **error)
{
  Fixture *f = user_data;

  if (f->cancelled)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "test cancellation");
  return f->cancelled;
}

static void
mock_report (gpointer user_data, const gchar *message)
{
  Fixture *f = user_data;

  g_assert_false (f->asserted);
  g_string_append_printf (f->reports, "%s\n", message);
}

static void
setup (Fixture *f, gconstpointer unused)
{
  guint8 *payload = g_malloc (FIRMWARE_SIZE);

  (void) unused;
  for (gsize i = 0; i < FIRMWARE_SIZE; i++)
    payload[i] = (i * 13 + 7) & 0xff;
  f->firmware = g_bytes_new_take (payload, FIRMWARE_SIZE);
  f->events = g_string_new (NULL);
  f->reports = g_string_new (NULL);
  f->family = 0x1534;
  f->geometry = 0x5858;
  f->otp = 0x14;
  f->fe = 0x11;
  f->config_marker = 0xbb;
  f->firmware_version = 0x40;
  f->agc_version = 0x10;
  f->poll_error = G_IO_ERROR_FAILED;
  f->io = (Fte3600MedionIdentifyIo){
    .user_data = f,
    .max_transfer = TRANSFER_SIZE,
    .exchange = mock_exchange,
    .set_reset = mock_set_reset,
    .reset_and_sync = mock_reset_sync,
    .wait = mock_wait,
    .check_cancelled = mock_cancelled,
    .report = mock_report,
  };
}

static void
teardown (Fixture *f, gconstpointer unused)
{
  (void) unused;
  g_assert_false (f->asserted);
  g_assert_false (f->otp_enabled);
  g_bytes_unref (f->firmware);
  g_string_free (f->events, TRUE);
  g_string_free (f->reports, TRUE);
}

static void
run_success (Fixture *f)
{
  g_autoptr(GError) error = NULL;
  Fte3600Identity identity = { 0 };

  g_assert_true (fte3600_medion_test_ft9338 (&f->io, f->firmware, &identity, &error));
  g_assert_no_error (error);
  g_assert_cmpint (identity.sensor, ==, FTE3600_SENSOR_FT9338);
  g_assert_cmpint (identity.evidence, ==, FTE3600_IDENTITY_RUNTIME_GEOMETRY);
  g_assert_cmphex (identity.response, ==, 0x5858);
  g_assert_cmpuint (f->uploads, ==, 1);
  g_assert_cmpuint (f->readbacks, ==, 1);
  g_assert_cmpuint (f->assertions, ==, 2);
  g_assert_cmpuint (f->config_writes, ==, 3);
}

static gchar *
run_failure (Fixture *f, GIOErrorEnum code)
{
  g_autoptr(GError) error = NULL;
  Fte3600Identity identity = { .sensor = FTE3600_SENSOR_FT9361,
                               .evidence = FTE3600_IDENTITY_RUNTIME_GEOMETRY,
                               .response = 0x7070, .otp = 0x14 };
  const Fte3600Identity empty = { 0 };

  g_assert_false (fte3600_medion_test_ft9338 (&f->io, f->firmware, &identity, &error));
  g_assert_error (error, G_IO_ERROR, (gint) code);
  g_assert_cmpmem (&identity, sizeof identity, &empty, sizeof empty);
  return g_strdup (error->message);
}

static void
test_exact_trace (Fixture *f, gconstpointer unused)
{
  const gchar *expected =
    "[900000];[06f900];[05fa85c0000411ee020000];W2;[09f6a401];[04fb85c000000000];"
    "SYNC;[08f7c800];[09f6c8a1];[09f6f11d];[08f7f400];[09f6f441];[08f7f300];[09f6f400];"
    "SYNC;[09f6c8ff];[09f6caff];[09f6cbff];[09f6b9bf];[09f6b9ff];W20;UPLOAD;W2;READBACK;"
    "H;W10;L;W20;H;W10;H;W10;L;W20;H;W80;[10ef20000000];"
    "[11ee010100];W1;[11ee410f00];W1;[11ee30bb00];W1;[10ef300000];"
    "[10ef140000];[10ef150000];[10ef1a0000];[10ef3c0000];H;";

  (void) unused;
  run_success (f);
  g_assert_cmpstr (f->events->str, ==, expected);
  g_assert_nonnull (strstr (f->reports->str, "family = 1534"));
  g_assert_nonnull (strstr (f->reports->str, "RX 91 82 73 14, offset 3"));
  g_assert_nonnull (strstr (f->reports->str, "no image captured"));
}

static void
test_unsupported_otp (Fixture *f, gconstpointer value)
{
  g_autofree gchar *message = NULL;

  f->otp = GPOINTER_TO_UINT (value);
  message = run_failure (f, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_nonnull (strstr (message, "OTP"));
  g_assert_cmpuint (f->uploads, ==, 0);
  g_assert_cmpuint (f->disables, ==, 1);
  g_assert_cmpuint (f->assertions, ==, 0);
  g_assert_cmpuint (f->releases, ==, 1);
}

static void
test_blank_otp_default (Fixture *f, gconstpointer unused)
{
  (void) unused;
  f->otp = 0xff;
  run_success (f);
  g_assert_nonnull (strstr (f->reports->str, "blank-OTP default"));
}

static void
test_boot_a (Fixture *f, gconstpointer value)
{
  f->marker = 0xef;
  f->fe = GPOINTER_TO_UINT (value);
  if (f->fe == 2)
    {
      g_autofree gchar *message = run_failure (f, G_IO_ERROR_NOT_SUPPORTED);

      g_assert_nonnull (strstr (message, "FT9536"));
      g_assert_cmpuint (f->uploads, ==, 0);
    }
  else
    {
      run_success (f);
      g_assert_nonnull (strstr (f->reports->str, "boot-A default"));
    }
  g_assert_true (g_str_has_prefix (f->events->str,
                                   "[900000];SYNC;[08f7cb00];[09f6cb60];"
                                   "[09f6fd11];[09f6fe11];[08f7fe00];"));
  g_assert_cmpuint (f->disables, ==, 0);
}

static void
test_a8_family (Fixture *f, gconstpointer value)
{
  g_autofree gchar *message = NULL;

  f->family = GPOINTER_TO_UINT (value);
  message = run_failure (f, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_nonnull (strstr (message, "selects A8"));
  g_assert_cmpuint (f->uploads + f->syncs, ==, 0);
  g_assert_cmpuint (f->releases, ==, 1);
}

static void
test_readback_failure (Fixture *f, gconstpointer value)
{
  g_autofree gchar *message = NULL;

  f->cancel_readback = GPOINTER_TO_UINT (value);
  f->corrupt_readback = !f->cancel_readback;
  message = run_failure (f, f->cancel_readback ? G_IO_ERROR_CANCELLED : G_IO_ERROR_INVALID_DATA);
  g_assert_cmpuint (f->uploads, ==, 1);
  g_assert_cmpuint (f->readbacks, ==, 1);
  g_assert_cmpuint (f->assertions + f->status_reads + f->config_writes, ==, 0);
  g_assert_cmpuint (f->releases, ==, 1);
}

static void
test_otp_cleanup (Fixture *f, gconstpointer value)
{
  g_autofree gchar *message = NULL;

  f->cancel_enable = GPOINTER_TO_UINT (value) == 1;
  f->fail_enable = !f->cancel_enable;
  f->omit_callback_error = GPOINTER_TO_UINT (value) == 2;
  message = run_failure (f, f->cancel_enable ? G_IO_ERROR_CANCELLED : G_IO_ERROR_FAILED);
  g_assert_cmpuint (f->disables, ==, 1);
  g_assert_cmpuint (f->uploads, ==, 0);
  g_assert_cmpuint (f->releases, ==, 1);
  g_assert_true (g_str_has_suffix (f->events->str, "[09f6f441];[09f6f400];H;"));
}

static void
test_start_cleanup (Fixture *f, gconstpointer value)
{
  g_autofree gchar *message = NULL;

  f->cancel_start = GPOINTER_TO_UINT (value);
  f->fail_assertion = !f->cancel_start;
  message = run_failure (f, f->cancel_start ? G_IO_ERROR_CANCELLED : G_IO_ERROR_FAILED);
  g_assert_cmpuint (f->assertions, ==, 1);
  g_assert_cmpuint (f->status_reads, ==, 0);
  g_assert_true (g_str_has_suffix (f->events->str, "H;W10;L;W20;H;H;"));
}

static void
test_runtime_validation (Fixture *f, gconstpointer value)
{
  g_autofree gchar *message = NULL;

  switch (GPOINTER_TO_UINT (value))
    {
    case 0: f->config_marker = 0;
      break;

    case 1: f->geometry = 0;
      break;

    case 2: f->firmware_version = 0x23;
      break;

    case 3: f->agc_version = 0x13;
      break;

    default: g_assert_not_reached ();
    }
  message = run_failure (f, G_IO_ERROR_INVALID_DATA);
  g_assert_nonnull (strstr (message, "Hardware validation failed"));
  g_assert_cmpuint (f->uploads, ==, 1);
  g_assert_cmpuint (f->config_writes, ==, 3);
  g_assert_cmpuint (f->syncs, ==, 2);
}

static void
test_polling (Fixture *f, gconstpointer value)
{
  guint mode = GPOINTER_TO_UINT (value);

  f->failed_polls = mode ? 0 : 1;
  f->busy_polls = mode ? 20 : 2;
  if (mode)
    {
      g_autofree gchar *message = run_failure (f, G_IO_ERROR_TIMED_OUT);

      g_assert_cmpuint (f->status_reads, ==, 20);
      g_assert_cmpuint (f->config_writes, ==, 0);
      g_assert_cmpuint (f->waits_2, ==, 22);
    }
  else
    {
      run_success (f);
      g_assert_cmpuint (f->status_reads, ==, 3);
      g_assert_cmpuint (f->waits_2, ==, 4);
    }
}

static void
test_fatal_poll_error (Fixture *f, gconstpointer unused)
{
  g_autofree gchar *message = NULL;

  (void) unused;
  f->failed_polls = 20;
  f->poll_error = G_IO_ERROR_BROKEN_PIPE;
  message = run_failure (f, G_IO_ERROR_BROKEN_PIPE);
  g_assert_cmpuint (f->status_reads, ==, 1);
  g_assert_cmpuint (f->config_writes, ==, 0);
}

static void
test_cleanup_error (Fixture *f, gconstpointer unused)
{
  g_autofree gchar *message = NULL;

  (void) unused;
  f->otp = 0;
  f->fail_release = TRUE;
  message = run_failure (f, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_nonnull (strstr (message, "OTP 00"));
  g_assert_nonnull (strstr (message, "release reset"));
  g_assert_cmpuint (f->uploads, ==, 0);
}

static void
test_inputs (Fixture *f, gconstpointer value)
{
  g_autofree gchar *message = NULL;

  switch (GPOINTER_TO_UINT (value))
    {
    case 0: f->io.max_transfer = TRANSFER_SIZE - 1;
      break;

    case 1: f->io.reset_and_sync = NULL;
      break;

    case 2:
      g_bytes_unref (f->firmware);
      f->firmware = g_bytes_new_static ("wrong", 5);
      break;

    default: g_assert_not_reached ();
    }
  message = run_failure (f, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpuint (f->exchanges + f->syncs + f->releases, ==, 0);
}

static void
test_cancel_before_start (Fixture *f, gconstpointer unused)
{
  g_autofree gchar *message = NULL;

  (void) unused;
  f->cancelled = TRUE;
  message = run_failure (f, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint (f->exchanges + f->syncs + f->releases, ==, 0);
}

static void
test_sync_failure (Fixture *f, gconstpointer unused)
{
  g_autofree gchar *message = NULL;

  (void) unused;
  f->fail_sync = TRUE;
  message = run_failure (f, G_IO_ERROR_FAILED);
  g_assert_nonnull (strstr (message, "reset and sync"));
  g_assert_cmpuint (f->uploads, ==, 0);
  g_assert_cmpuint (f->releases, ==, 1);
}

#define ADD(name, value, function) \
  g_test_add ("/medion/ft9338/" name, Fixture, GUINT_TO_POINTER (value), setup, function, teardown)

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  ADD ("exact-vendor-trace", 0, test_exact_trace);
  ADD ("1534-otp00-no-upload", 0x00, test_unsupported_otp);
  ADD ("otp-ft9536-no-upload", 0x24, test_unsupported_otp);
  ADD ("otp-unknown-no-upload", 0x34, test_unsupported_otp);
  ADD ("blank-otp-vendor-default", 0, test_blank_otp_default);
  ADD ("boot-a-vendor-default", 0x11, test_boot_a);
  ADD ("boot-a-ft9536-no-upload", 2, test_boot_a);
  ADD ("a8-2b50-no-upload", 0x2b50, test_a8_family);
  ADD ("a8-95a8-no-upload", 0x95a8, test_a8_family);
  ADD ("a8-23dd-no-upload", 0x23dd, test_a8_family);
  ADD ("readback-last-byte-mismatch", 0, test_readback_failure);
  ADD ("cancel-readback-no-start", 1, test_readback_failure);
  ADD ("failed-otp-enable-cleanup", 0, test_otp_cleanup);
  ADD ("cancel-otp-enable-cleanup", 1, test_otp_cleanup);
  ADD ("callback-missing-error", 2, test_otp_cleanup);
  ADD ("failed-start-assertion-releases", 0, test_start_cleanup);
  ADD ("cancel-start-pulse-completes", 1, test_start_cleanup);
  ADD ("configuration-marker-validation", 0, test_runtime_validation);
  ADD ("geometry-validation", 1, test_runtime_validation);
  ADD ("firmware-version-validation", 2, test_runtime_validation);
  ADD ("agc-version-validation", 3, test_runtime_validation);
  ADD ("transient-status-read-and-busy", 0, test_polling);
  ADD ("status-poll-limit", 1, test_polling);
  ADD ("invalid-session-poll-fails-once", 0, test_fatal_poll_error);
  ADD ("cleanup-preserves-primary-error", 0, test_cleanup_error);
  ADD ("insufficient-transfer", 0, test_inputs);
  ADD ("missing-reset-sync", 1, test_inputs);
  ADD ("wrong-firmware-size", 2, test_inputs);
  ADD ("cancel-before-hardware", 0, test_cancel_before_start);
  ADD ("reset-sync-failure-releases", 0, test_sync_failure);
  return g_test_run ();
}
