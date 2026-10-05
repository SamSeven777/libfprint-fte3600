/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Independently expressed, explicitly requested Medion ROM diagnostics.
 */
#include "fte3600-medion-identify.h"
#include "drivers/fte3600-protocol.h"
#include "drivers/fte3600-legacy-recovery-protocol.h"
#include "drivers/fte3600-legacy-recovery-timing.h"

#include <stdarg.h>
#include <string.h>

typedef struct
{
  const Fte3600MedionIdentifyIo *io;
  gboolean                       touched;
  gboolean                       otp_pending;
  gboolean                       otp_38;
} Diagnostic;

typedef struct
{
  Fte3600Identity identity;
  guint8          boot_marker;
  guint16         family;
} Observation;

static void report (Diagnostic  *diag,
                    const gchar *format,
                    ...) G_GNUC_PRINTF (2, 3);

static void
report (Diagnostic *diag, const gchar *format, ...)
{
  va_list args;
  g_autofree gchar *message = NULL;

  if (!diag->io->report)
    return;
  va_start (args, format);
  message = g_strdup_vprintf (format, args);
  va_end (args);
  diag->io->report (diag->io->user_data, message);
}

/* Retain the first domain/code and diagnostic, including cleanup failures. */
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
proceed (Diagnostic *diag, GError **error)
{
  gboolean cancelled = diag->io->check_cancelled (diag->io->user_data, error);

  if (!cancelled && !*error)
    return TRUE;
  if (!*error)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                         "Medion sensor identification cancelled");
  return FALSE;
}

static gboolean
delay (Diagnostic *diag, guint milliseconds, GError **error)
{
  return callback_result (diag->io->wait (diag->io->user_data, milliseconds, error),
                          "wait", error);
}

static gboolean
reset_value (Diagnostic *diag, gboolean asserted, GError **error)
{
  return callback_result (diag->io->set_reset (diag->io->user_data, asserted, error),
                          asserted ? "assert reset" : "release reset", error);
}

/* Do not consult cancellation inside a pulse. Even a failed assertion can have
 * changed the pin, so hold the low interval and attempt release in that case. */
static gboolean
reset_pulse (Diagnostic *diag, GError **error)
{
  g_autoptr(GError) failure = NULL;
  GError *step_error = NULL;

  diag->touched = TRUE;
  report (diag, "reset physical H10/L20/H");
  if (!reset_value (diag, FALSE, &failure) ||
      !delay (diag, FTE3600_RESET_HIGH_MS, &failure))
    goto release;

  reset_value (diag, TRUE, &failure);
  delay (diag, FTE3600_RESET_LOW_MS, &step_error);
  merge_error (&failure, step_error);
  step_error = NULL;

release:
  reset_value (diag, FALSE, &step_error);
  merge_error (&failure, step_error);
  if (failure)
    {
      g_propagate_error (error, g_steal_pointer (&failure));
      return FALSE;
    }
  return TRUE;
}

static gboolean
exchange (Diagnostic *diag, const guint8 *tx, guint8 *rx, gsize length,
          gboolean cancellable, GError **error)
{
  if (cancellable && !proceed (diag, error))
    return FALSE;
  if (!length || length > diag->io->max_transfer)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                           "Diagnostic transaction exceeds transport capacity");
      return FALSE;
    }
  memset (rx, 0, length);
  return callback_result (diag->io->exchange (diag->io->user_data, tx, rx,
                                              length, error),
                          "SPI exchange", error);
}

static gboolean
command (Diagnostic *diag, Fte3600Command which, guint8 *rx, GError **error)
{
  guint8 tx[FTE3600_COMMAND_MAX_SIZE];
  gsize length = fpi_fte3600_build_command (tx, sizeof tx, which, error);

  return length && exchange (diag, tx, rx, length, TRUE, error);
}

static gboolean
register_read (Diagnostic *diag, gboolean application, gboolean boot38,
               guint8 reg, guint8 *value, GError **error)
{
  guint8 tx[FTE3600_REG_WRITE_SIZE];
  guint8 rx[sizeof tx];
  gsize length;

  if (application)
    length = fpi_fte3600_build_app_read (tx, sizeof tx, reg, 1, error);
  else if (boot38)
    length = fpi_fte3600_build_boot38_read (tx, sizeof tx, reg);
  else
    length = fpi_fte3600_build_boot_read (tx, sizeof tx, reg, 1, error);

  if (!length || !exchange (diag, tx, rx, length, TRUE, error))
    return FALSE;
  *value = rx[boot38 ? FTE3600_BOOT38_RESULT_OFFSET : FTE3600_REG_RESULT_OFFSET];
  report (diag, "%s read %02x = %02x", application ? "app" : boot38 ? "boot38" : "bootA8",
          reg, *value);
  return TRUE;
}

static gboolean
register_write (Diagnostic *diag, gboolean boot38, guint8 reg, guint8 value,
                gboolean cancellable, GError **error)
{
  guint8 tx[FTE3600_REG_WRITE_SIZE];
  guint8 rx[sizeof tx];
  gsize length;

  if (boot38)
    length = fpi_fte3600_build_boot38_write (tx, sizeof tx, reg, value);
  else
    length = fpi_fte3600_build_boot_write (tx, sizeof tx, reg, value, error);
  report (diag, "%s write %02x = %02x", boot38 ? "boot38" : "bootA8", reg, value);
  return length && exchange (diag, tx, rx, length, cancellable, error);
}

static gboolean
read_geometry (Diagnostic *diag, guint16 *geometry, GError **error)
{
  guint8 high, low;

  if (!register_read (diag, TRUE, FALSE, FTE3600_REG_SENSOR_ID_HIGH, &high, error) ||
      !register_read (diag, TRUE, FALSE, FTE3600_REG_SENSOR_ID_LOW, &low, error))
    return FALSE;
  *geometry = ((guint16) high << 8) | low;
  return TRUE;
}

/* A positive response must repeat unchanged. A first blank response carries no
 * identity and may enter the MCU/settle fallback; unknown nonempty data may not
 * authorize ROM writes. */
static gboolean
awake_identity (Diagnostic *diag, Fte3600Identity *identity, GError **error)
{
  guint16 first, second;

  if (!read_geometry (diag, &first, error))
    return FALSE;
  if (first == 0 || first == 0xffff)
    return TRUE;
  *identity = fpi_fte3600_identify_runtime (first >> 8, first & 0xff);
  if (identity->sensor == FTE3600_SENSOR_UNKNOWN)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Unknown nonempty awake geometry %04x; no ROM writes attempted", first);
      return FALSE;
    }
  if (!read_geometry (diag, &second, error))
    return FALSE;
  if (first != second)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Awake geometry changed: %04x versus %04x", first, second);
      return FALSE;
    }
  return proceed (diag, error);
}

static gboolean
wake_application (Diagnostic *diag, Fte3600Identity *identity, GError **error)
{
  guint8 tx[FTE3600_COMMAND_MAX_SIZE], rx[sizeof tx];
  gsize length;

  for (guint attempt = 0; attempt < FTE3600_LEGACY_WAKE_MAX_ATTEMPTS; attempt++)
    {
      if (!proceed (diag, error))
        return FALSE;
      report (diag, "legacy software wake attempt %u/%u", attempt + 1,
              FTE3600_LEGACY_WAKE_MAX_ATTEMPTS);
      length = fpi_fte3600_build_command (tx, sizeof tx, FTE3600_COMMAND_SOFT_RESET, error);
      /* Like the normal driver, finish the bounded pair before observing
       * cancellation. A failed transfer is never retried or followed by ROM. */
      if (!length || !exchange (diag, tx, rx, length, FALSE, error) ||
          !delay (diag, FTE3600_SOFT_RESET_INTERVAL_MS, error) ||
          !exchange (diag, tx, rx, length, FALSE, error) ||
          !delay (diag, FTE3600_LEGACY_WAKE_REPLY_MS, error) ||
          !proceed (diag, error))
        return FALSE;
      if (attempt == 0)
        {
          if (!awake_identity (diag, identity, error))
            return FALSE;
          if (identity->sensor != FTE3600_SENSOR_UNKNOWN)
            return TRUE;
        }

      length = fpi_fte3600_build_app_read (tx, sizeof tx, FTE3600_REG_MCU_STATUS, 2, error);
      if (!length || !exchange (diag, tx, rx, length, TRUE, error))
        return FALSE;
      report (diag, "app read 20/21 = %02x %02x", rx[FTE3600_REG_RESULT_OFFSET],
              rx[FTE3600_REG_RESULT_OFFSET + 1]);
      if (rx[FTE3600_REG_RESULT_OFFSET] == FTE3600_MCU_IDLE_HIGH &&
          rx[FTE3600_REG_RESULT_OFFSET + 1] == FTE3600_MCU_IDLE_LOW)
        {
          if (!proceed (diag, error) || !delay (diag, FTE3600_LEGACY_WAKE_GEOMETRY_MS, error))
            return FALSE;
          return awake_identity (diag, identity, error);
        }
      if (attempt + 1 < FTE3600_LEGACY_WAKE_MAX_ATTEMPTS &&
          (!proceed (diag, error) || !delay (diag, FTE3600_LEGACY_WAKE_RETRY_MS, error)))
        return FALSE;
    }
  return proceed (diag, error);
}

static gboolean
disable_otp (Diagnostic *diag, gboolean cancellable, GError **error)
{
  if (!register_write (diag, diag->otp_38, FTE3600_BOOT_REG_OTP_CONTROL, 0,
                       cancellable, error))
    return FALSE;
  diag->otp_pending = FALSE;
  return TRUE;
}

/* Once ROM state may have changed, cleanup is independent of cancellation.
 * Try reset even when disabling OTP failed, but do not conceal that error. */
static gboolean
cleanup (Diagnostic *diag, GError **error)
{
  g_autoptr(GError) failure = NULL;
  GError *step_error = NULL;

  if (!diag->touched)
    return TRUE;
  report (diag, "cleanup ROM access and reset");
  if (diag->otp_pending)
    disable_otp (diag, FALSE, &failure);
  reset_pulse (diag, &step_error);
  merge_error (&failure, step_error);
  step_error = NULL;
  /* 180 ms covers the longest known legacy startup, including unknown results. */
  delay (diag, FTE3600_BOOT38_FT9536_START_MS, &step_error);
  merge_error (&failure, step_error);
  if (failure)
    {
      g_prefix_error (&failure, "cleanup failed: ");
      report (diag, "%s", failure->message);
      g_propagate_error (error, g_steal_pointer (&failure));
      return FALSE;
    }
  diag->touched = FALSE;
  return TRUE;
}

static gboolean
reset_sync (Diagnostic *diag, GError **error)
{
  guint8 rx[FTE3600_COMMAND_MAX_SIZE];

  if (!proceed (diag, error) || !reset_pulse (diag, error))
    return FALSE;
  report (diag, "boot sync 55 aa");
  return command (diag, FTE3600_COMMAND_BOOT_SYNC, rx, error);
}

static gboolean
read_otp (Diagnostic *diag, gboolean boot38, guint8 *otp, GError **error)
{
  guint8 value;

  if (!register_read (diag, FALSE, boot38, FTE3600_BOOT_REG_OTP_CONFIG, &value, error) ||
      !register_write (diag, boot38, FTE3600_BOOT_REG_OTP_CONFIG,
                       boot38 ? value | FTE3600_BOOT38_OTP_ENABLE : FTE3600_BOOT_OTP_CONFIG,
                       TRUE, error) ||
      !register_write (diag, boot38, FTE3600_BOOT_REG_OTP_ADDRESS,
                       FTE3600_BOOT_OTP_ADDRESS, TRUE, error) ||
      !register_read (diag, FALSE, boot38, FTE3600_BOOT_REG_OTP_CONTROL, &value, error))
    return FALSE;

  diag->otp_38 = boot38;
  diag->otp_pending = TRUE; /* The write could reach hardware even on I/O failure. */
  if (!register_write (diag, boot38, FTE3600_BOOT_REG_OTP_CONTROL,
                       value | FTE3600_BOOT_OTP_ENABLE, TRUE, error) ||
      !register_read (diag, FALSE, boot38, FTE3600_BOOT_REG_OTP_DATA, otp, error) ||
      !disable_otp (diag, TRUE, error))
    return FALSE;
  return TRUE;
}

static gboolean
boot_a (Diagnostic *diag, Observation *observation, GError **error)
{
  guint8 value;

  if (!reset_sync (diag, error) ||
      !register_read (diag, FALSE, TRUE, FTE3600_BOOT38_CONFIG_CB, &value, error) ||
      !register_write (diag, TRUE, FTE3600_BOOT38_CONFIG_CB,
                       value | FTE3600_BOOT38_IDENTIFY_ENABLE, TRUE, error) ||
      !register_write (diag, TRUE, FTE3600_BOOT38_CONFIG_FD,
                       FTE3600_BOOT38_IDENTIFY_VALUE, TRUE, error) ||
      !register_write (diag, TRUE, FTE3600_BOOT38_ID_FE,
                       FTE3600_BOOT38_IDENTIFY_VALUE, TRUE, error) ||
      !register_read (diag, FALSE, TRUE, FTE3600_BOOT38_ID_FE, &value, error))
    return FALSE;

  observation->identity = fpi_fte3600_identify_boot_a (value);
  report (diag, "boot-A FE=%02x%s", value,
          value == 2 ? " identifies FT9536" : " remains unknown; no FT9338 default");
  return TRUE;
}

static gboolean
rom_round (Diagnostic *diag, Observation *observation, GError **error)
{
  guint8 rx[FTE3600_COMMAND_MAX_SIZE];
  guint8 otp;

  if (!command (diag, FTE3600_COMMAND_BOOT_PROBE, rx, error))
    return FALSE;
  observation->boot_marker = rx[FTE3600_BOOT_PROBE_RESULT_OFFSET];
  report (diag, "boot 90 marker = %02x", observation->boot_marker);
  if (observation->boot_marker == FTE3600_BOOT_A_MARKER)
    return boot_a (diag, observation, error);

  /* Windows 0x2819c issues this fixed mailbox query before choosing an OTP
   * protocol. A non-A8 reply is NOT by itself positive evidence for FT9338. */
  if (!proceed (diag, error))
    return FALSE;
  diag->touched = TRUE;
  report (diag, "ROM enter 06 f9 00; fixed family mailbox 85c0 query");
  if (!command (diag, FTE3600_COMMAND_BOOT_ENTER, rx, error) ||
      !command (diag, FTE3600_COMMAND_FAMILY_QUERY, rx, error) ||
      !delay (diag, FTE3600_FAMILY_QUERY_DELAY_MS, error) ||
      !command (diag, FTE3600_COMMAND_FAMILY_TRIGGER, rx, error) ||
      !command (diag, FTE3600_COMMAND_FAMILY_READ, rx, error))
    return FALSE;

  observation->family = ((guint16) rx[FTE3600_FAMILY_RESULT_OFFSET] << 8) |
                        rx[FTE3600_FAMILY_RESULT_OFFSET + 1];
  report (diag, "ROM family = %04x", observation->family);
  if (observation->family == 0x2b50 || observation->family == 0x95a8 ||
      observation->family == 0x23dd)
    {
      if (!read_otp (diag, FALSE, &otp, error))
        return FALSE;
      observation->identity = fpi_fte3600_identify_a8_spi (observation->family, otp);
      /* An all-ones bus is not proof of the valid low-nibble f variant. */
      if (otp == 0 || otp == 0xff)
        observation->identity.sensor = FTE3600_SENSOR_UNKNOWN;
      report (diag, "A8 family=%04x OTP=%02x low-nibble=%x", observation->family,
              otp, otp & 0x0f);
    }
  else
    {
      report (diag, "non-A8 response; explicit Medion boot38 candidate experiment");
      if (!reset_sync (diag, error) || !read_otp (diag, TRUE, &otp, error))
        return FALSE;
      observation->identity = fpi_fte3600_identify_boot_b38_spi (otp);
      observation->identity.response = observation->family;
      report (diag, "boot38 OTP=%02x upper-nibble=%x; candidate evidence only",
              otp, otp >> 4);
    }
  return TRUE;
}

gboolean
fte3600_medion_identify_legacy (const Fte3600MedionIdentifyIo *io,
                                Fte3600Identity *result, GError **error)
{
  Diagnostic diag = { .io = io };
  Observation observed[2] = { 0 };
  guint16 runtime[2];

  g_autoptr(GError) failure = NULL;

  if (result)
    memset (result, 0, sizeof *result);
  if (!io || !result || !io->exchange || !io->set_reset ||
      !io->wait || !io->check_cancelled)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Legacy diagnostic requires complete I/O callbacks and a result");
      return FALSE;
    }
  if (io->max_transfer < FTE3600_COMMAND_MAX_SIZE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                           "Legacy diagnostic requires an 11-byte transaction capacity");
      return FALSE;
    }

  report (&diag, "application geometry; no firmware will be loaded");
  for (guint round = 0; round < G_N_ELEMENTS (runtime); round++)
    if (!read_geometry (&diag, &runtime[round], &failure))
      goto out;
  if (runtime[0] != runtime[1])
    {
      g_set_error (&failure, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Application geometry changed: %04x versus %04x", runtime[0], runtime[1]);
      goto out;
    }
  observed[0].identity = fpi_fte3600_identify_runtime (runtime[0] >> 8, runtime[0] & 0xff);
  if (observed[0].identity.sensor != FTE3600_SENSOR_UNKNOWN)
    {
      if (!proceed (&diag, &failure))
        goto out;
      *result = observed[0].identity;
      return TRUE;
    }
  if (runtime[0] != 0 && runtime[0] != 0xffff)
    {
      g_set_error (&failure, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Unknown nonempty runtime geometry %04x; no ROM writes attempted", runtime[0]);
      goto out;
    }

  if (!wake_application (&diag, &observed[0].identity, &failure))
    goto out;
  if (observed[0].identity.sensor != FTE3600_SENSOR_UNKNOWN)
    {
      *result = observed[0].identity;
      return TRUE;
    }

  memset (observed, 0, sizeof observed);
  for (guint round = 0; round < G_N_ELEMENTS (observed); round++)
    {
      GError *cleanup_error = NULL;

      report (&diag, "ROM diagnostic round %u", round + 1);
      rom_round (&diag, &observed[round], &failure);
      cleanup (&diag, &cleanup_error);
      merge_error (&failure, cleanup_error);
      if (failure || !proceed (&diag, &failure))
        goto out;
      if (observed[round].identity.sensor == FTE3600_SENSOR_UNKNOWN)
        {
          g_set_error_literal (&failure, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                               "ROM observations do not identify a supported legacy candidate");
          goto out;
        }
    }

  if (observed[0].boot_marker != observed[1].boot_marker ||
      observed[0].family != observed[1].family ||
      observed[0].identity.sensor != observed[1].identity.sensor ||
      observed[0].identity.evidence != observed[1].identity.evidence ||
      observed[0].identity.response != observed[1].identity.response ||
      observed[0].identity.otp != observed[1].identity.otp)
    {
      g_set_error_literal (&failure, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "ROM observations changed between independent rounds");
      goto out;
    }

  *result = observed[0].identity;
  return TRUE;

out:
  /* Each ROM round already ran cleanup, even on failure. Application reads or
   * software-wake failures must not trigger a GPIO reset or OTP write. */
  if (!failure)
    g_set_error_literal (&failure, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Legacy diagnostic failed without a reported reason");
  g_propagate_error (error, g_steal_pointer (&failure));
  return FALSE;
}
