/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Synthetic SPI/GPIO fixture: no device, firmware or biometric input.
 */
#include "examples/fte3600-medion-identify.h"

#include <string.h>

typedef struct
{
  Fte3600MedionIdentifyIo io;
  guint16 runtime[2];
  guint16 family[2];
  guint8 otp[2];
  guint8 marker[2];
  guint8 fe[2];
  guint app_reads;
  guint boot_reads;
  guint exchanges;
  guint resets;
  guint waits;
  guint disables;
  guint syncs;
  guint fail_exchange;
  guint fail_wait;
  guint cancel_reset;
  gboolean cancel_enable;
  gboolean cancel_read;
  gboolean cancelled;
  gboolean omit_error;
  gboolean fail_cleanup_disable;
  gboolean fail_cleanup_assert;
  gboolean in_cleanup;
  gboolean asserted;
  gboolean otp_active;
  GString *events;
  GString *reports;
} Fixture;

static gboolean
injected (GError **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "injected failure");
  return FALSE;
}

static gboolean
mock_exchange (gpointer user_data, const guint8 *tx, guint8 *rx,
               gsize length, GError **error)
{
  Fixture *f = user_data;
  guint round;

  f->exchanges++;
  g_assert_false (f->asserted);
  g_assert_cmpuint (length, <=, 11);
  g_string_append_c (f->events, '[');
  for (gsize i = 0; i < length; i++)
    {
      g_assert_cmphex (rx[i], ==, 0);
      g_string_append_printf (f->events, "%02x", tx[i]);
    }
  g_string_append_c (f->events, ']');

  if (tx[0] == 0x10)
    {
      g_assert_cmpuint (length, ==, 5);
      g_assert_cmphex (tx[1], ==, 0xef);
      g_assert_cmphex (tx[2], ==, (f->app_reads % 2) ? 0x15 : 0x14);
      g_assert_cmphex (tx[3] | tx[4], ==, 0);
      g_assert_cmpuint (f->app_reads, <, 4);
      rx[4] = (f->app_reads % 2) ? f->runtime[f->app_reads / 2] & 0xff :
              f->runtime[f->app_reads / 2] >> 8;
      f->app_reads++;
      goto complete;
    }
  if (tx[0] == 0x90)
    {
      const guint8 packet[] = { 0x90, 0, 0 };

      g_assert_cmpmem (tx, length, packet, sizeof packet);
      g_assert_cmpuint (f->boot_reads, <, 2);
      rx[2] = f->marker[f->boot_reads++];
      goto complete;
    }

  g_assert_cmpuint (f->boot_reads, >, 0);
  round = f->boot_reads - 1;
  if (tx[0] == 0x06)
    {
      const guint8 packet[] = { 0x06, 0xf9, 0 };

      g_assert_cmpmem (tx, length, packet, sizeof packet);
    }
  else if (tx[0] == 0x55)
    {
      const guint8 packet[] = { 0x55, 0xaa };

      g_assert_cmpmem (tx, length, packet, sizeof packet);
      f->syncs++;
    }
  else if (tx[0] == 0x05)
    {
      /* The sole permitted memory write is the fixed four-byte family query.
       * No address-zero upload or arbitrary payload can pass this fixture. */
      const guint8 packet[] = { 0x05, 0xfa, 0x85, 0xc0, 0, 4, 0x11, 0xee, 2, 0, 0 };

      g_assert_cmpmem (tx, length, packet, sizeof packet);
    }
  else if (tx[0] == 0x04)
    {
      const guint8 packet[] = { 0x04, 0xfb, 0x85, 0xc0, 0, 0, 0, 0 };

      g_assert_cmpmem (tx, length, packet, sizeof packet);
      rx[6] = f->family[round] >> 8;
      rx[7] = f->family[round] & 0xff;
    }
  else if (tx[0] == 0x09 && tx[2] == 0xa4)
    {
      const guint8 packet[] = { 0x09, 0xf6, 0xa4, 1 };

      g_assert_cmpmem (tx, length, packet, sizeof packet);
    }
  else
    {
      gboolean a8 = f->family[round] == 0x2b50 || f->family[round] == 0x95a8 ||
                    f->family[round] == 0x23dd;
      gsize offset = a8 ? 4 : 3;

      g_assert_cmpuint (length, ==, a8 ? 5 : 4);
      g_assert_true (tx[0] == 0x08 || tx[0] == 0x09);
      g_assert_cmphex (tx[0] ^ tx[1], ==, 0xff);
      if (a8)
        g_assert_cmphex (tx[4], ==, 0);
      if (tx[0] == 0x08)
        {
          g_assert_cmphex (tx[3], ==, 0);
          switch (tx[2])
            {
            case 0xc8:
            case 0xcb:
              rx[offset] = 0x40;
              break;

            case 0xf4:
              rx[offset] = 0x80;
              break;

            case 0xf3:
              g_assert_true (f->otp_active);
              rx[offset] = f->otp[round];
              f->cancelled |= f->cancel_read;
              break;

            case 0xfe:
              rx[offset] = f->fe[round];
              break;

            default:
              g_assert_not_reached ();
            }
        }
      else if (tx[2] == 0xf4)
        {
          g_assert_true (tx[3] == 0 || tx[3] == 0x81);
          f->otp_active = tx[3] != 0;
          if (f->otp_active)
            f->cancelled |= f->cancel_enable;
          else
            {
              f->disables++;
              if (f->in_cleanup && f->fail_cleanup_disable)
                return injected (error);
            }
        }
      else
        {
          switch (tx[2])
            {
            case 0xc8: g_assert_cmphex (tx[3], ==, a8 ? 0xdf : 0x41); break;
            case 0xcb: g_assert_cmphex (tx[3], ==, 0x60); break;
            case 0xf1: g_assert_cmphex (tx[3], ==, 0x1d); break;
            case 0xfd:
            case 0xfe: g_assert_cmphex (tx[3], ==, 0x11); break;
            default: g_assert_not_reached ();
            }
        }
    }

complete:
  if (f->exchanges == f->fail_exchange)
    return f->omit_error ? FALSE : injected (error);
  return TRUE;
}

static gboolean
mock_reset (gpointer user_data, gboolean asserted, GError **error)
{
  Fixture *f = user_data;

  f->resets++;
  g_string_append_printf (f->events, "R%u;", asserted);
  f->asserted = asserted;
  if (asserted)
    f->otp_active = FALSE;
  f->cancelled |= f->resets == f->cancel_reset;
  if (f->in_cleanup && asserted && f->fail_cleanup_assert)
    return injected (error);
  return TRUE;
}

static gboolean
mock_wait (gpointer user_data, guint milliseconds, GError **error)
{
  Fixture *f = user_data;

  f->waits++;
  g_string_append_printf (f->events, "W%u;", milliseconds);
  if (milliseconds == 180)
    f->in_cleanup = FALSE;
  if (f->waits == f->fail_wait)
    return injected (error);
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

  if (g_str_has_prefix (message, "cleanup ROM"))
    f->in_cleanup = TRUE;
  g_string_append_printf (f->reports, "%s\n", message);
}

static void
fixture_init (Fixture *f)
{
  memset (f, 0, sizeof *f);
  f->io = (Fte3600MedionIdentifyIo) {
    .user_data = f, .max_transfer = 11, .exchange = mock_exchange,
    .set_reset = mock_reset, .wait = mock_wait,
    .check_cancelled = mock_cancelled, .report = mock_report,
  };
  f->otp[0] = f->otp[1] = 0x11;
  f->fe[0] = f->fe[1] = 2;
  f->events = g_string_new (NULL);
  f->reports = g_string_new (NULL);
}

static void
fixture_clear (Fixture *f)
{
  g_assert_false (f->asserted);
  g_string_free (f->events, TRUE);
  g_string_free (f->reports, TRUE);
}

static Fte3600Identity
run_success (Fixture *f)
{
  Fte3600Identity result = { 0 };
  g_autoptr(GError) error = NULL;

  g_assert_true (fte3600_medion_identify_legacy (&f->io, &result, &error));
  g_assert_no_error (error);
  g_assert_cmpint (result.sensor, !=, FTE3600_SENSOR_UNKNOWN);
  g_assert_false (f->otp_active);
  return result;
}

static GError *
run_failure (Fixture *f, GIOErrorEnum code)
{
  Fte3600Identity result;
  Fte3600Identity empty = { 0 };
  GError *error = NULL;

  memset (&result, 0xff, sizeof result);
  g_assert_false (fte3600_medion_identify_legacy (&f->io, &result, &error));
  g_assert_error (error, G_IO_ERROR, (gint) code);
  g_assert_cmpmem (&result, sizeof result, &empty, sizeof empty);
  return error;
}

static void
test_runtime (void)
{
  const guint16 signatures[] = { 0x5858, 0x6060, 0x4050, 0x4080 };
  const Fte3600Sensor sensors[] = { FTE3600_SENSOR_FT9338, FTE3600_SENSOR_FT9348,
                                   FTE3600_SENSOR_FT9361, FTE3600_SENSOR_FT9536 };

  for (guint i = 0; i < G_N_ELEMENTS (signatures); i++)
    {
      Fixture f;
      Fte3600Identity result;

      fixture_init (&f);
      f.runtime[0] = f.runtime[1] = signatures[i];
      result = run_success (&f);
      g_assert_cmpint (result.sensor, ==, sensors[i]);
      g_assert_cmpint (result.evidence, ==, FTE3600_IDENTITY_RUNTIME_GEOMETRY);
      g_assert_cmphex (result.response, ==, signatures[i]);
      g_assert_cmpuint (f.exchanges, ==, 4);
      g_assert_cmpuint (f.resets, ==, 0);
      fixture_clear (&f);
    }
}

static void
test_runtime_rejection (void)
{
  const guint16 signatures[][2] = { { 0x1234, 0x1234 }, { 0x00ff, 0x00ff },
                                    { 0, 0xffff }, { 0x5858, 0x6060 }, { 0x5858, 0 } };

  for (guint i = 0; i < G_N_ELEMENTS (signatures); i++)
    {
      Fixture f;
      g_autoptr(GError) error = NULL;

      fixture_init (&f);
      memcpy (f.runtime, signatures[i], sizeof f.runtime);
      error = run_failure (&f, i < 2 ? G_IO_ERROR_NOT_SUPPORTED : G_IO_ERROR_INVALID_DATA);
      g_assert_cmpuint (f.exchanges, ==, 4);
      g_assert_cmpuint (f.resets, ==, 0);
      fixture_clear (&f);
    }
}

static void
test_boot38_trace (void)
{
  const gchar *app = "[10ef140000][10ef150000][10ef140000][10ef150000]";
  const gchar *round = "[900000][06f900][05fa85c0000411ee020000]W2;[09f6a401]"
                       "[04fb85c000000000]R0;W10;R1;W20;R0;[55aa]"
                       "[08f7c800][09f6c841][09f6f11d][08f7f400][09f6f481]"
                       "[08f7f300][09f6f400]R0;W10;R1;W20;R0;W180;";

  for (guint all_ones = 0; all_ones < 2; all_ones++)
    {
      Fixture f;
      Fte3600Identity result;
      g_autofree gchar *expected = g_strconcat (app, round, round, NULL);

      fixture_init (&f);
      f.runtime[0] = f.runtime[1] = all_ones ? 0xffff : 0;
      f.family[0] = f.family[1] = all_ones ? 0xffff : 0;
      result = run_success (&f);
      g_assert_cmpint (result.sensor, ==, FTE3600_SENSOR_FT9338);
      g_assert_cmpint (result.evidence, ==, FTE3600_IDENTITY_ROM_BOOT_B38_SPI_OTP);
      g_assert_cmphex (result.response, ==, f.family[0]);
      g_assert_cmphex (result.otp, ==, 0x11);
      g_assert_cmpstr (f.events->str, ==, expected);
      g_assert_nonnull (strstr (f.reports->str, "candidate evidence only"));
      fixture_clear (&f);
    }
}

static void
test_a8 (void)
{
  const guint16 families[] = { 0x2b50, 0x95a8, 0x23dd };
  const guint8 otps[] = { 0x11, 0x22, 0x33, 0x04, 0x0e, 0x0f };

  for (guint i = 0; i < G_N_ELEMENTS (families); i++)
    for (guint j = 0; j < G_N_ELEMENTS (otps); j++)
      {
        Fixture f;
        Fte3600Identity result;

        fixture_init (&f);
        f.family[0] = f.family[1] = families[i];
        f.otp[0] = f.otp[1] = otps[j];
        result = run_success (&f);
        g_assert_cmpint (result.sensor, ==, j < 3 ? FTE3600_SENSOR_FT9348 : FTE3600_SENSOR_FT9361);
        g_assert_cmpint (result.evidence, ==, FTE3600_IDENTITY_ROM_A8_SPI_OTP);
        g_assert_cmphex (result.response, ==, families[i]);
        g_assert_cmphex (result.otp, ==, otps[j]);
        g_assert_cmpuint (f.syncs, ==, 0);
        g_assert_cmpuint (f.resets, ==, 6);
        g_assert_cmpuint (f.disables, ==, 2);
        g_assert_nonnull (strstr (f.events->str, "[08f7c80000][09f6c8df00][09f6f11d00]"));
        g_assert_null (strstr (f.reports->str, "boot38"));
        fixture_clear (&f);
      }
}

static void
test_unknown_otp (void)
{
  const guint8 otps[] = { 0, 0xff, 0x35 };

  for (guint a8 = 0; a8 < 2; a8++)
    for (guint i = 0; i < G_N_ELEMENTS (otps); i++)
      {
        Fixture f;
        g_autoptr(GError) error = NULL;

        fixture_init (&f);
        f.family[0] = f.family[1] = a8 ? 0x2b50 : 0;
        f.otp[0] = f.otp[1] = otps[i];
        error = run_failure (&f, G_IO_ERROR_NOT_SUPPORTED);
        g_assert_cmpuint (f.boot_reads, ==, 1);
        g_assert_cmpuint (f.disables, ==, 1);
        if (a8)
          g_assert_cmpuint (f.syncs, ==, 0);
        fixture_clear (&f);
      }
}

static void
test_unstable_rom (void)
{
  for (guint variant = 0; variant < 4; variant++)
    {
      Fixture f;
      g_autoptr(GError) error = NULL;

      fixture_init (&f);
      if (variant == 0)
        f.otp[1] = 0x12; /* Same candidate, different OTP byte. */
      if (variant == 1)
        f.family[1] = 0xffff; /* Both empty, but not stable. */
      if (variant == 2)
        f.marker[1] = 1;
      if (variant == 3)
        {
          f.family[0] = 0x2b50;
          f.family[1] = 0x95a8;
        }
      error = run_failure (&f, G_IO_ERROR_INVALID_DATA);
      g_assert_cmpuint (f.boot_reads, ==, 2);
      g_assert_false (f.otp_active);
      fixture_clear (&f);
    }
}

static void
test_boot_a (void)
{
  const guint8 values[] = { 2, 0, 1, 0xff };

  for (guint i = 0; i < G_N_ELEMENTS (values); i++)
    {
      Fixture f;

      fixture_init (&f);
      f.marker[0] = f.marker[1] = 0xef;
      f.fe[0] = f.fe[1] = values[i];
      if (i == 0)
        {
          Fte3600Identity result = run_success (&f);

          g_assert_cmpint (result.sensor, ==, FTE3600_SENSOR_FT9536);
          g_assert_cmpint (result.evidence, ==, FTE3600_IDENTITY_ROM_BOOT_A);
          g_assert_cmphex (result.response, ==, 2);
        }
      else
        {
          g_autoptr(GError) error = run_failure (&f, G_IO_ERROR_NOT_SUPPORTED);

          g_assert_nonnull (strstr (f.reports->str, "no FT9338 default"));
        }
      g_assert_cmpuint (f.disables, ==, 0);
      g_assert_null (strstr (f.events->str, "[05fa"));
      fixture_clear (&f);
    }
}

static void
test_preflight (void)
{
  Fixture f;
  g_autoptr(GError) error = NULL;

  fixture_init (&f);
  f.io.max_transfer = 10;
  error = run_failure (&f, G_IO_ERROR_MESSAGE_TOO_LARGE);
  g_assert_cmpuint (f.exchanges + f.resets + f.waits, ==, 0);
  g_clear_error (&error);
  f.io.max_transfer = 11;
  f.io.wait = NULL;
  error = run_failure (&f, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpuint (f.exchanges + f.resets, ==, 0);
  fixture_clear (&f);
}

static void
test_cancel (void)
{
  for (guint variant = 0; variant < 4; variant++)
    {
      Fixture f;
      g_autoptr(GError) error = NULL;

      fixture_init (&f);
      f.cancelled = variant == 0;
      f.cancel_reset = variant == 1 ? 2 : 0;
      f.cancel_enable = variant == 2;
      f.cancel_read = variant == 3;
      error = run_failure (&f, G_IO_ERROR_CANCELLED);
      if (variant == 0)
        g_assert_cmpuint (f.exchanges + f.resets, ==, 0);
      else
        {
          g_assert_nonnull (strstr (f.events->str, "R1;W20;R0;"));
          g_assert_true (g_str_has_suffix (f.events->str, "R0;W10;R1;W20;R0;W180;"));
          if (variant == 1)
            g_assert_cmpuint (f.syncs, ==, 0);
          else
            g_assert_cmpuint (f.disables, ==, 1);
        }
      g_assert_false (f.otp_active);
      fixture_clear (&f);
    }
}

static void
test_transfer_failure (void)
{
  Fixture reference;
  guint exchanges;

  fixture_init (&reference);
  run_success (&reference);
  exchanges = reference.exchanges;
  fixture_clear (&reference);
  /* Fail every transaction, including OTP enable, normal disable and each
   * second-round query. Errors never become a candidate or trigger a retry. */
  for (guint step = 1; step <= exchanges; step++)
    {
      Fixture f;
      g_autoptr(GError) error = NULL;

      fixture_init (&f);
      f.fail_exchange = step;
      error = run_failure (&f, G_IO_ERROR_FAILED);
      g_assert_nonnull (strstr (error->message, "SPI exchange"));
      g_assert_false (f.otp_active);
      fixture_clear (&f);
    }
}

static void
test_cleanup_failure (void)
{
  for (guint variant = 0; variant < 3; variant++)
    {
      Fixture f;
      g_autoptr(GError) error = NULL;

      fixture_init (&f);
      f.cancel_enable = variant < 2;
      f.fail_cleanup_disable = variant == 0;
      f.fail_cleanup_assert = variant > 0;
      error = run_failure (&f, variant < 2 ? G_IO_ERROR_CANCELLED : G_IO_ERROR_FAILED);
      g_assert_nonnull (strstr (error->message, "cleanup failed"));
      g_assert_true (g_str_has_suffix (f.events->str, "R0;W10;R1;W20;R0;W180;"));
      g_assert_cmpuint (f.boot_reads, ==, 1);
      fixture_clear (&f);
    }
}

static void
test_wait_failure (void)
{
  for (guint step = 1; step <= 6; step++)
    {
      Fixture f;
      g_autoptr(GError) error = NULL;

      fixture_init (&f);
      f.fail_wait = step;
      error = run_failure (&f, G_IO_ERROR_FAILED);
      g_assert_nonnull (strstr (error->message, "wait"));
      fixture_clear (&f);
    }
}

static void
test_callback_contract (void)
{
  Fixture f;
  g_autoptr(GError) error = NULL;

  fixture_init (&f);
  f.fail_exchange = 1;
  f.omit_error = TRUE;
  error = run_failure (&f, G_IO_ERROR_FAILED);
  g_assert_nonnull (strstr (error->message, "without an error"));
  g_assert_false (fte3600_medion_identify_legacy (NULL, NULL, NULL));
  fixture_clear (&f);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/medion-identify/runtime", test_runtime);
  g_test_add_func ("/medion-identify/runtime-rejection", test_runtime_rejection);
  g_test_add_func ("/medion-identify/boot38-trace", test_boot38_trace);
  g_test_add_func ("/medion-identify/a8", test_a8);
  g_test_add_func ("/medion-identify/unknown-otp", test_unknown_otp);
  g_test_add_func ("/medion-identify/unstable-rom", test_unstable_rom);
  g_test_add_func ("/medion-identify/boot-a", test_boot_a);
  g_test_add_func ("/medion-identify/preflight", test_preflight);
  g_test_add_func ("/medion-identify/cancel", test_cancel);
  g_test_add_func ("/medion-identify/transfer-failure", test_transfer_failure);
  g_test_add_func ("/medion-identify/cleanup-failure", test_cleanup_failure);
  g_test_add_func ("/medion-identify/wait-failure", test_wait_failure);
  g_test_add_func ("/medion-identify/callback-contract", test_callback_contract);
  return g_test_run ();
}
