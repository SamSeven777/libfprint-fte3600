/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Independently expressed shared factory negotiation for FW9369/FT93xx. */
#define FP_COMPONENT "fte3600"
#include "fte3600-special-probe.h"
#include "fte3600-special-probe-timing.h"
#include "fte3600-timing.h"
#include "fte3600-fw9369-protocol.h"
#include "fte3600-ft93xx-protocol.h"

typedef struct
{
  Fte3600Identity *result;
  Fte3600Identity  candidate;
  guint8           rx[FTE3600_FW9369_WORD_READ_SIZE];
  guint16          first_id;
  guint16          first_variant;
  guint            mode_attempts;
  guint            mode_configurations;
  guint            attempts;
  gboolean         touched;
} SpecialProbe;

enum {
  SPECIAL_VALIDATE,
  SPECIAL_ATTEMPT,
  SPECIAL_WAKE,
  SPECIAL_WAKE_DELAY,
  SPECIAL_READ_STATE,
  SPECIAL_CHECK_STATE,
  SPECIAL_IDLE_DELAY,
  SPECIAL_WAKE_END,
  SPECIAL_MODE_WRITE,
  SPECIAL_MODE_DELAY,
  SPECIAL_MODE_READ,
  SPECIAL_MODE_CHECK,
  SPECIAL_ID_READ,
  SPECIAL_ID_SAVE,
  SPECIAL_ID_REPEAT,
  SPECIAL_ID_CHECK,
  SPECIAL_VARIANT_READ,
  SPECIAL_VARIANT_SAVE,
  SPECIAL_VARIANT_REPEAT,
  SPECIAL_VARIANT_CHECK,
  SPECIAL_RESULT,
  SPECIAL_RETRY_RESET,
  SPECIAL_RETRY,
  SPECIAL_CLEANUP,
  SPECIAL_DONE,
  SPECIAL_NSTATES,
};

enum {
  RESET_BEGIN,
  RESET_RELEASE,
  RESET_HIGH_DELAY,
  RESET_ASSERT,
  RESET_LOW_DELAY,
  RESET_END,
  RESET_SETTLE,
  RESET_DONE,
  RESET_NSTATES,
};

static void
special_reset_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);

  /* Once started, finish the physical H10/L20/H sequence even if a GPIO
   * operation fails or cancellation arrives. Cleanup states preserve the
   * first error while still attempting the final deassertion. */
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case RESET_BEGIN:
      fpi_ssm_next_state (ssm);
      return;
    case RESET_RELEASE:
    case RESET_END:
      fpi_fte3600_set_hardware_reset (ssm, self, FALSE);
      return;
    case RESET_HIGH_DELAY:
      fpi_ssm_next_state_delayed (ssm, FTE3600_RESET_HIGH_MS);
      return;
    case RESET_ASSERT:
      fpi_fte3600_set_hardware_reset (ssm, self, TRUE);
      return;
    case RESET_LOW_DELAY:
      fpi_ssm_next_state_delayed (ssm, FTE3600_RESET_LOW_MS);
      return;
    case RESET_SETTLE:
      fpi_ssm_next_state_delayed (ssm, FTE3600_SPECIAL_RESET_SETTLE_MS);
      return;
    case RESET_DONE:
      fpi_ssm_mark_completed (ssm);
      return;
    default:
      g_assert_not_reached ();
    }
}

static void
special_reset (FpiSsm *ssm)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  SpecialProbe *data = fpi_ssm_get_data (ssm);
  FpiSsm *child;

  self->armed = FALSE;
  fpi_fte3600_clear_irq_source (self);
  /* The child always attempts the entire pulse, including on failure. Its
   * error must not cause the parent's cleanup to issue a second pulse. */
  data->touched = FALSE;
  child = fpi_ssm_new_full (FP_DEVICE (self), special_reset_handler,
                            RESET_NSTATES, RESET_RELEASE, "special-factory-reset");
  fpi_ssm_start_subsm (ssm, child);
}

static void
special_exchange (FpiSsm *ssm, const guint8 *packet, gsize length, GError *error)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  SpecialProbe *data = fpi_ssm_get_data (ssm);
  FpiSpiTransfer *transfer;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  if (!length || length > sizeof data->rx || length > self->max_transfer)
    {
      fpi_ssm_mark_failed (ssm, g_error_new_literal (
                             G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                             "Special-family probe exceeds the transport limit"));
      return;
    }
  memset (data->rx, 0, sizeof data->rx);
  transfer = fpi_spi_transfer_new_with_buffer_size (FP_DEVICE (self), self->spi_fd,
                                                    self->max_transfer);
  fpi_spi_transfer_write (transfer, length);
  memcpy (transfer->buffer_wr, packet, length);
  fpi_spi_transfer_read_full (transfer, data->rx, length, NULL);
  fpi_spi_transfer_set_full_duplex (transfer, TRUE);
  /* Even the first wake command may change state. Failed or cancelled writes
   * do not prove that no bytes reached the chip, so they require cleanup. */
  data->touched = TRUE;
  self->idle_verified = FALSE;
  fpi_fte3600_submit_transfer (ssm, transfer, TRUE);
}

static guint16
special_word (const SpecialProbe *data)
{
  const guint8 *p = data->rx + FTE3600_FW9369_WORD_RESULT_OFFSET;

  return ((guint16) p[0] << 8) | p[1];
}

static void
special_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  SpecialProbe *data = fpi_ssm_get_data (ssm);
  guint state = fpi_ssm_get_cur_state (ssm);
  guint8 packet[FTE3600_FW9369_WORD_READ_SIZE];

  g_autoptr(GError) error = NULL;
  gsize length = 0;
  guint16 value;

  if (state < SPECIAL_CLEANUP && fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;

  switch (state)
    {
    case SPECIAL_VALIDATE:
      if (self->max_transfer < FTE3600_FW9369_WORD_READ_SIZE)
        fpi_ssm_mark_failed (ssm, g_error_new_literal (
                               G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                               "Special-family identification requires a 12-byte transaction"));
      else
        fpi_ssm_next_state (ssm);
      return;

    case SPECIAL_WAKE:
      length = fpi_fte3600_fw9369_build_command (packet, sizeof packet,
                                                 FTE3600_FW9369_CMD_WAKE, &error);
      break;

    case SPECIAL_ATTEMPT:
      data->attempts++;
      data->mode_attempts = 0;
      data->mode_configurations = 0;
      fpi_ssm_next_state (ssm);
      return;

    case SPECIAL_WAKE_DELAY:
    case SPECIAL_IDLE_DELAY:
      fpi_ssm_next_state_delayed (ssm, FTE3600_SPECIAL_COMMAND_DELAY_MS);
      return;

    case SPECIAL_READ_STATE:
      length = fpi_fte3600_fw9369_build_sfr_read (packet, sizeof packet,
                                                  FTE3600_FW9369_SFR_STATE, &error);
      break;

    case SPECIAL_CHECK_STATE:
      if (data->rx[FTE3600_FW9369_SFR_RESULT_OFFSET] == FTE3600_FW9369_STATE_IDLE)
        {
          fpi_ssm_jump_to_state (ssm, SPECIAL_WAKE_END);
          return;
        }
      length = fpi_fte3600_fw9369_build_command (packet, sizeof packet,
                                                 FTE3600_FW9369_CMD_IDLE_1, &error);
      break;

    case SPECIAL_WAKE_END:
      length = fpi_fte3600_fw9369_build_command (packet, sizeof packet,
                                                 FTE3600_FW9369_CMD_WAKE_END, &error);
      break;

    case SPECIAL_MODE_WRITE:
      data->mode_attempts++;
      length = fpi_fte3600_fw9369_build_sfr_write (packet, sizeof packet,
                                                   FTE3600_FW9369_SFR_SPI_MODE, 1, &error);
      break;

    case SPECIAL_MODE_DELAY:
      fpi_ssm_next_state_delayed (ssm, FTE3600_SPECIAL_MODE_DELAY_MS);
      return;

    case SPECIAL_MODE_READ:
      length = fpi_fte3600_fw9369_build_sfr_read (packet, sizeof packet,
                                                  FTE3600_FW9369_SFR_SPI_MODE, &error);
      break;

    case SPECIAL_MODE_CHECK:
      if (data->rx[FTE3600_FW9369_SFR_RESULT_OFFSET] != 1 &&
          data->mode_attempts < FTE3600_SPECIAL_MODE_ATTEMPTS)
        {
          fpi_ssm_jump_to_state (ssm, SPECIAL_MODE_WRITE);
          return;
        }
      /* The factory invokes the C6 helper twice before reading the ID and
       * does not use either helper's exhausted-readback result as a gate.
       * Each helper has its own counter; transport errors still fail closed. */
      if (++data->mode_configurations < FTE3600_SPECIAL_MODE_CONFIG_PASSES)
        {
          data->mode_attempts = 0;
          fpi_ssm_jump_to_state (ssm, SPECIAL_MODE_WRITE);
        }
      else
        fpi_ssm_next_state (ssm);
      return;

    case SPECIAL_ID_READ:
    case SPECIAL_ID_REPEAT:
      length = fpi_fte3600_fw9369_build_word_read (packet, sizeof packet,
                                                   FTE3600_FW9369_WORD_CHIP_ID, &error);
      break;

    case SPECIAL_ID_SAVE:
      data->first_id = special_word (data);
      fpi_ssm_next_state (ssm);
      return;

    case SPECIAL_ID_CHECK:
      value = special_word (data);
      if (value != data->first_id)
        {
          /* Confirmation failure must not become permission to try legacy
           * ROM recovery after observing a different family on this bus. */
          fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                 FP_DEVICE_ERROR_PROTO,
                                 "Special-family identity changed from %04x to %04x",
                                 data->first_id, value));
          return;
        }
      /* Only these four responses are selected by the reference shared
       * factory path. FT9368 uses its separate metadata protocol. */
      if (value == 0x9362 || value == 0x9365 || value == 0x9391 || value == 0x9392)
        {
          data->candidate = fpi_fte3600_identify_special (value);
        }
      else
        {
          Fte3600Identity candidate = fpi_fte3600_identify_special (value);
          if (candidate.evidence == FTE3600_IDENTITY_KNOWN_UNMAPPED_ID)
            data->candidate = candidate;
          fpi_ssm_jump_to_state (ssm, SPECIAL_RESULT);
          return;
        }
      if (value == 0x9391)
        {
          /* Withhold the candidate until the independent variant register
           * also repeats; a factory family ID alone cannot exclude FT9395. */
          data->candidate = (Fte3600Identity){ 0 };
          fpi_ssm_next_state (ssm);
        }
      else
        {
          fpi_ssm_jump_to_state (ssm, SPECIAL_RESULT);
        }
      return;

    case SPECIAL_VARIANT_READ:
    case SPECIAL_VARIANT_REPEAT:
      length = fpi_fte3600_ft93xx_read16 (packet, sizeof packet,
                                          FT93XX_REG_VARIANT, &error);
      break;

    case SPECIAL_VARIANT_SAVE:
      if (!fpi_fte3600_ft93xx_read16_result (data->rx, FT93XX_REGISTER_READ_SIZE,
                                             &data->first_variant, &error))
        fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      else
        fpi_ssm_next_state (ssm);
      return;

    case SPECIAL_VARIANT_CHECK:
      if (!fpi_fte3600_ft93xx_read16_result (data->rx, FT93XX_REGISTER_READ_SIZE,
                                             &value, &error))
        {
          fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
        }
      else if (value != data->first_variant)
        {
          fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                 FP_DEVICE_ERROR_PROTO, "FT9391 variant response is unstable"));
        }
      else
        {
          data->candidate = fpi_fte3600_identify_special (value == 0x0fff ? 0x9395 : 0x9391);
          fpi_ssm_next_state (ssm);
        }
      return;

    case SPECIAL_RESULT:
      if (data->candidate.sensor != FTE3600_SENSOR_UNKNOWN)
        fpi_ssm_jump_to_state (ssm, SPECIAL_DONE);
      else if (data->candidate.evidence == FTE3600_IDENTITY_KNOWN_UNMAPPED_ID)
        fpi_ssm_jump_to_state (ssm, SPECIAL_CLEANUP);
      else if (data->attempts == 1)
        fpi_ssm_next_state (ssm);
      else
        /* The second ordinary negative goes directly to the next family:
         * there is no third hardware reset in the reference factory. */
        fpi_ssm_jump_to_state (ssm, SPECIAL_DONE);
      return;

    case SPECIAL_RETRY_RESET:
      special_reset (ssm);
      return;

    case SPECIAL_RETRY:
      fpi_ssm_jump_to_state (ssm, SPECIAL_ATTEMPT);
      return;

    case SPECIAL_CLEANUP:
      if (!data->touched)
        {
          fpi_ssm_jump_to_state (ssm, SPECIAL_DONE);
          return;
        }
      /* Linux additionally resets after an I/O/cancellation/identity error
       * or a known unsupported identity. Never restore a guessed C6 value. */
      special_reset (ssm);
      return;

    case SPECIAL_DONE:
      /* RESULT checks cancellation before the synchronous success path.
       * A cleanup reset may instead have waited asynchronously; report any
       * cancellation received there without starting a second pulse. */
      if (!data->touched && !fpi_ssm_get_error (ssm) &&
          fpi_fte3600_fail_if_cancelled (ssm, dev))
        return;
      if (!fpi_ssm_get_error (ssm))
        *data->result = data->candidate;
      fpi_ssm_mark_completed (ssm);
      return;

    default:
      g_assert_not_reached ();
    }
  special_exchange (ssm, packet, length, g_steal_pointer (&error));
}

FpiSsm *
fpi_fte3600_special_probe_new (FpiDeviceFte3600 *self, Fte3600Identity *result)
{
  SpecialProbe *data;
  FpiSsm *ssm;

  g_return_val_if_fail (self != NULL, NULL);
  g_return_val_if_fail (result != NULL, NULL);
  *result = (Fte3600Identity){ 0 };
  data = g_new0 (SpecialProbe, 1);
  data->result = result;
  ssm = fpi_ssm_new_full (FP_DEVICE (self), special_handler, SPECIAL_NSTATES,
                          SPECIAL_CLEANUP, "special-family-probe");
  fpi_ssm_set_data (ssm, data, g_free);
  return ssm;
}
