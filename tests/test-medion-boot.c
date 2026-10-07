/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Synthetic RAM-boot fixture. No vendor firmware, device or biometric input.
 */
#include "examples/fte3600-medion-boot.h"
#include "medion-reset-sync-fixture.h"

#include <string.h>

typedef struct
{
  Fte3600MedionIdentifyIo io;
  Fte3600Sensor           sensor;
  GBytes                 *firmware;
  guint16                 before[2];
  guint16                 after[2];
  guint8                  fw_version;
  guint8                  agc_version;
  guint                   exchanges;
  guint                   reset_calls;
  guint                   asserts;
  guint                   waits;
  guint                   uploads;
  guint                   readbacks;
  guint                   prep_writes;
  guint                   config_writes;
  gboolean                bad_config_marker;
  guint                   soft_resets;
  guint                   geometry_before;
  guint                   geometry_after;
  guint                   version_reads;
  guint                   status_reads;
  guint                   busy_replies;
  guint                   fail_exchange;
  guint                   fail_reset;
  guint                   fail_wait;
  guint                   cancel_reset;
  gboolean                cancel_upload;
  gboolean                cancel_readback;
  gboolean                cancelled;
  gboolean                asserted;
  gboolean                corrupt_last_byte;
  gboolean                final_status_busy;
  GString                *trace;
  GString                *reports;
} Fixture;

static gboolean
inject_failure (GError **error, const gchar *operation)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "injected %s failure", operation);
  return FALSE;
}

static gboolean
mock_exchange (gpointer user_data, const guint8 *tx, guint8 *rx,
               gsize length, GError **error)
{
  Fixture *f = user_data;
  gsize firmware_size;
  const guint8 *firmware = g_bytes_get_data (f->firmware, &firmware_size);

  f->exchanges++;
  g_assert_false (f->asserted);
  g_assert_nonnull (rx);
  for (gsize i = 0; i < length; i++)
    g_assert_cmphex (rx[i], ==, 0);

  if (tx[0] == 0x10)
    {
      guint8 reg = tx[2];

      g_assert_cmpuint (length, ==, reg == 0x20 ? 6 : 5);
      g_assert_cmphex (tx[1], ==, 0xef);
      for (gsize i = 3; i < length; i++)
        g_assert_cmphex (tx[i], ==, 0);
      g_string_append_printf (f->trace, "A%02x;", reg);
      if (reg == 0x14 || reg == 0x15)
        {
          guint *count = f->uploads ? &f->geometry_after : &f->geometry_before;
          const guint16 *values = f->uploads ? f->after : f->before;

          g_assert_cmpuint (*count, <, 4);
          g_assert_cmpuint (reg, ==, (*count % 2) ? 0x15 : 0x14);
          rx[4] = (*count % 2) ? values[*count / 2] & 0xff : values[*count / 2] >> 8;
          (*count)++;
        }
      else if (reg == 0x20)
        {
          g_assert_cmpuint (f->uploads, ==, 1);
          f->status_reads++;
          if (f->busy_replies)
            {
              f->busy_replies--;
            }
          else if (!f->final_status_busy || f->version_reads < 2)
            {
              rx[4] = 0xa5;
              rx[5] = 0x5a;
            }
        }
      else if (reg == 0x30)
        {
          g_assert_cmpuint (f->config_writes, ==, 3);
          rx[4] = f->bad_config_marker ? 0 : 0xbb;
        }
      else
        {
          g_assert_true (reg == 0x1a || reg == 0x3c);
          rx[4] = reg == 0x1a ? f->fw_version : f->agc_version;
          f->version_reads++;
        }
    }
  else if (tx[0] == 0x11)
    {
      const guint8 config[][5] = {
        { 0x11, 0xee, 0x01, 0x01, 0 }, { 0x11, 0xee, 0x41, 0x0f, 0 },
        { 0x11, 0xee, 0x30, 0xbb, 0 },
      };

      g_assert_cmpint (f->sensor, ==, FTE3600_SENSOR_FT9338);
      g_assert_cmpuint (f->uploads, ==, 1);
      g_assert_cmpuint (f->readbacks, ==, 1);
      g_assert_cmpuint (f->status_reads, >, 0);
      g_assert_cmpuint (f->config_writes, <, G_N_ELEMENTS (config));
      g_assert_cmpmem (tx, length, config[f->config_writes], sizeof config[0]);
      f->config_writes++;
      g_string_append_printf (f->trace, "C%02x=%02x;", tx[2], tx[3]);
    }
  else if (tx[0] == 0x55)
    {
      const guint8 sync[] = { 0x55, 0xaa };

      g_assert_cmpmem (tx, length, sync, sizeof sync);
      g_string_append (f->trace, "SYNC;");
    }
  else if (tx[0] == 0x09)
    {
      const guint8 prep[][4] = {
        { 0x09, 0xf6, 0xc8, 0xff }, { 0x09, 0xf6, 0xca, 0xff },
        { 0x09, 0xf6, 0xcb, 0xff }, { 0x09, 0xf6, 0xb9, 0xbf },
        { 0x09, 0xf6, 0xb9, 0xff },
      };

      g_assert_cmpint (f->sensor, ==, FTE3600_SENSOR_FT9338);
      g_assert_cmpuint (f->prep_writes, <, G_N_ELEMENTS (prep));
      g_assert_cmpmem (tx, length, prep[f->prep_writes], sizeof prep[0]);
      f->prep_writes++;
      g_string_append_printf (f->trace, "B%02x=%02x;", tx[2], tx[3]);
    }
  else if (tx[0] == 0x05)
    {
      const guint8 prefix38[] = { 0x05, 0xfa, 0, 0, 0x37, 0x68 };
      const guint8 prefix48[] = { 0x05, 0xfa, 0, 0, 0x28, 0x48 };

      g_assert_cmpuint (f->uploads, ==, 0);
      g_assert_cmpuint (length, ==, firmware_size + 7);
      g_assert_cmpmem (tx, 6, f->sensor == FTE3600_SENSOR_FT9338 ? prefix38 : prefix48, 6);
      g_assert_cmpmem (tx + 6, firmware_size, firmware, firmware_size);
      g_assert_cmphex (tx[length - 1], ==, 0);
      g_assert_cmpuint (f->prep_writes, ==, f->sensor == FTE3600_SENSOR_FT9338 ? 5 : 0);
      f->uploads++;
      f->cancelled |= f->cancel_upload;
      g_string_append (f->trace, "UPLOAD;");
    }
  else if (tx[0] == 0x04)
    {
      const guint8 prefix[] = { 0x04, 0xfb, 0, 0, 0x37, 0x70 };

      g_assert_cmpint (f->sensor, ==, FTE3600_SENSOR_FT9338);
      g_assert_cmpuint (f->uploads, ==, 1);
      g_assert_cmpuint (f->readbacks, ==, 0);
      g_assert_cmpuint (length, ==, 14192);
      g_assert_cmpmem (tx, sizeof prefix, prefix, sizeof prefix);
      for (gsize i = 6; i < length; i++)
        g_assert_cmphex (tx[i], ==, 0);
      memcpy (rx + 6, firmware, firmware_size);
      /* Non-payload trailer bytes deliberately differ; only the complete
       * firmware region is compared, including its final byte. */
      rx[length - 2] = 0x73;
      rx[length - 1] = 0x9c;
      if (f->corrupt_last_byte)
        rx[6 + firmware_size - 1] ^= 0x80;
      f->readbacks++;
      f->cancelled |= f->cancel_readback;
      g_string_append (f->trace, "READBACK;");
    }
  else if (tx[0] == 0x70)
    {
      g_assert_cmpint (f->sensor, ==, FTE3600_SENSOR_FT9348);
      g_assert_cmpuint (length, ==, 1);
      f->soft_resets++;
      g_string_append (f->trace, "SOFT;");
    }
  else
    {
      g_assert_not_reached ();
    }

  if (f->exchanges == f->fail_exchange)
    return inject_failure (error, "SPI");
  return TRUE;
}

static gboolean
mock_reset (gpointer user_data, gboolean asserted, GError **error)
{
  Fixture *f = user_data;

  f->reset_calls++;
  f->asserts += asserted;
  f->asserted = asserted;
  f->cancelled |= f->reset_calls == f->cancel_reset;
  g_string_append_printf (f->trace, "R%u;", asserted);
  return f->reset_calls != f->fail_reset || inject_failure (error, "GPIO");
}

static gboolean
mock_wait (gpointer user_data, guint milliseconds, GError **error)
{
  Fixture *f = user_data;

  f->waits++;
  g_string_append_printf (f->trace, "W%u;", milliseconds);
  return f->waits != f->fail_wait || inject_failure (error, "wait");
}

static gboolean
mock_cancelled (gpointer user_data, GError **error)
{
  Fixture *f = user_data;

  if (f->cancelled)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "injected cancellation");
  return f->cancelled;
}

static void
mock_report (gpointer user_data, const gchar *message)
{
  Fixture *f = user_data;

  g_string_append_printf (f->reports, "%s\n", message);
}

static gboolean
mock_reset_and_sync (gpointer user_data, GError **error)
{
  Fixture *f = user_data;

  return medion_fixture_reset_and_sync (&f->io, error);
}

static void
fixture_init (Fixture *f, Fte3600Sensor sensor)
{
  gsize size = sensor == FTE3600_SENSOR_FT9338 ? 14184 : 10312;
  guint8 *firmware = g_malloc (size);

  memset (f, 0, sizeof *f);
  f->sensor = sensor;
  f->after[0] = f->after[1] = sensor == FTE3600_SENSOR_FT9338 ? 0x5858 : 0x6060;
  f->fw_version = sensor == FTE3600_SENSOR_FT9338 ? 0x40 : 0x30;
  f->agc_version = sensor == FTE3600_SENSOR_FT9338 ? 0x10 : 0x31;
  for (gsize i = 0; i < size; i++)
    firmware[i] = (i * 37 + (i >> 8) * 19 + 11) & 0xff;
  f->firmware = g_bytes_new_take (firmware, size);
  f->trace = g_string_new (NULL);
  f->reports = g_string_new (NULL);
  f->io = (Fte3600MedionIdentifyIo){
    .user_data = f, .max_transfer = sensor == FTE3600_SENSOR_FT9338 ? 14192 : 10319,
    .exchange = mock_exchange, .set_reset = mock_reset,
    .reset_and_sync = mock_reset_and_sync, .wait = mock_wait,
    .check_cancelled = mock_cancelled, .report = mock_report,
  };
}

static void
fixture_clear (Fixture *f)
{
  g_assert_false (f->asserted);
  g_bytes_unref (f->firmware);
  g_string_free (f->trace, TRUE);
  g_string_free (f->reports, TRUE);
}

static Fte3600Identity
run_success (Fixture *f)
{
  Fte3600Identity result = { 0 };

  g_autoptr(GError) error = NULL;

  g_assert_true (fte3600_medion_boot (&f->io, f->sensor, f->firmware, &result, &error));
  g_assert_no_error (error);
  g_assert_cmpint (result.sensor, ==, f->sensor);
  g_assert_cmpint (result.evidence, ==, FTE3600_IDENTITY_RUNTIME_GEOMETRY);
  g_assert_cmphex (result.response, ==, f->after[0]);
  return result;
}

static GError *
run_failure (Fixture *f)
{
  Fte3600Identity result, empty = { 0 };
  GError *error = NULL;

  memset (&result, 0xff, sizeof result);
  g_assert_false (fte3600_medion_boot (&f->io, f->sensor, f->firmware, &result, &error));
  g_assert_nonnull (error);
  g_assert_cmpmem (&result, sizeof result, &empty, sizeof empty);
  return error;
}

static void
test_exact_boot (gconstpointer data)
{
  Fixture f;
  const gchar *expected38 =
    "A14;A15;A14;A15;R0;W10;R1;W20;R0;SYNC;"
    "Bc8=ff;Bca=ff;Bcb=ff;Bb9=bf;Bb9=ff;W20;UPLOAD;W2;READBACK;"
    "R0;W10;R1;W20;R0;W10;R0;W10;R1;W20;R0;W80;"
    "A20;C01=01;W1;C41=0f;W1;C30=bb;W1;A30;A14;A15;A14;A15;A1a;A3c;A20;R0;";
  const gchar *expected48 =
    "A14;A15;A14;A15;R0;W10;R1;W20;R0;SYNC;UPLOAD;W2;"
    "R0;W10;R1;W20;R0;W10;R0;W10;R1;W20;R0;W160;SOFT;W5;SOFT;W2;"
    "A20;A14;A15;A14;A15;A1a;A3c;A20;R0;";

  fixture_init (&f, GPOINTER_TO_INT (data));
  run_success (&f);
  g_assert_cmpstr (f.trace->str, ==, f.sensor == FTE3600_SENSOR_FT9338 ? expected38 : expected48);
  g_assert_cmpuint (f.uploads, ==, 1);
  g_assert_cmpuint (f.asserts, ==, 3);
  g_assert_cmpuint (f.readbacks, ==, f.sensor == FTE3600_SENSOR_FT9338 ? 1 : 0);
  g_assert_cmpuint (f.soft_resets, ==, f.sensor == FTE3600_SENSOR_FT9348 ? 2 : 0);
  fixture_clear (&f);
}

static void
test_allowed_runtime (void)
{
  const Fte3600Sensor sensors[] = { FTE3600_SENSOR_FT9338, FTE3600_SENSOR_FT9348 };

  for (guint i = 0; i < G_N_ELEMENTS (sensors); i++)
    for (guint variant = 0; variant < 2; variant++)
      {
        Fixture f;

        fixture_init (&f, sensors[i]);
        f.before[0] = f.before[1] = variant ? f.after[0] : 0xffff;
        run_success (&f);
        fixture_clear (&f);
      }
}

static void
test_runtime_rejected (void)
{
  const guint16 before[][2] = {
    { 0x6060, 0x6060 }, { 0x4050, 0x4050 }, { 0x1234, 0x1234 },
    { 0, 0xffff }, { 0x5858, 0 },
  };

  for (guint i = 0; i < G_N_ELEMENTS (before); i++)
    {
      Fixture f;
      g_autoptr(GError) error = NULL;

      fixture_init (&f, FTE3600_SENSOR_FT9338);
      memcpy (f.before, before[i], sizeof f.before);
      error = run_failure (&f);
      g_assert_cmpuint (f.reset_calls, ==, 0);
      g_assert_cmpuint (f.uploads, ==, 0);
      g_assert_cmpuint (f.prep_writes, ==, 0);
      fixture_clear (&f);
    }
}

static void
test_preflight (void)
{
  for (guint variant = 0; variant < 5; variant++)
    {
      Fixture f;
      g_autoptr(GError) error = NULL;

      fixture_init (&f, variant == 4 ? FTE3600_SENSOR_FT9348 : FTE3600_SENSOR_FT9338);
      if (variant == 0)
        {
          f.sensor = FTE3600_SENSOR_FT9361;
        }
      else if (variant == 1)
        {
          g_clear_pointer (&f.firmware, g_bytes_unref);
          f.firmware = g_bytes_new_static ("wrong", 5);
        }
      else if (variant == 2 || variant == 4)
        {
          f.io.max_transfer--;
        }
      else
        {
          f.io.exchange = NULL;
        }
      error = run_failure (&f);
      g_assert_cmpuint (f.exchanges, ==, 0);
      g_assert_cmpuint (f.reset_calls, ==, 0);
      fixture_clear (&f);
    }
}

static void
test_readback_last_byte (void)
{
  Fixture f;

  g_autoptr(GError) error = NULL;

  fixture_init (&f, FTE3600_SENSOR_FT9338);
  f.corrupt_last_byte = TRUE;
  error = run_failure (&f);
  g_assert_cmpuint (f.readbacks, ==, 1);
  g_assert_cmpuint (f.asserts, ==, 1); /* Never boot the unverified RAM image. */
  g_assert_cmpuint (f.status_reads, ==, 0);
  g_assert_true (g_str_has_suffix (f.trace->str, "READBACK;R0;"));
  fixture_clear (&f);
}

static void
test_application_rejected (void)
{
  for (guint variant = 0; variant < 7; variant++)
    {
      Fixture f;
      g_autoptr(GError) error = NULL;

      fixture_init (&f, FTE3600_SENSOR_FT9338);
      if (variant == 0)
        f.busy_replies = 100;
      else if (variant == 1)
        f.after[0] = f.after[1] = 0x6060;
      else if (variant == 2)
        f.after[1] = 0;
      else if (variant == 3)
        f.fw_version = 0x30;
      else if (variant == 4)
        f.agc_version = 0x31;
      else if (variant == 5)
        f.final_status_busy = TRUE;
      else
        f.bad_config_marker = TRUE;
      error = run_failure (&f);
      g_assert_cmpuint (f.uploads, ==, 1);
      g_assert_cmpuint (f.asserts, ==, 3); /* Failure cleanup must not reboot. */
      g_assert_cmpuint (f.status_reads, <=, 20);
      g_assert_true (g_str_has_suffix (f.trace->str, "R0;"));
      fixture_clear (&f);
    }
}

static void
test_bounded_poll (void)
{
  Fixture f;

  fixture_init (&f, FTE3600_SENSOR_FT9348);
  f.busy_replies = 19;
  run_success (&f);
  g_assert_cmpuint (f.status_reads, ==, 21); /* 20 startup reads, one final check. */
  g_assert_cmpuint (f.uploads, ==, 1);
  fixture_clear (&f);
}

static void
test_transfer_failures (gconstpointer data)
{
  Fixture baseline;
  guint transactions;

  fixture_init (&baseline, GPOINTER_TO_INT (data));
  run_success (&baseline);
  transactions = baseline.exchanges;
  fixture_clear (&baseline);
  for (guint at = 1; at <= transactions; at++)
    {
      Fixture f;
      g_autoptr(GError) error = NULL;

      fixture_init (&f, GPOINTER_TO_INT (data));
      f.fail_exchange = at;
      error = run_failure (&f);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
      g_assert_cmpuint (f.exchanges, ==, at); /* No retry and no later SPI cleanup. */
      g_assert_cmpuint (f.uploads, <=, 1);
      fixture_clear (&f);
    }
}

static void
test_cancellation (void)
{
  for (guint variant = 0; variant < 5; variant++)
    {
      Fixture f;
      g_autoptr(GError) error = NULL;

      fixture_init (&f, FTE3600_SENSOR_FT9338);
      f.cancelled = variant == 0;
      f.cancel_reset = variant == 1 ? 2 : variant == 4 ? 10 : 0;
      f.cancel_upload = variant == 2;
      f.cancel_readback = variant == 3;
      error = run_failure (&f);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      g_assert_cmpuint (f.asserts, ==, variant == 4 ? 3 : variant ? 1 : 0);
      if (variant == 1)
        {
          g_assert_nonnull (strstr (f.trace->str, "R1;W20;R0;"));
          g_assert_cmpuint (f.uploads, ==, 0);
        }
      fixture_clear (&f);
    }
}

static void
test_gpio_wait_failures (void)
{
  for (guint variant = 0; variant < 4; variant++)
    {
      Fixture f;
      g_autoptr(GError) error = NULL;

      fixture_init (&f, FTE3600_SENSOR_FT9338);
      if (variant < 2)
        f.fail_reset = variant ? 2 : 1;
      else
        f.fail_wait = variant == 2 ? 1 : 2;
      error = run_failure (&f);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
      g_assert_cmpuint (f.uploads, ==, 0);
      g_assert_true (g_str_has_suffix (f.trace->str, "R0;"));
      fixture_clear (&f);
    }
}

static void
test_cleanup_failure (void)
{
  Fixture f;

  g_autoptr(GError) error = NULL;

  fixture_init (&f, FTE3600_SENSOR_FT9338);
  f.fail_exchange = 5; /* Sync, after the first complete pulse. */
  f.fail_reset = 4;    /* The final release-only cleanup also fails. */
  error = run_failure (&f);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull (strstr (error->message, "SPI"));
  g_assert_nonnull (strstr (error->message, "GPIO"));
  g_assert_cmpuint (f.asserts, ==, 1);
  fixture_clear (&f);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_data_func ("/medion-boot/ft9338-exact", GINT_TO_POINTER (FTE3600_SENSOR_FT9338), test_exact_boot);
  g_test_add_data_func ("/medion-boot/ft9348-exact", GINT_TO_POINTER (FTE3600_SENSOR_FT9348), test_exact_boot);
  g_test_add_func ("/medion-boot/allowed-runtime", test_allowed_runtime);
  g_test_add_func ("/medion-boot/runtime-rejected", test_runtime_rejected);
  g_test_add_func ("/medion-boot/preflight", test_preflight);
  g_test_add_func ("/medion-boot/readback-last-byte", test_readback_last_byte);
  g_test_add_func ("/medion-boot/application-rejected", test_application_rejected);
  g_test_add_func ("/medion-boot/bounded-poll", test_bounded_poll);
  g_test_add_data_func ("/medion-boot/ft9338-transfer-failures", GINT_TO_POINTER (FTE3600_SENSOR_FT9338), test_transfer_failures);
  g_test_add_data_func ("/medion-boot/ft9348-transfer-failures", GINT_TO_POINTER (FTE3600_SENSOR_FT9348), test_transfer_failures);
  g_test_add_func ("/medion-boot/cancellation", test_cancellation);
  g_test_add_func ("/medion-boot/gpio-wait-failures", test_gpio_wait_failures);
  g_test_add_func ("/medion-boot/cleanup-failure", test_cleanup_failure);
  return g_test_run ();
}
