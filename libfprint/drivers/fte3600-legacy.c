/*
 * FocalTech FTE3600 sensor family driver
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "fte3600"

#include "fte3600-private.h"
#include "fte3600-timing.h"
#include "fte3600-protocol.h"
#include "fte3600-legacy-recovery.h"

typedef struct
{
  guint8   firmware_version;
  guint8   agc_version;
  guint8   mode_register;
  guint    config_delay_ms;
  gboolean cold_recovery;
  gboolean config_once;
  gboolean quick_mode;
} Fte3600LegacyConfig;

/* Values from independently documented protocol observations. Sharing an
 * operation family never substitutes one chip's firmware for another's. */
static const Fte3600LegacyConfig ft9338 = {
  .firmware_version = FTE3600_FT9338_FW_VERSION,
  .agc_version = FTE3600_FT9338_AGC_VERSION,
  .mode_register = FTE3600_REG_MODE,
  .config_delay_ms = FTE3600_38_CONFIG_DELAY_MS,
  .quick_mode = TRUE,
};
static const Fte3600LegacyConfig ft9536 = {
  .firmware_version = FTE3600_FT9536_FW_VERSION,
  .agc_version = FTE3600_FT9536_AGC_VERSION,
  .mode_register = FTE3600_REG_MODE_FT9536,
  .config_delay_ms = FTE3600_38_CONFIG_DELAY_MS,
};
static const Fte3600LegacyConfig ft95a8 = {
  .firmware_version = FTE3600_A8_FW_VERSION,
  .agc_version = FTE3600_A8_AGC_VERSION,
  .mode_register = FTE3600_REG_MODE,
  .config_delay_ms = FTE3600_A8_CONFIG_DELAY_MS,
  .cold_recovery = TRUE,
  .config_once = TRUE,
};

enum fte3600_init_state {
  FTE3600_INIT_DISPATCH,
  FTE3600_INIT_RETURN_IDLE,
  FTE3600_INIT_RETURN_IDLE_DONE,
  FTE3600_INIT_38_READ_MCU,
  FTE3600_INIT_38_CHECK_MCU,
  FTE3600_INIT_38_IDENTIFY,
  FTE3600_INIT_38_RECOVER,
  FTE3600_INIT_38_READY,
  FTE3600_INIT_RESET_1,
  FTE3600_INIT_RESET_DELAY,
  FTE3600_INIT_RESET_2,
  FTE3600_INIT_RESET_SETTLE,
  FTE3600_INIT_READ_MCU_STATUS,
  FTE3600_INIT_CHECK_MCU_STATUS,
  FTE3600_INIT_MCU_EXHAUSTED,
  FTE3600_INIT_IDENTIFY_BOOT,
  FTE3600_INIT_LOAD_FIRMWARE,
  FTE3600_INIT_FW_RESET_PREPARE,
  FTE3600_INIT_FW_RESET_HIGH,
  FTE3600_INIT_FW_RESET_ASSERT,
  FTE3600_INIT_FW_RESET_HOLD,
  FTE3600_INIT_FW_SYNC,
  FTE3600_INIT_FW_UPLOAD,
  FTE3600_INIT_FW_UPLOAD_SETTLE,
  FTE3600_INIT_HARD_RESET_PREPARE_1,
  FTE3600_INIT_HARD_RESET_HIGH_1,
  FTE3600_INIT_HARD_RESET_ASSERT_1,
  FTE3600_INIT_HARD_RESET_HOLD_1,
  FTE3600_INIT_HARD_RESET_DEASSERT_1,
  FTE3600_INIT_HARD_RESET_INTERVAL,
  FTE3600_INIT_HARD_RESET_PREPARE_2,
  FTE3600_INIT_HARD_RESET_HIGH_2,
  FTE3600_INIT_HARD_RESET_ASSERT_2,
  FTE3600_INIT_HARD_RESET_HOLD_2,
  FTE3600_INIT_HARD_RESET_DEASSERT_2,
  FTE3600_INIT_HARD_RESET_BOOT,
  FTE3600_INIT_READ_ID_HIGH,
  FTE3600_INIT_CHECK_ID_HIGH,
  FTE3600_INIT_READ_ID_LOW,
  FTE3600_INIT_CHECK_ID_LOW,
  FTE3600_INIT_READ_FW_VERSION,
  FTE3600_INIT_CHECK_FW_VERSION,
  FTE3600_INIT_READ_AGC_VERSION,
  FTE3600_INIT_CHECK_AGC_VERSION,
  FTE3600_INIT_READ_CONFIG_MARKER,
  FTE3600_INIT_CHECK_CONFIG_MARKER,
  FTE3600_INIT_WRITE_CONFIG_01,
  FTE3600_INIT_CONFIG_01_DELAY,
  FTE3600_INIT_WRITE_CONFIG_41,
  FTE3600_INIT_CONFIG_41_DELAY,
  FTE3600_INIT_WRITE_CONFIG_MARKER,
  FTE3600_INIT_CONFIG_MARKER_DELAY,
  FTE3600_INIT_VERIFY_CONFIG_MARKER,
  FTE3600_INIT_CHECK_CONFIG_VERIFY,
  FTE3600_INIT_WRITE_CONFIG_22,
  FTE3600_INIT_CONFIG_22_DELAY,
  FTE3600_INIT_WRITE_CONFIG_23,
  FTE3600_INIT_CONFIG_23_DELAY,
  FTE3600_INIT_FINAL_READ_MCU_STATUS,
  FTE3600_INIT_FINAL_CHECK_MCU_STATUS,
  FTE3600_INIT_DONE,
  FTE3600_INIT_NSTATES,
};

enum fte3600_arm_state {
  FTE3600_ARM_READ_MCU_STATUS,
  FTE3600_ARM_CHECK_MCU_STATUS,
  FTE3600_ARM_RECOVERY_RESET,
  FTE3600_ARM_WRITE_MODE,
  FTE3600_ARM_WRITE_ENABLE,
  FTE3600_ARM_WRITE_START,
  FTE3600_ARM_WRITE_QUICK_TRIGGER,
  FTE3600_ARM_DELAY,
  FTE3600_ARM_DRAIN_FINGER_STATUS,
  FTE3600_ARM_READ_ARMED_MCU_STATUS,
  FTE3600_ARM_CHECK_ARMED_MCU_STATUS,
  FTE3600_ARM_WRITE_IDLE_MODE,
  FTE3600_ARM_DONE,
  FTE3600_ARM_NSTATES,
};

enum fte3600_capture_state {
  FTE3600_CAPTURE_PREPARE_ARM,
  FTE3600_CAPTURE_WAIT_FINGER_IRQ,
  FTE3600_CAPTURE_POLL_MCU_STATUS,
  FTE3600_CAPTURE_CHECK_MCU_STATUS,
  FTE3600_CAPTURE_READ_FINGER_STATUS,
  FTE3600_CAPTURE_CHECK_FINGER_STATUS,
  FTE3600_CAPTURE_REARM,
  FTE3600_CAPTURE_REARM_DONE,
  FTE3600_CAPTURE_READ_IMAGE,
  FTE3600_CAPTURE_PROCESS_IMAGE,
  FTE3600_CAPTURE_CLEANUP_DISPATCH,
  FTE3600_CAPTURE_CLEANUP_REARM,
  FTE3600_CAPTURE_CLEANUP_REARM_DONE,
  FTE3600_CAPTURE_CLEANUP_RESET,
  FTE3600_CAPTURE_CLEANUP_RESET_DONE,
  FTE3600_CAPTURE_DONE,
  FTE3600_CAPTURE_NSTATES,
};

enum fte3600_reset_state {
  FTE3600_RESET_1,
  FTE3600_RESET_DELAY,
  FTE3600_RESET_2,
  FTE3600_RESET_READ_MODE,
  FTE3600_RESET_CHECK_MODE,
  FTE3600_RESET_STOP_START,
  FTE3600_RESET_STOP_ENABLE,
  FTE3600_RESET_STOP_DELAY,
  FTE3600_RESET_READ_MCU_STATUS,
  FTE3600_RESET_CHECK_MCU_STATUS,
  FTE3600_RESET_NSTATES,
};

static FpiSsm *fte3600_return_idle_new (FpiDeviceFte3600 *self, gboolean verify_idle);

typedef struct
{
  guint attempts;
  guint entry;
} LegacyInit;

typedef struct
{
  /* An idle post-arm reply may be a completed acquisition. Leave its IRQ
   * queued until the next capture, including during enrollment processing. */
  gboolean arm_completed;
} LegacyState;

typedef struct
{
  GSource *timeout;
} LegacyCapture;

static LegacyState *
fte3600_legacy_state (FpiDeviceFte3600 *self)
{
  if (!self->backend_data)
    self->backend_data = g_new0 (LegacyState, 1);
  return self->backend_data;
}

static void
fte3600_legacy_destroy (FpiDeviceFte3600 *self)
{
  g_clear_pointer (&self->backend_data, g_free);
}

static void
fte3600_capture_free (LegacyCapture *capture)
{
  g_clear_pointer (&capture->timeout, g_source_destroy);
  g_free (capture);
}

static void
fte3600_capture_irq_timeout (FpDevice *dev, gpointer user_data)
{
  FpiSsm *ssm = user_data;
  LegacyCapture *capture = fpi_ssm_get_data (ssm);

  g_clear_pointer (&capture->timeout, g_source_destroy);
  fpi_fte3600_clear_irq_source (FPI_DEVICE_FTE3600 (dev));
  fpi_ssm_mark_failed (
    ssm, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                             "FTE3600 legacy stayed idle without a new capture IRQ"));
}

static void
fte3600_init_recover (FpiSsm *ssm)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));

  self->init_firmware_upload_attempted = TRUE;
  self->init_hardware_reset_attempted = TRUE;
  fpi_ssm_jump_to_state (ssm, self->sensor->protocol == FTE3600_PROTOCOL_FT9338 ?
                         FTE3600_INIT_38_IDENTIFY : FTE3600_INIT_IDENTIFY_BOOT);
}

static void
fte3600_submit_command (FpiSsm *ssm, Fte3600Command command, gboolean cancellable)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));

  g_autoptr(GError) error = NULL;
  guint8 packet[FTE3600_COMMAND_MAX_SIZE];
  gsize length;
  FpiSpiTransfer *transfer;

  length = fpi_fte3600_build_command (packet, sizeof packet, command, &error);
  if (!length)
    {
      fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      return;
    }

  transfer = fpi_spi_transfer_new_with_buffer_size (FP_DEVICE (self), self->spi_fd,
                                                    self->max_transfer);
  fpi_spi_transfer_write (transfer, length);
  memcpy (transfer->buffer_wr, packet, length);
  fpi_fte3600_submit_transfer (ssm, transfer, cancellable);
}

static void
fte3600_submit_firmware_packet (FpiSsm       *ssm,
                                const guint8 *fw_data,
                                gsize         fw_len,
                                gboolean      cancellable)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));

  g_autoptr(FpiSpiTransfer) transfer = NULL;
  g_autoptr(GError) error = NULL;
  gsize pkt_len;

  if (fw_len > G_MAXUINT16)
    {
      fpi_ssm_mark_failed (ssm, g_error_new_literal (
                             G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                             "FTE3600 firmware exceeds the protocol length limit"));
      return;
    }

  pkt_len = FTE3600_FIRMWARE_HEADER_SIZE + fw_len + FTE3600_FIRMWARE_TRAILER_SIZE;
  if (pkt_len > self->max_transfer)
    {
      fpi_ssm_mark_failed (ssm, g_error_new_literal (
                             G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                             "SPI controller cannot transmit the complete FTE3600 firmware packet"));
      return;
    }

  transfer = fpi_spi_transfer_new_with_buffer_size (FP_DEVICE (self), self->spi_fd,
                                                    self->max_transfer);
  fpi_spi_transfer_write (transfer, pkt_len);
  if (!fpi_fte3600_build_firmware (transfer->buffer_wr, pkt_len,
                                   fw_data, fw_len, &error))
    {
      fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      return;
    }
  /* Keep chip select asserted for the complete packet. The full-duplex
   * helper rejects insufficient spidev buffers instead of splitting it. */
  fpi_spi_transfer_read (transfer, pkt_len);
  fpi_spi_transfer_set_full_duplex (transfer, TRUE);
  fpi_spi_transfer_set_sensitive (transfer, TRUE);
  fpi_fte3600_submit_transfer (ssm, g_steal_pointer (&transfer), cancellable);
}

static void
fte3600_submit_capture (FpiSsm *ssm)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  FpiSpiTransfer *transfer;

  memset (self->capture_rx, 0, self->capture_frame_size);
  transfer = fpi_spi_transfer_new_with_buffer_size (FP_DEVICE (self), self->spi_fd,
                                                    self->max_transfer);
  fpi_spi_transfer_write_full (transfer, self->capture_tx,
                               self->capture_frame_size, NULL);
  fpi_spi_transfer_read_full (transfer, self->capture_rx,
                              self->capture_frame_size, NULL);
  fpi_spi_transfer_set_full_duplex (transfer, TRUE);
  fpi_spi_transfer_set_sensitive (transfer, TRUE);
  fpi_fte3600_submit_transfer (ssm, transfer, TRUE);
}

static void
fte3600_init_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  const Fte3600LegacyConfig *config = self->backend->configuration;
  guint state = fpi_ssm_get_cur_state (ssm);
  guint8 value;

  /* Once reset or firmware upload has been asserted, always finish the complete
   * pulse train and return the active-low line high before observing cancellation. */
  if (!((state >= FTE3600_INIT_FW_RESET_PREPARE && state <= FTE3600_INIT_FW_UPLOAD_SETTLE) ||
        (state >= FTE3600_INIT_HARD_RESET_PREPARE_1 && state <= FTE3600_INIT_HARD_RESET_BOOT)) &&
      fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;

  switch (state)
    {
    case FTE3600_INIT_DISPATCH:
      {
        FpiSsm *parent = fpi_ssm_get_data (ssm);
        LegacyInit *init = fpi_ssm_get_data (parent);

        fpi_ssm_jump_to_state (ssm, init->entry);
      }
      return;

    case FTE3600_INIT_RETURN_IDLE:
      /* LoadFW returns a running application to idle before checking its
       * version. The A8 post-download reset tail is a separate path. */
      fpi_ssm_start_subsm (ssm, fte3600_return_idle_new (self, FALSE));
      return;

    case FTE3600_INIT_RETURN_IDLE_DONE:
      fpi_ssm_jump_to_state (ssm, config->cold_recovery ?
                             FTE3600_INIT_READ_MCU_STATUS : FTE3600_INIT_38_READ_MCU);
      return;

    case FTE3600_INIT_38_READ_MCU:
      fpi_fte3600_try_reg_read (ssm, FTE3600_REG_MCU_STATUS, 2, TRUE);
      return;

    case FTE3600_INIT_38_CHECK_MCU:
      if (fpi_fte3600_mcu_is_idle (self))
        {
          if (self->fast_open)
            fpi_ssm_jump_to_state_delayed (ssm, FTE3600_INIT_READ_ID_HIGH,
                                           FTE3600_LEGACY_WAKE_GEOMETRY_MS);
          else
            fpi_ssm_jump_to_state (ssm, FTE3600_INIT_READ_ID_HIGH);
        }
      else
        fpi_ssm_next_state (ssm);
      return;

    case FTE3600_INIT_38_IDENTIFY:
      if (self->rom_identity.evidence == FTE3600_IDENTITY_ROM_BOOT_A ||
          self->rom_identity.evidence == FTE3600_IDENTITY_ROM_BOOT_B38_SPI_OTP)
        fpi_ssm_next_state (ssm);
      else
        fpi_fte3600_start_boot_discovery (ssm);
      return;

    case FTE3600_INIT_38_RECOVER:
      self->init_firmware_upload_attempted = TRUE;
      fpi_ssm_start_subsm (ssm, fpi_fte3600_legacy38_recovery_new (self));
      return;

    case FTE3600_INIT_38_READY:
      /* DownloadSensorFirmware returns directly to InitMcuConfig. Geometry
       * and old version reads must not gate the cold configuration writes. */
      fpi_ssm_jump_to_state (ssm, FTE3600_INIT_WRITE_CONFIG_01);
      return;

    case FTE3600_INIT_RESET_1:
    case FTE3600_INIT_RESET_2:
      fte3600_submit_command (ssm, FTE3600_COMMAND_SOFT_RESET, TRUE);
      return;

    case FTE3600_INIT_RESET_DELAY:
      fpi_ssm_next_state_delayed (ssm, FTE3600_SOFT_RESET_INTERVAL_MS);
      return;

    case FTE3600_INIT_RESET_SETTLE:
      fpi_ssm_next_state_delayed (ssm, FTE3600_A8_RESET_SETTLE_MS);
      return;

    case FTE3600_INIT_READ_MCU_STATUS:
      fpi_fte3600_try_reg_read (ssm, FTE3600_REG_MCU_STATUS, 2, TRUE);
      return;

    case FTE3600_INIT_CHECK_MCU_STATUS:
      if (!fpi_fte3600_mcu_is_idle (self))
        {
          if (!config->cold_recovery)
            {
              fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                     FP_DEVICE_ERROR_NOT_SUPPORTED,
                                     "%s requires its matching firmware already running; cold recovery is not implemented",
                                     self->sensor->name));
              return;
            }
          /* Windows permits twenty status reads after hardware startup.
           * Keep the initial soft-reset path fast, but do not upload again
           * or fail merely because the first post-reset response is early. */
          if (self->init_hardware_reset_attempted &&
              ++self->init_mcu_status_attempts < FTE3600_INIT_MCU_MAX_ATTEMPTS)
            {
              fpi_ssm_jump_to_state_delayed (
                ssm, FTE3600_INIT_READ_MCU_STATUS, FTE3600_INIT_MCU_POLL_MS);
              return;
            }

          if (!self->init_hardware_reset_attempted)
            {
              self->init_hardware_reset_attempted = TRUE;
              self->armed = FALSE;
              fpi_fte3600_clear_irq_source (self);
              fpi_ssm_jump_to_state (ssm, FTE3600_INIT_HARD_RESET_PREPARE_1);
              return;
            }

          /* The vendor loop also waits after its twentieth failed read. */
          fpi_ssm_jump_to_state_delayed (ssm, FTE3600_INIT_MCU_EXHAUSTED,
                                         FTE3600_INIT_MCU_POLL_MS);
          return;
        }
      if (self->init_firmware_upload_attempted)
        fpi_ssm_jump_to_state (ssm, FTE3600_INIT_READ_CONFIG_MARKER);
      else if (self->fast_open)
        fpi_ssm_jump_to_state_delayed (ssm, FTE3600_INIT_READ_ID_HIGH,
                                       FTE3600_LEGACY_WAKE_GEOMETRY_MS);
      else
        fpi_ssm_jump_to_state (ssm, FTE3600_INIT_READ_ID_HIGH);
      return;

    case FTE3600_INIT_MCU_EXHAUSTED:
      if (!self->init_firmware_upload_attempted)
        {
          self->init_firmware_upload_attempted = TRUE;
          fpi_ssm_jump_to_state (ssm, FTE3600_INIT_IDENTIFY_BOOT);
        }
      else
        fpi_ssm_mark_failed (
          ssm, fpi_device_error_new_msg (
            FP_DEVICE_ERROR_PROTO,
            "FTE3600 legacy MCU did not return to idle after cold-boot firmware recovery (%02x %02x)",
            self->small_rx[4], self->small_rx[5]));
      return;

    case FTE3600_INIT_IDENTIFY_BOOT:
      if (fpi_fte3600_identity_allows_firmware (&self->rom_identity))
        fpi_ssm_next_state (ssm);
      else
        fpi_fte3600_start_boot_discovery (ssm);
      return;

    case FTE3600_INIT_LOAD_FIRMWARE:
      {
        const Fte3600Firmware *firmware = self->sensor->firmware;

        g_assert (config->cold_recovery);
        g_assert (fpi_fte3600_identity_allows_firmware (&self->rom_identity));
        g_assert (self->rom_identity.sensor == self->sensor->sensor);
        g_assert (self->sensor->firmware_count == 1);
        if (self->max_transfer < firmware->size + 7)
          {
            fpi_ssm_mark_failed (ssm, g_error_new_literal (
                                   G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                                   "SPI controller cannot transmit the complete FTE3600 firmware packet"));
            return;
          }
        const gchar *custom_path = g_getenv ("FTE3600_FIRMWARE_PATH");
        g_autofree gchar *default_path = g_build_filename ("/usr/lib/firmware", firmware->filename, NULL);
        const gchar *path = custom_path && *custom_path ? custom_path : default_path;
        GError *fw_error = NULL;

        self->armed = FALSE;
        fpi_fte3600_clear_irq_source (self);

        g_clear_pointer (&self->firmware_bytes, g_bytes_unref);
        self->firmware_bytes = fpi_fte3600_firmware_load (firmware, path, &fw_error);
        if (!self->firmware_bytes)
          {
            fp_warn ("FTE3600 legacy MCU not running (%02x %02x) and firmware loading failed: %s",
                     self->small_rx[4], self->small_rx[5], fw_error->message);
            fpi_ssm_mark_failed (ssm, fw_error);
            return;
          }

        fp_info ("FTE3600 legacy loading matching firmware (MCU %02x %02x; %zu bytes)",
                 self->small_rx[4], self->small_rx[5],
                 g_bytes_get_size (self->firmware_bytes));
        fpi_ssm_jump_to_state (ssm, FTE3600_INIT_FW_RESET_PREPARE);
        return;
      }

    case FTE3600_INIT_FW_RESET_PREPARE:
      fpi_fte3600_set_hardware_reset (ssm, self, FALSE);
      return;

    case FTE3600_INIT_FW_RESET_HIGH:
      fpi_ssm_next_state_delayed (ssm, FTE3600_RESET_HIGH_MS);
      return;

    case FTE3600_INIT_FW_RESET_ASSERT:
      fpi_fte3600_set_hardware_reset (ssm, self, TRUE);
      return;

    case FTE3600_INIT_FW_RESET_HOLD:
      fpi_ssm_next_state_delayed (ssm, FTE3600_RESET_LOW_MS);
      return;

    case FTE3600_INIT_FW_SYNC:
      /* Entry uses one H10/L20/H pulse followed immediately by 55 AA.
       * The 160 ms application startup delay belongs after upload. */
      fpi_fte3600_release_reset_and_sync (ssm);
      return;

    case FTE3600_INIT_FW_UPLOAD:
      {
        gsize fw_size = 0;
        const guint8 *fw_data = g_bytes_get_data (self->firmware_bytes, &fw_size);

        fte3600_submit_firmware_packet (ssm, fw_data, fw_size, FALSE);
        return;
      }

    case FTE3600_INIT_FW_UPLOAD_SETTLE:
      g_clear_pointer (&self->firmware_bytes, g_bytes_unref);
      fpi_ssm_jump_to_state_delayed (
        ssm, FTE3600_INIT_HARD_RESET_PREPARE_1, FTE3600_FW_UPLOAD_SETTLE_MS);
      return;

    case FTE3600_INIT_HARD_RESET_PREPARE_1:
      self->init_mcu_status_attempts = 0;
      G_GNUC_FALLTHROUGH;

    case FTE3600_INIT_HARD_RESET_PREPARE_2:
      fpi_fte3600_set_hardware_reset (ssm, self, FALSE);
      return;

    case FTE3600_INIT_HARD_RESET_HIGH_1:
    case FTE3600_INIT_HARD_RESET_HIGH_2:
      fpi_ssm_next_state_delayed (ssm, FTE3600_RESET_HIGH_MS);
      return;

    case FTE3600_INIT_HARD_RESET_ASSERT_1:
    case FTE3600_INIT_HARD_RESET_ASSERT_2:
      fpi_fte3600_set_hardware_reset (ssm, self, TRUE);
      return;

    case FTE3600_INIT_HARD_RESET_HOLD_1:
    case FTE3600_INIT_HARD_RESET_HOLD_2:
      fpi_ssm_next_state_delayed (ssm, FTE3600_RESET_LOW_MS);
      return;

    case FTE3600_INIT_HARD_RESET_DEASSERT_1:
    case FTE3600_INIT_HARD_RESET_DEASSERT_2:
      fpi_fte3600_set_hardware_reset (ssm, self, FALSE);
      return;

    case FTE3600_INIT_HARD_RESET_INTERVAL:
      /* The next pulse includes its own 10 ms high preamble: high is held
       * for a total of 20 ms between the two low pulses. */
      fpi_ssm_next_state_delayed (ssm, FTE3600_RESET_GAP_MS);
      return;

    case FTE3600_INIT_HARD_RESET_BOOT:
      fpi_ssm_jump_to_state_delayed (
        ssm, FTE3600_INIT_RESET_1, FTE3600_RESET_BOOT_MS);
      return;

    case FTE3600_INIT_READ_ID_HIGH:
      if (self->init_firmware_upload_attempted)
        {
          fpi_ssm_jump_to_state (ssm, FTE3600_INIT_READ_CONFIG_MARKER);
          return;
        }
      fpi_fte3600_submit_reg_read (ssm, FTE3600_REG_SENSOR_ID_HIGH, 1, TRUE);
      return;

    case FTE3600_INIT_CHECK_ID_HIGH:
      value = fpi_fte3600_read_result_byte (self);
      if (value != self->sensor->width)
        {
          fpi_ssm_mark_failed (
            ssm, fpi_device_error_new_msg (
              FP_DEVICE_ERROR_NOT_SUPPORTED,
              "Unexpected FTE3600 sensor ID high byte %02x", value));
          return;
        }
      fpi_ssm_next_state (ssm);
      return;

    case FTE3600_INIT_READ_ID_LOW:
      fpi_fte3600_submit_reg_read (ssm, FTE3600_REG_SENSOR_ID_LOW, 1, TRUE);
      return;

    case FTE3600_INIT_CHECK_ID_LOW:
      value = fpi_fte3600_read_result_byte (self);
      if (value != self->sensor->height)
        {
          fpi_ssm_mark_failed (
            ssm, fpi_device_error_new_msg (
              FP_DEVICE_ERROR_NOT_SUPPORTED,
              "Unexpected FTE3600 sensor ID low byte %02x", value));
          return;
        }
      fpi_ssm_next_state (ssm);
      return;

    case FTE3600_INIT_READ_FW_VERSION:
      fpi_fte3600_try_reg_read (ssm, FTE3600_REG_FW_VERSION, 1, TRUE);
      return;

    case FTE3600_INIT_CHECK_FW_VERSION:
      value = fpi_fte3600_read_result_byte (self);
      if (!self->small_rx_valid || value != config->firmware_version)
        {
          fte3600_init_recover (ssm);
          return;
        }
      fpi_ssm_next_state (ssm);
      return;

    case FTE3600_INIT_READ_AGC_VERSION:
      fpi_fte3600_try_reg_read (ssm, FTE3600_REG_AGC_VERSION, 1, TRUE);
      return;

    case FTE3600_INIT_CHECK_AGC_VERSION:
      value = fpi_fte3600_read_result_byte (self);
      if (!self->small_rx_valid || value != config->agc_version)
        {
          fte3600_init_recover (ssm);
          return;
        }
      fpi_ssm_next_state (ssm);
      return;

    case FTE3600_INIT_READ_CONFIG_MARKER:
      if (!config->config_once)
        {
          fpi_ssm_jump_to_state (ssm, FTE3600_INIT_WRITE_CONFIG_01);
          return;
        }
      fpi_fte3600_submit_reg_read (ssm, FTE3600_REG_CONFIG_MARKER, 1, TRUE);
      return;

    case FTE3600_INIT_CHECK_CONFIG_MARKER:
      if (config->config_once && fpi_fte3600_read_result_byte (self) == FTE3600_CONFIGURED_MARKER)
        {
          fpi_ssm_jump_to_state (
            ssm, FTE3600_INIT_FINAL_READ_MCU_STATUS);
          return;
        }
      fpi_ssm_next_state (ssm);
      return;

    case FTE3600_INIT_WRITE_CONFIG_01:
      self->idle_verified = FALSE;
      fpi_fte3600_submit_reg_write (ssm, FTE3600_REG_CONFIG_01, FTE3600_CONFIG_01_ENABLE, TRUE);
      return;

    case FTE3600_INIT_CONFIG_01_DELAY:
    case FTE3600_INIT_CONFIG_41_DELAY:
    case FTE3600_INIT_CONFIG_MARKER_DELAY:
    case FTE3600_INIT_CONFIG_22_DELAY:
    case FTE3600_INIT_CONFIG_23_DELAY:
      fpi_ssm_next_state_delayed (ssm, config->config_delay_ms);
      return;

    case FTE3600_INIT_WRITE_CONFIG_41:
      fpi_fte3600_submit_reg_write (ssm, FTE3600_REG_CONFIG_41, FTE3600_CONFIG_41_VALUE, TRUE);
      return;

    case FTE3600_INIT_WRITE_CONFIG_MARKER:
      fpi_fte3600_submit_reg_write (ssm, FTE3600_REG_CONFIG_MARKER, FTE3600_CONFIGURED_MARKER, TRUE);
      return;

    case FTE3600_INIT_VERIFY_CONFIG_MARKER:
      fpi_fte3600_submit_reg_read (ssm, FTE3600_REG_CONFIG_MARKER, 1, TRUE);
      return;

    case FTE3600_INIT_CHECK_CONFIG_VERIFY:
      value = fpi_fte3600_read_result_byte (self);
      if (value != FTE3600_CONFIGURED_MARKER)
        {
          /* Both vendor helpers log this value and continue. Transfer errors
           * still propagate; the A8 22/23 writes must not be skipped here. */
          fp_warn ("%s configuration marker is %02x, expected bb; continuing as Windows does",
                   self->sensor->name, value);
        }
      if (config->config_once)
        fpi_ssm_next_state (ssm);
      else
        fpi_ssm_jump_to_state (ssm, FTE3600_INIT_FINAL_READ_MCU_STATUS);
      return;

    case FTE3600_INIT_WRITE_CONFIG_22:
      fpi_fte3600_submit_reg_write (ssm, FTE3600_REG_CONFIG_22, FTE3600_CONFIG_22_VALUE, TRUE);
      return;

    case FTE3600_INIT_WRITE_CONFIG_23:
      fpi_fte3600_submit_reg_write (ssm, FTE3600_REG_CONFIG_23, FTE3600_CONFIG_23_VALUE, TRUE);
      return;

    case FTE3600_INIT_FINAL_READ_MCU_STATUS:
      fpi_fte3600_try_reg_read (ssm, FTE3600_REG_MCU_STATUS, 2, TRUE);
      return;

    case FTE3600_INIT_FINAL_CHECK_MCU_STATUS:
      /* Keep an accurate Linux lifecycle flag, not an additional vendor
       * configuration gate. Arming handles a busy MCU through return-idle. */
      self->idle_verified = fpi_fte3600_mcu_is_idle (self);
      fpi_ssm_next_state (ssm);
      return;

    case FTE3600_INIT_DONE:
      fpi_ssm_mark_completed (ssm);
      return;

    case FTE3600_INIT_NSTATES:
      g_assert_not_reached ();
    }
}

static void
fte3600_arm_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  const Fte3600LegacyConfig *config = self->backend->configuration;
  guint mode = GPOINTER_TO_UINT (fpi_ssm_get_data (ssm));

  if (fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FTE3600_ARM_READ_MCU_STATUS:
      fpi_fte3600_submit_reg_read (ssm, FTE3600_REG_MCU_STATUS, 2, TRUE);
      return;

    case FTE3600_ARM_CHECK_MCU_STATUS:
      if (fpi_fte3600_mcu_is_idle (self))
        {
          fpi_ssm_jump_to_state (ssm, FTE3600_ARM_WRITE_MODE);
        }
      else
        {
          fp_dbg ("FTE3600 legacy unexpectedly busy before capture rearm; recovering");
          fpi_ssm_next_state (ssm);
        }
      return;

    case FTE3600_ARM_RECOVERY_RESET:
      /* Return-idle performs the mode-dependent stop and confirms MCU idle.
       * A failed recovery propagates instead of writing arm into a busy MCU. */
      fpi_ssm_start_subsm (ssm, self->backend->create_reset (self));
      return;

    case FTE3600_ARM_WRITE_MODE:
      {
        g_autoptr(GError) error = NULL;

        fte3600_legacy_state (self)->arm_completed = FALSE;
        if (!fpi_fte3600_drain_irq_events (self, &error))
          {
            fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
            return;
          }
      }
      fpi_fte3600_submit_reg_write (ssm, config->mode_register, mode, TRUE);
      return;

    case FTE3600_ARM_WRITE_ENABLE:
      if (mode == FTE3600_MODE_QUICK_CAPTURE)
        {
          fpi_ssm_jump_to_state (ssm, FTE3600_ARM_WRITE_QUICK_TRIGGER);
          return;
        }
      fpi_fte3600_submit_reg_write (ssm, FTE3600_REG_ENABLE, FTE3600_CAPTURE_ENABLE, TRUE);
      return;

    case FTE3600_ARM_WRITE_START:
      fpi_fte3600_submit_reg_write (ssm, FTE3600_REG_START, FTE3600_CAPTURE_ENABLE, TRUE);
      return;

    case FTE3600_ARM_WRITE_QUICK_TRIGGER:
      if (mode == FTE3600_MODE_QUICK_CAPTURE)
        fpi_fte3600_submit_reg_write (ssm, FTE3600_REG_QUICK_TRIGGER, FTE3600_QUICK_TRIGGER, TRUE);
      else
        fpi_ssm_next_state (ssm);
      return;

    case FTE3600_ARM_DELAY:
      if (mode == FTE3600_MODE_QUICK_CAPTURE)
        {
          fpi_ssm_jump_to_state (ssm, FTE3600_ARM_READ_ARMED_MCU_STATUS);
          return;
        }
      fpi_ssm_next_state_delayed (ssm, FTE3600_ARM_DELAY_MS);
      return;

    case FTE3600_ARM_DRAIN_FINGER_STATUS:
      fpi_fte3600_submit_reg_read (ssm, FTE3600_REG_FINGER_STATUS, 1, TRUE);
      return;

    case FTE3600_ARM_READ_ARMED_MCU_STATUS:
      fpi_fte3600_submit_reg_read (ssm, FTE3600_REG_MCU_STATUS, 2, TRUE);
      return;

    case FTE3600_ARM_CHECK_ARMED_MCU_STATUS:
      if (fpi_fte3600_mcu_is_idle (self))
        {
          /* SwitchNextSensorWorkMode writes mode zero on this outcome.
           * Preserve the queued IRQ; do not retrigger or await another frame
           * before handing the preceding enrollment image to the matcher. */
          fpi_ssm_next_state (ssm);
        }
      else
        {
          fpi_ssm_jump_to_state (ssm, FTE3600_ARM_DONE);
        }
      return;

    case FTE3600_ARM_WRITE_IDLE_MODE:
      fte3600_legacy_state (self)->arm_completed = TRUE;
      fpi_fte3600_submit_reg_write (ssm, config->mode_register, FTE3600_MODE_IDLE, TRUE);
      return;

    case FTE3600_ARM_DONE:
      fpi_ssm_mark_completed (ssm);
      return;

    case FTE3600_ARM_NSTATES:
      g_assert_not_reached ();
    }
}

static FpiSsm *
fte3600_new_arm_ssm (FpiDeviceFte3600 *self, guint mode)
{
  FpiSsm *ssm = fpi_ssm_new (FP_DEVICE (self), fte3600_arm_handler, FTE3600_ARM_NSTATES);

  fpi_ssm_set_data (ssm, GUINT_TO_POINTER (mode), NULL);
  return ssm;
}

static void
fte3600_capture_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  const Fte3600LegacyConfig *config = self->backend->configuration;
  LegacyCapture *capture = fpi_ssm_get_data (ssm);
  gint state = fpi_ssm_get_cur_state (ssm);
  guint8 finger_status;

  if (state < FTE3600_CAPTURE_CLEANUP_DISPATCH &&
      fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;

  switch (state)
    {
    case FTE3600_CAPTURE_PREPARE_ARM:
      if (self->armed)
        fpi_ssm_next_state (ssm);
      else
        fpi_ssm_start_subsm (ssm, fte3600_new_arm_ssm (self, FTE3600_MODE_WAIT_FINGER));
      return;

    case FTE3600_CAPTURE_WAIT_FINGER_IRQ:
      if (fte3600_legacy_state (self)->arm_completed)
        {
          /* Bound a missing completion only when its image is requested.
           * Time spent processing the previous image cannot expire this wait. */
          capture->timeout = fpi_device_add_timeout (
            dev, FTE3600_IDLE_IRQ_TIMEOUT_MS, fte3600_capture_irq_timeout, ssm, NULL);
        }
      self->armed = TRUE;
      fpi_fte3600_wait_for_irq (ssm);
      return;

    case FTE3600_CAPTURE_POLL_MCU_STATUS:
      g_clear_pointer (&capture->timeout, g_source_destroy);
      fte3600_legacy_state (self)->arm_completed = FALSE;
      fpi_fte3600_submit_reg_read (ssm, FTE3600_REG_MCU_STATUS, 2, TRUE);
      return;

    case FTE3600_CAPTURE_CHECK_MCU_STATUS:
      if (fpi_fte3600_mcu_is_idle (self))
        {
          fpi_ssm_next_state (ssm);
        }
      else if (g_get_monotonic_time () >= self->capture_ready_deadline)
        {
          fpi_ssm_mark_failed (
            ssm, g_error_new_literal (
              G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
              "FTE3600 legacy did not become ready after its finger IRQ"));
        }
      else
        {
          fpi_ssm_jump_to_state_delayed (ssm, FTE3600_CAPTURE_POLL_MCU_STATUS,
                                         FTE3600_POLL_DELAY_MS);
        }
      return;

    case FTE3600_CAPTURE_READ_FINGER_STATUS:
      fpi_fte3600_submit_reg_read (ssm, FTE3600_REG_FINGER_STATUS, 1, TRUE);
      return;

    case FTE3600_CAPTURE_CHECK_FINGER_STATUS:
      finger_status = fpi_fte3600_read_result_byte (self);
      if (finger_status == FTE3600_FINGER_PRESENT || finger_status == FTE3600_FINGER_PRESENT_ALT)
        {
          self->false_irq_count = 0;
          fpi_device_report_finger_status (
            dev, FP_FINGER_STATUS_NEEDED | FP_FINGER_STATUS_PRESENT);
          fpi_ssm_jump_to_state (ssm, FTE3600_CAPTURE_READ_IMAGE);
        }
      else
        {
          self->false_irq_count++;
          if (self->false_irq_count >= FTE3600_MAX_FALSE_IRQS)
            {
              fpi_ssm_mark_failed (
                ssm, g_error_new_literal (
                  G_IO_ERROR, G_IO_ERROR_FAILED,
                  "FTE3600 legacy produced too many consecutive non-finger "
                  "interrupts"));
              return;
            }
          fp_dbg ("Ignoring non-finger status %02x", finger_status);
          fpi_ssm_next_state (ssm);
        }
      return;

    case FTE3600_CAPTURE_REARM:
      self->armed = FALSE;
      fpi_ssm_start_subsm (ssm, fte3600_new_arm_ssm (
                            self, config->quick_mode ? FTE3600_MODE_QUICK_CAPTURE : FTE3600_MODE_WAIT_FINGER));
      return;

    case FTE3600_CAPTURE_REARM_DONE:
      fpi_ssm_jump_to_state (ssm, FTE3600_CAPTURE_WAIT_FINGER_IRQ);
      return;

    case FTE3600_CAPTURE_READ_IMAGE:
      fte3600_submit_capture (ssm);
      return;

    case FTE3600_CAPTURE_PROCESS_IMAGE:
      fpi_fte3600_clear_captured_image (self);
      self->captured_image =
        fp_image_new (self->sensor->width, self->sensor->height);
      self->captured_image->flags |= FPI_IMAGE_PARTIAL;

      {
        const guint8 *src = &self->capture_rx[FTE3600_IMAGE_DATA_OFFSET];
        guint8 *dst = self->captured_image->data;
        gsize i = 0;

        for (; i + sizeof (guint64) <= self->image_size; i += sizeof (guint64))
          {
            guint64 word;
            memcpy (&word, src + i, sizeof (word));
            word = ~word;
            memcpy (dst + i, &word, sizeof (word));
          }
        for (; i < self->image_size; i++)
          dst[i] = (guint8) ~src[i];
      }

      fp_dbg ("Captured FTE3600 image (turnaround %02x %02x)",
              self->capture_rx[6], self->capture_rx[7]);
      fpi_fte3600_secure_clear (self->capture_rx, self->capture_frame_size);
      /* Register 0x1d is a latched event result, not a live contact signal.
       * Cleanup rearms the chip-specific mode only when another enrollment
       * stage is expected. A terminal capture returns to idle before its
       * action completes; stale GPIO events are drained before the next arm. */
      fpi_ssm_next_state (ssm);
      return;

    case FTE3600_CAPTURE_CLEANUP_DISPATCH:
      g_clear_pointer (&capture->timeout, g_source_destroy);
      if (fpi_ssm_get_error (ssm))
        {
          fpi_fte3600_secure_clear (self->capture_rx,
                                    self->capture_frame_size);
          self->armed = FALSE;
          fpi_ssm_jump_to_state (ssm, FTE3600_CAPTURE_CLEANUP_RESET);
        }
      else if (fpi_device_get_current_action (dev) != FPI_DEVICE_ACTION_ENROLL ||
               self->enroll_stages_passed + 1 >=
               (guint) fp_device_get_nr_enroll_stages (dev))
        {
          fp_dbg ("Resetting FTE3600 before terminal action completion");
          self->armed = FALSE;
          fpi_ssm_jump_to_state (ssm, FTE3600_CAPTURE_CLEANUP_RESET);
        }
      else
        {
          fp_dbg ("Immediately rearming FTE3600 for the next enrollment stage");
          fpi_ssm_next_state (ssm);
        }
      return;

    case FTE3600_CAPTURE_CLEANUP_REARM:
      self->armed = FALSE;
      fpi_ssm_start_subsm (ssm, fte3600_new_arm_ssm (
                            self, config->quick_mode ? FTE3600_MODE_QUICK_CAPTURE : FTE3600_MODE_WAIT_FINGER));
      return;

    case FTE3600_CAPTURE_CLEANUP_REARM_DONE:
      if (fpi_ssm_get_error (ssm))
        {
          self->armed = FALSE;
          fpi_ssm_jump_to_state (ssm, FTE3600_CAPTURE_CLEANUP_RESET);
        }
      else
        {
          self->armed = TRUE;
          fpi_ssm_jump_to_state (ssm, FTE3600_CAPTURE_DONE);
        }
      return;

    case FTE3600_CAPTURE_CLEANUP_RESET:
      self->armed = FALSE;
      fpi_fte3600_clear_irq_source (self);
      fpi_ssm_start_subsm (ssm, self->backend->create_reset (self));
      return;

    case FTE3600_CAPTURE_CLEANUP_RESET_DONE:
      fpi_ssm_next_state (ssm);
      return;

    case FTE3600_CAPTURE_DONE:
      fpi_ssm_mark_completed (ssm);
      return;

    case FTE3600_CAPTURE_NSTATES:
      g_assert_not_reached ();
    }
}

static void
fte3600_reset_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  const Fte3600LegacyConfig *config = self->backend->configuration;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FTE3600_RESET_1:
      if (self->backend_data)
        ((LegacyState *) self->backend_data)->arm_completed = FALSE;
      self->idle_verified = FALSE;
      G_GNUC_FALLTHROUGH;

    case FTE3600_RESET_2:
      fte3600_submit_command (ssm, FTE3600_COMMAND_SOFT_RESET, FALSE);
      return;

    case FTE3600_RESET_DELAY:
      fpi_ssm_next_state_delayed (ssm, FTE3600_SOFT_RESET_INTERVAL_MS);
      return;

    case FTE3600_RESET_READ_MODE:
      fpi_fte3600_submit_reg_read (ssm, config->mode_register, 1, FALSE);
      return;

    case FTE3600_RESET_CHECK_MODE:
      {
        guint8 mode = fpi_fte3600_read_result_byte (self);
        if (self->small_rx_valid &&
            (mode == FTE3600_MODE_QUICK_CAPTURE || mode == FTE3600_MODE_3 || mode == FTE3600_MODE_4))
          fpi_ssm_jump_to_state (ssm, FTE3600_RESET_READ_MCU_STATUS);
        else
          fpi_ssm_next_state (ssm);
        return;
      }

    case FTE3600_RESET_STOP_START:
      fpi_fte3600_submit_reg_write (ssm, FTE3600_REG_START, FTE3600_CAPTURE_DISABLE, FALSE);
      return;

    case FTE3600_RESET_STOP_ENABLE:
      fpi_fte3600_submit_reg_write (ssm, FTE3600_REG_ENABLE, FTE3600_CAPTURE_DISABLE, FALSE);
      return;

    case FTE3600_RESET_STOP_DELAY:
      fpi_ssm_next_state_delayed (ssm, FTE3600_ARM_DELAY_MS);
      return;

    case FTE3600_RESET_READ_MCU_STATUS:
      if (!GPOINTER_TO_INT (fpi_ssm_get_data (ssm)))
        {
          fpi_ssm_mark_completed (ssm);
          return;
        }
      fpi_fte3600_submit_reg_read (ssm, FTE3600_REG_MCU_STATUS, 2, FALSE);
      return;

    case FTE3600_RESET_CHECK_MCU_STATUS:
      if (!GPOINTER_TO_INT (fpi_ssm_get_data (ssm)))
        {
          fpi_ssm_mark_completed (ssm);
          return;
        }
      if (!fpi_fte3600_mcu_is_idle (self))
        {
          fpi_ssm_mark_failed (
            ssm, fpi_device_error_new_msg (
              FP_DEVICE_ERROR_PROTO,
              "FTE3600 legacy MCU did not return to idle after reset "
              "(%02x %02x)",
              self->small_rx[4], self->small_rx[5]));
          return;
        }
      self->idle_verified = TRUE;
      fpi_ssm_mark_completed (ssm);
      return;

    case FTE3600_RESET_NSTATES:
      g_assert_not_reached ();
    }
}

static void
fte3600_init_attempt_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  FpiSsm *parent = fpi_ssm_get_data (ssm);
  LegacyInit *init = fpi_ssm_get_data (parent);
  guint state = fpi_ssm_get_cur_state (ssm);
  gboolean downloading = state == FTE3600_INIT_38_RECOVER ||
                         (state >= FTE3600_INIT_LOAD_FIRMWARE && state <= FTE3600_INIT_HARD_RESET_BOOT) ||
                         (state >= FTE3600_INIT_RESET_1 && state <= FTE3600_INIT_MCU_EXHAUSTED);

  if (!error)
    {
      fpi_ssm_mark_completed (parent);
      return;
    }

  /* DistributeSensorFirmware retries the complete download, never resumes a
   * partial payload or restarts the application after a failed RAM check. */
  if (downloading && self->init_firmware_upload_attempted &&
      fpi_fte3600_identity_allows_firmware (&self->rom_identity) &&
      self->rom_identity.sensor == self->sensor->sensor &&
      init->attempts < FTE3600_FIRMWARE_MAX_ATTEMPTS &&
      (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED) ||
       g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
       g_error_matches (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO)))
    {
      fp_dbg ("Retrying %s firmware download after attempt %u: %s",
              self->sensor->name, init->attempts, error->message);
      g_clear_error (&error);
      if (fpi_fte3600_fail_if_cancelled (parent, dev))
        return;
      if (!fpi_fte3600_transport_check (dev, &error))
        {
          fpi_ssm_mark_failed (parent, error);
          return;
        }
      self->session_failed = FALSE;
      init->entry = self->sensor->protocol == FTE3600_PROTOCOL_FT9338 ?
                    FTE3600_INIT_38_RECOVER : FTE3600_INIT_LOAD_FIRMWARE;
      fpi_ssm_jump_to_state (parent, 0);
      return;
    }
  if (downloading && self->init_firmware_upload_attempted)
    self->session_failed = TRUE;
  fpi_ssm_mark_failed (parent, error);
}

static void
fte3600_init_attempt (FpiSsm *ssm, FpDevice *dev)
{
  LegacyInit *init = fpi_ssm_get_data (ssm);
  FpiSsm *child = fpi_ssm_new (dev, fte3600_init_handler, FTE3600_INIT_NSTATES);

  init->attempts++;
  fpi_ssm_set_data (child, ssm, NULL);
  fpi_ssm_start (child, fte3600_init_attempt_complete);
}

static FpiSsm *
fte3600_legacy_create_init (FpiDeviceFte3600 *self)
{
  FpiSsm *ssm = fpi_ssm_new (FP_DEVICE (self), fte3600_init_attempt, 1);
  LegacyInit *init = g_new0 (LegacyInit, 1);

  init->entry = FTE3600_INIT_RETURN_IDLE;
  fpi_ssm_set_data (ssm, init, g_free);
  return ssm;
}

static FpiSsm *
fte3600_legacy_create_capture (FpiDeviceFte3600 *self)
{
  FpiSsm *ssm = fpi_ssm_new_full (FP_DEVICE (self), fte3600_capture_handler,
                                  FTE3600_CAPTURE_NSTATES,
                                  FTE3600_CAPTURE_CLEANUP_DISPATCH, "FTE3600 legacy capture");

  fpi_ssm_set_data (ssm, g_new0 (LegacyCapture, 1), (GDestroyNotify) fte3600_capture_free);
  return ssm;
}

static FpiSsm *
fte3600_return_idle_new (FpiDeviceFte3600 *self, gboolean verify_idle)
{
  FpiSsm *ssm;

  /* Continue bounded cleanup after the first error, retaining that error. */
  ssm = fpi_ssm_new_full (FP_DEVICE (self), fte3600_reset_handler,
                          FTE3600_RESET_NSTATES, FTE3600_RESET_DELAY,
                          "FTE3600 legacy return idle");
  fpi_ssm_set_data (ssm, GINT_TO_POINTER (verify_idle), NULL);
  return ssm;
}

static FpiSsm *
fte3600_legacy_create_reset (FpiDeviceFte3600 *self)
{
  return fte3600_return_idle_new (self, TRUE);
}

static gboolean
fte3600_legacy_prepare_capture (FpiDeviceFte3600 *self, GError **error)
{
  return fpi_fte3600_build_image_read (self->capture_tx, self->capture_frame_size,
                                       self->image_size, error) != 0;
}

#define LEGACY_BACKEND(config) \
  { \
    .configuration = &(config), \
    .bytes_per_pixel = 1, \
    .frame_overhead = FTE3600_IMAGE_DATA_OFFSET, \
    .prepare_capture = fte3600_legacy_prepare_capture, \
    .destroy = fte3600_legacy_destroy, \
    .create_init = fte3600_legacy_create_init, \
    .create_capture = fte3600_legacy_create_capture, \
    .create_reset = fte3600_legacy_create_reset, \
  }

static const Fte3600Backend backend_9338 = LEGACY_BACKEND (ft9338);
static const Fte3600Backend backend_9536 = LEGACY_BACKEND (ft9536);
static const Fte3600Backend backend_a8 = LEGACY_BACKEND (ft95a8);

const Fte3600Backend *
fpi_fte3600_legacy_backend (Fte3600Sensor sensor)
{
  switch (sensor)
    {
    case FTE3600_SENSOR_FT9338: return &backend_9338;

    case FTE3600_SENSOR_FT9536: return &backend_9536;

    case FTE3600_SENSOR_FT9348:
    case FTE3600_SENSOR_FT9361: return &backend_a8;

    case FTE3600_SENSOR_UNKNOWN:
    case FTE3600_SENSOR_FT9365:
    case FTE3600_SENSOR_FT9368:
    case FTE3600_SENSOR_FT9369:
    case FTE3600_SENSOR_FT9769:
    case FTE3600_SENSOR_COUNT:
    default: return NULL;
    }
}
