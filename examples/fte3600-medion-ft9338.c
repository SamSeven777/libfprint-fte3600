/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Independently expressed FT9338 diagnostic from observed Windows behavior.
 * Evidence: ftWbioUmdfDriverV2.dll, package 2.0.3.102; SPI factory 23faf-24244,
 * application wake 28344/28c54, legacy dispatch 2819c,
 * boot-A 27414, boot-B OTP 27650, RAM startup 365c0, MCU configuration 36a30.
 */
#include "fte3600-medion-ft9338.h"
#include "drivers/fte3600-protocol.h"
#include "drivers/fte3600-legacy-recovery-protocol.h"
#include "drivers/fte3600-legacy-recovery-timing.h"
#include "drivers/fte3600-ft9368-protocol.h"
#include "drivers/fte3600-fw9369-protocol.h"
#include "drivers/fte3600-special-probe-timing.h"

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
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
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
special_command (Test *test, Fte3600Fw9369Command cmd, GError **error)
{
  gsize length = fpi_fte3600_fw9369_build_command (test->tx, FT9338_TRANSFER_SIZE,
                                                   cmd, error);

  return length && exchange (test, length, error);
}

static gboolean
special_read (Test *test, guint8 reg, guint8 *value, GError **error)
{
  gsize length = fpi_fte3600_fw9369_build_sfr_read (test->tx, FT9338_TRANSFER_SIZE,
                                                    reg, error);

  if (!length || !exchange (test, length, error))
    return FALSE;
  *value = test->rx[FTE3600_FW9369_SFR_RESULT_OFFSET];
  return TRUE;
}

static gboolean
special_probe (Test *test, guint16 *id, GError **error)
{
  guint8 value;
  gsize length;

  /* SPI factory 2411c/241a7 -> fba0(0) -> 10f0c. */
  if (!special_command (test, FTE3600_FW9369_CMD_WAKE, error) ||
      !delay (test, FTE3600_SPECIAL_COMMAND_DELAY_MS, error) ||
      !special_read (test, FTE3600_FW9369_SFR_STATE, &value, error))
    return FALSE;
  if (value != FTE3600_FW9369_STATE_IDLE &&
      (!special_command (test, FTE3600_FW9369_CMD_IDLE_1, error) ||
       !delay (test, FTE3600_SPECIAL_COMMAND_DELAY_MS, error)))
    return FALSE;
  if (!special_command (test, FTE3600_FW9369_CMD_WAKE_END, error))
    return FALSE;

  /* 10a74 and 10d70 each call fd94. Exhausted C6 acknowledgement does not
   * suppress the subsequent ID read; an actual transport error still stops. */
  for (guint pass = 0; pass < FTE3600_SPECIAL_MODE_CONFIG_PASSES; pass++)
    {
      for (guint attempt = 0; attempt < FTE3600_SPECIAL_MODE_ATTEMPTS; attempt++)
        {
          length = fpi_fte3600_fw9369_build_sfr_write (test->tx, FT9338_TRANSFER_SIZE,
                                                       FTE3600_FW9369_SFR_SPI_MODE, 1, error);
          if (!length || !exchange (test, length, error) ||
              !delay (test, FTE3600_SPECIAL_MODE_DELAY_MS, error) ||
              !special_read (test, FTE3600_FW9369_SFR_SPI_MODE, &value, error))
            return FALSE;
          if (value == 1)
            break;
        }
      report (test, "factory C6 pass %u: readback %02x", pass + 1, value);
    }
  length = fpi_fte3600_fw9369_build_word_read (test->tx, FT9338_TRANSFER_SIZE,
                                               FTE3600_FW9369_WORD_CHIP_ID, error);
  if (!length || !exchange (test, length, error))
    return FALSE;
  *id = ((guint16) test->rx[FTE3600_FW9369_WORD_RESULT_OFFSET] << 8) |
        test->rx[FTE3600_FW9369_WORD_RESULT_OFFSET + 1];
  report (test, "factory word ID = %04x", *id);
  return TRUE;
}

static gboolean
soft_reset_pair (Test *test, GError **error)
{
  gsize length = fpi_fte3600_build_command (test->tx, FT9338_TRANSFER_SIZE,
                                            FTE3600_COMMAND_SOFT_RESET, error);

  /* Finish the bounded pair before observing a cancellation received during
  * the first write or the 5 ms wait. Never replay a failed SPI transfer. */
  return length && proceed (test, error) && exchange_unchecked (test, length, error) &&
         delay (test, FTE3600_SOFT_RESET_INTERVAL_MS, error) &&
         exchange_unchecked (test, length, error) && proceed (test, error);
}

static gboolean
factory_prepare (Test *test, guint round, gboolean *running, GError **error)
{
  guint8 status[2], geometry[2];
  guint16 id;
  gsize length;

  *running = FALSE;
  if (!proceed (test, error))
    return FALSE;
  test->touched = TRUE;
  /* 23faf-24244: round 2 adds a reset and a third FT9368 query. It is a
   * real discovery retry, not one of rounds 3/4's forced FT9368 downloads. */
  report (test, "factory application round %u/%u", round + 1,
          FTE3600_FACTORY_FT9338_ROUNDS);
  for (guint attempt = 0;
       attempt < FTE3600_FACTORY_FT9368_ATTEMPTS + (round == 2); attempt++)
    {
      if (round == 2 && attempt == 1 &&
          (!reset_pulse (test, error) ||
           !delay (test, FTE3600_FACTORY_RETRY_RESET_MS, error)))
        return FALSE;
      length = fpi_fte3600_ft9368_read (test->tx, FT9338_TRANSFER_SIZE,
                                        FTE3600_FT9368_WAKE, 0);
      if (!exchange (test, length, error) ||
          !delay (test, FTE3600_FACTORY_FT9368_WAKE_MS, error))
        return FALSE;
      length = fpi_fte3600_ft9368_read (test->tx, FT9338_TRANSFER_SIZE,
                                        FTE3600_FT9368_INFO, FTE3600_FT9368_INFO_SIZE);
      if (!exchange (test, length, error))
        return FALSE;
      /* 26d9c extracts the model at information bytes 19/20. Do not
       * reinterpret a positive other-family result as a legacy device. */
      id = ((guint16) test->rx[FTE3600_FT9368_HEADER + 19] << 8) |
           test->rx[FTE3600_FT9368_HEADER + 20];
      report (test, "factory FT9368 information ID = %04x", id);
      if (id == 0x9368)
        goto other_sensor;
    }
  for (guint attempt = 0; attempt < 2; attempt++)
    {
      if (!special_probe (test, &id, error))
        return FALSE;
      Fte3600Identity candidate = fpi_fte3600_identify_special (id);

      if (candidate.sensor != FTE3600_SENSOR_UNKNOWN ||
          candidate.evidence == FTE3600_IDENTITY_KNOWN_UNMAPPED_ID)
        goto other_sensor;
      if (attempt == 0 &&
          (!reset_pulse (test, error) ||
           !delay (test, FTE3600_SPECIAL_RESET_SETTLE_MS, error)))
        return FALSE;
    }
  if (round != 1)
    return TRUE;
  /* 24221 -> 28344 -> 270e4: paired 70, then MCU status immediately.
   * The 350 ms wait applies only after idle, never before boot sync. */
  for (guint attempt = 0; attempt < FTE3600_LEGACY_WAKE_MAX_ATTEMPTS; attempt++)
    {
      if (!soft_reset_pair (test, error) ||
          !app_read (test, FTE3600_REG_MCU_STATUS, status, sizeof status, error))
        return FALSE;
      report (test, "factory application wake %u/%u: MCU %02x %02x", attempt + 1,
              FTE3600_LEGACY_WAKE_MAX_ATTEMPTS, status[0], status[1]);
      if (status[0] == FTE3600_MCU_IDLE_HIGH && status[1] == FTE3600_MCU_IDLE_LOW)
        {
          if (!delay (test, FTE3600_LEGACY_WAKE_GEOMETRY_MS, error) ||
              !app_read (test, FTE3600_REG_SENSOR_ID_HIGH, geometry, 1, error) ||
              !app_read (test, FTE3600_REG_SENSOR_ID_LOW, geometry + 1, 1, error))
            return FALSE;
          id = ((guint16) geometry[0] << 8) | geometry[1];
          if (id != 0x5858)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Running application geometry %04x is not FT9338; no firmware uploaded", id);
              return FALSE;
            }
          *running = TRUE;
          return TRUE;
        }
      if (attempt + 1 < FTE3600_LEGACY_WAKE_MAX_ATTEMPTS &&
          !delay (test, FTE3600_LEGACY_WAKE_RETRY_MS, error))
        return FALSE;
    }
  report (test, "factory application checks exhausted; entering legacy ROM selection");
  return TRUE;

other_sensor:
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "Factory identified another sensor (%04x); FT9338 firmware will not be uploaded", id);
  return FALSE;
}

static gboolean
return_idle (Test *test, GError **error)
{
  guint8 mode;

  /* 28668: this also runs after ROM selection, before any RAM download. */
  if (!soft_reset_pair (test, error) ||
      !app_read (test, FTE3600_REG_MODE, &mode, 1, error))
    return FALSE;
  if (mode != FTE3600_MODE_QUICK_CAPTURE && mode != FTE3600_MODE_3 && mode != FTE3600_MODE_4 &&
      (!app_write (test, FTE3600_REG_START, FTE3600_CAPTURE_DISABLE, error) ||
       !app_write (test, FTE3600_REG_ENABLE, FTE3600_CAPTURE_DISABLE, error) ||
       !delay (test, FTE3600_ARM_DELAY_MS, error)))
    return FALSE;
  return TRUE;
}

static gboolean
firmware_ready (Test *test, gboolean *ready, GError **error)
{
  guint8 version, agc;

  *ready = FALSE;
  /* 2c8bc checks only these two bytes. No extra MCU/geometry reads, and a
   * version-read failure must reach the caller's download fallback. */
  if (!app_read (test, FTE3600_REG_FW_VERSION, &version, 1, error) ||
      !app_read (test, FTE3600_REG_AGC_VERSION, &agc, 1, error))
    return FALSE;
  report (test, "existing firmware = %02x, AGC = %02x", version, agc);
  *ready = version == FTE3600_FT9338_FW_VERSION && agc == FTE3600_FT9338_AGC_VERSION;
  return TRUE;
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
      gsize first = 0, mismatches = 0;
      gboolean all_zero = TRUE, all_ff = TRUE;
      const guint8 *readback = test->rx + FTE3600_BOOT38_READBACK_OFFSET;

      for (gsize i = 0; i < FT9338_FIRMWARE_SIZE; i++)
        {
          all_zero &= readback[i] == 0;
          all_ff &= readback[i] == 0xff;
          if (readback[i] != firmware[i])
            {
              if (mismatches == 0)
                first = i;
              mismatches++;
            }
        }
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "FT9338 RAM readback mismatch: first offset %" G_GSIZE_FORMAT
                   " expected %02x received %02x; %" G_GSIZE_FORMAT
                   "/14184 differ; all-zero=%s all-ff=%s; application not started",
                   first, firmware[first], readback[first], mismatches,
                   all_zero ? "yes" : "no", all_ff ? "yes" : "no");
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
download_with_retries (Test *test, const guint8 *firmware, GError **error)
{
  for (guint attempt = 1; attempt <= FTE3600_FIRMWARE_MAX_ATTEMPTS; attempt++)
    {
      g_autoptr(GError) failure = NULL;

      if (download_and_start (test, firmware, &failure))
        return TRUE;
      if (attempt == FTE3600_FIRMWARE_MAX_ATTEMPTS ||
          (!g_error_matches (failure, G_IO_ERROR, G_IO_ERROR_FAILED) &&
           !g_error_matches (failure, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) &&
           !g_error_matches (failure, G_IO_ERROR, G_IO_ERROR_INVALID_DATA)))
        {
          g_propagate_error (error, g_steal_pointer (&failure));
          return FALSE;
        }
      if (!proceed (test, error))
        return FALSE;
      report (test, "retry FT9338 RAM download after attempt %u: %s", attempt, failure->message);
    }
  g_assert_not_reached ();
}

static gboolean
configure_mcu (Test *test, GError **error)
{
  guint8 marker;

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
  return TRUE;
}

static gboolean
enter_sensor_mode (Test *test, GError **error)
{
  guint8 status[2], finger;

  /* 2c754 finishes with 287d4(1). The saved argument is written to the mode
   * register before enable/start; only the still-idle fallback writes zero.
   * A busy MCU first goes through 28668, without an invented polling loop. */
  report (test, "enter FT9338 sensor mode");
  if (!app_read (test, FTE3600_REG_MCU_STATUS, status, sizeof status, error))
    return FALSE;
  if ((status[0] != FTE3600_MCU_IDLE_HIGH || status[1] != FTE3600_MCU_IDLE_LOW) &&
      !return_idle (test, error))
    return FALSE;
  if (!app_write (test, FTE3600_REG_MODE, FTE3600_MODE_WAIT_FINGER, error) ||
      !app_write (test, FTE3600_REG_ENABLE, FTE3600_CAPTURE_ENABLE, error) ||
      !app_write (test, FTE3600_REG_START, FTE3600_CAPTURE_ENABLE, error) ||
      !delay (test, FTE3600_ARM_DELAY_MS, error) ||
      !app_read (test, FTE3600_REG_FINGER_STATUS, &finger, 1, error) ||
      !app_read (test, FTE3600_REG_MCU_STATUS, status, sizeof status, error))
    return FALSE;
  report (test, "sensor mode: finger status %02x, MCU %02x %02x",
          finger, status[0], status[1]);
  /* 289ce writes mode=0 if the MCU stayed idle. Windows reports this
   * condition but does not fail or replay initialization. */
  if (status[0] == FTE3600_MCU_IDLE_HIGH && status[1] == FTE3600_MCU_IDLE_LOW)
    {
      report (test, "WARNING: MCU remained idle after sensor-mode entry");
      return app_write (test, FTE3600_REG_MODE, FTE3600_MODE_IDLE, error);
    }
  return TRUE;
}

static gboolean
retryable (const GError *error)
{
  return g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED) ||
         g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
         g_error_matches (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static gboolean
load_firmware (Test *test, const guint8 *firmware, GError **error)
{
  g_autoptr(GError) version_error = NULL;
  gboolean ready = FALSE;

  /* 2c754: ReturnIdle -> CheckFwVersion -> optional download -> InitMcuConfig
   * -> SwitchNextSensorWorkMode(1), for cold and running FT9338 alike. */
  if (!return_idle (test, error))
    return FALSE;
  if (!firmware_ready (test, &ready, &version_error))
    {
      if (!retryable (version_error))
        {
          g_propagate_error (error, g_steal_pointer (&version_error));
          return FALSE;
        }
      report (test, "firmware version check failed; download required: %s",
              version_error->message);
    }
  if (!ready && !download_with_retries (test, firmware, error))
    return FALSE;
  return configure_mcu (test, error);
}

static gboolean
factory_initialize (Test *test, const guint8 *firmware, gboolean selected, GError **error)
{
  for (guint round = 0; round < FTE3600_FACTORY_FT9338_ROUNDS; round++)
    {
      g_autoptr(GError) failure = NULL;
      gboolean running = FALSE;

      if (!factory_prepare (test, round, &running, &failure))
        g_prefix_error (&failure, "factory preparation: ");
      else if (round == 0)
        continue;
      else if (!running && !selected && !select_ft9338 (test, &failure))
        g_prefix_error (&failure, "factory identification: ");
      else if (!load_firmware (test, firmware, &failure))
        g_prefix_error (&failure, "FT9338 initialization: ");
      else
        /* Windows ignores this last helper's result. The diagnostic reports
         * a real I/O failure, but must not turn it into another RAM download. */
        return enter_sensor_mode (test, error);

      /* Do not cross into another factory round with uncertain OTP state.
      * Cancellation and session loss are terminal, not vendor retries. */
      if (test->otp_enabled || !retryable (failure) ||
          round + 1 == FTE3600_FACTORY_FT9338_ROUNDS)
        {
          g_propagate_error (error, g_steal_pointer (&failure));
          return FALSE;
        }
      report (test, "factory round %u failed: %s; retrying vendor discovery",
              round + 1, failure->message);
    }
  g_assert_not_reached ();
}

static gboolean
validate_runtime (Test *test, Fte3600Identity *result, GError **error)
{
  guint8 geometry[2];
  guint8 firmware_version;
  guint8 agc_version;

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

static gboolean
run_ft9338 (const Fte3600MedionIdentifyIo *io, GBytes *firmware,
            gboolean selected, Fte3600Identity *result, GError **error)
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

  if (selected)
    report (&test, "manual FT9338 selection: skip ROM/OTP classification, retain factory preparation and initialization");
  if (!factory_initialize (&test, payload, selected, &failure))
    g_prefix_error (&failure, "FT9338 factory startup: ");
  else if (!validate_runtime (&test, &verified, &failure))
    g_prefix_error (&failure, "FT9338 runtime validation: ");

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
  report (&test, "FT9338 startup validated; MCU configuration sequence completed "
                 "(see marker readback above); no image captured");
  return TRUE;
}

gboolean
fte3600_medion_test_ft9338 (const Fte3600MedionIdentifyIo *io,
                            GBytes *firmware, Fte3600Identity *result, GError **error)
{
  return run_ft9338 (io, firmware, FALSE, result, error);
}

gboolean
fte3600_medion_start_ft9338 (const Fte3600MedionIdentifyIo *io,
                             GBytes *firmware, Fte3600Identity *result, GError **error)
{
  return run_ft9338 (io, firmware, TRUE, result, error);
}
