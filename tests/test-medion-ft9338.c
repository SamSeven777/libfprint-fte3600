/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Synthetic protocol fixture, with no vendor firmware or biometric input.
 */
#include "examples/fte3600-medion-ft9338.h"
#include "examples/fte3600-medion-boot.h"

#include <string.h>

#define FIRMWARE_SIZE 14184u
#define TRANSFER_SIZE 14192u

typedef struct
{
  Fte3600MedionIdentifyIo io;
  GBytes                 *firmware;
  GString                *events;
  GString                *reports;
  GString                *factory_events;
  gboolean                factory_done;
  gboolean                manual;
  gboolean                warm;
  gboolean                fail_factory_io;
  gboolean                cancel_factory_reset;
  gboolean                cancel_factory_wake;
  gboolean                outdated_warm;
  guint8                  warm_mode;
  guint                   stop_writes;
  guint                   start_writes;
  guint                   mode_writes;
  guint                   mode_status_reads;
  gboolean                configured;
  gboolean                started;
  gboolean                mode_initial_busy;
  gboolean                mode_stays_idle;
  guint                   otp_reads;
  gboolean                first_otp_zero;
  guint                   version_reads;
  guint                   agc_reads;
  guint                   version_failures;
  GIOErrorEnum            version_error;
  gboolean                fail_sensor_mode;
  gboolean                readback_zero;
  guint                   information_reads;
  guint                   factory_assertions;
  guint                   factory_releases;
  guint                   wake_reads;
  guint                   c6_reads;
  guint                   word_reads;
  guint16                 special_id;
  guint16                 info_id;
  guint8                  c6_value;
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
  guint                   fail_uploads;
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
mock_factory_exchange (Fixture *f, const guint8 *tx, guint8 *rx,
                       gsize length, GError **error)
{
  g_string_append_c (f->factory_events, '[');
  for (gsize i = 0; i < length; i++)
    g_string_append_printf (f->factory_events, "%02x", tx[i]);
  g_string_append (f->factory_events, "];");
  if (f->fail_factory_io)
    return injected (f, G_IO_ERROR_BROKEN_PIPE, error);
  switch (tx[0])
    {
    case 0xff:
      g_assert_cmpuint (length, ==, 4);
      break;

    case 0x91:
      g_assert_cmpuint (length, ==, 39);
      f->information_reads++;
      rx[26] = f->info_id >> 8;
      rx[27] = f->info_id & 0xff;
      break;

    case 0x5a:
    case 0xc0:
    case 0xa5:
      g_assert_cmpuint (length, ==, 3);
      break;

    case 0x08:
      g_assert_cmpuint (length, ==, 5);
      g_assert_true (tx[2] == 0x80 || tx[2] == 0xc6);
      if (tx[2] == 0xc6)
        {
          f->c6_reads++;
          rx[4] = f->c6_value;
        }
      break;

    case 0x09:
      {
        const guint8 expected[] = { 0x09, 0xf6, 0xc6, 1 };
        g_assert_cmpmem (tx, length, expected, sizeof expected);
        break;
      }

    case 0x04:
      {
        const guint8 expected[] = { 0x04, 0xfb, 0x9a, 0x8b, 0, 1, 0, 0, 0, 0, 0, 0 };
        g_assert_cmpmem (tx, length, expected, sizeof expected);
        f->word_reads++;
        if (f->word_reads == 6)
          f->factory_done = TRUE; /* Round 2 enters ROM without legacy wake. */
        rx[6] = f->special_id >> 8;
        rx[7] = f->special_id & 0xff;
        break;
      }

    case 0x70:
      g_assert_cmpuint (length, ==, 1);
      f->cancelled |= f->cancel_factory_wake;
      break;

    case 0x10:
      if (tx[2] == 0x20)
        {
          g_assert_cmpuint (length, ==, 6);
          f->wake_reads++;
          if (f->warm)
            {
              rx[4] = 0xa5;
              rx[5] = 0x5a;
            }
          else if (f->wake_reads == 6)
            {
              f->factory_done = TRUE;
            }
        }
      else
        {
          g_assert_true (f->warm);
          g_assert_cmpuint (length, ==, 5);
          g_assert_true (tx[2] == 0x14 || tx[2] == 0x15);
          rx[4] = tx[2] == 0x14 ? f->geometry >> 8 : f->geometry & 0xff;
          f->factory_done = tx[2] == 0x15;
        }
      break;

    default:
      /* A ROM query or firmware write before preparation is a regression. */
      g_assert_not_reached ();
    }
  return TRUE;
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
  if (tx[0] == 0xff)
    {
      f->factory_done = FALSE;
      f->configured = FALSE;
      f->started = FALSE;
    }
  if (!f->factory_done)
    return mock_factory_exchange (f, tx, rx, length, error);

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
          if (f->uploads <= f->fail_uploads)
            return injected (f, G_IO_ERROR_FAILED, error);
          return TRUE;
        }
      else
        {
          const guint8 header[] = { 0x04, 0xfb, 0, 0, 0x37, 0x70 };

          g_assert_cmpuint (length, ==, TRANSFER_SIZE);
          g_assert_cmpmem (tx, sizeof header, header, sizeof header);
          for (gsize i = sizeof header; i < length; i++)
            g_assert_cmphex (tx[i], ==, 0);
          g_assert_cmpuint (f->uploads, >, f->readbacks);
          memcpy (rx + 6, firmware, FIRMWARE_SIZE);
          if (f->corrupt_readback)
            rx[6 + FIRMWARE_SIZE - 1] ^= 1;
          if (f->readback_zero)
            memset (rx + 6, 0, FIRMWARE_SIZE);
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
          f->otp_reads++;
          rx[3] = f->first_otp_zero && f->otp_reads == 1 ? 0 : f->otp;
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
      if (tx[2] == 0x20)
        {
          g_assert_cmpuint (length, ==, 6);
          if (f->configured)
            {
              f->mode_status_reads++;
              if ((f->mode_status_reads == 1 && !f->mode_initial_busy) ||
                  (f->mode_status_reads == 2 && f->mode_stays_idle))
                {
                  rx[4] = 0xa5;
                  rx[5] = 0x5a;
                }
              return TRUE;
            }
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
        case 0x76: rx[4] = f->warm_mode;
          break;

        case 0x30: rx[4] = f->config_marker;
          break;

        case 0x1d: rx[4] = 0;
          break;

        case 0x14: rx[4] = f->geometry >> 8;
          break;

        case 0x15: rx[4] = f->geometry & 0xff;
          break;

        case 0x1a:
          f->version_reads++;
          if (f->version_reads <= f->version_failures)
            return injected (f, f->version_error, error);
          rx[4] = !f->started && (!f->warm || f->outdated_warm) ? 0 : f->firmware_version;
          break;

        case 0x3c:
          f->agc_reads++;
          rx[4] = !f->started && !f->warm ? 0 : f->agc_version;
          break;

        default: g_assert_not_reached ();
        }
      return TRUE;

    case 0x11:
      g_assert_cmpuint (length, ==, 5);
      g_assert_cmphex (tx[1], ==, 0xee);
      g_assert_cmphex (tx[4], ==, 0);
      if (tx[2] == 0x1e || tx[2] == 0x1f)
        {
          g_assert_cmphex (tx[3], <=, 1);
          if (tx[3] == 0)
            f->stop_writes++;
          else
            f->start_writes++;
          return TRUE;
        }
      if (tx[2] == 0x76)
        {
          g_assert_cmphex (tx[3], ==, f->mode_writes == 0 ? 1 : 0);
          f->mode_writes++;
          if (f->fail_sensor_mode)
            return injected (f, G_IO_ERROR_FAILED, error);
          return TRUE;
        }
      f->config_writes++;
      f->configured = tx[2] == 0x30;
      return TRUE;

    case 0x70:
      g_assert_cmpuint (length, ==, 1);
      return TRUE;

    default:
      g_assert_not_reached ();
    }
}

static gboolean
mock_set_reset (gpointer user_data, gboolean asserted, GError **error)
{
  Fixture *f = user_data;

  f->asserted = asserted;
  if (!f->factory_done)
    {
      g_string_append (f->factory_events, asserted ? "L;" : "H;");
      if (asserted)
        {
          f->factory_assertions++;
          f->cancelled |= f->cancel_factory_reset;
        }
      else
        {
          f->factory_releases++;
        }
      return TRUE;
    }
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
  f->started = FALSE;
  f->asserted = FALSE;
  g_string_append (f->events, "SYNC;");
  return f->fail_sync ? injected (f, G_IO_ERROR_FAILED, error) : TRUE;
}

static gboolean
mock_wait (gpointer user_data, guint milliseconds, GError **error)
{
  Fixture *f = user_data;

  (void) error;
  g_string_append_printf (f->factory_done ? f->events : f->factory_events,
                          "W%u;", milliseconds);
  f->waits_2 += milliseconds == 2;
  if (milliseconds == 80)
    f->started = TRUE;
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
  f->factory_events = g_string_new (NULL);
  f->c6_value = 1;
  f->warm_mode = 2;
  f->family = 0x1534;
  f->geometry = 0x5858;
  f->otp = 0x14;
  f->fe = 0x11;
  f->config_marker = 0xbb;
  f->firmware_version = 0x40;
  f->agc_version = 0x10;
  f->poll_error = G_IO_ERROR_FAILED;
  f->version_error = G_IO_ERROR_FAILED;
  f->manual = strstr (g_test_get_path (), "/manual/") != NULL;
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
  g_string_free (f->factory_events, TRUE);
}

/* Exercise the public manual dispatcher, not just its implementation helper. */
static gboolean
run_engine (Fixture *f, Fte3600Identity *identity, GError **error)
{
  if (f->manual)
    return fte3600_medion_boot (&f->io, FTE3600_SENSOR_FT9338,
                                f->firmware, identity, error);
  return fte3600_medion_test_ft9338 (&f->io, f->firmware, identity, error);
}

static void
run_success (Fixture *f)
{
  g_autoptr(GError) error = NULL;
  Fte3600Identity identity = { 0 };

  g_assert_true (run_engine (f, &identity, &error));
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

  g_assert_false (run_engine (f, &identity, &error));
  g_assert_error (error, G_IO_ERROR, (gint) code);
  g_assert_cmpmem (&identity, sizeof identity, &empty, sizeof empty);
  return g_strdup (error->message);
}

static void
test_exact_trace (Fixture *f, gconstpointer unused)
{
  g_autoptr(GString) preparation = g_string_new (NULL);
  const gchar *expected =
    "[900000];[06f900];[05fa85c0000411ee020000];W2;[09f6a401];[04fb85c000000000];"
    "SYNC;[08f7c800];[09f6c8a1];[09f6f11d];[08f7f400];[09f6f441];[08f7f300];[09f6f400];"
    "[70];W5;[70];[10ef760000];[10ef1a0000];[10ef3c0000];"
    "SYNC;[09f6c8ff];[09f6caff];[09f6cbff];[09f6b9bf];[09f6b9ff];W20;UPLOAD;W2;READBACK;"
    "H;W10;L;W20;H;W10;H;W10;L;W20;H;W80;[10ef20000000];"
    "[11ee010100];W1;[11ee410f00];W1;[11ee30bb00];W1;[10ef300000];"
    "[10ef20000000];[11ee760100];[11ee1f0100];[11ee1e0100];W10;[10ef1d0000];[10ef20000000];"
    "[10ef140000];[10ef150000];[10ef1a0000];[10ef3c0000];H;";

  (void) unused;
  run_success (f);
  /* Independent literal trace of the vendor entry path, before the legacy
   * subroutine trace below. Omitting preparation can no longer pass this test. */
  for (guint round = 0; round < 2; round++)
    {
      for (guint attempt = 0; attempt < 2; attempt++)
        {
          g_string_append (preparation, "[ff000000];W5;[91800020");
          for (guint i = 0; i < 35; i++)
            g_string_append (preparation, "00");
          g_string_append (preparation, "];");
        }
      for (guint attempt = 0; attempt < 2; attempt++)
        {
          g_string_append (preparation,
                           "[5aa500];W1;[08f7800000];[c03f00];W1;[a55a00];"
                           "[09f6c601];W4;[08f7c60000];[09f6c601];W4;[08f7c60000];"
                           "[04fb9a8b0001000000000000];");
          if (attempt == 0)
            g_string_append (preparation, "H;W10;L;W20;H;W10;");
        }
    }
  for (guint attempt = 0; attempt < 6; attempt++)
    {
      g_string_append (preparation, "[70];W5;[70];[10ef20000000];");
      if (attempt < 5)
        g_string_append (preparation, "W5;");
    }
  g_assert_cmpstr (f->factory_events->str, ==, preparation->str);
  g_assert_cmpuint (f->factory_assertions, ==, 2);
  g_assert_cmpuint (f->wake_reads, ==, 6);
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
  message = run_failure (f, f->otp == 0x24 ? G_IO_ERROR_NOT_SUPPORTED : G_IO_ERROR_INVALID_DATA);
  g_assert_nonnull (strstr (message, "OTP"));
  g_assert_cmpuint (f->uploads, ==, 0);
  g_assert_cmpuint (f->disables, ==, f->otp == 0x24 ? 1 : 2);
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
  g_assert_cmpuint (f->uploads, ==, f->cancel_readback ? 1 : 10);
  g_assert_cmpuint (f->readbacks, ==, f->cancel_readback ? 1 : 10);
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
  g_assert_cmpuint (f->assertions, ==, f->cancel_start ? 1 : 10);
  g_assert_cmpuint (f->status_reads, ==, 0);
  g_assert_true (g_str_has_suffix (f->events->str, "H;W10;L;W20;H;H;"));
}

static void
test_runtime_validation (Fixture *f, gconstpointer value)
{
  g_autofree gchar *message = NULL;

  switch (GPOINTER_TO_UINT (value))
    {
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
  g_assert_cmpuint (f->syncs, ==, f->manual ? 1 : 2);
}

static void
test_marker_warning (Fixture *f, gconstpointer unused)
{
  (void) unused;
  f->config_marker = 0;
  run_success (f);
  g_assert_nonnull (strstr (f->reports->str, "WARNING: MCU configuration marker is 00"));
  g_assert_nonnull (strstr (f->reports->str, "continuing as Windows does"));
  g_assert_null (strstr (f->reports->str, "configuration validated"));
  g_assert_cmpuint (f->status_reads, ==, 1);
}

static void
test_polling (Fixture *f, gconstpointer value)
{
  guint mode = GPOINTER_TO_UINT (value);

  f->failed_polls = mode ? 0 : 1;
  f->busy_polls = mode ? 200 : 2;
  if (mode)
    {
      g_autofree gchar *message = run_failure (f, G_IO_ERROR_TIMED_OUT);

      g_assert_cmpuint (f->status_reads, ==, 200);
      g_assert_cmpuint (f->config_writes, ==, 0);
      g_assert_cmpuint (f->uploads, ==, 10);
      g_assert_cmpuint (f->waits_2, ==, f->manual ? 210 : 212);
    }
  else
    {
      run_success (f);
      g_assert_cmpuint (f->status_reads, ==, 3);
      g_assert_cmpuint (f->waits_2, ==, f->manual ? 3 : 4);
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
  message = run_failure (f, G_IO_ERROR_INVALID_DATA);
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
  g_assert_cmpuint (f->factory_assertions, ==, 4);
  g_assert_cmpuint (f->releases, ==, 1);
}

#define ADD(name, value, function) \
  g_test_add ("/medion/ft9338/" name, Fixture, GUINT_TO_POINTER (value), setup, function, teardown)

static void
test_download_retry (Fixture *f, gconstpointer unused)
{
  g_autoptr(GError) error = NULL;
  Fte3600Identity identity = { 0 };

  (void) unused;
  f->fail_uploads = 1;
  g_assert_true (fte3600_medion_test_ft9338 (&f->io, f->firmware, &identity, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (f->uploads, ==, 2);
  g_assert_cmpuint (f->readbacks, ==, 1);
  g_assert_cmpuint (f->syncs, ==, 3);
  g_assert_cmpuint (f->assertions, ==, 2);
  g_assert_cmpuint (f->config_writes, ==, 3);
}

static void
test_factory_c6_exhaustion (Fixture *f, gconstpointer unused)
{
  (void) unused;
  f->c6_value = 0;
  run_success (f);
  g_assert_cmpuint (f->c6_reads, ==, 4 * 2 * 31);
  g_assert_cmpuint (f->word_reads, ==, 4);
}

static void
test_factory_other_sensor (Fixture *f, gconstpointer value)
{
  g_autofree gchar *message = NULL;
  guint16 id = GPOINTER_TO_UINT (value);

  if (id == 0x9368)
    f->info_id = id;
  else
    f->special_id = id;
  f->c6_value = 0; /* Missing C6 acknowledgement must not conceal an ID. */
  message = run_failure (f, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_nonnull (strstr (message, "another sensor"));
  g_assert_cmpuint (f->uploads + f->syncs, ==, 0);
  g_assert_cmpuint (f->wake_reads, ==, 0);
  g_assert_cmpstr (f->events->str, ==, "");
}

static void
test_factory_error (Fixture *f, gconstpointer value)
{
  g_autofree gchar *message = NULL;

  f->cancel_factory_reset = GPOINTER_TO_UINT (value);
  f->fail_factory_io = !f->cancel_factory_reset;
  message = run_failure (f, f->cancel_factory_reset ? G_IO_ERROR_CANCELLED : G_IO_ERROR_BROKEN_PIPE);
  g_assert_nonnull (strstr (message, "factory preparation"));
  g_assert_cmpuint (f->uploads + f->syncs, ==, 0);
  g_assert_false (f->asserted);
  if (f->cancel_factory_reset)
    g_assert_true (g_str_has_suffix (f->factory_events->str, "H;W10;L;W20;H;H;"));
}

static void
test_factory_warm (Fixture *f, gconstpointer value)
{
  g_autoptr(GError) error = NULL;
  Fte3600Identity identity;

  f->warm = TRUE;
  f->warm_mode = GPOINTER_TO_UINT (value);
  g_assert_true (fte3600_medion_test_ft9338 (&f->io, f->firmware, &identity, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (f->wake_reads, ==, 1);
  g_assert_cmpuint (f->uploads + f->syncs, ==, 0);
  g_assert_cmpuint (f->config_writes, ==, 3);
  g_assert_cmpuint (f->stop_writes, ==, f->warm_mode == 1 ? 2 : 0);
  g_assert_true (g_str_has_suffix (f->factory_events->str,
                                   "[10ef20000000];W350;[10ef140000];[10ef150000];"));
  g_assert_null (strstr (f->events->str, "[900000]"));
  g_assert_cmphex (identity.response, ==, 0x5858);
}

static void
test_factory_cancel_wake (Fixture *f, gconstpointer unused)
{
  g_autofree gchar *message = NULL;

  (void) unused;
  f->cancel_factory_wake = TRUE;
  message = run_failure (f, G_IO_ERROR_CANCELLED);
  g_assert_true (g_str_has_suffix (f->factory_events->str, "[70];W5;[70];H;"));
  g_assert_cmpuint (f->wake_reads + f->syncs + f->uploads, ==, 0);
}

static void
test_warm_update (Fixture *f, gconstpointer unused)
{
  (void) unused;
  f->warm = TRUE;
  f->outdated_warm = TRUE;
  run_success (f);
  g_assert_cmpuint (f->syncs, ==, 1);
  g_assert_null (strstr (f->events->str, "[900000]"));
}

static void
test_family_1574 (Fixture *f, gconstpointer unused)
{
  (void) unused;
  f->family = 0x1574;
  run_success (f);
  g_assert_nonnull (strstr (f->reports->str, "factory family = 1574"));
}

static void
test_factory_wrong_geometry (Fixture *f, gconstpointer unused)
{
  g_autofree gchar *message = NULL;

  (void) unused;
  f->warm = TRUE;
  f->geometry = 0x4050;
  message = run_failure (f, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_nonnull (strstr (message, "4050 is not FT9338"));
  g_assert_cmpuint (f->uploads + f->syncs, ==, 0);
}

static void
test_factory_otp_retry (Fixture *f, gconstpointer unused)
{
  (void) unused;
  f->first_otp_zero = TRUE;
  run_success (f);
  g_assert_cmpuint (f->information_reads, ==, 7);
  g_assert_cmpuint (f->word_reads, ==, 6);
  g_assert_cmpuint (f->wake_reads, ==, 6);
  g_assert_cmpuint (f->factory_assertions, ==, 4);
  g_assert_cmpuint (f->otp_reads, ==, 2);
  g_assert_cmpuint (f->syncs, ==, 3);
  g_assert_nonnull (strstr (f->factory_events->str, "H;W10;L;W20;H;W300;[ff000000];W5;"));
  g_assert_nonnull (strstr (f->reports->str, "OTP 00 has no Windows boot38 chip mapping"));
}

static void
test_version_read_fallback (Fixture *f, gconstpointer value)
{
  f->version_failures = 1;
  f->version_error = GPOINTER_TO_UINT (value);
  if (f->version_error == G_IO_ERROR_BROKEN_PIPE)
    {
      g_autofree gchar *message = run_failure (f, G_IO_ERROR_BROKEN_PIPE);

      g_assert_cmpuint (f->uploads, ==, 0);
      g_assert_cmpuint (f->version_reads, ==, 1);
      g_assert_cmpuint (f->agc_reads, ==, 0);
      g_assert_cmpuint (f->word_reads, ==, 4);
    }
  else
    {
      run_success (f);
      /* 2c8bc returns immediately on the first read failure. The only AGC
       * read in this case is the final diagnostic validation. */
      g_assert_cmpuint (f->version_reads, ==, 2);
      g_assert_cmpuint (f->agc_reads, ==, 1);
      g_assert_nonnull (strstr (f->events->str, "[10ef760000];[10ef1a0000];SYNC;"));
    }
}

static void
test_sensor_mode (Fixture *f, gconstpointer value)
{
  guint variant = GPOINTER_TO_UINT (value);

  f->mode_initial_busy = variant == 1;
  f->mode_stays_idle = variant == 2;
  f->fail_sensor_mode = variant == 3;
  if (f->fail_sensor_mode)
    {
      g_autofree gchar *message = run_failure (f, G_IO_ERROR_FAILED);

      g_assert_cmpuint (f->uploads, ==, 1);
      g_assert_cmpuint (f->word_reads, ==, 4);
      g_assert_cmpuint (f->start_writes, ==, 0);
      return;
    }
  run_success (f);
  g_assert_cmpuint (f->mode_writes, ==, f->mode_stays_idle ? 2 : 1);
  g_assert_cmpuint (f->start_writes, ==, 2);
  g_assert_cmpuint (f->mode_status_reads, ==, 2);
  if (f->mode_initial_busy)
    g_assert_nonnull (strstr (f->events->str,
                              "[10ef300000];[10ef20000000];[70];W5;[70];[10ef760000];[11ee760100];"));
  else if (f->mode_stays_idle)
    g_assert_nonnull (strstr (f->events->str,
                              "[10ef1d0000];[10ef20000000];[11ee760000];[10ef140000];"));
}

static void
test_readback_diagnostics (Fixture *f, gconstpointer value)
{
  g_autofree gchar *message = NULL;

  f->readback_zero = GPOINTER_TO_UINT (value);
  f->corrupt_readback = !f->readback_zero;
  message = run_failure (f, G_IO_ERROR_INVALID_DATA);
  g_assert_cmpuint (f->uploads, ==, 10);
  g_assert_cmpuint (f->start_writes + f->config_writes, ==, 0);
  if (f->readback_zero)
    {
      g_assert_nonnull (strstr (message, "first offset 0 expected 07 received 00"));
      g_assert_nonnull (strstr (message, "all-zero=yes all-ff=no"));
    }
  else
    {
      g_assert_nonnull (strstr (message, "first offset 14183"));
      g_assert_nonnull (strstr (message, "1/14184 differ; all-zero=no all-ff=no"));
    }
}

static void
test_manual_entry (Fixture *f, gconstpointer unused)
{
  (void) unused;
  f->otp = 0; /* Explicit selection does not need ROM/OTP. */
  run_success (f);
  g_assert_cmpuint (f->otp_reads, ==, 0);
  g_assert_cmpuint (f->word_reads, ==, 4);
  g_assert_cmpuint (f->wake_reads, ==, 6);
  g_assert_cmpuint (f->syncs, ==, 1);
  g_assert_true (g_str_has_prefix (f->events->str,
                                   "[70];W5;[70];[10ef760000];[10ef1a0000];[10ef3c0000];SYNC;"));
  g_assert_nonnull (strstr (f->events->str,
                            "[10ef300000];[10ef20000000];[11ee760100];[11ee1f0100];[11ee1e0100];W10;"));
  g_assert_null (strstr (f->events->str, "[900000]"));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  ADD ("manual/full-initialization-without-otp", 0, test_manual_entry);
  ADD ("manual/other-family-stops", 0x9362, test_factory_other_sensor);
  ADD ("manual/version-read-fallback", G_IO_ERROR_FAILED, test_version_read_fallback);
  ADD ("manual/cancel-before-hardware", 0, test_cancel_before_start);
  ADD ("manual/cancel-readback", 1, test_readback_failure);
  ADD ("manual/readback-mismatch", 0, test_readback_diagnostics);
  ADD ("manual/configuration-marker-warning", 0, test_marker_warning);
  ADD ("manual/geometry-validation", 1, test_runtime_validation);
  ADD ("manual/firmware-validation", 2, test_runtime_validation);
  ADD ("manual/agc-validation", 3, test_runtime_validation);
  ADD ("manual/transient-status-error", 0, test_polling);
  ADD ("manual/status-poll-limit", 1, test_polling);
  ADD ("manual/session-loss", 0, test_fatal_poll_error);
  ADD ("manual/sensor-mode-failure-no-redownload", 3, test_sensor_mode);
  ADD ("manual/wrong-size", 2, test_inputs);
  ADD ("manual/insufficient-transfer", 0, test_inputs);
  ADD ("manual/cancel-start-pulse-completes", 1, test_start_cleanup);
  ADD ("exact-vendor-trace", 0, test_exact_trace);
  ADD ("factory-otp-recovered-in-third-round", 0, test_factory_otp_retry);
  ADD ("version-read-failure-download", G_IO_ERROR_FAILED, test_version_read_fallback);
  ADD ("version-read-timeout-download", G_IO_ERROR_TIMED_OUT, test_version_read_fallback);
  ADD ("version-read-session-loss-no-download", G_IO_ERROR_BROKEN_PIPE, test_version_read_fallback);
  ADD ("sensor-mode-busy-returns-idle", 1, test_sensor_mode);
  ADD ("sensor-mode-idle-rewrites-mode", 2, test_sensor_mode);
  ADD ("sensor-mode-io-error-does-not-redownload", 3, test_sensor_mode);
  ADD ("readback-zero-diagnostic", 1, test_readback_diagnostics);
  ADD ("readback-mismatch-diagnostic", 0, test_readback_diagnostics);
  ADD ("factory-c6-exhausted-still-reads-id", 0, test_factory_c6_exhaustion);
  ADD ("factory-ft9368-stops-before-legacy", 0x9368, test_factory_other_sensor);
  ADD ("factory-ft9362-stops-before-legacy", 0x9362, test_factory_other_sensor);
  ADD ("factory-unmapped-id-stops-before-legacy", 0x9395, test_factory_other_sensor);
  ADD ("factory-io-error-stops-before-legacy", 0, test_factory_error);
  ADD ("factory-cancel-reset-releases-pin", 1, test_factory_error);
  ADD ("factory-cancel-wake-finishes-pair", 0, test_factory_cancel_wake);
  ADD ("factory-warm-mode1-skips-download", 1, test_factory_warm);
  ADD ("factory-warm-mode2-skips-download", 2, test_factory_warm);
  ADD ("factory-warm-mode3-skips-download", 3, test_factory_warm);
  ADD ("factory-warm-mode4-skips-download", 4, test_factory_warm);
  ADD ("factory-warm-old-version-download", 0, test_warm_update);
  ADD ("1574-uses-b38-otp", 0, test_family_1574);
  ADD ("factory-other-geometry-stops-before-legacy", 0, test_factory_wrong_geometry);
  ADD ("transient-upload-restarts-download", 0, test_download_retry);
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
  ADD ("configuration-marker-warning", 0, test_marker_warning);
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
