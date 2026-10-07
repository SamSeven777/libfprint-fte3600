/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Independently expressed FT9338/FT9536 boot identification and RAM recovery. */
#define FP_COMPONENT "fte3600"
#include "fte3600-legacy-recovery.h"
#include "fte3600-legacy-recovery-protocol.h"
#include "fte3600-legacy-recovery-timing.h"
#include <string.h>

typedef struct
{
  gboolean        boot_a;
  gboolean        touched;
  guint8          rx[4];
  guint8          otp;
  Fte3600Identity runtime;
  Fte3600Identity candidate;
} Identify;

typedef struct
{
  GBytes  *firmware;
  guint8  *readback;
  gsize    frame_size;
  guint    attempts;
  gboolean touched;
} Recovery;

enum {
  ID_VALIDATE, ID_RELEASE, ID_HIGH, ID_ASSERT, ID_LOW, ID_SYNC,
  ID_READ_CONFIG, ID_WRITE_CONFIG, ID_WRITE_ADDRESS, ID_READ_CONTROL,
  ID_WRITE_CONTROL, ID_READ_ID, ID_SAVE_ID, ID_DISABLE_OTP, ID_CLASSIFY,
  ID_CLEANUP_RELEASE, ID_CLEANUP_HIGH, ID_CLEANUP_ASSERT, ID_CLEANUP_LOW,
  ID_CLEANUP_DEASSERT, ID_CLEANUP_SETTLE, ID_COMMIT, ID_DONE, ID_NSTATES,
};

enum {
  REC_VALIDATE, REC_RELEASE, REC_HIGH, REC_ASSERT, REC_LOW,
  REC_SYNC, REC_C8, REC_CA, REC_CB, REC_B9_PREPARE, REC_B9_COMMIT,
  REC_CONFIG_WAIT, REC_UPLOAD, REC_UPLOAD_WAIT, REC_READBACK, REC_VERIFY,
  REC_START_RELEASE1, REC_START_HIGH1, REC_START_ASSERT1,
  REC_START_LOW1, REC_START_DEASSERT1, REC_START_GAP,
  REC_START_RELEASE2, REC_START_HIGH2, REC_START_ASSERT2,
  REC_START_LOW2, REC_START_DEASSERT2, REC_START_WAIT,
  REC_READ_MCU, REC_CHECK_MCU, REC_READ_WIDTH, REC_CHECK_WIDTH,
  REC_READ_HEIGHT, REC_CHECK_HEIGHT, REC_CLEANUP_RELEASE, REC_DONE, REC_NSTATES,
};

static void
boot38_fail (FpiSsm *ssm, const gchar *message)
{
  fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "%s", message));
}

static void
boot38_exchange (FpiSsm *ssm, const guint8 *tx, guint8 *rx,
                 gsize length, gboolean cancellable)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  FpiSpiTransfer *transfer;

  if (!length || length > self->max_transfer)
    {
      boot38_fail (ssm, "FT9338-family boot transaction exceeds the transport limit");
      return;
    }
  transfer = fpi_spi_transfer_new_with_buffer_size (FP_DEVICE (self), self->spi_fd,
                                                    self->max_transfer);
  fpi_spi_transfer_write (transfer, length);
  memcpy (transfer->buffer_wr, tx, length);
  if (rx)
    {
      memset (rx, 0, length);
      fpi_spi_transfer_read_full (transfer, rx, length, NULL);
    }
  else
    {
      fpi_spi_transfer_read (transfer, length);
    }
  fpi_spi_transfer_set_full_duplex (transfer, TRUE);
  fpi_spi_transfer_set_sensitive (transfer, TRUE);
  fpi_fte3600_submit_transfer (ssm, transfer, cancellable);
}

static void
boot38_read (FpiSsm *ssm, Identify *data, guint8 reg)
{
  guint8 frame[FTE3600_BOOT38_REGISTER_SIZE];

  fpi_fte3600_build_boot38_read (frame, sizeof frame, reg);
  boot38_exchange (ssm, frame, data->rx, sizeof frame, TRUE);
}

static void
boot38_write (FpiSsm *ssm, guint8 reg, guint8 value)
{
  guint8 frame[FTE3600_BOOT38_REGISTER_SIZE];

  fpi_fte3600_build_boot38_write (frame, sizeof frame, reg, value);
  boot38_exchange (ssm, frame, NULL, sizeof frame, TRUE);
}

static gboolean
boot38_runtime_valid (const FpiDeviceFte3600 *self, const Fte3600Identity *identity)
{
  if (self->family == 0x1534)
    return TRUE;
  return identity->evidence == FTE3600_IDENTITY_RUNTIME_GEOMETRY &&
         ((identity->sensor == FTE3600_SENSOR_FT9338 && identity->response == 0x5858) ||
          (identity->sensor == FTE3600_SENSOR_FT9536 && identity->response == 0x4080));
}

static void
identify_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  Identify *data = fpi_ssm_get_data (ssm);
  guint step = fpi_ssm_get_cur_state (ssm);
  guint8 value = data->rx[FTE3600_BOOT38_RESULT_OFFSET];

  if (step < ID_CLEANUP_RELEASE && !(step >= ID_HIGH && step <= ID_SYNC) &&
      fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;
  switch (step)
    {
    case ID_VALIDATE:
      if (data->boot_a ? self->discovery_rx[FTE3600_BOOT_PROBE_RESULT_OFFSET] != FTE3600_BOOT_A_MARKER :
          !boot38_runtime_valid (self, &data->runtime))
        boot38_fail (ssm, "FT9338-family boot identification lacks a positive current-session context");
      else
        fpi_ssm_next_state (ssm);
      return;

    case ID_RELEASE:
      data->touched = TRUE;
      self->armed = FALSE;
      fpi_fte3600_clear_irq_source (self);
      fpi_fte3600_set_hardware_reset (ssm, self, FALSE);
      return;

    case ID_HIGH:
    case ID_CLEANUP_HIGH:
      fpi_ssm_next_state_delayed (ssm, FTE3600_RESET_HIGH_MS);
      return;

    case ID_ASSERT:
    case ID_CLEANUP_ASSERT:
      fpi_fte3600_set_hardware_reset (ssm, self, TRUE);
      return;

    case ID_LOW:
    case ID_CLEANUP_LOW:
      fpi_ssm_next_state_delayed (ssm, FTE3600_RESET_LOW_MS);
      return;

    case ID_CLEANUP_DEASSERT:
      fpi_fte3600_set_hardware_reset (ssm, self, FALSE);
      return;

    case ID_SYNC:
      fpi_fte3600_release_reset_and_sync (ssm);
      return;

    case ID_READ_CONFIG:
      boot38_read (ssm, data, data->boot_a ? FTE3600_BOOT38_CONFIG_CB : FTE3600_BOOT38_CONFIG_C8);
      return;

    case ID_WRITE_CONFIG:
      boot38_write (ssm, data->boot_a ? FTE3600_BOOT38_CONFIG_CB : FTE3600_BOOT38_CONFIG_C8,
                    value | (data->boot_a ? FTE3600_BOOT38_IDENTIFY_ENABLE : FTE3600_BOOT38_OTP_ENABLE));
      return;

    case ID_WRITE_ADDRESS:
      boot38_write (ssm, data->boot_a ? FTE3600_BOOT38_CONFIG_FD : FTE3600_BOOT_REG_OTP_ADDRESS,
                    data->boot_a ? FTE3600_BOOT38_IDENTIFY_VALUE : FTE3600_BOOT38_OTP_ADDRESS);
      return;

    case ID_READ_CONTROL:
      if (data->boot_a)
        fpi_ssm_next_state (ssm);
      else
        boot38_read (ssm, data, FTE3600_BOOT_REG_OTP_CONTROL);
      return;

    case ID_WRITE_CONTROL:
      boot38_write (ssm, data->boot_a ? FTE3600_BOOT38_ID_FE : FTE3600_BOOT_REG_OTP_CONTROL,
                    data->boot_a ? FTE3600_BOOT38_IDENTIFY_VALUE : value | FTE3600_BOOT38_OTP_ENABLE);
      return;

    case ID_READ_ID:
      boot38_read (ssm, data, data->boot_a ? FTE3600_BOOT38_ID_FE : FTE3600_BOOT_REG_OTP_DATA);
      return;

    case ID_SAVE_ID:
      data->otp = value;
      fpi_ssm_next_state (ssm);
      return;

    case ID_DISABLE_OTP:
      if (data->boot_a)
        fpi_ssm_next_state (ssm);
      else
        boot38_write (ssm, FTE3600_BOOT_REG_OTP_CONTROL, 0);
      return;

    case ID_CLASSIFY:
      data->candidate = data->boot_a ? fpi_fte3600_identify_boot_a (data->otp) :
                        fpi_fte3600_identify_boot_b38_spi (data->otp);
      if (!data->boot_a)
        data->candidate.response = (self->family == 0x1534) ? 0x1534 : data->runtime.response;
      if (data->candidate.sensor == FTE3600_SENSOR_UNKNOWN ||
          (!data->boot_a && self->family != 0x1534 && data->candidate.sensor != data->runtime.sensor) ||
          (data->runtime.sensor != FTE3600_SENSOR_UNKNOWN && data->runtime.sensor != data->candidate.sensor) ||
          (self->probed_sensor != FTE3600_SENSOR_UNKNOWN && self->probed_sensor != data->candidate.sensor))
        fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                               FP_DEVICE_ERROR_NOT_SUPPORTED,
                               "FT9338-family ROM identity is unknown or conflicts with its positive runtime identity"));
      else
        fpi_ssm_next_state (ssm);
      return;

    case ID_CLEANUP_RELEASE:
      if (!data->touched)
        fpi_ssm_jump_to_state (ssm, ID_DONE);
      else
        fpi_fte3600_set_hardware_reset (ssm, self, FALSE);
      return;

    case ID_CLEANUP_SETTLE:
      fpi_ssm_next_state_delayed (ssm,
                                  data->candidate.sensor == FTE3600_SENSOR_FT9338 ||
                                  data->runtime.sensor == FTE3600_SENSOR_FT9338 ?
                                  FTE3600_BOOT38_FT9338_START_MS : FTE3600_BOOT38_FT9536_START_MS);
      return;

    case ID_COMMIT:
      if (!fpi_ssm_get_error (ssm))
        self->identity = self->rom_identity = data->candidate;
      fpi_ssm_next_state (ssm);
      return;

    case ID_DONE:
      fpi_ssm_mark_completed (ssm);
      return;

    default:
      g_assert_not_reached ();
    }
}

static gboolean
recovery_load (FpiDeviceFte3600 *self, Recovery *data, GError **error)
{
  const Fte3600Identity *identity = &self->rom_identity;
  const Fte3600Firmware *firmware;
  const gchar *custom_path = g_getenv ("FTE3600_FIRMWARE_PATH");
  g_autofree gchar *path = NULL;
  gboolean valid;

  valid = (identity->sensor == FTE3600_SENSOR_FT9536 &&
           identity->evidence == FTE3600_IDENTITY_ROM_BOOT_A && identity->response == 2) ||
          (identity->evidence == FTE3600_IDENTITY_ROM_BOOT_B38_SPI_OTP &&
           fpi_fte3600_identify_boot_b38_spi (identity->otp).sensor == identity->sensor &&
           ((identity->sensor == FTE3600_SENSOR_FT9338 &&
             (identity->response == 0x5858 || identity->response == 0x1534)) ||
            (identity->sensor == FTE3600_SENSOR_FT9536 &&
             (identity->response == 0x4080 || identity->response == 0x1534))));
  if (!valid || !self->sensor || self->sensor->sensor != identity->sensor ||
      self->identity.sensor != identity->sensor ||
      self->identity.evidence != identity->evidence ||
      self->identity.response != identity->response || self->identity.otp != identity->otp ||
      self->sensor->firmware_count != 1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           "FT9338-family RAM upload requires its verified current-session ROM identity");
      return FALSE;
    }
  firmware = self->sensor->firmware;
  if (firmware->role != FTE3600_FIRMWARE_APPLICATION ||
      firmware->size != (identity->sensor == FTE3600_SENSOR_FT9338 ? 14184 : 11934) ||
      firmware->size + FTE3600_BOOT38_READBACK_OVERHEAD > self->max_transfer)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                           "FT9338-family firmware requires a complete bounded upload and readback transaction");
      return FALSE;
    }
  path = g_build_filename ("/usr/lib/firmware", firmware->filename, NULL);
  data->firmware = fpi_fte3600_firmware_load (firmware,
                                              custom_path && *custom_path ? custom_path : path, error);
  if (!data->firmware)
    return FALSE;
  data->frame_size = firmware->size + FTE3600_BOOT38_READBACK_OVERHEAD;
  data->readback = g_malloc0 (data->frame_size);
  return TRUE;
}

static void
recovery_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  Recovery *data = fpi_ssm_get_data (ssm);
  guint step = fpi_ssm_get_cur_state (ssm);

  /* A reset pulse already in progress must finish before cancellation is
   * observed. Starting the application is ordinary work, never error cleanup. */
  if (step < REC_CLEANUP_RELEASE &&
      !(step >= REC_HIGH && step <= REC_SYNC) &&
      !(step >= REC_START_HIGH1 && step <= REC_START_DEASSERT1) &&
      !(step >= REC_START_HIGH2 && step <= REC_START_DEASSERT2) &&
      fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;
  switch (step)
    {
    case REC_VALIDATE:
      {
        g_autoptr(GError) error = NULL;
        if (!recovery_load (self, data, &error))
          fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
        else
          fpi_ssm_next_state (ssm);
        return;
      }

    case REC_RELEASE:
      data->touched = TRUE;
      self->armed = FALSE;
      fpi_fte3600_clear_irq_source (self);
      fpi_fte3600_set_hardware_reset (ssm, self, FALSE);
      return;

    case REC_HIGH:
    case REC_START_HIGH1:
    case REC_START_HIGH2:
      fpi_ssm_next_state_delayed (ssm, FTE3600_RESET_HIGH_MS);
      return;

    case REC_ASSERT:
    case REC_START_ASSERT1:
    case REC_START_ASSERT2:
      fpi_fte3600_set_hardware_reset (ssm, self, TRUE);
      return;

    case REC_LOW:
    case REC_START_LOW1:
    case REC_START_LOW2:
      fpi_ssm_next_state_delayed (ssm, FTE3600_RESET_LOW_MS);
      return;

    case REC_START_DEASSERT1:
    case REC_START_DEASSERT2:
      fpi_fte3600_set_hardware_reset (ssm, self, FALSE);
      return;

    case REC_SYNC:
      fpi_fte3600_release_reset_and_sync (ssm);
      return;

    case REC_C8:
      boot38_write (ssm, FTE3600_BOOT38_CONFIG_C8, FTE3600_BOOT38_CONFIG_ALL);
      return;

    case REC_CA:
      boot38_write (ssm, FTE3600_BOOT38_CONFIG_CA, FTE3600_BOOT38_CONFIG_ALL);
      return;

    case REC_CB:
      boot38_write (ssm, FTE3600_BOOT38_CONFIG_CB, FTE3600_BOOT38_CONFIG_ALL);
      return;

    case REC_B9_PREPARE:
      boot38_write (ssm, FTE3600_BOOT38_CONFIG_B9, FTE3600_BOOT38_CONFIG_PREPARE);
      return;

    case REC_B9_COMMIT:
      boot38_write (ssm, FTE3600_BOOT38_CONFIG_B9, FTE3600_BOOT38_CONFIG_ALL);
      return;

    case REC_CONFIG_WAIT:
      fpi_ssm_next_state_delayed (ssm, FTE3600_BOOT38_CONFIG_MS);
      return;

    case REC_UPLOAD:
    case REC_READBACK:
      {
        gsize size = 0;
        const guint8 *bytes = g_bytes_get_data (data->firmware, &size);
        g_autofree guint8 *frame = g_malloc0 (data->frame_size);
        g_autoptr(GError) error = NULL;
        gsize length = step == REC_UPLOAD ?
                       fpi_fte3600_build_firmware (frame, data->frame_size, bytes, size, &error) :
                       fpi_fte3600_build_boot38_readback (frame, data->frame_size, size);
        if (!length)
          {
            if (error)
              fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
            else
              boot38_fail (ssm, "Invalid FT9338-family RAM readback frame");
          }
        else
          {
            boot38_exchange (ssm, frame, step == REC_UPLOAD ? NULL : data->readback, length, TRUE);
          }
        return;
      }

    case REC_UPLOAD_WAIT:
      fpi_ssm_next_state_delayed (ssm, FTE3600_BOOT38_UPLOAD_MS);
      return;

    case REC_VERIFY:
      if (memcmp (data->readback + FTE3600_BOOT38_READBACK_OFFSET,
                  g_bytes_get_data (data->firmware, NULL), g_bytes_get_size (data->firmware)))
        boot38_fail (ssm, "FT9338-family RAM readback does not match the validated firmware");
      else
        fpi_ssm_next_state (ssm);
      return;

    case REC_START_RELEASE1:
    case REC_START_RELEASE2:
      fpi_fte3600_set_hardware_reset (ssm, self, FALSE);
      return;

    case REC_START_GAP:
      fpi_ssm_next_state_delayed (ssm, FTE3600_RESET_GAP_MS);
      return;

    case REC_START_WAIT:
      fpi_ssm_next_state_delayed (ssm, self->sensor->sensor == FTE3600_SENSOR_FT9338 ?
                                  FTE3600_BOOT38_FT9338_START_MS : FTE3600_BOOT38_FT9536_START_MS);
      return;

    case REC_READ_MCU:
      fpi_fte3600_submit_reg_read (ssm, FTE3600_REG_MCU_STATUS, 2, FALSE);
      return;

    case REC_CHECK_MCU:
      if (fpi_fte3600_mcu_is_idle (self))
        fpi_ssm_next_state (ssm);
      else if (++data->attempts < FTE3600_BOOT38_POLL_ATTEMPTS)
        fpi_ssm_jump_to_state_delayed (ssm, REC_READ_MCU, FTE3600_BOOT38_POLL_MS);
      else
        boot38_fail (ssm, "FT9338-family application did not reach idle after RAM recovery");
      return;

    case REC_READ_WIDTH:
      fpi_fte3600_submit_reg_read (ssm, FTE3600_REG_SENSOR_ID_HIGH, 1, FALSE);
      return;

    case REC_CHECK_WIDTH:
      if (!self->small_rx_valid || fpi_fte3600_read_result_byte (self) != self->sensor->width)
        boot38_fail (ssm, "FT9338-family application width does not match its ROM identity");
      else
        fpi_ssm_next_state (ssm);
      return;

    case REC_READ_HEIGHT:
      fpi_fte3600_submit_reg_read (ssm, FTE3600_REG_SENSOR_ID_LOW, 1, FALSE);
      return;

    case REC_CHECK_HEIGHT:
      if (!self->small_rx_valid || fpi_fte3600_read_result_byte (self) != self->sensor->height)
        {
          boot38_fail (ssm, "FT9338-family application height does not match its ROM identity");
        }
      else
        {
          self->idle_verified = TRUE;
          fpi_ssm_next_state (ssm);
        }
      return;

    case REC_CLEANUP_RELEASE:
      if (fpi_ssm_get_error (ssm))
        {
          /* Neither an incomplete upload nor a failed RAM comparison may
           * trigger application startup. Tell the parent to close this session
           * without issuing the runtime reset/status sequence either. */
          self->session_failed = TRUE;
          self->idle_verified = FALSE;
          if (data->touched)
            {
              fpi_fte3600_set_hardware_reset (ssm, self, FALSE);
              return;
            }
        }
      fpi_ssm_next_state (ssm);
      return;

    case REC_DONE:
      fpi_ssm_mark_completed (ssm);
      return;

    default:
      g_assert_not_reached ();
    }
}

static void
recovery_free (Recovery *data)
{
  g_clear_pointer (&data->firmware, g_bytes_unref);
  g_free (data->readback);
  g_free (data);
}

FpiSsm *
fpi_fte3600_legacy38_identify_new (FpiDeviceFte3600 *self, gboolean boot_a)
{
  FpiSsm *ssm = fpi_ssm_new_full (FP_DEVICE (self), identify_handler,
                                  ID_NSTATES, ID_CLEANUP_RELEASE, "FT9338-family boot identification");
  Identify *data = g_new0 (Identify, 1);

  data->boot_a = boot_a;
  data->runtime = self->identity;
  fpi_ssm_set_data (ssm, data, g_free);
  return ssm;
}

FpiSsm *
fpi_fte3600_legacy38_recovery_new (FpiDeviceFte3600 *self)
{
  FpiSsm *ssm = fpi_ssm_new_full (FP_DEVICE (self), recovery_handler,
                                  REC_NSTATES, REC_CLEANUP_RELEASE, "FT9338-family verified RAM recovery");

  fpi_ssm_set_data (ssm, g_new0 (Recovery, 1), (GDestroyNotify) recovery_free);
  return ssm;
}
