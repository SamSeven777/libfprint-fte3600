/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Independently expressed, explicit FT9338/FT9348 volatile RAM startup.
 */
#include "fte3600-medion-boot.h"
#include "drivers/fte3600-protocol.h"
#include "drivers/fte3600-legacy-recovery-protocol.h"
#include "drivers/fte3600-legacy-recovery-timing.h"

#include <stdarg.h>
#include <string.h>

typedef struct
{
  const Fte3600MedionIdentifyIo *io;
  const Fte3600SensorDescriptor *sensor;
  guint8                        *tx;
  guint8                        *rx;
  gsize                          capacity;
  gboolean                       touched;
  Fte3600Identity                verified;
} Boot;

static void report (Boot        *boot,
                    const gchar *format,
                    ...) G_GNUC_PRINTF (2, 3);

static void
report (Boot *boot, const gchar *format, ...)
{
  va_list args;
  g_autofree gchar *message = NULL;

  if (!boot->io->report)
    return;
  va_start (args, format);
  message = g_strdup_vprintf (format, args);
  va_end (args);
  boot->io->report (boot->io->user_data, message);
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
proceed (Boot *boot, GError **error)
{
  gboolean cancelled = boot->io->check_cancelled (boot->io->user_data, error);

  if (!cancelled && !*error)
    return TRUE;
  if (!*error)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                         "Medion RAM startup cancelled");
  return FALSE;
}

static gboolean
delay (Boot *boot, guint milliseconds, GError **error)
{
  return callback_result (boot->io->wait (boot->io->user_data, milliseconds, error),
                          "wait", error);
}

static gboolean
reset_value (Boot *boot, gboolean asserted, GError **error)
{
  return callback_result (boot->io->set_reset (boot->io->user_data, asserted, error),
                          asserted ? "assert reset" : "release reset", error);
}

/* Cancellation is deferred until the complete H10/L20/H pulse finishes. A
 * failed assertion may still have changed hardware: hold, then try release. */
static gboolean
reset_pulse (Boot *boot, GError **error)
{
  g_autoptr(GError) failure = NULL;
  GError *step_error = NULL;

  if (!proceed (boot, error))
    return FALSE;
  boot->touched = TRUE;
  report (boot, "reset physical H10/L20/H");
  if (!reset_value (boot, FALSE, &failure) ||
      !delay (boot, FTE3600_RESET_HIGH_MS, &failure))
    goto release;

  reset_value (boot, TRUE, &failure);
  delay (boot, FTE3600_RESET_LOW_MS, &step_error);
  merge_error (&failure, step_error);
  step_error = NULL;

release:
  reset_value (boot, FALSE, &step_error);
  merge_error (&failure, step_error);
  if (failure)
    {
      g_propagate_error (error, g_steal_pointer (&failure));
      return FALSE;
    }
  return TRUE;
}

static gboolean
exchange (Boot *boot, gsize length, GError **error)
{
  if (!proceed (boot, error))
    return FALSE;
  if (!length || length > boot->capacity || length > boot->io->max_transfer)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                           "RAM startup transaction exceeds transport capacity");
      return FALSE;
    }
  memset (boot->rx, 0, length);
  return callback_result (boot->io->exchange (boot->io->user_data, boot->tx,
                                              boot->rx, length, error),
                          "SPI exchange", error);
}

static gboolean
command (Boot *boot, Fte3600Command which, GError **error)
{
  gsize size = fpi_fte3600_build_command (boot->tx, boot->capacity, which, error);

  return size && exchange (boot, size, error);
}

static gboolean
read_register (Boot *boot, guint8 reg, guint count, GError **error)
{
  gsize size = fpi_fte3600_build_app_read (boot->tx, boot->capacity, reg, count, error);

  if (!size || !exchange (boot, size, error))
    return FALSE;
  if (count == 2)
    report (boot, "app read %02x = %02x %02x", reg,
            boot->rx[FTE3600_REG_RESULT_OFFSET], boot->rx[FTE3600_REG_RESULT_OFFSET + 1]);
  else
    report (boot, "app read %02x = %02x", reg, boot->rx[FTE3600_REG_RESULT_OFFSET]);
  return TRUE;
}

static gboolean
read_geometry (Boot *boot, guint16 *value, GError **error)
{
  guint8 high;

  if (!read_register (boot, FTE3600_REG_SENSOR_ID_HIGH, 1, error))
    return FALSE;
  high = boot->rx[FTE3600_REG_RESULT_OFFSET];
  if (!read_register (boot, FTE3600_REG_SENSOR_ID_LOW, 1, error))
    return FALSE;
  *value = ((guint16) high << 8) | boot->rx[FTE3600_REG_RESULT_OFFSET];
  return TRUE;
}

static gboolean
check_geometry (Boot *boot, gboolean before_start, GError **error)
{
  guint16 first, second;
  Fte3600Identity observed;

  if (!read_geometry (boot, &first, error) || !read_geometry (boot, &second, error))
    return FALSE;
  if (first != second)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Application geometry changed: %04x versus %04x", first, second);
      return FALSE;
    }
  if (before_start && (first == 0 || first == 0xffff))
    return TRUE;
  observed = fpi_fte3600_identify_runtime (first >> 8, first & 0xff);
  if (observed.sensor != boot->sensor->sensor)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Application geometry %04x does not match selected %s", first, boot->sensor->name);
      return FALSE;
    }
  if (!before_start)
    boot->verified = observed;
  return TRUE;
}

static gboolean
prepare_38 (Boot *boot, GError **error)
{
  const guint8 registers[] = { FTE3600_BOOT38_CONFIG_C8, FTE3600_BOOT38_CONFIG_CA,
                               FTE3600_BOOT38_CONFIG_CB, FTE3600_BOOT38_CONFIG_B9,
                               FTE3600_BOOT38_CONFIG_B9 };
  const guint8 values[] = { FTE3600_BOOT38_CONFIG_ALL, FTE3600_BOOT38_CONFIG_ALL,
                            FTE3600_BOOT38_CONFIG_ALL, FTE3600_BOOT38_CONFIG_PREPARE,
                            FTE3600_BOOT38_CONFIG_ALL };

  for (guint i = 0; i < G_N_ELEMENTS (registers); i++)
    {
      gsize size = fpi_fte3600_build_boot38_write (boot->tx, boot->capacity,
                                                   registers[i], values[i]);

      report (boot, "boot38 write %02x = %02x", registers[i], values[i]);
      if (!size || !exchange (boot, size, error))
        return FALSE;
    }
  return proceed (boot, error) && delay (boot, FTE3600_BOOT38_CONFIG_MS, error);
}

static gboolean
upload (Boot *boot, const guint8 *firmware, gsize length, GError **error)
{
  gsize size = fpi_fte3600_build_firmware (boot->tx, boot->capacity, firmware, length, error);

  report (boot, "%s RAM upload: %" G_GSIZE_FORMAT " bytes", boot->sensor->name, length);
  return size && exchange (boot, size, error) &&
         proceed (boot, error) && delay (boot, FTE3600_FW_UPLOAD_SETTLE_MS, error);
}

static gboolean
verify_38_readback (Boot *boot, const guint8 *firmware, gsize length, GError **error)
{
  gsize size = fpi_fte3600_build_boot38_readback (boot->tx, boot->capacity, length);

  report (boot, "FT9338 complete RAM readback");
  if (!size || !exchange (boot, size, error))
    return FALSE;
  if (memcmp (firmware, boot->rx + FTE3600_BOOT38_READBACK_OFFSET, length) != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "FT9338 RAM readback differs from the complete supplied firmware");
      return FALSE;
    }
  return TRUE;
}

static gboolean
start_application (Boot *boot, GError **error)
{
  gboolean is_38 = boot->sensor->sensor == FTE3600_SENSOR_FT9338;

  if (!reset_pulse (boot, error) || !proceed (boot, error) ||
      !delay (boot, FTE3600_RESET_GAP_MS, error) ||
      !reset_pulse (boot, error) || !proceed (boot, error) ||
      !delay (boot, is_38 ? FTE3600_BOOT38_FT9338_START_MS : FTE3600_RESET_BOOT_MS, error))
    return FALSE;
  if (is_38)
    return TRUE;
  report (boot, "A8 application software reset");
  return command (boot, FTE3600_COMMAND_SOFT_RESET, error) &&
         proceed (boot, error) && delay (boot, FTE3600_SOFT_RESET_INTERVAL_MS, error) &&
         command (boot, FTE3600_COMMAND_SOFT_RESET, error) &&
         proceed (boot, error) && delay (boot, FTE3600_A8_RESET_SETTLE_MS, error);
}

static gboolean
wait_idle (Boot *boot, guint attempts, GError **error)
{
  for (guint i = 0; i < attempts; i++)
    {
      if (!read_register (boot, FTE3600_REG_MCU_STATUS, 2, error))
        return FALSE;
      if (boot->rx[FTE3600_REG_RESULT_OFFSET] == FTE3600_MCU_IDLE_HIGH &&
          boot->rx[FTE3600_REG_RESULT_OFFSET + 1] == FTE3600_MCU_IDLE_LOW)
        return TRUE;
      if (i + 1 < attempts &&
          (!proceed (boot, error) || !delay (boot, FTE3600_INIT_MCU_POLL_MS, error)))
        return FALSE;
    }
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                       "Application did not confirm MCU idle a5 5a");
  return FALSE;
}

static gboolean
check_version (Boot *boot, guint8 reg, guint8 expected, GError **error)
{
  if (!read_register (boot, reg, 1, error))
    return FALSE;
  if (boot->rx[FTE3600_REG_RESULT_OFFSET] != expected)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Application register %02x returned %02x, expected %02x", reg,
                   boot->rx[FTE3600_REG_RESULT_OFFSET], expected);
      return FALSE;
    }
  return TRUE;
}

static gboolean
run_boot (Boot *boot, const guint8 *firmware, gsize length, GError **error)
{
  gboolean is_38 = boot->sensor->sensor == FTE3600_SENSOR_FT9338;

  report (boot, "explicit %s RAM experiment; checking initial application geometry", boot->sensor->name);
  if (!check_geometry (boot, TRUE, error) || !reset_pulse (boot, error))
    return FALSE;
  report (boot, "boot synchronization 55 aa");
  if (!command (boot, FTE3600_COMMAND_BOOT_SYNC, error) ||
      (is_38 && !prepare_38 (boot, error)) ||
      !upload (boot, firmware, length, error) ||
      (is_38 && !verify_38_readback (boot, firmware, length, error)) ||
      !start_application (boot, error) ||
      !wait_idle (boot, FTE3600_INIT_MCU_MAX_ATTEMPTS, error) ||
      !check_geometry (boot, FALSE, error) ||
      !check_version (boot, FTE3600_REG_FW_VERSION,
                      is_38 ? FTE3600_FT9338_FW_VERSION : FTE3600_A8_FW_VERSION, error) ||
      !check_version (boot, FTE3600_REG_AGC_VERSION,
                      is_38 ? FTE3600_FT9338_AGC_VERSION : FTE3600_A8_AGC_VERSION, error) ||
      !wait_idle (boot, 1, error))
    return FALSE;
  return proceed (boot, error);
}

gboolean
fte3600_medion_boot (const Fte3600MedionIdentifyIo *io, Fte3600Sensor sensor,
                     GBytes *firmware, Fte3600Identity *result, GError **error)
{
  Boot boot = { .io = io };
  const guint8 *payload;
  gsize expected_size, length;
  gboolean ok;

  g_autoptr(GError) failure = NULL;

  if (result)
    memset (result, 0, sizeof *result);
  if (!io || !firmware || !result || !io->exchange || !io->set_reset ||
      !io->wait || !io->check_cancelled ||
      (sensor != FTE3600_SENSOR_FT9338 && sensor != FTE3600_SENSOR_FT9348))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "RAM experiment requires explicit FT9338 or FT9348, firmware, and complete I/O callbacks");
      return FALSE;
    }
  boot.sensor = fpi_fte3600_sensor_get (sensor);
  expected_size = boot.sensor->firmware[0].size;
  payload = g_bytes_get_data (firmware, &length);
  if (length != expected_size)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "%s firmware size is %" G_GSIZE_FORMAT ", expected %" G_GSIZE_FORMAT,
                   boot.sensor->name, length, expected_size);
      return FALSE;
    }
  boot.capacity = length + (sensor == FTE3600_SENSOR_FT9338 ? FTE3600_BOOT38_READBACK_OVERHEAD :
                            FTE3600_FIRMWARE_HEADER_SIZE + FTE3600_FIRMWARE_TRAILER_SIZE);
  if (io->max_transfer < boot.capacity)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                   "%s needs an unsplit %" G_GSIZE_FORMAT "-byte transaction; transport permits %" G_GSIZE_FORMAT,
                   boot.sensor->name, boot.capacity, io->max_transfer);
      return FALSE;
    }
  boot.tx = g_try_malloc0 (boot.capacity);
  boot.rx = g_try_malloc0 (boot.capacity);
  if (!boot.tx || !boot.rx)
    {
      g_set_error_literal (&failure, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                           "Could not allocate bounded RAM startup buffers");
      ok = FALSE;
    }
  else
    {
      ok = run_boot (&boot, payload, length, &failure);
    }

  /* Do not reset into an incomplete or unverified RAM payload on failure.
   * Release is attempted even when cancellation or a previous GPIO write failed.
   * This is a pin-state cleanup, not a claim that ROM/application idle recovered. */
  if (boot.touched)
    {
      GError *release_error = NULL;

      if (!reset_value (&boot, FALSE, &release_error))
        {
          g_prefix_error (&release_error, "cleanup failed: ");
          merge_error (&failure, release_error);
          ok = FALSE;
        }
    }
  if (ok && !proceed (&boot, &failure))
    ok = FALSE;
  if (!ok && !failure)
    g_set_error_literal (&failure, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "RAM startup failed without a reported reason");
  if (failure)
    {
      if (boot.touched)
        report (&boot, "RAM startup failed; reset release attempted, ROM/application state remains unverified");
      g_propagate_error (error, g_steal_pointer (&failure));
      ok = FALSE;
    }
  else
    {
      *result = boot.verified;
      report (&boot, "%s application startup verified; runtime geometry and versions match", boot.sensor->name);
    }
  g_free (boot.tx);
  g_free (boot.rx);
  return ok;
}
