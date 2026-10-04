/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Bounded FT9368 PRAM verification and explicitly requested flash update. */
#define FP_COMPONENT "fte3600"
#include "fte3600-ft9368-update.h"
#include "fte3600-ft9368-protocol.h"
#include "fte3600-ft9368-timing.h"
#include <string.h>

typedef struct
{
  GBytes  *app;
  GBytes  *pram;
  guint8   rx[FTE3600_FT9368_MAX_PACKET];
  gsize    offset;
  gsize    chunk;
  guint16  checksum;
  guint16  expected_status;
  guint    poll_attempts;
  gboolean touched;
  gboolean erase_started;
  gboolean cleanup;
} Update;

typedef struct
{
  Update *update;
  gsize   length;
  guint8  frame[FTE3600_FT9368_MAX_PACKET];
} Spi0Command;

enum {
  UPDATE_VALIDATE,
  UPDATE_RESET_RELEASE, UPDATE_RESET_HIGH, UPDATE_RESET_ASSERT,
  UPDATE_RESET_LOW, UPDATE_RESET_DEASSERT, UPDATE_RESET_SETTLE,
  UPDATE_MODE, UPDATE_UNLOCK, UPDATE_PRAM_CONFIG,
  UPDATE_PRAM_WRITE, UPDATE_PRAM_WRITE_NEXT,
  UPDATE_PRAM_SELECT, UPDATE_PRAM_SELECT_WAIT, UPDATE_PRAM_READ, UPDATE_PRAM_VERIFY,
  UPDATE_PRAM_START, UPDATE_PRAM_EXECUTE, UPDATE_PRAM_WAIT,
  UPDATE_HANDSHAKE, UPDATE_HANDSHAKE_WAIT, UPDATE_BOOT_ID, UPDATE_CHECK_BOOT_ID,
  UPDATE_FLASH_CONFIG1, UPDATE_FLASH_CONFIG2, UPDATE_ERASE,
  UPDATE_ERASE_POLL, UPDATE_ERASE_CHECK,
  UPDATE_ADDRESS, UPDATE_DATA, UPDATE_PROGRAM_POLL, UPDATE_PROGRAM_CHECK,
  UPDATE_CHECKSUM_START, UPDATE_CHECKSUM_RANGE, UPDATE_CHECKSUM_WAIT,
  UPDATE_CHECKSUM_READ, UPDATE_CHECKSUM_CHECK, UPDATE_REBOOT,
  UPDATE_CLEANUP_RELEASE, UPDATE_CLEANUP_HIGH, UPDATE_CLEANUP_ASSERT,
  UPDATE_CLEANUP_LOW, UPDATE_CLEANUP_DEASSERT, UPDATE_CLEANUP_WAIT,
  UPDATE_VERIFY_WAKE, UPDATE_VERIFY_WAIT, UPDATE_VERIFY_INFO, UPDATE_VERIFY_CHECK,
  UPDATE_DONE, UPDATE_NSTATES,
};

static void
update_free (Update *data)
{
  g_clear_pointer (&data->app, g_bytes_unref);
  g_clear_pointer (&data->pram, g_bytes_unref);
  fpi_fte3600_secure_clear (data, sizeof *data);
  g_free (data);
}

static void
update_error (FpiSsm *ssm, const gchar *message)
{
  fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "%s", message));
}

static void
update_exchange (FpiSsm *ssm, Update *data, const guint8 *frame, gsize length)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  FpiSpiTransfer *transfer;

  if (!length || length > sizeof data->rx || length > self->max_transfer)
    {
      update_error (ssm, "FT9368 update packet exceeds the verified transport limit");
      return;
    }
  memset (data->rx, 0, sizeof data->rx);
  transfer = fpi_spi_transfer_new_with_buffer_size (FP_DEVICE (self), self->spi_fd,
                                                    self->max_transfer);
  fpi_spi_transfer_write (transfer, length);
  memcpy (transfer->buffer_wr, frame, length);
  fpi_spi_transfer_read_full (transfer, data->rx, length, NULL);
  fpi_spi_transfer_set_full_duplex (transfer, TRUE);
  fpi_spi_transfer_set_sensitive (transfer, TRUE);
  /* Once flash erasure starts, interruption must not leave an otherwise
   * successful programming sequence incomplete. The parent reports a pending
   * cancellation after this bounded update and verification has finished. */
  fpi_fte3600_submit_transfer (ssm, transfer, !data->erase_started && !data->cleanup);
}

static void
spi0_handler (FpiSsm *ssm, FpDevice *dev)
{
  Spi0Command *command = fpi_ssm_get_data (ssm);
  guint8 wake[4];

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case 0:
      fpi_fte3600_ft9368_read (wake, sizeof wake, FTE3600_FT9368_WAKE, 0);
      update_exchange (ssm, command->update, wake, sizeof wake);
      return;

    case 1:
      /* Windows requests a short inter-command delay; rounding upward avoids
       * relying on a busy wait or microsecond host scheduling guarantees. */
      fpi_ssm_next_state_delayed (ssm, FTE3600_FT9368_SHORT_DELAY_MS);
      return;

    case 2:
      update_exchange (ssm, command->update, command->frame, command->length);
      return;

    default:
      g_assert_not_reached ();
    }
}

static void
update_spi0 (FpiSsm *ssm, Update *data, const guint8 *frame, gsize length)
{
  Spi0Command *command;
  FpiSsm *child;

  if (!length || length > FTE3600_FT9368_MAX_PACKET)
    {
      update_error (ssm, "Invalid FT9368 update command");
      return;
    }
  command = g_new0 (Spi0Command, 1);
  command->update = data;
  command->length = length;
  memcpy (command->frame, frame, length);
  child = fpi_ssm_new (fpi_ssm_get_device (ssm), spi0_handler, 3);
  fpi_ssm_set_data (child, command, g_free);
  fpi_ssm_start_subsm (ssm, child);
}

static void
update_read (FpiSsm *ssm, Update *data, guint16 command, gsize length)
{
  guint8 frame[FTE3600_FT9368_MAX_PACKET];
  gsize size = fpi_fte3600_ft9368_read (frame, sizeof frame, command, length);

  update_spi0 (ssm, data, frame, size);
}

static void
update_write (FpiSsm *ssm, Update *data, guint8 command,
              const guint8 *payload, gsize length)
{
  guint8 frame[FTE3600_FT9368_MAX_PACKET];
  gsize size = fpi_fte3600_ft9368_write (frame, sizeof frame, command, payload, length);

  update_spi0 (ssm, data, frame, size);
}

static gboolean
update_load (FpiDeviceFte3600 *self, Update *data, GError **error)
{
  const gchar *directory = g_getenv ("FTE3600_FIRMWARE_DIR");

  if (g_strcmp0 (g_getenv ("FTE3600_FT9368_UPDATE"), "1") != 0 ||
      self->identity.sensor != FTE3600_SENSOR_FT9368 ||
      self->identity.evidence != FTE3600_IDENTITY_SPECIAL_CHIP_ID ||
      self->identity.response != 0x9368 || self->sensor->firmware_count != 2)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           "FT9368 flash update needs explicit opt-in and a positive chip identity");
      return FALSE;
    }
  if (!directory || !*directory)
    directory = "/usr/lib/firmware";
  for (guint i = 0; i < self->sensor->firmware_count; i++)
    {
      const Fte3600Firmware *firmware = &self->sensor->firmware[i];
      g_autofree gchar *path = g_build_filename (directory, firmware->filename, NULL);
      GBytes **target;

      if (firmware->role == FTE3600_FIRMWARE_APPLICATION)
        {
          target = &data->app;
        }
      else if (firmware->role == FTE3600_FIRMWARE_PRAMBOOT)
        {
          target = &data->pram;
        }
      else
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                               "FT9368 firmware catalog has an unknown role");
          return FALSE;
        }

      if (*target)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                               "FT9368 firmware catalog has duplicate roles");
          return FALSE;
        }
      *target = fpi_fte3600_firmware_load (firmware, path, error);
      if (!*target)
        return FALSE;
    }
  if (!data->app || !data->pram ||
      g_bytes_get_size (data->app) != FTE3600_FT9368_APP_SIZE ||
      g_bytes_get_size (data->pram) != FTE3600_FT9368_PRAM_SIZE ||
      !fpi_fte3600_ft9368_checksum (g_bytes_get_data (data->app, NULL),
                                    g_bytes_get_size (data->app), &data->checksum))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "FT9368 firmware pair is incompatible with this updater");
      return FALSE;
    }
  return TRUE;
}

static guint16
update_status (Update *data)
{
  return ((guint16) data->rx[FTE3600_FT9368_HEADER] << 8) |
         data->rx[FTE3600_FT9368_HEADER + 1];
}

static void
update_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  Update *data = fpi_ssm_get_data (ssm);
  guint step = fpi_ssm_get_cur_state (ssm);
  guint8 frame[FTE3600_FT9368_MAX_PACKET];
  guint8 payload[6] = { 0 };
  gsize length;

  if (step < UPDATE_CLEANUP_RELEASE && !data->erase_started &&
      !(step >= UPDATE_RESET_HIGH && step <= UPDATE_RESET_DEASSERT) &&
      fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;
  switch (step)
    {
    case UPDATE_VALIDATE:
      {
        g_autoptr(GError) error = NULL;
        if (!update_load (self, data, &error))
          fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
        else
          fpi_ssm_next_state (ssm);
        return;
      }

    case UPDATE_RESET_RELEASE:
      data->touched = TRUE;
      fpi_fte3600_set_hardware_reset (ssm, self, FALSE);
      return;

    case UPDATE_RESET_HIGH:
    case UPDATE_CLEANUP_HIGH:
      fpi_ssm_next_state_delayed (ssm, FTE3600_RESET_HIGH_MS);
      return;

    case UPDATE_RESET_ASSERT:
    case UPDATE_CLEANUP_ASSERT:
      fpi_fte3600_set_hardware_reset (ssm, self, TRUE);
      return;

    case UPDATE_RESET_LOW:
    case UPDATE_CLEANUP_LOW:
      fpi_ssm_next_state_delayed (ssm, FTE3600_RESET_LOW_MS);
      return;

    case UPDATE_RESET_DEASSERT:
    case UPDATE_CLEANUP_DEASSERT:
      fpi_fte3600_set_hardware_reset (ssm, self, FALSE);
      return;

    case UPDATE_RESET_SETTLE:
      /* ROM entry follows reset promptly. The application startup delay is
       * only valid after programming, not before the PRAM mode command. */
      fpi_ssm_next_state_delayed (ssm, FTE3600_FT9368_SHORT_DELAY_MS);
      return;

    case UPDATE_MODE:
      length = fpi_fte3600_ft9368_mode (frame, sizeof frame, FTE3600_FT9368_MODE_LOAD);
      update_exchange (ssm, data, frame, length);
      return;

    case UPDATE_UNLOCK:
      length = fpi_fte3600_ft9368_sfr (frame, sizeof frame, FTE3600_FT9368_SFR_UNLOCK, FTE3600_FT9368_SFR_UNLOCK_VALUE);
      update_exchange (ssm, data, frame, length);
      return;

    case UPDATE_PRAM_CONFIG:
      length = fpi_fte3600_ft9368_sfr (frame, sizeof frame, FTE3600_FT9368_SFR_CONFIG, FTE3600_FT9368_SFR_CONFIG_VALUE);
      data->offset = 0;
      update_exchange (ssm, data, frame, length);
      return;

    case UPDATE_PRAM_WRITE:
      data->chunk = MIN (FTE3600_FT9368_PRAM_CHUNK, FTE3600_FT9368_PRAM_SIZE - data->offset);
      length = fpi_fte3600_ft9368_pram_write (
        frame, sizeof frame, data->offset,
        (const guint8 *) g_bytes_get_data (data->pram, NULL) + data->offset, data->chunk);
      update_exchange (ssm, data, frame, length);
      return;

    case UPDATE_PRAM_WRITE_NEXT:
      data->offset += data->chunk;
      if (data->offset < FTE3600_FT9368_PRAM_SIZE)
        {
          fpi_ssm_jump_to_state (ssm, UPDATE_PRAM_WRITE);
        }
      else
        {
          data->offset = 0;
          fpi_ssm_next_state (ssm);
        }
      return;

    case UPDATE_PRAM_SELECT:
      data->chunk = MIN (256, FTE3600_FT9368_PRAM_SIZE - data->offset);
      length = fpi_fte3600_ft9368_pram_select (frame, sizeof frame, data->offset, data->chunk);
      update_exchange (ssm, data, frame, length);
      return;

    case UPDATE_PRAM_SELECT_WAIT:
      fpi_ssm_next_state_delayed (ssm, FTE3600_FT9368_SHORT_DELAY_MS);
      return;

    case UPDATE_PRAM_READ:
      memset (frame, 0, data->chunk + 1);
      frame[0] = FTE3600_FT9368_PRAM_READ;
      update_exchange (ssm, data, frame, data->chunk + 1);
      return;

    case UPDATE_PRAM_VERIFY:
      if (memcmp (data->rx + 1,
                  (const guint8 *) g_bytes_get_data (data->pram, NULL) + data->offset,
                  data->chunk))
        {
          update_error (ssm, "FT9368 PRAM readback differs from the validated image");
        }
      else
        {
          data->offset += data->chunk;
          if (data->offset < FTE3600_FT9368_PRAM_SIZE)
            fpi_ssm_jump_to_state (ssm, UPDATE_PRAM_SELECT);
          else
            fpi_ssm_next_state (ssm);
        }
      return;

    case UPDATE_PRAM_START:
      length = fpi_fte3600_ft9368_sfr (frame, sizeof frame, FTE3600_FT9368_SFR_START, FTE3600_FT9368_SFR_START_VALUE);
      update_exchange (ssm, data, frame, length);
      return;

    case UPDATE_PRAM_EXECUTE:
      length = fpi_fte3600_ft9368_mode (frame, sizeof frame, FTE3600_FT9368_MODE_EXECUTE);
      update_exchange (ssm, data, frame, length);
      return;

    case UPDATE_PRAM_WAIT:
    case UPDATE_HANDSHAKE_WAIT:
      fpi_ssm_next_state_delayed (ssm, FTE3600_FT9368_PRAM_START_MS);
      return;

    case UPDATE_HANDSHAKE:
      update_read (ssm, data, FTE3600_FT9368_FLASH_HANDSHAKE, 0);
      return;

    case UPDATE_BOOT_ID:
      update_read (ssm, data, FTE3600_FT9368_IMAGE, 2);
      return;

    case UPDATE_CHECK_BOOT_ID:
      if (update_status (data) != FTE3600_FT9368_BOOT_ID)
        update_error (ssm, "FT9368 PRAM boot identity was not confirmed");
      else
        fpi_ssm_next_state (ssm);
      return;

    case UPDATE_FLASH_CONFIG1:
      payload[0] = 0x0a;
      update_write (ssm, data, FTE3600_FT9368_FLASH_CONFIG1, payload, 1);
      return;

    case UPDATE_FLASH_CONFIG2:
      payload[0] = 0x0c;
      update_write (ssm, data, FTE3600_FT9368_FLASH_CONFIG2, payload, 1);
      return;

    case UPDATE_ERASE:
      data->erase_started = TRUE;
      data->poll_attempts = 0;
      update_write (ssm, data, FTE3600_FT9368_FLASH_ERASE, payload, 1);
      return;

    case UPDATE_ERASE_POLL:
    case UPDATE_PROGRAM_POLL:
      update_read (ssm, data, FTE3600_FT9368_FLASH_STATUS, 2);
      return;

    case UPDATE_ERASE_CHECK:
      if (update_status (data) == FTE3600_FT9368_ERASE_ACK)
        {
          data->offset = 0;
          fpi_ssm_next_state (ssm);
        }
      else if (++data->poll_attempts >= FTE3600_FT9368_ERASE_ATTEMPTS)
        {
          update_error (ssm, "FT9368 flash erase acknowledgment timed out");
        }
      else
        {
          fpi_ssm_jump_to_state_delayed (ssm, UPDATE_ERASE_POLL, FTE3600_FT9368_UPDATE_POLL_MS);
        }
      return;

    case UPDATE_ADDRESS:
      data->chunk = MIN (FTE3600_FT9368_APP_CHUNK, FTE3600_FT9368_APP_SIZE - data->offset);
      data->expected_status = fpi_fte3600_ft9368_program_ack (data->offset, data->chunk);
      data->poll_attempts = 0;
      payload[0] = data->offset >> 16;
      payload[1] = data->offset >> 8;
      payload[2] = data->offset;
      update_write (ssm, data, FTE3600_FT9368_FLASH_ADDRESS, payload, 3);
      return;

    case UPDATE_DATA:
      update_write (ssm, data, FTE3600_FT9368_FLASH_DATA,
                    (const guint8 *) g_bytes_get_data (data->app, NULL) + data->offset,
                    data->chunk);
      return;

    case UPDATE_PROGRAM_CHECK:
      if (update_status (data) == data->expected_status)
        {
          data->offset += data->chunk;
          if (data->offset < FTE3600_FT9368_APP_SIZE)
            fpi_ssm_jump_to_state (ssm, UPDATE_ADDRESS);
          else
            fpi_ssm_next_state (ssm);
        }
      else if (++data->poll_attempts >= FTE3600_FT9368_PROGRAM_ATTEMPTS)
        {
          update_error (ssm, "FT9368 flash program acknowledgment timed out");
        }
      else
        {
          fpi_ssm_jump_to_state_delayed (ssm, UPDATE_PROGRAM_POLL, FTE3600_FT9368_UPDATE_POLL_MS);
        }
      return;

    case UPDATE_CHECKSUM_START:
      update_read (ssm, data, FTE3600_FT9368_CHECKSUM_START, 0);
      return;

    case UPDATE_CHECKSUM_RANGE:
      payload[3] = FTE3600_FT9368_APP_SIZE >> 16;
      payload[4] = (FTE3600_FT9368_APP_SIZE >> 8) & 0xff;
      payload[5] = FTE3600_FT9368_APP_SIZE & 0xff;
      update_write (ssm, data, FTE3600_FT9368_CHECKSUM_RANGE, payload, sizeof payload);
      return;

    case UPDATE_CHECKSUM_WAIT:
      fpi_ssm_next_state_delayed (ssm, FTE3600_FT9368_CHECKSUM_MS);
      return;

    case UPDATE_CHECKSUM_READ:
      update_read (ssm, data, FTE3600_FT9368_CHECKSUM_READ, 2);
      return;

    case UPDATE_CHECKSUM_CHECK:
      if (update_status (data) != data->checksum)
        update_error (ssm, "FT9368 flash checksum differs from the validated application");
      else
        fpi_ssm_next_state (ssm);
      return;

    case UPDATE_REBOOT:
      update_read (ssm, data, FTE3600_FT9368_REBOOT, 0);
      return;

    case UPDATE_CLEANUP_RELEASE:
      data->cleanup = TRUE;
      if (!data->touched)
        fpi_ssm_jump_to_state (ssm, UPDATE_DONE);
      else
        fpi_fte3600_set_hardware_reset (ssm, self, FALSE);
      return;

    case UPDATE_CLEANUP_WAIT:
      fpi_ssm_next_state_delayed (ssm, FTE3600_FT9368_FLASH_BOOT_MS);
      return;

    case UPDATE_VERIFY_WAKE:
      length = fpi_fte3600_ft9368_read (frame, sizeof frame, FTE3600_FT9368_WAKE, 0);
      update_exchange (ssm, data, frame, length);
      return;

    case UPDATE_VERIFY_WAIT:
      fpi_ssm_next_state_delayed (ssm, FTE3600_FT9368_VERIFY_WAKE_MS);
      return;

    case UPDATE_VERIFY_INFO:
      length = fpi_fte3600_ft9368_read (frame, sizeof frame, FTE3600_FT9368_INFO,
                                        FTE3600_FT9368_INFO_SIZE);
      update_exchange (ssm, data, frame, length);
      return;

    case UPDATE_VERIFY_CHECK:
      {
        Fte3600Ft9368Info info;
        if (!fpi_fte3600_ft9368_parse_info (data->rx + FTE3600_FT9368_HEADER,
                                            FTE3600_FT9368_INFO_SIZE, &info) ||
            info.version != FTE3600_FT9368_PROGRAMMED_VERSION)
          update_error (ssm, "FT9368 application did not confirm identity and version after update");
        else
          fpi_ssm_next_state (ssm);
        return;
      }

    case UPDATE_DONE:
      fpi_ssm_mark_completed (ssm);
      return;

    default:
      g_assert_not_reached ();
    }
}

FpiSsm *
fpi_fte3600_ft9368_update_new (FpiDeviceFte3600 *self)
{
  FpiSsm *ssm = fpi_ssm_new_full (FP_DEVICE (self), update_handler,
                                  UPDATE_NSTATES, UPDATE_CLEANUP_RELEASE,
                                  "FT9368 explicit persistent firmware update");

  fpi_ssm_set_data (ssm, g_new0 (Update, 1), (GDestroyNotify) update_free);
  return ssm;
}
