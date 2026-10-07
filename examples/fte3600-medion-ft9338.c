/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Independently expressed FT9338 diagnostic from observed Windows behavior.
 * Evidence: ftWbioUmdfDriverV2.dll, package 2.0.3.102; factory dispatch 2819c,
 * boot-A 272b0, boot-B OTP 27650, RAM startup 365c0, MCU configuration 36a30.
 */
#include "fte3600-medion-ft9338.h"
#include "drivers/fte3600-protocol.h"
#include "drivers/fte3600-legacy-recovery-protocol.h"
#include "drivers/fte3600-legacy-recovery-timing.h"

#include <stdarg.h>
#include <string.h>

#define FT9338_FIRMWARE_SIZE 14184u
#define FT9338_TRANSFER_SIZE (FT9338_FIRMWARE_SIZE + FTE3600_BOOT38_READBACK_OVERHEAD)

typedef struct
{
  const Fte3600MedionIdentifyIo *io;
  guint8                        *tx;
  guint8                        *rx;
  gboolean                       touched;
  gboolean                       otp_enabled;
} Test;

static void report (Test        *test,
                    const gchar *format,
                    ...) G_GNUC_PRINTF (2, 3);

static void
report (Test *test, const gchar *format, ...)
{
  va_list args;
  g_autofree gchar *message = NULL;

  if (!test->io->report)
    return;
  va_start (args, format);
  message = g_strdup_vprintf (format, args);
  va_end (args);
  test->io->report (test->io->user_data, message);
}

static void
merge_error (GError **first, GError *next)
{
  GError *combined;

  if (!next)
    return;
  if (!*first)
    {
      *first = next;
      return;
    }
  combined = g_error_new ((*first)->domain, (*first)->code,
                          "%s; %s", (*first)->message, next->message);
  g_clear_error (first);
  g_error_free (next);
  *first = combined;
}

static gboolean
callback_result (gboolean ok, const gchar *operation, GError **error)
{
  if (ok && !*error)
    return TRUE;
  if (!*error)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "I/O callback failed without an error");
  g_prefix_error (error, "%s: ", operation);
  return FALSE;
}

static gboolean
proceed (Test *test, GError **error)
{
  gboolean cancelled = test->io->check_cancelled (test->io->user_data, error);

  if (!cancelled && !*error)
    return TRUE;
  if (!*error)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                         "FT9338 test cancelled");
  return FALSE;
}

static gboolean
delay (Test *test, guint milliseconds, GError **error)
{
  return callback_result (test->io->wait (test->io->user_data, milliseconds, error),
                          "wait", error);
}

static gboolean
reset_value (Test *test, gboolean asserted, GError **error)
{
  return callback_result (test->io->set_reset (test->io->user_data, asserted, error),
                          asserted ? "assert reset" : "release reset", error);
}

static gboolean
exchange_unchecked (Test *test, gsize length, GError **error)
{
  g_assert (length > 0 && length <= FT9338_TRANSFER_SIZE);
  memset (test->rx, 0, length);
  return callback_result (test->io->exchange (test->io->user_data,
                                              test->tx, test->rx, length, error),
                          "SPI transaction", error);
}

static gboolean
exchange (Test *test, gsize length, GError **error)
{
  return proceed (test, error) && exchange_unchecked (test, length, error) &&
         proceed (test, error);
}

static gboolean
command (Test *test, Fte3600Command cmd, GError **error)
{
  gsize length = fpi_fte3600_build_command (test->tx, FT9338_TRANSFER_SIZE,
                                            cmd, error);

  return length && exchange (test, length, error);
}

static gboolean
boot_read (Test *test, guint8 reg, guint8 *value, GError **error)
{
  gsize length = fpi_fte3600_build_boot38_read (test->tx, FT9338_TRANSFER_SIZE, reg);

  if (!exchange (test, length, error))
    {
      g_prefix_error (error, "boot38 read %02x: ", reg);
      return FALSE;
    }
  *value = test->rx[FTE3600_BOOT38_RESULT_OFFSET];
  report (test, "boot38 register %02x = %02x (RX %02x %02x %02x %02x, offset 3)",
          reg, *value, test->rx[0], test->rx[1], test->rx[2], test->rx[3]);
  return TRUE;
}

static gboolean
boot_write (Test *test, guint8 reg, guint8 value, GError **error)
{
  gsize length = fpi_fte3600_build_boot38_write (test->tx, FT9338_TRANSFER_SIZE,
                                                 reg, value);

  report (test, "boot38 register %02x <- %02x", reg, value);
  return exchange (test, length, error);
}

/* Disabling OTP is cleanup, so cancellation cannot suppress this transaction.
 * Keep the flag after a failed write: its physical outcome is uncertain. */
static gboolean
disable_otp (Test *test, GError **error)
{
  gsize length = fpi_fte3600_build_boot38_write (test->tx, FT9338_TRANSFER_SIZE,
                                                 FTE3600_BOOT_REG_OTP_CONTROL, 0);

  report (test, "disable OTP access (f4 <- 00)");
  if (!exchange_unchecked (test, length, error))
    return FALSE;
  test->otp_enabled = FALSE;
  return TRUE;
}

static gboolean
reset_and_sync (Test *test, GError **error)
{
  if (!proceed (test, error))
    return FALSE;
  test->touched = TRUE;
  report (test, "reset physical H10/L20/H, immediately followed by 55 aa");
  return callback_result (test->io->reset_and_sync (test->io->user_data, error),
                          "reset and sync", error) && proceed (test, error);
}

/* A failed low request may have reached the controller. Complete its low hold
 * and release before considering either its error or cancellation. */
static gboolean
reset_pulse (Test *test, GError **error)
{
  g_autoptr(GError) failure = NULL;
  GError *step_error = NULL;

  if (!proceed (test, error))
    return FALSE;
  test->touched = TRUE;
  if (!reset_value (test, FALSE, &failure) ||
      !delay (test, FTE3600_RESET_HIGH_MS, &failure))
    goto release;

  reset_value (test, TRUE, &failure);
  delay (test, FTE3600_RESET_LOW_MS, &step_error);
  merge_error (&failure, step_error);
  step_error = NULL;

release:
  reset_value (test, FALSE, &step_error);
  merge_error (&failure, step_error);
  if (failure)
    {
      g_propagate_error (error, g_steal_pointer (&failure));
      return FALSE;
    }
  return proceed (test, error);
}

static gboolean
select_boot_a (Test *test, GError **error)
{
  guint8 value;

  report (test, "factory boot-A identification");
  if (!reset_and_sync (test, error) ||
      !boot_read (test, FTE3600_BOOT38_CONFIG_CB, &value, error) ||
      !boot_write (test, FTE3600_BOOT38_CONFIG_CB,
                   value | FTE3600_BOOT38_IDENTIFY_ENABLE, error) ||
      !boot_write (test, FTE3600_BOOT38_CONFIG_FD,
                   FTE3600_BOOT38_IDENTIFY_VALUE, error) ||
      !boot_write (test, FTE3600_BOOT38_ID_FE,
                   FTE3600_BOOT38_IDENTIFY_VALUE, error) ||
      !boot_read (test, FTE3600_BOOT38_ID_FE, &value, error))
    return FALSE;

  if (value == 2)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Boot-A selects FT9536; this test only supports FT9338");
      return FALSE;
    }
  /* Windows 273c8-273d6: FE==02 selects FT9536; every other value defaults
   * to FT9338. This is an explicit test of that fallback, not a silicon ID. */
  report (test, "Windows boot-A default selects FT9338 (fe=%02x); "
                "this is not a positive silicon ID", value);
  return TRUE;
}

static gboolean
select_boot_b (Test *test, GError **error)
{
  guint16 family;
  guint8 value;
  guint8 otp;

  report (test, "factory boot-B family query");
  if (!proceed (test, error))
    return FALSE;
  /* BOOT_ENTER changes the session even if a following mailbox transfer
   * fails. Arrange release-only cleanup for that partial transition. */
  test->touched = TRUE;
  if (!command (test, FTE3600_COMMAND_BOOT_ENTER, error) ||
      !command (test, FTE3600_COMMAND_FAMILY_QUERY, error) ||
      !delay (test, FTE3600_FAMILY_QUERY_DELAY_MS, error) ||
      !command (test, FTE3600_COMMAND_FAMILY_TRIGGER, error) ||
      !command (test, FTE3600_COMMAND_FAMILY_READ, error))
    return FALSE;

  family = ((guint16) test->rx[FTE3600_FAMILY_RESULT_OFFSET] << 8) |
           test->rx[FTE3600_FAMILY_RESULT_OFFSET + 1];
  report (test, "factory family = %04x", family);
  /* These are the three explicit A8 comparisons in Windows 2831b-28333.
   * Other mailbox values, including 1534, proceed to the boot38 OTP reader.
   * The mailbox alone does not identify FT9338. */
  if (family == 0x2b50 || family == 0x95a8 || family == 0x23dd)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Factory family %04x selects A8; FT9338 test stopped", family);
      return FALSE;
    }

  report (test, "factory boot38 OTP identification");
  if (!reset_and_sync (test, error) ||
      !boot_read (test, FTE3600_BOOT_REG_OTP_CONFIG, &value, error) ||
      !boot_write (test, FTE3600_BOOT_REG_OTP_CONFIG,
                   value | FTE3600_BOOT38_OTP_ENABLE, error) ||
      !boot_write (test, FTE3600_BOOT_REG_OTP_ADDRESS,
                   FTE3600_BOOT38_OTP_ADDRESS, error) ||
      !boot_read (test, FTE3600_BOOT_REG_OTP_CONTROL, &value, error))
    return FALSE;

  test->otp_enabled = TRUE;
  if (!boot_write (test, FTE3600_BOOT_REG_OTP_CONTROL,
                   value | FTE3600_BOOT38_OTP_ENABLE, error) ||
      !boot_read (test, FTE3600_BOOT_REG_OTP_DATA, &otp, error) ||
      !disable_otp (test, error) || !proceed (test, error))
    return FALSE;

  if (otp == 0xff)
    {
      report (test, "Windows blank-OTP default selects FT9338 (otp=ff); "
                    "this is not a positive silicon ID");
      return TRUE;
    }
  if ((otp >> 4) == 1)
    {
      report (test, "Windows OTP classification selects FT9338 (otp=%02x)", otp);
      return TRUE;
    }
  if ((otp >> 4) == 2)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "OTP %02x selects FT9536; this test only supports FT9338", otp);
  else
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "OTP %02x has no Windows boot38 chip mapping; no firmware uploaded",
                 otp);
  return FALSE;
}

static gboolean
select_ft9338 (Test *test, GError **error)
{
  guint8 marker;

  report (test, "factory boot marker");
  if (!command (test, FTE3600_COMMAND_BOOT_PROBE, error))
    return FALSE;
  marker = test->rx[FTE3600_BOOT_PROBE_RESULT_OFFSET];
  report (test, "boot marker = %02x", marker);
  return marker == FTE3600_BOOT_A_MARKER ? select_boot_a (test, error) :
         select_boot_b (test, error);
}

static gboolean
app_read (Test *test, guint8 reg, guint8 *values, gsize count, GError **error)
{
  gsize length = fpi_fte3600_build_app_read (test->tx, FT9338_TRANSFER_SIZE,
                                             reg, count, error);

  if (!length || !exchange (test, length, error))
    return FALSE;
  memcpy (values, test->rx + FTE3600_REG_RESULT_OFFSET, count);
  return TRUE;
}

static gboolean
app_write (Test *test, guint8 reg, guint8 value, GError **error)
{
  gsize length = fpi_fte3600_build_app_write (test->tx, FT9338_TRANSFER_SIZE,
                                              reg, value, error);

  report (test, "application register %02x <- %02x", reg, value);
  return length && exchange (test, length, error);
}

static gboolean
wait_idle (Test *test, GError **error)
{
  guint8 status[2] = { 0, 0 };

  g_autoptr(GError) read_error = NULL;

  for (guint attempt = 0; attempt < FTE3600_BOOT38_POLL_ATTEMPTS; attempt++)
    {
      g_clear_error (&read_error);
      if (app_read (test, FTE3600_REG_MCU_STATUS, status, sizeof status, &read_error))
        {
          report (test, "MCU poll %u = %02x %02x", attempt + 1, status[0], status[1]);
          if (status[0] == FTE3600_MCU_IDLE_HIGH && status[1] == FTE3600_MCU_IDLE_LOW)
            return TRUE;
        }
      else if ((!g_error_matches (read_error, G_IO_ERROR, G_IO_ERROR_FAILED) &&
                !g_error_matches (read_error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) ||
               !proceed (test, error))
        {
          if (!*error)
            g_propagate_error (error, g_steal_pointer (&read_error));
          return FALSE;
        }
      else
        {
          report (test, "MCU poll %u failed: %s", attempt + 1, read_error->message);
        }

      if (!delay (test, FTE3600_BOOT38_POLL_MS, error))
        return FALSE;
    }
  if (read_error)
    g_propagate_prefixed_error (error, g_steal_pointer (&read_error),
                                "MCU did not become idle in 20 polls: ");
  else
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                 "MCU did not become idle in 20 polls (last status %02x %02x)",
                 status[0], status[1]);
  return FALSE;
}

static gboolean
download_and_start (Test *test, const guint8 *firmware, GError **error)
{
  gsize length;

  report (test, "FT9338 volatile RAM download");
  if (!reset_and_sync (test, error) ||
      !boot_write (test, FTE3600_BOOT38_CONFIG_C8, FTE3600_BOOT38_CONFIG_ALL, error) ||
      !boot_write (test, FTE3600_BOOT38_CONFIG_CA, FTE3600_BOOT38_CONFIG_ALL, error) ||
      !boot_write (test, FTE3600_BOOT38_CONFIG_CB, FTE3600_BOOT38_CONFIG_ALL, error) ||
      !boot_write (test, FTE3600_BOOT38_CONFIG_B9, FTE3600_BOOT38_CONFIG_PREPARE, error) ||
      !boot_write (test, FTE3600_BOOT38_CONFIG_B9, FTE3600_BOOT38_CONFIG_ALL, error) ||
      !delay (test, FTE3600_BOOT38_CONFIG_MS, error))
    return FALSE;

  length = fpi_fte3600_build_firmware (test->tx, FT9338_TRANSFER_SIZE, firmware,
                                       FT9338_FIRMWARE_SIZE, error);
  if (!length || !exchange (test, length, error) ||
      !delay (test, FTE3600_BOOT38_UPLOAD_MS, error))
    return FALSE;
  report (test, "verify all 14184 firmware bytes by RAM readback");
  length = fpi_fte3600_build_boot38_readback (test->tx, FT9338_TRANSFER_SIZE,
                                              FT9338_FIRMWARE_SIZE);
  if (!exchange (test, length, error))
    return FALSE;
  if (memcmp (test->rx + FTE3600_BOOT38_READBACK_OFFSET,
              firmware, FT9338_FIRMWARE_SIZE) != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "FT9338 RAM readback mismatch; application not started");
      return FALSE;
    }

  report (test, "start FT9338: two hardware reset pulses, then 80 ms");
  return reset_pulse (test, error) &&
         delay (test, FTE3600_RESET_GAP_MS, error) &&
         reset_pulse (test, error) &&
         delay (test, FTE3600_BOOT38_FT9338_START_MS, error) &&
         wait_idle (test, error);
}

static gboolean
configure_and_validate (Test *test, Fte3600Identity *result, GError **error)
{
  guint8 marker;
  guint8 geometry[2];
  guint8 firmware_version;
  guint8 agc_version;

  /* The FT9338 cold path calls MCU configuration directly after RAM startup.
   * No software 70 reset or old driver's warm initialization is inserted. */
  report (test, "FT9338 MCU configuration");
  if (!app_write (test, FTE3600_REG_CONFIG_01, FTE3600_CONFIG_01_ENABLE, error) ||
      !delay (test, FTE3600_38_CONFIG_DELAY_MS, error) ||
      !app_write (test, FTE3600_REG_CONFIG_41, FTE3600_CONFIG_41_VALUE, error) ||
      !delay (test, FTE3600_38_CONFIG_DELAY_MS, error) ||
      !app_write (test, FTE3600_REG_CONFIG_MARKER, FTE3600_CONFIGURED_MARKER, error) ||
      !delay (test, FTE3600_38_CONFIG_DELAY_MS, error) ||
      !app_read (test, FTE3600_REG_CONFIG_MARKER, &marker, 1, error))
    return FALSE;
  report (test, "MCU configuration marker = %02x", marker);
  /* Windows 36b6e-36bc2 logs a non-BB marker and returns success. Do not add
   * a startup gate, corrective writes or a retry sequence for this reply. */
  if (marker != FTE3600_CONFIGURED_MARKER)
    report (test, "WARNING: MCU configuration marker is %02x, expected bb; "
                  "continuing as Windows does", marker);

  report (test, "read-only FT9338 runtime validation");
  if (!app_read (test, FTE3600_REG_SENSOR_ID_HIGH, geometry, 1, error) ||
      !app_read (test, FTE3600_REG_SENSOR_ID_LOW, geometry + 1, 1, error) ||
      !app_read (test, FTE3600_REG_FW_VERSION, &firmware_version, 1, error) ||
      !app_read (test, FTE3600_REG_AGC_VERSION, &agc_version, 1, error))
    return FALSE;
  report (test, "runtime geometry = %02x %02x, firmware = %02x, AGC = %02x",
          geometry[0], geometry[1], firmware_version, agc_version);
  if (geometry[0] != 0x58 || geometry[1] != 0x58 ||
      firmware_version != FTE3600_FT9338_FW_VERSION ||
      agc_version != FTE3600_FT9338_AGC_VERSION)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Hardware validation failed: expected geometry 58 58, "
                           "firmware 40 and AGC 10");
      return FALSE;
    }
  *result = (Fte3600Identity){
    .sensor = FTE3600_SENSOR_FT9338,
    .evidence = FTE3600_IDENTITY_RUNTIME_GEOMETRY,
    .response = 0x5858,
  };
  return TRUE;
}

gboolean
fte3600_medion_test_ft9338 (const Fte3600MedionIdentifyIo *io,
                            GBytes *firmware, Fte3600Identity *result, GError **error)
{
  g_autoptr(GError) failure = NULL;
  g_autofree guint8 *tx = NULL;
  g_autofree guint8 *rx = NULL;
  GError *cleanup_error = NULL;
  Fte3600Identity verified = { 0 };
  Test test = { .io = io };
  const guint8 *payload;
  gsize size;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  if (result)
    memset (result, 0, sizeof *result);
  if (!io || !result || !firmware || !io->exchange || !io->set_reset ||
      !io->reset_and_sync || !io->wait || !io->check_cancelled)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "FT9338 test requires firmware, result and complete I/O callbacks");
      return FALSE;
    }
  payload = g_bytes_get_data (firmware, &size);
  if (size != FT9338_FIRMWARE_SIZE || io->max_transfer < FT9338_TRANSFER_SIZE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "FT9338 test requires 14184 firmware bytes and 14192-byte SPI transfers");
      return FALSE;
    }
  tx = g_malloc0 (FT9338_TRANSFER_SIZE);
  rx = g_malloc0 (FT9338_TRANSFER_SIZE);
  test.tx = tx;
  test.rx = rx;

  if (!select_ft9338 (&test, &failure))
    g_prefix_error (&failure, "factory identification: ");
  else if (!download_and_start (&test, payload, &failure))
    g_prefix_error (&failure, "FT9338 RAM startup: ");
  else if (!configure_and_validate (&test, &verified, &failure))
    g_prefix_error (&failure, "FT9338 MCU configuration/runtime validation: ");

  if (test.otp_enabled)
    {
      disable_otp (&test, &cleanup_error);
      merge_error (&failure, cleanup_error);
      cleanup_error = NULL;
    }
  if (test.touched)
    {
      report (&test, "cleanup: release reset high without another reset pulse");
      reset_value (&test, FALSE, &cleanup_error);
      merge_error (&failure, cleanup_error);
      cleanup_error = NULL;
    }
  if (!failure)
    proceed (&test, &failure);
  if (failure)
    {
      g_propagate_error (error, g_steal_pointer (&failure));
      return FALSE;
    }
  *result = verified;
  report (&test, "FT9338 RAM startup validated; MCU configuration sequence completed "
                 "(see marker readback above); no image captured");
  return TRUE;
}
