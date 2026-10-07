/*
 * FocalTech FTE3600 sensor family driver
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "fte3600"

#include "fte3600-private.h"
#include "fte3600-timing.h"
#include "fte3600-protocol.h"
#include "fte3600-ft9368-protocol.h"
#include "fte3600-legacy-recovery.h"
#include "fte3600-special-probe.h"
#include <linux/spi/spidev.h>

enum fte3600_discover_state {
  DISCOVER_READ_HIGH,
  DISCOVER_SAVE_HIGH,
  DISCOVER_READ_LOW,
  DISCOVER_CHECK_RUNTIME,
  DISCOVER_BOOT,
  DISCOVER_CHECK_BOOT,
  DISCOVER_IDENTIFY_38,
  DISCOVER_IDENTIFY_38_DONE,
  DISCOVER_IDENTIFY_B38,
  DISCOVER_IDENTIFY_B38_DONE,
  DISCOVER_ENTER,
  DISCOVER_QUERY,
  DISCOVER_QUERY_DELAY,
  DISCOVER_TRIGGER,
  DISCOVER_READ_FAMILY,
  DISCOVER_CHECK_FAMILY,
  DISCOVER_READ_C8,
  DISCOVER_WRITE_C8,
  DISCOVER_SELECT_OTP,
  DISCOVER_READ_CONTROL,
  DISCOVER_ENABLE_OTP,
  DISCOVER_READ_OTP,
  DISCOVER_SAVE_OTP,
  DISCOVER_DISABLE_OTP,
  DISCOVER_CHECK_OTP,
  DISCOVER_CLEANUP_PREPARE,
  DISCOVER_CLEANUP_HIGH,
  DISCOVER_CLEANUP_ASSERT,
  DISCOVER_CLEANUP_HOLD,
  DISCOVER_CLEANUP_RELEASE,
  DISCOVER_CLEANUP_SETTLE,
  DISCOVER_DONE,
  DISCOVER_NSTATES,
};

static void
fte3600_discovery_packet (FpiSsm *ssm, const guint8 *packet, gsize length)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  FpiSpiTransfer *transfer;

  if (!length || length > sizeof (self->discovery_rx) || length > self->max_transfer)
    {
      fpi_ssm_mark_failed (ssm, g_error_new_literal (
                             G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                             "FTE3600 discovery packet exceeds the available transfer buffer"));
      return;
    }
  memset (self->discovery_rx, 0, sizeof (self->discovery_rx));
  transfer = fpi_spi_transfer_new_with_buffer_size (FP_DEVICE (self), self->spi_fd,
                                                    self->max_transfer);
  fpi_spi_transfer_write (transfer, length);
  memcpy (transfer->buffer_wr, packet, length);
  fpi_spi_transfer_read_full (transfer, self->discovery_rx, length, NULL);
  fpi_spi_transfer_set_full_duplex (transfer, TRUE);
  fpi_fte3600_submit_transfer (ssm, transfer, TRUE);
}

static void
fte3600_discovery_reg (FpiSsm *ssm, gboolean write, guint8 reg, guint8 value)
{
  g_autoptr(GError) error = NULL;
  guint8 packet[FTE3600_REG_WRITE_SIZE];
  gsize length;

  if (write)
    length = fpi_fte3600_build_boot_write (packet, sizeof packet, reg, value, &error);
  else
    length = fpi_fte3600_build_boot_read (packet, sizeof packet, reg, 1, &error);
  if (!length)
    {
      fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      return;
    }

  fte3600_discovery_packet (ssm, packet, length);
}

static void
fte3600_discovery_command (FpiSsm *ssm, Fte3600Command command)
{
  g_autoptr(GError) error = NULL;
  guint8 packet[FTE3600_COMMAND_MAX_SIZE];
  gsize length;

  length = fpi_fte3600_build_command (packet, sizeof packet, command, &error);
  if (!length)
    {
      fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      return;
    }

  fte3600_discovery_packet (ssm, packet, length);
}

static void
fte3600_discover_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  guint state = fpi_ssm_get_cur_state (ssm);
  guint8 low;
  Fte3600Identity identity;

  if (state < DISCOVER_CLEANUP_PREPARE && fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;

  switch (state)
    {
    case DISCOVER_READ_HIGH:
      if (self->discovery_boot_only)
        {
          fpi_ssm_jump_to_state (ssm, DISCOVER_BOOT);
          return;
        }
      fpi_fte3600_submit_reg_read (ssm, FTE3600_REG_SENSOR_ID_HIGH, 1, TRUE);
      return;

    case DISCOVER_SAVE_HIGH:
      self->identity_high = fpi_fte3600_read_result_byte (self);
      fpi_ssm_next_state (ssm);
      return;

    case DISCOVER_READ_LOW:
      fpi_fte3600_submit_reg_read (ssm, FTE3600_REG_SENSOR_ID_LOW, 1, TRUE);
      return;

    case DISCOVER_CHECK_RUNTIME:
      low = fpi_fte3600_read_result_byte (self);
      identity = fpi_fte3600_identify_runtime (self->identity_high, low);
      if (identity.sensor != FTE3600_SENSOR_UNKNOWN)
        {
          self->identity = identity;
          fpi_ssm_mark_completed (ssm);
        }
      else if ((self->identity_high == 0 && low == 0) ||
               (self->identity_high == 0xff && low == 0xff))
        {
          /* No application signature: require a positive ROM family AND OTP
           * identity. An absent application is never an FT9361 default. */
          fpi_ssm_next_state (ssm);
        }
      else
        {
          fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                 FP_DEVICE_ERROR_NOT_SUPPORTED,
                                 "Unsupported FTE3600 runtime signature %02x %02x (sensor class %u)",
                                 self->identity_high, low, identity.sensor));
        }
      return;

    case DISCOVER_BOOT:
      fte3600_discovery_command (ssm, FTE3600_COMMAND_BOOT_PROBE);
      return;

    case DISCOVER_CHECK_BOOT:
      if (self->discovery_rx[FTE3600_BOOT_PROBE_RESULT_OFFSET] == FTE3600_BOOT_A_MARKER ||
          (self->identity.evidence == FTE3600_IDENTITY_RUNTIME_GEOMETRY &&
           (self->identity.sensor == FTE3600_SENSOR_FT9338 || self->identity.sensor == FTE3600_SENSOR_FT9536)))
        fpi_ssm_next_state (ssm);
      else
        fpi_ssm_jump_to_state (ssm, DISCOVER_ENTER);
      return;

    case DISCOVER_IDENTIFY_38:
      fpi_ssm_start_subsm (ssm, fpi_fte3600_legacy38_identify_new (
                             self, self->discovery_rx[FTE3600_BOOT_PROBE_RESULT_OFFSET] == FTE3600_BOOT_A_MARKER));
      return;

    case DISCOVER_IDENTIFY_38_DONE:
    case DISCOVER_IDENTIFY_B38_DONE:
      fpi_ssm_mark_completed (ssm);
      return;

    case DISCOVER_IDENTIFY_B38:
      /* FAMILY_READ has replaced the boot-probe receive buffer. Its header
      * bytes carry no boot-edition evidence. 1534 selects the B38 path. */
      fpi_ssm_start_subsm (ssm, fpi_fte3600_legacy38_identify_new (self, FALSE));
      return;

    case DISCOVER_ENTER:
      /* The boot probe only reads a marker. Recover hardware state only once
       * a command that changes it has been attempted, including failed writes. */
      self->discovery_touched = TRUE;
      fte3600_discovery_command (ssm, FTE3600_COMMAND_BOOT_ENTER);
      return;

    case DISCOVER_QUERY:
      fte3600_discovery_command (ssm, FTE3600_COMMAND_FAMILY_QUERY);
      return;

    case DISCOVER_QUERY_DELAY:
      fpi_ssm_next_state_delayed (ssm, FTE3600_FAMILY_QUERY_DELAY_MS);
      return;

    case DISCOVER_TRIGGER:
      fte3600_discovery_command (ssm, FTE3600_COMMAND_FAMILY_TRIGGER);
      return;

    case DISCOVER_READ_FAMILY:
      fte3600_discovery_command (ssm, FTE3600_COMMAND_FAMILY_READ);
      return;

    case DISCOVER_CHECK_FAMILY:
      self->family = ((guint16) self->discovery_rx[FTE3600_FAMILY_RESULT_OFFSET] << 8) |
                     self->discovery_rx[FTE3600_FAMILY_RESULT_OFFSET + 1];
      if (self->family == 0x1534)
        {
          fp_dbg ("FTE3600 Boot-B38 ROM family %04x detected; branching to B38 identification",
                  self->family);
          fpi_ssm_jump_to_state (ssm, DISCOVER_IDENTIFY_B38);
          return;
        }
      if (self->family != 0x2b50 && self->family != 0x95a8 && self->family != 0x23dd)
        fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                               FP_DEVICE_ERROR_NOT_SUPPORTED, "Unsupported FTE3600 ROM family %04x", self->family));
      else
        fpi_ssm_next_state (ssm);
      return;

    case DISCOVER_READ_C8:
      fte3600_discovery_reg (ssm, FALSE, FTE3600_BOOT_REG_OTP_CONFIG, 0);
      return;

    case DISCOVER_WRITE_C8:
      fte3600_discovery_reg (ssm, TRUE, FTE3600_BOOT_REG_OTP_CONFIG, FTE3600_BOOT_OTP_CONFIG);
      return;

    case DISCOVER_SELECT_OTP:
      fte3600_discovery_reg (ssm, TRUE, FTE3600_BOOT_REG_OTP_ADDRESS, FTE3600_BOOT_OTP_ADDRESS);
      return;

    case DISCOVER_READ_CONTROL:
      fte3600_discovery_reg (ssm, FALSE, FTE3600_BOOT_REG_OTP_CONTROL, 0);
      return;

    case DISCOVER_ENABLE_OTP:
      fte3600_discovery_reg (ssm, TRUE, FTE3600_BOOT_REG_OTP_CONTROL,
                             self->discovery_rx[FTE3600_REG_RESULT_OFFSET] | FTE3600_BOOT_OTP_ENABLE);
      return;

    case DISCOVER_READ_OTP:
      fte3600_discovery_reg (ssm, FALSE, FTE3600_BOOT_REG_OTP_DATA, 0);
      return;

    case DISCOVER_SAVE_OTP:
      self->otp = self->discovery_rx[FTE3600_REG_RESULT_OFFSET];
      fpi_ssm_next_state (ssm);
      return;

    case DISCOVER_DISABLE_OTP:
      fte3600_discovery_reg (ssm, TRUE, FTE3600_BOOT_REG_OTP_CONTROL, 0);
      return;

    case DISCOVER_CHECK_OTP:
      identity = fpi_fte3600_identify_a8_spi (self->family, self->otp);
      if (!fpi_fte3600_identity_allows_firmware (&identity))
        {
          fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                 FP_DEVICE_ERROR_NOT_SUPPORTED,
                                 "Unsupported FTE3600 ROM/OTP identity %04x/%02x", self->family, self->otp));
        }
      else if ((self->identity.sensor != FTE3600_SENSOR_UNKNOWN &&
                self->identity.sensor != identity.sensor) ||
               (self->probed_sensor != FTE3600_SENSOR_UNKNOWN &&
                self->probed_sensor != identity.sensor))
        {
          fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                 FP_DEVICE_ERROR_NOT_SUPPORTED,
                                 "FTE3600 ROM identity conflicts with the previously detected sensor"));
        }
      else
        {
          self->identity = identity;
          self->rom_identity = identity;
          fp_dbg ("%s identified from ROM family %04x and OTP %02x",
                  fpi_fte3600_sensor_get (identity.sensor)->name,
                  self->family, self->otp);
          fpi_ssm_next_state (ssm);
        }
      return;

    case DISCOVER_CLEANUP_PREPARE:
      if (!self->discovery_touched)
        fpi_ssm_jump_to_state (ssm, DISCOVER_DONE);
      else
        fpi_fte3600_set_hardware_reset (ssm, self, FALSE);
      return;

    case DISCOVER_CLEANUP_HIGH:
      fpi_ssm_next_state_delayed (ssm, FTE3600_RESET_HIGH_MS);
      return;

    case DISCOVER_CLEANUP_ASSERT:
      fpi_fte3600_set_hardware_reset (ssm, self, TRUE);
      return;

    case DISCOVER_CLEANUP_HOLD:
      fpi_ssm_next_state_delayed (ssm, FTE3600_RESET_LOW_MS);
      return;

    case DISCOVER_CLEANUP_RELEASE:
      fpi_fte3600_set_hardware_reset (ssm, self, FALSE);
      return;

    case DISCOVER_CLEANUP_SETTLE:
      fpi_ssm_next_state_delayed (ssm, FTE3600_RESET_BOOT_MS);
      return;

    case DISCOVER_DONE:
      fpi_ssm_mark_completed (ssm);
      return;

    default:
      g_assert_not_reached ();
    }
}

void
fpi_fte3600_start_boot_discovery (FpiSsm *parent)
{
  FpDevice *dev = fpi_ssm_get_device (parent);
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  FpiSsm *child;

  child = fpi_fte3600_discovery_new (self, TRUE);
  fpi_ssm_start_subsm (parent, child);
}

static FpiSsm *
fte3600_legacy_discovery_new (FpiDeviceFte3600 *self, gboolean boot_only)
{
  self->discovery_boot_only = boot_only;
  self->discovery_touched = FALSE;
  return fpi_ssm_new_full (FP_DEVICE (self), fte3600_discover_handler,
                           DISCOVER_NSTATES, DISCOVER_CLEANUP_PREPARE,
                           "FTE3600 sensor discovery");
}

enum {
  LEGACY_WAKE_FIRST,
  LEGACY_WAKE_INTERVAL,
  LEGACY_WAKE_SECOND,
  LEGACY_WAKE_NSTATES,
};

/* An inactive legacy application may return zeros until both soft-reset
 * commands have completed. Finish the bounded pair before observing cancel;
 * no reset GPIO is asserted and this never uploads firmware. */
static void
fte3600_legacy_wake_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  FpiSpiTransfer *transfer;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case LEGACY_WAKE_FIRST:
    case LEGACY_WAKE_SECOND:
      self->idle_verified = FALSE;
      transfer = fpi_spi_transfer_new_with_buffer_size (dev, self->spi_fd,
                                                        self->max_transfer);
      fpi_spi_transfer_write (transfer, FTE3600_SOFT_RESET_SIZE);
      transfer->buffer_wr[0] = FTE3600_OPCODE_SOFT_RESET;
      /* Full duplex validates the returned byte count; a write-only transfer
       * must not let a zero-byte result advance this wake handshake. */
      fpi_spi_transfer_read (transfer, FTE3600_SOFT_RESET_SIZE);
      fpi_spi_transfer_set_full_duplex (transfer, TRUE);
      fpi_fte3600_submit_transfer (ssm, transfer, FALSE);
      break;

    case LEGACY_WAKE_INTERVAL:
      fpi_ssm_next_state_delayed (ssm, FTE3600_SOFT_RESET_INTERVAL_MS);
      break;

    default:
      g_assert_not_reached ();
    }
}

/* Reference SPI factory rounds 0/1: FT9368, special family, then (round 1)
 * legacy firmware status and geometry. Capability-gated alternate connections
 * and repeated identity validation are Linux integration safeguards. Later
 * reference rounds that guess a firmware family are deliberately excluded. */
enum {
  IDENTIFY_RUNTIME_HIGH, IDENTIFY_RUNTIME_SAVE,
  IDENTIFY_RUNTIME_LOW, IDENTIFY_RUNTIME_CHECK,
  IDENTIFY_FACTORY_BEGIN, IDENTIFY_9368_WAKE, IDENTIFY_9368_DELAY,
  IDENTIFY_9368, IDENTIFY_9368_CHECK,
  IDENTIFY_NEGOTIATE, IDENTIFY_NEGOTIATE_CHECK, IDENTIFY_NEXT_CONNECTION,
  IDENTIFY_LEGACY_WAKE_BEGIN, IDENTIFY_LEGACY_WAKE_PAIR,
  IDENTIFY_LEGACY_WAKE_MCU, IDENTIFY_LEGACY_WAKE_CHECK_MCU,
  IDENTIFY_LEGACY_WAKE_SETTLE, IDENTIFY_LEGACY_WAKE_HIGH,
  IDENTIFY_LEGACY_WAKE_SAVE, IDENTIFY_LEGACY_WAKE_LOW,
  IDENTIFY_LEGACY_WAKE_CHECK, IDENTIFY_LEGACY_WAKE_NEXT,
  IDENTIFY_ROM, IDENTIFY_DONE, IDENTIFY_CLEANUP, IDENTIFY_NSTATES,
};

typedef struct
{
  guint32         original_mode;
  gboolean        alternate;
  gboolean        rom_alternate;
  gboolean        wake_alternate;
  guint           factory_round;
  guint           ft9368_attempts;
  guint           wake_attempts;
  gboolean        unknown_application[2];
  Fte3600Identity candidate;
  Fte3600Identity special_result;
  gboolean        confirming;
} Fte3600Discovery;

static gboolean
fte3600_confirm_identity (FpiSsm *ssm, Fte3600Identity identity, guint repeat_state)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  Fte3600Discovery *data = fpi_ssm_get_data (ssm);

  if (identity.sensor == FTE3600_SENSOR_UNKNOWN)
    return FALSE;
  if (!data->confirming)
    {
      data->candidate = identity;
      data->confirming = TRUE;
      fpi_ssm_jump_to_state (ssm, repeat_state);
    }
  else if (data->candidate.sensor != identity.sensor ||
           data->candidate.response != identity.response)
    {
      fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                             FP_DEVICE_ERROR_PROTO, "FTE3600 identity is unstable"));
    }
  else
    {
      self->identity = identity;
      fp_dbg ("Detected %s (SPI mode 0x%x, CS %s)",
              fpi_fte3600_sensor_get (identity.sensor)->name, self->spi_mode,
              (self->transport_capabilities & FTE3600_TRANSPORT_CAP_CS_POLARITY) ?
              "switchable" : "controller-managed");
      fpi_ssm_mark_completed (ssm);
    }
  return TRUE;
}

static void
fte3600_next_protocol (FpiSsm *ssm, guint state)
{
  Fte3600Discovery *data = fpi_ssm_get_data (ssm);

  /* A successful first observation followed by an invalid reply is an error,
  * not a reason to try a more permissive interpretation of the same chip. */
  if (data->confirming)
    fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                           FP_DEVICE_ERROR_PROTO, "FTE3600 identity could not be confirmed"));
  else
    fpi_ssm_jump_to_state (ssm, state);
}

static void
fte3600_rom_probe_done (FpiSsm *child, FpDevice *dev, GError *error)
{
  FpiSsm *parent = fpi_ssm_get_data (child);
  Fte3600Discovery *data = fpi_ssm_get_data (parent);
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);

  if (!error)
    {
      fpi_ssm_jump_to_state (parent, IDENTIFY_DONE);
    }
  else if (!data->rom_alternate &&
           (self->transport_capabilities & FTE3600_TRANSPORT_CAP_CS_POLARITY) &&
           g_error_matches (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED))
    {
      g_clear_error (&error);
      data->rom_alternate = TRUE;
      if (fpi_fte3600_set_cs_polarity (self, !(data->original_mode & SPI_CS_HIGH), &error))
        fpi_ssm_jump_to_state (parent, IDENTIFY_ROM);
      else
        fpi_ssm_mark_failed (parent, error);
    }
  else
    {
      fpi_ssm_mark_failed (parent, error);
    }
}

static void
fte3600_identify_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  Fte3600Discovery *data = fpi_ssm_get_data (ssm);
  guint state = fpi_ssm_get_cur_state (ssm);

  g_autoptr(GError) error = NULL;
  guint8 packet[64];
  gsize length;
  Fte3600Identity identity;
  Fte3600Ft9368Info info;
  FpiSsm *child;

  if (state < IDENTIFY_CLEANUP && fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;
  switch (state)
    {
    case IDENTIFY_RUNTIME_HIGH:
      if (!self->transport_ops || !self->transport_ops->probe_runtime_first)
        {
          fpi_ssm_jump_to_state (ssm, IDENTIFY_FACTORY_BEGIN);
          return;
        }
      fpi_fte3600_submit_reg_read (ssm, FTE3600_REG_SENSOR_ID_HIGH, 1, TRUE);
      return;

    case IDENTIFY_RUNTIME_SAVE:
      self->identity_high = fpi_fte3600_read_result_byte (self);
      fpi_ssm_next_state (ssm);
      return;

    case IDENTIFY_RUNTIME_LOW:
      fpi_fte3600_submit_reg_read (ssm, FTE3600_REG_SENSOR_ID_LOW, 1, TRUE);
      return;

    case IDENTIFY_RUNTIME_CHECK:
      identity = fpi_fte3600_identify_runtime (self->identity_high,
                                               fpi_fte3600_read_result_byte (self));
      if (fte3600_confirm_identity (ssm, identity, IDENTIFY_RUNTIME_HIGH))
        return;
      if (identity.response != 0 && identity.response != 0xffff)
        {
          fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                 FP_DEVICE_ERROR_NOT_SUPPORTED,
                                 "Unrecognized running application geometry %04x", identity.response));
          return;
        }
      /* A lost confirmation must fail, rather than reset a running device.
       * With no application evidence, retain the complete factory path. */
      fte3600_next_protocol (ssm, IDENTIFY_FACTORY_BEGIN);
      return;

    case IDENTIFY_FACTORY_BEGIN:
      data->ft9368_attempts = 0;
      fpi_ssm_next_state (ssm);
      return;

    case IDENTIFY_9368_WAKE:
      if (self->max_transfer < FTE3600_FT9368_HEADER + FTE3600_FT9368_INFO_SIZE)
        {
          fte3600_next_protocol (ssm, IDENTIFY_NEGOTIATE);
          return;
        }
      /* 26d9c: the factory reads full application information after wake.
       * It does not enter flash programming on this path. */
      data->ft9368_attempts++;
      length = fpi_fte3600_ft9368_read (packet, sizeof packet, FTE3600_FT9368_WAKE, 0);
      break;

    case IDENTIFY_9368_DELAY:
      fpi_ssm_next_state_delayed (ssm, FTE3600_FACTORY_FT9368_WAKE_MS);
      return;

    case IDENTIFY_9368:
      length = fpi_fte3600_ft9368_read (packet, sizeof packet, FTE3600_FT9368_INFO, FTE3600_FT9368_INFO_SIZE);
      break;

    case IDENTIFY_9368_CHECK:
      if (fpi_fte3600_ft9368_parse_info (self->discovery_rx + FTE3600_FT9368_HEADER, FTE3600_FT9368_INFO_SIZE, &info) &&
          fte3600_confirm_identity (ssm, fpi_fte3600_identify_special (0x9368), IDENTIFY_9368))
        return;
      if (self->discovery_rx[26] == 0x93 && self->discovery_rx[27] == 0x68)
        {
          fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                 FP_DEVICE_ERROR_PROTO, "FT9368 returned incompatible application metadata"));
          return;
        }
      /* In factory rounds 0/1, a negative first ReadChipID is followed by
       * another complete wake/read at 240ce. A lost positive confirmation is
       * still an error, not permission to start a different protocol. */
      fte3600_next_protocol (ssm, data->ft9368_attempts < FTE3600_FACTORY_FT9368_ATTEMPTS ?
                             IDENTIFY_9368_WAKE : IDENTIFY_NEGOTIATE);
      return;

    case IDENTIFY_NEGOTIATE:
      fpi_ssm_start_subsm (ssm, fpi_fte3600_special_probe_new (self, &data->special_result));
      return;

    case IDENTIFY_NEGOTIATE_CHECK:
      if (data->special_result.sensor != FTE3600_SENSOR_UNKNOWN)
        {
          self->identity = data->special_result;
          fpi_ssm_mark_completed (ssm);
          return;
        }
      if (data->special_result.evidence == FTE3600_IDENTITY_KNOWN_UNMAPPED_ID)
        {
          /* A repeated silicon identity cannot be reinterpreted as a legacy
           * application after reset, even if its geometry registers reply. */
          fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                 FP_DEVICE_ERROR_NOT_SUPPORTED,
                                 "Unsupported FocalTech silicon ID %04x",
                                 data->special_result.response));
          return;
        }
      fpi_ssm_next_state (ssm);
      return;

    case IDENTIFY_NEXT_CONNECTION:
      if (!data->alternate && (self->transport_capabilities & FTE3600_TRANSPORT_CAP_CS_POLARITY))
        {
          data->alternate = TRUE;
          if (fpi_fte3600_set_cs_polarity (self, !(data->original_mode & SPI_CS_HIGH), &error))
            fpi_ssm_jump_to_state (ssm, IDENTIFY_FACTORY_BEGIN);
          else
            fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
          return;
        }
      if (!fpi_fte3600_set_cs_polarity (self, data->original_mode & SPI_CS_HIGH, &error))
        {
          fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
          return;
        }
      if (++data->factory_round < FTE3600_FACTORY_APPLICATION_ROUNDS)
        {
          /* Reference round zero skips legacy detection and repeats the
           * application factory. Do not collapse these into a fast path. */
          data->alternate = FALSE;
          fpi_ssm_jump_to_state (ssm, IDENTIFY_FACTORY_BEGIN);
        }
      else
        {
          fpi_ssm_next_state (ssm);
        }
      return;

    case IDENTIFY_LEGACY_WAKE_BEGIN:
      /* MultiCheckFWExist runs in reference round one. Geometry is read only
       * after its positive MCU status and the factory's 350 ms settle. */
      data->wake_attempts = 0;
      fpi_ssm_next_state (ssm);
      return;

    case IDENTIFY_LEGACY_WAKE_PAIR:
      data->wake_attempts++;
      fpi_ssm_start_subsm (ssm, fpi_ssm_new_full (dev, fte3600_legacy_wake_handler,
                                                  LEGACY_WAKE_NSTATES, LEGACY_WAKE_NSTATES,
                                                  "FTE3600 legacy application wake"));
      return;

    case IDENTIFY_LEGACY_WAKE_MCU:
      /* CheckFWExist reads status immediately after the second 70. */
      fpi_fte3600_submit_reg_read (ssm, FTE3600_REG_MCU_STATUS, 2, TRUE);
      return;

    case IDENTIFY_LEGACY_WAKE_CHECK_MCU:
      fp_dbg ("Legacy wake MCU response: %02x %02x (attempt %u/%u, reported CS_HIGH %s)",
              self->small_rx[FTE3600_REG_RESULT_OFFSET],
              self->small_rx[FTE3600_REG_RESULT_OFFSET + 1], data->wake_attempts,
              FTE3600_LEGACY_WAKE_MAX_ATTEMPTS,
              (self->spi_mode & SPI_CS_HIGH) ? "high" : "low");
      if (fpi_fte3600_mcu_is_idle (self))
        fpi_ssm_next_state (ssm);
      else if (data->wake_attempts < FTE3600_LEGACY_WAKE_MAX_ATTEMPTS)
        fpi_ssm_jump_to_state_delayed (ssm, IDENTIFY_LEGACY_WAKE_PAIR,
                                       FTE3600_LEGACY_WAKE_RETRY_MS);
      else
        fpi_ssm_jump_to_state (ssm, IDENTIFY_LEGACY_WAKE_NEXT);
      return;

    case IDENTIFY_LEGACY_WAKE_SETTLE:
      fpi_ssm_next_state_delayed (ssm, FTE3600_LEGACY_WAKE_GEOMETRY_MS);
      return;

    case IDENTIFY_LEGACY_WAKE_HIGH:
      fpi_fte3600_submit_reg_read (ssm, FTE3600_REG_SENSOR_ID_HIGH, 1, TRUE);
      return;

    case IDENTIFY_LEGACY_WAKE_SAVE:
      self->identity_high = fpi_fte3600_read_result_byte (self);
      fpi_ssm_next_state (ssm);
      return;

    case IDENTIFY_LEGACY_WAKE_LOW:
      fpi_fte3600_submit_reg_read (ssm, FTE3600_REG_SENSOR_ID_LOW, 1, TRUE);
      return;

    case IDENTIFY_LEGACY_WAKE_CHECK:
      identity = fpi_fte3600_identify_runtime (self->identity_high, fpi_fte3600_read_result_byte (self));
      fp_dbg ("Legacy wake 14/15 response: %04x (runtime geometry)", identity.response);
      if (identity.sensor == FTE3600_SENSOR_UNKNOWN && identity.response != 0 && identity.response != 0xffff)
        data->unknown_application[data->wake_alternate] = TRUE;
      if (!fte3600_confirm_identity (ssm, identity, IDENTIFY_LEGACY_WAKE_HIGH))
        fte3600_next_protocol (ssm, IDENTIFY_LEGACY_WAKE_NEXT);
      return;

    case IDENTIFY_LEGACY_WAKE_NEXT:
      if (!data->wake_alternate && (self->transport_capabilities & FTE3600_TRANSPORT_CAP_CS_POLARITY))
        {
          data->wake_alternate = TRUE;
          if (fpi_fte3600_set_cs_polarity (self, !(data->original_mode & SPI_CS_HIGH), &error))
            fpi_ssm_jump_to_state (ssm, IDENTIFY_LEGACY_WAKE_BEGIN);
          else
            fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
        }
      else if (fpi_fte3600_set_cs_polarity (self, data->original_mode & SPI_CS_HIGH, &error))
        {
          fpi_ssm_next_state (ssm);
        }
      else
        {
          fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
        }
      return;

    case IDENTIFY_ROM:
      if (data->unknown_application[data->rom_alternate])
        {
          if (!data->rom_alternate && (self->transport_capabilities & FTE3600_TRANSPORT_CAP_CS_POLARITY))
            {
              data->rom_alternate = TRUE;
              if (fpi_fte3600_set_cs_polarity (self, !(data->original_mode & SPI_CS_HIGH), &error))
                fpi_ssm_jump_to_state (ssm, IDENTIFY_ROM);
              else
                fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
            }
          else
            {
              fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                     FP_DEVICE_ERROR_NOT_SUPPORTED, "Unrecognized FTE3600 application signature"));
            }
          return;
        }
      child = fte3600_legacy_discovery_new (self, TRUE);
      fpi_ssm_set_data (child, ssm, NULL);
      fpi_ssm_start (child, fte3600_rom_probe_done);
      return;

    case IDENTIFY_DONE:
      fpi_ssm_mark_completed (ssm);
      return;

    case IDENTIFY_CLEANUP:
      if (fpi_ssm_get_error (ssm) &&
          !fpi_fte3600_set_cs_polarity (self, data->original_mode & SPI_CS_HIGH, &error))
        fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      else
        fpi_ssm_mark_completed (ssm);
      return;

    default:
      g_assert_not_reached ();
    }
  if (!length)
    fpi_ssm_mark_failed (ssm, error ? g_steal_pointer (&error) :
                         fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Invalid discovery packet"));
  else
    fte3600_discovery_packet (ssm, packet, length);
}

FpiSsm *
fpi_fte3600_discovery_new (FpiDeviceFte3600 *self, gboolean boot_only)
{
  FpiSsm *ssm;
  Fte3600Discovery *data;

  if (boot_only)
    return fte3600_legacy_discovery_new (self, TRUE);
  data = g_new0 (Fte3600Discovery, 1);
  data->original_mode = self->spi_mode;
  ssm = fpi_ssm_new_full (FP_DEVICE (self), fte3600_identify_handler,
                          IDENTIFY_NSTATES, IDENTIFY_CLEANUP, "FTE3600 protocol discovery");
  fpi_ssm_set_data (ssm, data, g_free);
  return ssm;
}
