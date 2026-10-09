/*
 * FW9369 host-controlled sensor (silicon ID 0x9362)
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Protocol facts: docs/fte3600/protocols.md. Calibration and image
 * processing are independent implementations; no vendor executable or table
 * is embedded in this driver.
 */
#define FP_COMPONENT "fte3600"

#include "fte3600-fw9369.h"
#include "fte3600-fw9369-protocol.h"
#include "fte3600-special-probe.h"
#include "fte3600-special-probe-timing.h"

/* Hardware waits are milliseconds. Calibration limits are host policy. */
#define COMMAND_DELAY_MS 1
#define MAX_POLL_ATTEMPTS 10
#define MAX_CALIBRATION_ATTEMPTS 12
#define MAX_STABILITY_ATTEMPTS 10
#define REQUIRED_STABLE_SAMPLES 3
#define MAX_SPURIOUS_EVENTS 64
#define COMMUNICATION_WAKE_ATTEMPTS 10
#define COMMUNICATION_SETTLE_MS 20
#define RECOVERY_SETTLE_MS 5
/* Host policy: bound consecutive recoveries without a usable finger event. */
#define MAX_EVENT_RECOVERIES 3
/* Windows default four-channel detector (10BEC, 141E4, 120CC). */
#define FDT_DOWN_THRESHOLD 50
#define FDT_UP_THRESHOLD 45
#define FDT_BASE_MARGIN 30
#define RELEASE_CHECK_SAMPLES 3
#define RELEASE_CHECK_TOLERANCE 49
#define IMAGE_SPIKE_THRESHOLD 300
#define IMAGE_NEIGHBOR_THRESHOLD 100

typedef struct
{
  guint16  baseline[FTE3600_FW9369_PIXELS];
  guint16  raw[FTE3600_FW9369_PIXELS];
  guint16  fdt[4];
  guint16  fdt_previous[4];
  guint16  fdt_base[4];
  guint16  process;
  guint16  events;
  guint16  id;
  guint16  spi_mode;
  guint8   image_dac;
  guint8   fdt_dac;
  guint    low_dac;
  guint    high_dac;
  guint    attempts;
  guint    stable;
  guint    spurious;
  guint    mode_attempts;
  guint    mode_passes;
  gboolean smic;
  gboolean calibrated;
  gboolean init_started;
  gboolean release_armed;
  gboolean communication_stopped_fdt;
  gboolean identity_lost;
} Fw9369Data;

typedef enum {
  OP_COMMAND, OP_SFR_WRITE, OP_SFR_READ, OP_WORD_WRITE, OP_WORD_READ,
  OP_UPDATE_WORD, OP_POLL_SFR, OP_POLL_WORD, OP_FDT_READ, OP_FDT_BASE,
  OP_IMAGE_READ,
} FwOperation;

typedef struct
{
  FwOperation kind;
  guint16     address;
  guint16     value;
  guint16     mask;
  guint       delay_ms;
  guint       attempts;
  guint16    *result;
  guint16     source;
} FwStep;

typedef struct
{
  GArray  *steps;
  guint    index;
  guint    phase;
  guint    attempts;
  guint16  value;
  guint8   rx[FTE3600_FW9369_FDT_WRITE_SIZE];
  gboolean failed_transfer;
  gboolean best_effort;
  GError  *error;
} FwScript;

enum { SCRIPT_TRANSFER, SCRIPT_RESULT, SCRIPT_STATES };

static Fw9369Data *
get_data (FpiDeviceFte3600 *self)
{
  return self->backend_data;
}

static void
lose_identity (FpiDeviceFte3600 *self)
{
  Fw9369Data *data = get_data (self);

  data->identity_lost = TRUE;
  data->calibrated = FALSE;
  self->session_failed = TRUE;
  self->armed = FALSE;
  self->idle_verified = FALSE;
}

static void
script_free (FwScript *script)
{
  g_array_unref (script->steps);
  g_clear_error (&script->error);
  fpi_fte3600_secure_clear (script, sizeof *script);
  g_free (script);
}

static void
script_transfer_cb (FpiSpiTransfer *transfer, FpDevice *dev,
                    gpointer user_data, GError *error)
{
  FwScript *script = fpi_ssm_get_data (transfer->ssm);

  if (error && script->best_effort)
    {
      if (!script->error)
        script->error = error;
      else
        g_error_free (error);
      script->failed_transfer = TRUE;
      fpi_ssm_next_state (transfer->ssm);
      return;
    }
  fpi_ssm_spi_transfer_cb (transfer, dev, user_data, error);
}

static guint16
read_word (const guint8 *bytes)
{
  return ((guint16) bytes[0] << 8) | bytes[1];
}

static void
script_run (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  Fw9369Data *data = get_data (self);
  FwScript *script = fpi_ssm_get_data (ssm);
  FwStep *step;
  guint8 tx[FTE3600_FW9369_FDT_WRITE_SIZE];

  g_autoptr(GError) error = NULL;
  gsize length = 0;
  guint16 value;
  FpiSpiTransfer *transfer;

  if (!script->best_effort && fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;
  if (script->index == script->steps->len)
    {
      if (script->error)
        fpi_ssm_mark_failed (ssm, g_steal_pointer (&script->error));
      else
        fpi_ssm_mark_completed (ssm);
      return;
    }
  step = &g_array_index (script->steps, FwStep, script->index);
  if (fpi_ssm_get_cur_state (ssm) == SCRIPT_RESULT)
    {
      value = step->kind == OP_SFR_READ || step->kind == OP_POLL_SFR ?
              script->rx[FTE3600_FW9369_SFR_RESULT_OFFSET] :
              read_word (script->rx + FTE3600_FW9369_WORD_RESULT_OFFSET);
      if (!script->failed_transfer)
        {
          if (step->kind == OP_UPDATE_WORD && script->phase == 0)
            {
              script->value = (value & ~step->mask) | (step->value & step->mask);
              script->phase = 1;
              fpi_ssm_jump_to_state (ssm, SCRIPT_TRANSFER);
              return;
            }
          if (step->kind == OP_UPDATE_WORD && script->phase == 1)
            {
              script->phase = 2;
              fpi_ssm_jump_to_state (ssm, SCRIPT_TRANSFER);
              return;
            }
          if (step->kind == OP_UPDATE_WORD &&
              (value & step->mask) != (script->value & step->mask))
            {
              fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                     FP_DEVICE_ERROR_PROTO, "FW9369 register %04x did not retain configuration",
                                     step->address));
              return;
            }
          if (step->kind == OP_POLL_WORD || step->kind == OP_POLL_SFR)
            {
              if ((value & step->mask) != step->value)
                {
                  if (++script->attempts < step->attempts)
                    {
                      fpi_ssm_jump_to_state_delayed (ssm, SCRIPT_TRANSFER, 1);
                      return;
                    }
                  if (script->error)
                    fpi_ssm_mark_failed (ssm, g_steal_pointer (&script->error));
                  else
                    fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                           FP_DEVICE_ERROR_PROTO, "FW9369 register %04x state %04x did not become %04x",
                                           step->address, value, step->value));
                  return;
                }
            }
          if (step->result)
            *step->result = value;
          if (step->kind == OP_FDT_READ)
            for (guint i = 0; i < FTE3600_FW9369_FDT_CHANNELS; i++)
              data->fdt[i] = read_word (script->rx + 6 + 2 * i);
          if (step->kind == OP_IMAGE_READ &&
              !fpi_fte3600_fw9369_decode_frame (self->capture_rx,
                                                self->capture_frame_size,
                                                data->raw, G_N_ELEMENTS (data->raw),
                                                &error))
            {
              fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
              return;
            }
        }
      script->failed_transfer = FALSE;
      script->index++;
      script->phase = 0;
      script->attempts = 0;
      if (step->delay_ms)
        fpi_ssm_jump_to_state_delayed (ssm, SCRIPT_TRANSFER, step->delay_ms);
      else
        fpi_ssm_jump_to_state (ssm, SCRIPT_TRANSFER);
      return;
    }

  switch (step->kind)
    {
    case OP_COMMAND:
      length = fpi_fte3600_fw9369_build_command (tx, sizeof tx, step->value, &error);
      break;

    case OP_SFR_WRITE:
      length = fpi_fte3600_fw9369_build_sfr_write (tx, sizeof tx, step->address, step->value, &error);
      break;

    case OP_SFR_READ:
    case OP_POLL_SFR:
      length = fpi_fte3600_fw9369_build_sfr_read (tx, sizeof tx, step->address, &error);
      break;

    case OP_WORD_WRITE:
      length = fpi_fte3600_fw9369_build_word_write (tx, sizeof tx, step->address, step->value, &error);
      break;

    case OP_UPDATE_WORD:
      if (script->phase == 1)
        {
          length = fpi_fte3600_fw9369_build_word_write (tx, sizeof tx, step->address, script->value, &error);
          break;
        }
      G_GNUC_FALLTHROUGH;

    case OP_WORD_READ:
    case OP_POLL_WORD:
      length = fpi_fte3600_fw9369_build_word_read (tx, sizeof tx,
                                                   step->kind == OP_UPDATE_WORD && script->phase == 0 ? step->source : step->address,
                                                   &error);
      break;

    case OP_FDT_READ:
      length = fpi_fte3600_fw9369_build_fdt_read (tx, sizeof tx, data->smic, &error);
      break;

    case OP_FDT_BASE:
      length = fpi_fte3600_fw9369_build_fdt_base (tx, sizeof tx, data->smic,
                                                  data->fdt_base, 4, &error);
      break;

    case OP_IMAGE_READ:
      length = self->capture_frame_size;
      break;
    }
  if (!length)
    {
      fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      return;
    }
  transfer = fpi_spi_transfer_new_with_buffer_size (dev, self->spi_fd, self->max_transfer);
  if (step->kind == OP_IMAGE_READ)
    {
      fpi_spi_transfer_write_full (transfer, self->capture_tx, length, NULL);
      fpi_spi_transfer_read_full (transfer, self->capture_rx, length, NULL);
      fpi_spi_transfer_set_sensitive (transfer, TRUE);
    }
  else
    {
      fpi_spi_transfer_write (transfer, length);
      memcpy (transfer->buffer_wr, tx, length);
      memset (script->rx, 0, sizeof script->rx);
      fpi_spi_transfer_read_full (transfer, script->rx, length, NULL);
      if (step->kind == OP_FDT_READ || step->kind == OP_FDT_BASE)
        fpi_spi_transfer_set_sensitive (transfer, TRUE);
    }
  transfer->ssm = ssm;
  fpi_spi_transfer_set_full_duplex (transfer, TRUE);
  fpi_spi_transfer_submit (transfer,
                           script->best_effort ? NULL : fpi_device_get_cancellable (dev),
                           script_transfer_cb, NULL);
}

static FpiSsm *
new_script (FpiDeviceFte3600 *self, const gchar *name, gboolean best_effort)
{
  FpiSsm *ssm = fpi_ssm_new_full (FP_DEVICE (self), script_run, SCRIPT_STATES,
                                  SCRIPT_STATES, name);
  FwScript *script = g_new0 (FwScript, 1);

  script->steps = g_array_new (FALSE, FALSE, sizeof (FwStep));
  script->best_effort = best_effort;
  fpi_ssm_set_data (ssm, script, (GDestroyNotify) script_free);
  return ssm;
}

static void
add_step (FpiSsm *ssm, FwOperation kind, guint16 address, guint16 value,
          guint16 mask, guint delay_ms, guint attempts, guint16 *result)
{
  FwScript *script = fpi_ssm_get_data (ssm);
  FwStep step = { kind, address, value, mask, delay_ms, attempts, result, address };

  g_array_append_val (script->steps, step);
}

static void
command (FpiSsm *ssm, Fte3600Fw9369Command cmd)
{
  add_step (ssm, OP_COMMAND, 0, cmd, 0,
            cmd == FTE3600_FW9369_CMD_WAKE_END ? 0 : COMMAND_DELAY_MS, 0, NULL);
}

static void
word_write (FpiSsm *ssm, guint16 address, guint16 value)
{
  add_step (ssm, OP_WORD_WRITE, address, value, 0, 0, 0, NULL);
}

static void
update_word (FpiSsm *ssm, guint16 address, guint16 mask, guint16 value)
{
  add_step (ssm, OP_UPDATE_WORD, address, value, mask, 0, 0, NULL);
}

static void
sfr_write (FpiSsm *ssm, guint8 address, guint8 value)
{
  add_step (ssm, OP_SFR_WRITE, address, value, 0, 0, 0, NULL);
}

static void
copy_word_field (FpiSsm *ssm, guint16 source, guint16 address,
                 guint16 mask, guint16 value)
{
  FwScript *script = fpi_ssm_get_data (ssm);

  update_word (ssm, address, mask, value);
  g_array_index (script->steps, FwStep, script->steps->len - 1).source = source;
}

static void
wake (FpiSsm *ssm)
{
  command (ssm, FTE3600_FW9369_CMD_WAKE);
  /* C0 is the documented recovery when wake did not reach state 0x50.
   * Issuing it unconditionally also explicitly stops the previous mode. */
  command (ssm, FTE3600_FW9369_CMD_IDLE_1);
  add_step (ssm, OP_POLL_SFR, FTE3600_FW9369_SFR_STATE,
            FTE3600_FW9369_STATE_IDLE, 0xff, 0, MAX_POLL_ATTEMPTS, NULL);
  command (ssm, FTE3600_FW9369_CMD_WAKE_END);
}

static void
scan_rate (FpiSsm *ssm, gboolean image)
{
  update_word (ssm, FTE3600_FW9369_WORD_RATE, 0x3f80, (image ? 9 : 19) << 7);
  update_word (ssm, FTE3600_FW9369_WORD_RATE_A, 0x3fff,
               ((image ? 9 : 19) << 7) | (image ? 3 : 7));
  update_word (ssm, FTE3600_FW9369_WORD_RATE_B, 0x3fff,
               ((image ? 4 : 9) << 7) | (image ? 8 : 17));
}

static void
image_mode (FpiSsm *ssm, Fw9369Data *data)
{
  wake (ssm);
  update_word (ssm, FTE3600_FW9369_WORD_DAC, 0xffff, 0xfc80 | data->image_dac);
  update_word (ssm, FTE3600_FW9369_WORD_SCAN_CONTROL, 0xffff, 0x4ffe);
  update_word (ssm, FTE3600_FW9369_WORD_SAMPLE, 0xffff, 0x27ca);
  scan_rate (ssm, TRUE);
  update_word (ssm, FTE3600_FW9369_WORD_INTEGRATION, 0xffff,
               ((data->smic ? 150 : 200) - 1) * 32 + 1);
  update_word (ssm, FTE3600_FW9369_WORD_CHANNEL, 0xffff, 2);
  update_word (ssm, FTE3600_FW9369_WORD_ANALOG, 0x00f0, 0);
  update_word (ssm, FTE3600_FW9369_WORD_PIXEL_CONTROL, 0x03ff, 0x01fe);
  update_word (ssm, FTE3600_FW9369_WORD_EVENT_MASK,
               FTE3600_FW9369_EVENT_DATA | FTE3600_FW9369_EVENT_AFE,
               FTE3600_FW9369_EVENT_DATA | FTE3600_FW9369_EVENT_AFE);
  sfr_write (ssm, FTE3600_FW9369_SFR_CLOCK, 244);
  sfr_write (ssm, FTE3600_FW9369_SFR_TIMER_ENABLE, 0);
  sfr_write (ssm, FTE3600_FW9369_SFR_TIMER_HIGH, 2000 >> 8);
  sfr_write (ssm, FTE3600_FW9369_SFR_TIMER_LOW, 2000 & 0xff);
  sfr_write (ssm, FTE3600_FW9369_SFR_TIMER_ENABLE, 1);
}

static FpiSsm *
new_image_scan (FpiDeviceFte3600 *self)
{
  FpiSsm *ssm = new_script (self, "FW9369 image scan", FALSE);

  image_mode (ssm, get_data (self));
  word_write (ssm, FTE3600_FW9369_WORD_EVENT_CLEAR, FTE3600_FW9369_EVENT_DATA);
  command (ssm, FTE3600_FW9369_CMD_IMAGE);
  add_step (ssm, OP_POLL_SFR, FTE3600_FW9369_SFR_STATE,
            FTE3600_FW9369_STATE_IMAGE, 0xff, 0, MAX_POLL_ATTEMPTS, NULL);
  /* The scan bit is a trigger, not a configuration field to read back. */
  add_step (ssm, OP_WORD_WRITE, FTE3600_FW9369_WORD_SCAN_CONTROL, 0x4fff,
            0, 1, 0, NULL);
  add_step (ssm, OP_IMAGE_READ, 0, 0, 0, 0, 0, NULL);
  word_write (ssm, FTE3600_FW9369_WORD_EVENT_CLEAR, FTE3600_FW9369_EVENT_DATA);
  return ssm;
}

static void
fdt_mode (FpiSsm *ssm, Fw9369Data *data, gboolean calibrating)
{
  wake (ssm);
  update_word (ssm, FTE3600_FW9369_WORD_DAC, 0xffff, 0xfc80 | data->fdt_dac);
  update_word (ssm, FTE3600_FW9369_WORD_CALIBRATION, 0x07ff, calibrating ? 0 : 0x0600);
  update_word (ssm, FTE3600_FW9369_WORD_FDT_CONTROL, 0xffff, 0x0f8e);
  update_word (ssm, FTE3600_FW9369_WORD_SCAN_CONTROL, 0xffff, 0x07fe);
  update_word (ssm, FTE3600_FW9369_WORD_SAMPLE, 0xffff, 0x27c8);
  update_word (ssm, FTE3600_FW9369_WORD_INTEGRATION, 0xffff, 0x1671);
  update_word (ssm, FTE3600_FW9369_WORD_FDT_INTEGRATION, 0xffff, 0x0801);
  update_word (ssm, FTE3600_FW9369_WORD_CHANNEL, 0x0007, 5);
  scan_rate (ssm, FALSE);
  update_word (ssm, FTE3600_FW9369_WORD_ANALOG, 0x0010, 0);
  copy_word_field (ssm, FTE3600_FW9369_WORD_ANALOG,
                   FTE3600_FW9369_WORD_FDT_ANALOG, 0x03ff, 900);
  update_word (ssm, FTE3600_FW9369_WORD_FDT_FILTER, 0x03fc, 0);
  sfr_write (ssm, FTE3600_FW9369_SFR_BANK_UNLOCK, 0x5a);
  update_word (ssm, 0x00c0, 0x1fff, 0x0444);
  update_word (ssm, 0x00c1, 0x003f, 0x0021);
  update_word (ssm, 0x00c2, 0x00c0, 0x00c0);
  sfr_write (ssm, FTE3600_FW9369_SFR_BANK_UNLOCK, 0);
  update_word (ssm, FTE3600_FW9369_WORD_FDT_THRESHOLDS, 0xffff,
               (FDT_UP_THRESHOLD << 8) | FDT_DOWN_THRESHOLD);
  update_word (ssm, FTE3600_FW9369_WORD_FDT_COUNT, 0x0007, 3);
  update_word (ssm, FTE3600_FW9369_WORD_FDT_ENABLE, 0xffff, 0x00ff);
  update_word (ssm, FTE3600_FW9369_WORD_EVENT_MASK,
               FTE3600_FW9369_EVENT_MANUAL, FTE3600_FW9369_EVENT_MANUAL);
}

static FpiSsm *
new_fdt_sample (FpiDeviceFte3600 *self, gboolean calibrating)
{
  FpiSsm *ssm = new_script (self, "FW9369 FDT sample", FALSE);

  fdt_mode (ssm, get_data (self), calibrating);
  word_write (ssm, FTE3600_FW9369_WORD_EVENT_CLEAR, FTE3600_FW9369_EVENT_MANUAL);
  command (ssm, FTE3600_FW9369_CMD_FDT);
  word_write (ssm, FTE3600_FW9369_WORD_FDT_TRIGGER, 1);
  add_step (ssm, OP_POLL_WORD, FTE3600_FW9369_WORD_EVENTS,
            FTE3600_FW9369_EVENT_MANUAL, FTE3600_FW9369_EVENT_MANUAL, 0, 5, NULL);
  add_step (ssm, OP_FDT_READ, 0, 0, 0, 0, 0, NULL);
  word_write (ssm, FTE3600_FW9369_WORD_EVENT_CLEAR, FTE3600_FW9369_EVENT_MANUAL);
  return ssm;
}

static FpiSsm *
new_arm (FpiDeviceFte3600 *self, gboolean wait_release)
{
  FpiSsm *ssm = new_script (self, "FW9369 finger detection", FALSE);

  fdt_mode (ssm, get_data (self), FALSE);
  sfr_write (ssm, FTE3600_FW9369_SFR_BANK_UNLOCK, 0x5a);
  add_step (ssm, OP_FDT_BASE, 0, 0, 0, 0, 0, NULL);
  sfr_write (ssm, FTE3600_FW9369_SFR_BANK_UNLOCK, 0);
  /* The documented release detector clears bits 0 and 7; bit 1 selects a
   * manual sample and must be clear in either automatic detection mode. */
  update_word (ssm, FTE3600_FW9369_WORD_FDT_CONTROL, 0x0083,
               wait_release ? 0 : 0x0081);
  /* Data/AFE IRQs belong to explicit scans. Wait only for real finger events. */
  update_word (ssm, FTE3600_FW9369_WORD_EVENT_MASK, 0x007f,
               FTE3600_FW9369_EVENT_DOWN | FTE3600_FW9369_EVENT_UP |
               FTE3600_FW9369_EVENT_INVALID);
  word_write (ssm, FTE3600_FW9369_WORD_EVENT_CLEAR, 0x002f);
  command (ssm, FTE3600_FW9369_CMD_FDT);
  return ssm;
}

enum { RESET_COMMAND, RESET_DONE, RESET_STATES };

static void
reset_run (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);

  if (fpi_ssm_get_cur_state (ssm) == RESET_COMMAND)
    {
      FpiSsm *script = new_script (self, "FW9369 stop and verify", TRUE);

      self->armed = FALSE;
      self->idle_verified = FALSE;
      get_data (self)->release_armed = FALSE;
      fpi_fte3600_clear_irq_source (self);
      /* Relock even if a cancelled configuration left the bank open. */
      sfr_write (script, FTE3600_FW9369_SFR_BANK_UNLOCK, 0);
      command (script, FTE3600_FW9369_CMD_IDLE_1);
      add_step (script, OP_POLL_SFR, FTE3600_FW9369_SFR_STATE,
                FTE3600_FW9369_STATE_IDLE, 0xff, 0, MAX_POLL_ATTEMPTS, NULL);
      fpi_ssm_start_subsm (ssm, script);
    }
  else
    {
      self->idle_verified = TRUE;
      fpi_ssm_mark_completed (ssm);
    }
}

static FpiSsm *
create_reset (FpiDeviceFte3600 *self)
{
  return fpi_ssm_new (FP_DEVICE (self), reset_run, RESET_STATES);
}

enum { SHUTDOWN_STOP, SHUTDOWN_MASK, SHUTDOWN_ACK, SHUTDOWN_SLEEP, SHUTDOWN_DONE, SHUTDOWN_STATES };

static void
shutdown_run (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  FpiSsm *script;

  /* Each remaining cleanup stage is attempted, retaining the first error.
   * Successful C1 delivery does not establish a readable sleep-state value. */
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case SHUTDOWN_STOP:
      fpi_ssm_start_subsm (ssm, create_reset (self));
      break;

    case SHUTDOWN_MASK:
      self->idle_verified = FALSE;
      script = new_script (self, "FW9369 disable events", TRUE);
      update_word (script, FTE3600_FW9369_WORD_EVENT_MASK,
                   FTE3600_FW9369_EVENTS_KNOWN, 0);
      fpi_ssm_start_subsm (ssm, script);
      break;

    case SHUTDOWN_ACK:
      script = new_script (self, "FW9369 acknowledge events", TRUE);
      word_write (script, FTE3600_FW9369_WORD_EVENT_CLEAR,
                  FTE3600_FW9369_EVENTS_KNOWN);
      fpi_ssm_start_subsm (ssm, script);
      break;

    case SHUTDOWN_SLEEP:
      script = new_script (self, "FW9369 deep sleep", TRUE);
      command (script, FTE3600_FW9369_CMD_IDLE_2);
      fpi_ssm_start_subsm (ssm, script);
      break;

    case SHUTDOWN_DONE:
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

static FpiSsm *
create_shutdown (FpiDeviceFte3600 *self)
{
  return fpi_ssm_new_full (FP_DEVICE (self), shutdown_run, SHUTDOWN_STATES,
                           SHUTDOWN_MASK, "FW9369 shutdown");
}

typedef struct
{
  guint confirmations;
  guint wakes;
} Communication;

enum { COMM_READ, COMM_CHECK, COMM_WAKE, COMM_SETTLE, COMM_STATES };

static void
communication_run (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  Fw9369Data *data = get_data (self);
  Communication *check = fpi_ssm_get_data (ssm);
  FpiSsm *script;

  if (fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case COMM_READ:
      script = new_script (self, "FW9369 check communication", FALSE);
      add_step (script, OP_SFR_WRITE, FTE3600_FW9369_SFR_SPI_MODE,
                1, 0, FTE3600_SPECIAL_MODE_DELAY_MS, 0, NULL);
      add_step (script, OP_WORD_READ, FTE3600_FW9369_WORD_CHIP_ID,
                0, 0, 0, 0, &data->id);
      fpi_ssm_start_subsm (ssm, script);
      break;

    case COMM_CHECK:
      if (data->id == FTE3600_FW9369_CHIP_ID)
        {
          if (++check->confirmations == 3)
            fpi_ssm_mark_completed (ssm);
          else
            fpi_ssm_jump_to_state (ssm, COMM_READ);
        }
      else if ((data->id != 0 && data->id != 0xffff) ||
               check->wakes == COMMUNICATION_WAKE_ATTEMPTS)
        {
          if (data->id != 0 && data->id != 0xffff)
            lose_identity (self);
          fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                 FP_DEVICE_ERROR_PROTO,
                                 "FW9369 communication identity is %04x", data->id));
        }
      else
        {
          check->confirmations = 0;
          fpi_ssm_next_state (ssm);
        }
      break;

    case COMM_WAKE:
      check->wakes++;
      data->communication_stopped_fdt = TRUE;
      script = new_script (self, "FW9369 restore communication", FALSE);
      wake (script);
      fpi_ssm_start_subsm (ssm, script);
      break;

    case COMM_SETTLE:
      fpi_ssm_jump_to_state_delayed (ssm, COMM_READ, COMMUNICATION_SETTLE_MS);
      break;
    }
}

static FpiSsm *
new_communication (FpiDeviceFte3600 *self)
{
  FpiSsm *ssm = fpi_ssm_new (FP_DEVICE (self), communication_run, COMM_STATES);

  fpi_ssm_set_data (ssm, g_new0 (Communication, 1), g_free);
  return ssm;
}

typedef enum { DAC_READY, DAC_RETRY, DAC_FAILED } DacResult;

static DacResult
adjust_dac (Fw9369Data *data, guint8 *dac, guint sample)
{
  if (sample >= 450 && sample <= 575)
    return DAC_READY;
  if (++data->attempts >= MAX_CALIBRATION_ATTEMPTS)
    return DAC_FAILED;
  if (sample < 450)
    data->low_dac = *dac + 1;
  else
    data->high_dac = *dac - 1;
  if (data->low_dac > data->high_dac)
    return DAC_FAILED;
  *dac = data->low_dac + (data->high_dac - data->low_dac) / 2;
  return DAC_RETRY;
}

static void
start_dac_search (Fw9369Data *data)
{
  data->low_dac = FTE3600_FW9369_DAC_MIN;
  data->high_dac = FTE3600_FW9369_DAC_MAX;
  data->attempts = 0;
  data->stable = 0;
}

enum {
  INIT_FAST_RESET, INIT_SPI, INIT_CHECK_MODE, INIT_READ_ID, INIT_CHECK_SPI,
  INIT_READ_PROCESS, INIT_CHECK_PROCESS,
  INIT_ANALOG, INIT_FDT_SAMPLE, INIT_FDT_ADJUST, INIT_FDT_STABLE_SAMPLE,
  INIT_FDT_STABLE_CHECK, INIT_IMAGE_SAMPLE, INIT_IMAGE_ADJUST,
  INIT_BASE_SAMPLE, INIT_BASE_CHECK, INIT_IDLE, INIT_DONE, INIT_STATES,
};

static void
init_run (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  Fw9369Data *data = get_data (self);
  FpiSsm *script;
  guint average;
  gboolean stable;
  DacResult dac_result;

  if (fpi_ssm_get_cur_state (ssm) < INIT_IDLE &&
      fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case INIT_FAST_RESET:
      self->idle_verified = FALSE;
      data->calibrated = FALSE;
      data->init_started = FALSE;
      data->mode_attempts = 0;
      data->mode_passes = 0;
      /* Cached identity does not imply an awake sensor: shutdown enters C1.
       * Reuse the factory reset before accessing SPI configuration or ID;
       * the full discovery path already establishes communication. */
      if (GPOINTER_TO_UINT (fpi_ssm_get_data (ssm)))
        fpi_ssm_start_subsm (ssm, fpi_fte3600_special_reset_new (self));
      else
        fpi_ssm_next_state (ssm);
      break;

    case INIT_SPI:
      data->init_started = TRUE;
      self->idle_verified = FALSE;
      data->calibrated = FALSE;
      data->image_dac = FTE3600_FW9369_IMAGE_DAC;
      data->fdt_dac = FTE3600_FW9369_FDT_DAC;
      script = new_script (self, "FW9369 transport setup", FALSE);
      add_step (script, OP_SFR_WRITE, FTE3600_FW9369_SFR_SPI_MODE,
                1, 0, FTE3600_SPECIAL_MODE_DELAY_MS, 0, NULL);
      add_step (script, OP_SFR_READ, FTE3600_FW9369_SFR_SPI_MODE,
                0, 0, 0, 0, &data->spi_mode);
      fpi_ssm_start_subsm (ssm, script);
      break;

    case INIT_CHECK_MODE:
      /* Windows FD94 is called twice before the ID read (100D7/10D79).
       * Each call retries 31 times; an exhausted readback is not an ID veto. */
      if (data->spi_mode != 1 &&
          ++data->mode_attempts < FTE3600_SPECIAL_MODE_ATTEMPTS)
        {
          fpi_ssm_jump_to_state (ssm, INIT_SPI);
        }
      else if (++data->mode_passes < FTE3600_SPECIAL_MODE_CONFIG_PASSES)
        {
          data->mode_attempts = 0;
          fpi_ssm_jump_to_state (ssm, INIT_SPI);
        }
      else
        {
          fpi_ssm_next_state (ssm);
        }
      break;

    case INIT_READ_ID:
      script = new_script (self, "FW9369 silicon identity", FALSE);
      add_step (script, OP_WORD_READ, FTE3600_FW9369_WORD_CHIP_ID,
                0, 0, 0, 0, &data->id);
      fpi_ssm_start_subsm (ssm, script);
      break;

    case INIT_CHECK_SPI:
      if (data->id != FTE3600_FW9369_CHIP_ID)
        {
          if (data->id != 0 && data->id != 0xffff &&
              data->id != FTE3600_FW9369_CHIP_ID)
            lose_identity (self);
          fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                 FP_DEVICE_ERROR_PROTO, "FW9369 silicon identity changed"));
          break;
        }
      fpi_ssm_next_state (ssm);
      break;

    case INIT_READ_PROCESS:
      script = new_script (self, "FW9369 process identity", FALSE);
      wake (script);
      add_step (script, OP_SFR_READ, FTE3600_FW9369_SFR_PROCESS,
                0, 0, 0, 0, &data->process);
      word_write (script, FTE3600_FW9369_WORD_EVENT_CLEAR, 0xffff);
      fpi_ssm_start_subsm (ssm, script);
      break;

    case INIT_CHECK_PROCESS:
      data->smic = (data->process >> 2) == 0x13;
      if ((data->process >> 2) != 0 && !data->smic)
        {
          fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                 FP_DEVICE_ERROR_NOT_SUPPORTED, "FW9369 manufacturing process %02x is not established",
                                 data->process));
          break;
        }
      start_dac_search (data);
      fpi_ssm_next_state (ssm);
      break;

    case INIT_ANALOG:
      script = new_script (self, "FW9369 analog setup", FALSE);
      image_mode (script, data);
      fpi_ssm_start_subsm (ssm, script);
      break;

    case INIT_FDT_SAMPLE:
      fpi_ssm_start_subsm (ssm, new_fdt_sample (self, TRUE));
      break;

    case INIT_FDT_ADJUST:
      average = 0;
      for (guint i = 0; i < G_N_ELEMENTS (data->fdt); i++)
        average += data->fdt[i];
      dac_result = adjust_dac (data, &data->fdt_dac, average / 4);
      if (dac_result != DAC_READY)
        {
          if (dac_result == DAC_RETRY)
            fpi_ssm_jump_to_state (ssm, INIT_FDT_SAMPLE);
          else
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                   FP_DEVICE_ERROR_PROTO, "FW9369 touch ADC calibration failed"));
          break;
        }
      data->attempts = data->stable = 0;
      for (guint i = 0; i < G_N_ELEMENTS (data->fdt_base); i++)
        data->fdt_base[i] = G_MAXUINT16;
      fpi_ssm_next_state (ssm);
      break;

    case INIT_FDT_STABLE_SAMPLE:
      fpi_ssm_start_subsm (ssm, new_fdt_sample (self, FALSE));
      break;

    case INIT_FDT_STABLE_CHECK:
      stable = data->attempts != 0;
      for (guint i = 0; i < G_N_ELEMENTS (data->fdt); i++)
        {
          if (data->fdt[i] < 300 || data->fdt[i] > 700)
            {
              fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                     FP_DEVICE_ERROR_PROTO, "FW9369 FDT baseline outside calibrated range"));
              return;
            }
          stable &= ABS ((gint) data->fdt[i] - data->fdt_previous[i]) <= 5;
          data->fdt_previous[i] = data->fdt[i];
          data->fdt_base[i] = MIN (data->fdt_base[i], data->fdt[i]);
        }
      data->stable = stable ? data->stable + 1 : 0;
      if (data->stable < REQUIRED_STABLE_SAMPLES)
        {
          if (++data->attempts >= MAX_STABILITY_ATTEMPTS)
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                   FP_DEVICE_ERROR_PROTO, "FW9369 touch calibration is unstable; clear the sensor and reopen"));
          else
            fpi_ssm_jump_to_state (ssm, INIT_FDT_STABLE_SAMPLE);
          break;
        }
      for (guint i = 0; i < G_N_ELEMENTS (data->fdt_base); i++)
        data->fdt_base[i] -= FDT_BASE_MARGIN;
      start_dac_search (data);
      fpi_ssm_next_state (ssm);
      break;

    case INIT_IMAGE_SAMPLE:
      fpi_ssm_start_subsm (ssm, new_image_scan (self));
      break;

    case INIT_IMAGE_ADJUST:
      dac_result = adjust_dac (data, &data->image_dac,
                               fpi_fte3600_fw9369_image_median (data->raw));
      if (dac_result != DAC_READY)
        {
          if (dac_result == DAC_RETRY)
            fpi_ssm_jump_to_state (ssm, INIT_IMAGE_SAMPLE);
          else
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                   FP_DEVICE_ERROR_PROTO, "FW9369 image ADC calibration failed"));
          break;
        }
      /* Calibration requires an uncovered sensor. Temporal stability below only
       * checks repeatability: it cannot establish that a stationary object is
       * absent. A release event after a later capture must never retroactively
       * be treated as evidence that this initial baseline was empty. */
      memcpy (data->baseline, data->raw, sizeof data->baseline);
      data->stable = data->attempts = 0;
      fpi_ssm_next_state (ssm);
      break;

    case INIT_BASE_SAMPLE:
      fpi_ssm_start_subsm (ssm, new_image_scan (self));
      break;

    case INIT_BASE_CHECK:
      average = 0;
      for (guint i = 0; i < FTE3600_FW9369_PIXELS; i++)
        average += ABS ((gint) data->raw[i] - data->baseline[i]);
      stable = average / FTE3600_FW9369_PIXELS <= 20;
      data->stable = stable ? data->stable + 1 : 0;
      for (guint i = 0; i < FTE3600_FW9369_PIXELS; i++)
        data->baseline[i] = ((guint) data->baseline[i] + data->raw[i]) / 2;
      if (data->stable < REQUIRED_STABLE_SAMPLES)
        {
          if (++data->attempts >= MAX_STABILITY_ATTEMPTS)
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                   FP_DEVICE_ERROR_PROTO, "FW9369 image baseline is unstable; clear the sensor and reopen"));
          else
            fpi_ssm_jump_to_state (ssm, INIT_BASE_SAMPLE);
          break;
        }
      fpi_ssm_next_state (ssm);
      break;

    case INIT_IDLE:
      /* A failed or cancelled wake has not established register access.
       * The reset child has already attempted its final deassertion. */
      if (!data->init_started)
        {
          self->session_failed = TRUE;
          fpi_ssm_jump_to_state (ssm, INIT_DONE);
        }
      else if (data->identity_lost)
        {
          fpi_ssm_jump_to_state (ssm, INIT_DONE);
        }
      else
        {
          fpi_ssm_start_subsm (ssm, create_reset (self));
        }
      break;

    case INIT_DONE:
      data->calibrated = fpi_ssm_get_error (ssm) == NULL;
      fpi_fte3600_secure_clear (data->raw, sizeof data->raw);
      fpi_fte3600_secure_clear (self->capture_rx, self->capture_frame_size);
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

static FpiSsm *
new_init (FpiDeviceFte3600 *self, gboolean reset_from_sleep)
{
  FpiSsm *ssm = fpi_ssm_new_full (FP_DEVICE (self), init_run, INIT_STATES,
                                  INIT_IDLE, "FW9369 initialization");

  fpi_ssm_set_data (ssm, GUINT_TO_POINTER (reset_from_sleep), NULL);
  return ssm;
}

static FpiSsm *
create_init (FpiDeviceFte3600 *self)
{
  return new_init (self, self->fast_open);
}

typedef struct
{
  gboolean wait_release;
  guint    recoveries;
  guint    release_retries;
  guint    release_samples;
  guint    release_sum[FTE3600_FW9369_FDT_CHANNELS];
  guint16  next_fdt_base[FTE3600_FW9369_FDT_CHANNELS];
} Capture;

enum {
  CAPTURE_ARM, CAPTURE_RESTART_FDT, CAPTURE_WAIT, CAPTURE_COMMUNICATION,
  CAPTURE_EVENTS, CAPTURE_DIAGNOSTIC, CAPTURE_DIAGNOSTIC_DONE, CAPTURE_ACK, CAPTURE_CHECK,
  CAPTURE_RECOVER, CAPTURE_RECOVER_DONE,
  CAPTURE_RELEASE_SAMPLE, CAPTURE_RELEASE_CHECK,
  CAPTURE_BASE_BEFORE, CAPTURE_BASE_BEFORE_CHECK, CAPTURE_BASE_IMAGE,
  CAPTURE_BASE_AFTER, CAPTURE_BASE_COMMIT,
  CAPTURE_SCAN, CAPTURE_IMAGE, CAPTURE_IDLE, CAPTURE_REARM_RELEASE,
  CAPTURE_REARM_CHECK, CAPTURE_REARM_CLEANUP, CAPTURE_DONE, CAPTURE_STATES,
};

static void
log_fdt (Fw9369Data *data, const char *reason)
{
  fp_dbg ("FW9369 %s: events %04x, FDT DAC %u, image DAC %u; "
          "raw [%u %u %u %u], base [%u %u %u %u], delta [%d %d %d %d]",
          reason, data->events, data->fdt_dac, data->image_dac,
          data->fdt[0], data->fdt[1], data->fdt[2], data->fdt[3],
          data->fdt_base[0], data->fdt_base[1], data->fdt_base[2], data->fdt_base[3],
          (gint) data->fdt_base[0] - data->fdt[0],
          (gint) data->fdt_base[1] - data->fdt[1],
          (gint) data->fdt_base[2] - data->fdt[2],
          (gint) data->fdt_base[3] - data->fdt[3]);
}

static gboolean
release_sample_valid (Fw9369Data *data)
{
  guint near_base = 0;

  /* 141E4: three manual UP samples; each must have at least three
   * channels within +/-49 of the existing baseline. The default UP
   * classifier requires all four channels (129CC, cfg_init 10BEC). */
  for (guint i = 0; i < FTE3600_FW9369_FDT_CHANNELS; i++)
    {
      gint delta = (gint) data->fdt_base[i] - data->fdt[i];

      if (delta >= FDT_UP_THRESHOLD)
        return FALSE;
      near_base += ABS (delta) <= RELEASE_CHECK_TOLERANCE;
    }
  return near_base >= 3;
}

static gboolean
baseline_sample_clear (Fw9369Data *data, const guint16 *base)
{
  /* Windows brackets the scan with manual DOWN checks (12334). For an
   * asynchronous background update, also retain the existing four-channel
   * UP criterion: one covered channel must not be accepted merely because
   * fewer than four channels satisfy DOWN. No new threshold or I/O. */
  for (guint i = 0; i < FTE3600_FW9369_FDT_CHANNELS; i++)
    {
      gint delta = (gint) base[i] - data->fdt[i];

      if (delta < -FDT_DOWN_THRESHOLD || delta >= FDT_UP_THRESHOLD)
        return FALSE;
    }
  return TRUE;
}

static void
retry_release (FpiSsm *ssm, Fw9369Data *data, Capture *capture)
{
  log_fdt (data, "release recheck rejected");
  fpi_fte3600_secure_clear (data->raw, sizeof data->raw);
  if (++capture->release_retries >= MAX_SPURIOUS_EVENTS)
    fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                           FP_DEVICE_ERROR_PROTO, "FW9369 could not confirm finger release"));
  else
    fpi_ssm_jump_to_state (ssm, CAPTURE_ARM);
}

static guint
update_image_baseline (Fw9369Data *data)
{
  guint8 accept[(FTE3600_FW9369_PIXELS + 7) / 8] = { 0 };
  guint updated = 0;

  /* Normal runtime update (16294): follow upward background drift, retaining
   * the old baseline for negative excursions and isolated large spikes.
   * Compare against an unchanged baseline throughout this pass. */
  for (guint i = 0; i < FTE3600_FW9369_PIXELS; i++)
    {
      guint x = i % FTE3600_FW9369_WIDTH;
      guint y = i / FTE3600_FW9369_WIDTH;
      gint delta = (gint) data->raw[i] - data->baseline[i];

      if (delta <= 0)
        continue;
      if (delta > IMAGE_SPIKE_THRESHOLD && x > 0 && x + 1 < FTE3600_FW9369_WIDTH &&
          y > 0 && y + 1 < FTE3600_FW9369_HEIGHT &&
          (gint) data->raw[i - 1] - data->baseline[i - 1] < IMAGE_NEIGHBOR_THRESHOLD &&
          (gint) data->raw[i + 1] - data->baseline[i + 1] < IMAGE_NEIGHBOR_THRESHOLD &&
          (gint) data->raw[i - FTE3600_FW9369_WIDTH] - data->baseline[i - FTE3600_FW9369_WIDTH] < IMAGE_NEIGHBOR_THRESHOLD &&
          (gint) data->raw[i + FTE3600_FW9369_WIDTH] - data->baseline[i + FTE3600_FW9369_WIDTH] < IMAGE_NEIGHBOR_THRESHOLD)
        continue;
      accept[i / 8] |= 1u << (i % 8);
    }
  for (guint i = 0; i < FTE3600_FW9369_PIXELS; i++)
    if (accept[i / 8] & (1u << (i % 8)))
      {
        data->baseline[i] = data->raw[i];
        updated++;
      }
  return updated;
}

static void
capture_run (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  Fw9369Data *data = get_data (self);
  Capture *capture = fpi_ssm_get_data (ssm);
  gboolean wait_release = capture->wait_release;

  g_autoptr(GError) error = NULL;
  FpiSsm *script;

  if (fpi_ssm_get_cur_state (ssm) < CAPTURE_IDLE &&
      fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CAPTURE_ARM:
      if (!data->calibrated)
        {
          fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                 FP_DEVICE_ERROR_PROTO, "FW9369 has no validated baseline"));
          break;
        }
      self->idle_verified = FALSE;
      data->spurious = 0;
      data->communication_stopped_fdt = FALSE;
      if (wait_release && data->release_armed)
        {
          /* Capture armed this detector before the asynchronous matcher ran.
           * Keep its event latch and bridge IRQ: an UP during matching must
           * not be lost by draining or configuring the detector again. */
          if (!self->armed)
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                   FP_DEVICE_ERROR_PROTO, "FW9369 release detector lost its armed state"));
          else
            fpi_ssm_jump_to_state (ssm, CAPTURE_WAIT);
          break;
        }
      data->release_armed = FALSE;
      if (!fpi_fte3600_drain_irq_events (self, &error))
        {
          fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
          break;
        }
      fpi_ssm_start_subsm (ssm, new_arm (self, wait_release));
      break;

    case CAPTURE_RESTART_FDT:
      if (!data->communication_stopped_fdt)
        {
          fpi_ssm_next_state (ssm);
          break;
        }
      /* Communication recovery used C0, which stopped the detector. Keep
       * its configured DOWN/UP mode and baseline, and restart with C2 only.
       * Do not drain IRQs or clear event latches: a new event may already
       * have arrived after the preceding status read and acknowledge. */
      script = new_script (self, "FW9369 resume finger detection", FALSE);
      command (script, FTE3600_FW9369_CMD_FDT);
      data->communication_stopped_fdt = FALSE;
      fpi_ssm_start_subsm (ssm, script);
      break;

    case CAPTURE_WAIT:
      self->armed = TRUE;
      fpi_fte3600_wait_for_irq (ssm);
      break;

    case CAPTURE_EVENTS:
      script = new_script (self, "FW9369 interrupt status", FALSE);
      add_step (script, OP_WORD_READ, FTE3600_FW9369_WORD_EVENTS,
                0, 0, 0, 0, &data->events);
      fpi_ssm_start_subsm (ssm, script);
      break;

    case CAPTURE_COMMUNICATION:
      fpi_ssm_start_subsm (ssm, new_communication (self));
      break;

    case CAPTURE_DIAGNOSTIC:
      if (!(data->events & FTE3600_FW9369_EVENT_INVALID))
        {
          fpi_ssm_jump_to_state (ssm, CAPTURE_ACK);
          break;
        }
      /* Read the latched detector samples before acknowledgement or any
       * mode change. Four aggregate channels, never fingerprint pixels. */
      data->calibrated = FALSE;
      script = new_script (self, "FW9369 invalid detector snapshot", FALSE);
      add_step (script, OP_FDT_READ, 0, 0, 0, 0, 0, NULL);
      fpi_ssm_start_subsm (ssm, script);
      break;

    case CAPTURE_DIAGNOSTIC_DONE:
      log_fdt (data, "INVALID latched detector");
      fpi_ssm_next_state (ssm);
      break;

    case CAPTURE_ACK:
      script = new_script (self, "FW9369 interrupt acknowledge", FALSE);
      word_write (script, FTE3600_FW9369_WORD_EVENT_CLEAR, data->events);
      fpi_ssm_start_subsm (ssm, script);
      break;

    case CAPTURE_CHECK:
      if (data->events & (FTE3600_FW9369_EVENT_RESET | FTE3600_FW9369_EVENT_ESD |
                          FTE3600_FW9369_EVENT_INVALID))
        {
          data->calibrated = FALSE;
          data->release_armed = FALSE;
          data->communication_stopped_fdt = FALSE;
          self->armed = FALSE;
          self->idle_verified = FALSE;
          if (capture->recoveries == MAX_EVENT_RECOVERIES)
            {
              fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                     FP_DEVICE_ERROR_PROTO,
                                     "FW9369 events %04x persist after %u reinitializations",
                                     data->events, capture->recoveries));
              break;
            }
          capture->recoveries++;
          fp_dbg ("FW9369 events %04x; reinitializing (%u/%u)",
                  data->events, capture->recoveries, MAX_EVENT_RECOVERIES);
          /* Windows query_event_status maps INVALID to the ESD path
           * (103D3 -> 10824). Its IRQ handler waits 5 ms, then calls
           * init_chip (2DE7C -> 2DF4A -> FED8), not merely FDT auto_start.
           * Keep the action and its enrollment progress while recovering. */
          fpi_ssm_jump_to_state_delayed (ssm, CAPTURE_RECOVER, RECOVERY_SETTLE_MS);
          break;
        }
      /* Simultaneous UP and DOWN does not establish event ordering. During
       * release wait, require an unambiguous new UP from the release mode;
       * stale latches were acknowledged before this detector was started. */
      if (wait_release ?
          (data->events & (FTE3600_FW9369_EVENT_DOWN | FTE3600_FW9369_EVENT_UP)) !=
          FTE3600_FW9369_EVENT_UP :
          !(data->events & FTE3600_FW9369_EVENT_DOWN))
        {
          if (++data->spurious >= MAX_SPURIOUS_EVENTS)
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                   FP_DEVICE_ERROR_PROTO, "FW9369 produced repeated unrelated interrupts"));
          else
            fpi_ssm_jump_to_state (ssm, CAPTURE_RESTART_FDT);
          break;
        }
      if (wait_release)
        {
          data->release_armed = FALSE;
          capture->release_samples = 0;
          memset (capture->release_sum, 0, sizeof capture->release_sum);
          fpi_ssm_jump_to_state (ssm, CAPTURE_RELEASE_SAMPLE);
        }
      else
        {
          fpi_device_report_finger_status (dev, FP_FINGER_STATUS_PRESENT);
          fpi_ssm_jump_to_state (ssm, CAPTURE_SCAN);
        }
      break;

    case CAPTURE_RECOVER:
      /* This is a live IRQ recovery, not a reopen from C1. In particular,
      * fast_open must not add its GPIO reset to the vendor recovery path.
      * Reuse initialization, including ID, DAC and baseline validation;
      * transfers retain their ordinary cancellation/generation checks. */
      fpi_ssm_start_subsm (ssm, new_init (self, FALSE));
      break;

    case CAPTURE_RECOVER_DONE:
      data->events = 0;
      fp_dbg ("FW9369 recovery complete; FDT DAC %u, image DAC %u; resuming %s detection",
              data->fdt_dac, data->image_dac, wait_release ? "UP" : "DOWN");
      /* Calibration is not a finger event. A release wait still needs an
       * unambiguous UP; capture must wait for a new DOWN before scanning. */
      fpi_ssm_jump_to_state (ssm, CAPTURE_ARM);
      break;

    case CAPTURE_RELEASE_SAMPLE:
      fpi_ssm_start_subsm (ssm, new_fdt_sample (self, FALSE));
      break;

    case CAPTURE_RELEASE_CHECK:
      if (!release_sample_valid (data))
        {
          retry_release (ssm, data, capture);
          break;
        }
      for (guint i = 0; i < FTE3600_FW9369_FDT_CHANNELS; i++)
        capture->release_sum[i] += data->fdt[i];
      if (++capture->release_samples < RELEASE_CHECK_SAMPLES)
        {
          fpi_ssm_jump_to_state (ssm, CAPTURE_RELEASE_SAMPLE);
          break;
        }
      /* 120CC -> 11A54: max(old base + 30, mean of three) - 30.
       * Stage the result: cancellation or a returning finger must not
       * publish a partially updated pair of detection/image baselines. */
      for (guint i = 0; i < FTE3600_FW9369_FDT_CHANNELS; i++)
        capture->next_fdt_base[i] = MAX ((guint) data->fdt_base[i] + FDT_BASE_MARGIN,
                                        capture->release_sum[i] / RELEASE_CHECK_SAMPLES) - FDT_BASE_MARGIN;
      fpi_ssm_next_state (ssm);
      break;

    case CAPTURE_BASE_BEFORE:
    case CAPTURE_BASE_AFTER:
      fpi_ssm_start_subsm (ssm, new_fdt_sample (self, FALSE));
      break;

    case CAPTURE_BASE_BEFORE_CHECK:
      if (!baseline_sample_clear (data, capture->next_fdt_base))
        retry_release (ssm, data, capture);
      else
        fpi_ssm_next_state (ssm);
      break;

    case CAPTURE_BASE_IMAGE:
      /* Windows' normal UP path brackets its single background scan with
       * manual finger checks (10547..105B6). No DAC search or GPIO reset. */
      fpi_ssm_start_subsm (ssm, new_image_scan (self));
      break;

    case CAPTURE_BASE_COMMIT:
    {
      guint updated;

      if (!baseline_sample_clear (data, capture->next_fdt_base))
        {
          retry_release (ssm, data, capture);
          break;
        }
      memcpy (data->fdt_base, capture->next_fdt_base, sizeof data->fdt_base);
      updated = update_image_baseline (data);
      fp_dbg ("FW9369 release baseline update: %u image pixels", updated);
      log_fdt (data, "release baseline committed");
      fpi_fte3600_secure_clear (data->raw, sizeof data->raw);
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);
      fpi_ssm_jump_to_state (ssm, CAPTURE_IDLE);
      break;
    }

    case CAPTURE_SCAN:
      fpi_ssm_start_subsm (ssm, new_image_scan (self));
      break;

    case CAPTURE_IMAGE:
      fpi_fte3600_clear_captured_image (self);
      self->captured_image = fp_image_new (FTE3600_FW9369_WIDTH, FTE3600_FW9369_HEIGHT);
      self->captured_image->flags |= FPI_IMAGE_PARTIAL;
      if (!fpi_fte3600_fw9369_make_image (data->baseline, data->raw,
                                          FTE3600_FW9369_PIXELS,
                                          self->captured_image->data,
                                          FTE3600_FW9369_PIXELS, &error))
        {
          fpi_fte3600_clear_captured_image (self);
          fpi_ssm_mark_failed (ssm, fpi_device_retry_new_msg (
                                 FP_DEVICE_RETRY_GENERAL, "%s", error->message));
          break;
        }
      fpi_fte3600_secure_clear (data->raw, sizeof data->raw);
      fpi_ssm_next_state (ssm);
      break;

    case CAPTURE_IDLE:
      if (data->identity_lost)
        fpi_ssm_jump_to_state (ssm, CAPTURE_DONE);
      else
        fpi_ssm_start_subsm (ssm, create_reset (self));
      break;

    case CAPTURE_REARM_RELEASE:
      if (wait_release || fpi_ssm_get_error (ssm) ||
          fpi_device_get_current_action (dev) != FPI_DEVICE_ACTION_ENROLL ||
          self->enroll_stages_passed + 1 >= (guint) fp_device_get_nr_enroll_stages (dev))
        {
          fpi_ssm_jump_to_state (ssm, CAPTURE_DONE);
          break;
        }
      /* Match the core's existing terminal-stage convention: successful
       * final acquisition must be idle. Earlier acquisitions preserve UP
       * events across matching; a rejected final sample arms on demand. */
      self->idle_verified = FALSE;
      if (!fpi_fte3600_drain_irq_events (self, &error))
        {
          fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
          break;
        }
      fpi_ssm_start_subsm (ssm, new_arm (self, TRUE));
      break;

    case CAPTURE_REARM_CHECK:
      if (fpi_ssm_get_error (ssm))
        {
          fpi_ssm_next_state (ssm);
        }
      else
        {
          data->release_armed = TRUE;
          self->armed = TRUE;
          fpi_ssm_jump_to_state (ssm, CAPTURE_DONE);
        }
      break;

    case CAPTURE_REARM_CLEANUP:
      fpi_ssm_start_subsm (ssm, create_reset (self));
      break;

    case CAPTURE_DONE:
      if (fpi_ssm_get_error (ssm))
        fpi_fte3600_clear_captured_image (self);
      fpi_fte3600_secure_clear (data->raw, sizeof data->raw);
      fpi_fte3600_secure_clear (self->capture_rx, self->capture_frame_size);
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

static void
capture_free (Capture *capture)
{
  fpi_fte3600_secure_clear (capture, sizeof *capture);
  g_free (capture);
}

static FpiSsm *
new_capture (FpiDeviceFte3600 *self, gboolean wait_release)
{
  FpiSsm *ssm = fpi_ssm_new_full (FP_DEVICE (self), capture_run, CAPTURE_STATES,
                                  CAPTURE_IDLE, wait_release ?
                                  "FW9369 wait for finger release" : "FW9369 capture");
  Capture *capture = g_new0 (Capture, 1);

  capture->wait_release = wait_release;
  fpi_ssm_set_data (ssm, capture, (GDestroyNotify) capture_free);
  return ssm;
}

static FpiSsm *
create_capture (FpiDeviceFte3600 *self)
{
  return new_capture (self, FALSE);
}

static FpiSsm *
create_wait_release (FpiDeviceFte3600 *self)
{
  return new_capture (self, TRUE);
}

static gboolean
prepare_capture (FpiDeviceFte3600 *self, GError **error)
{
  if (self->image_size != FTE3600_FW9369_PIXELS ||
      self->capture_frame_size != FTE3600_FW9369_FRAME_SIZE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "FW9369 requires a 64 by 80 sensor and complete 16-bit frame");
      return FALSE;
    }
  if (!fpi_fte3600_fw9369_build_image_read (self->capture_tx,
                                            self->capture_frame_size, error))
    return FALSE;
  self->backend_data = g_new0 (Fw9369Data, 1);
  return TRUE;
}

static void
destroy (FpiDeviceFte3600 *self)
{
  if (self->backend_data)
    {
      fpi_fte3600_secure_clear (self->backend_data, sizeof (Fw9369Data));
      g_clear_pointer (&self->backend_data, g_free);
    }
}

const Fte3600Backend *
fpi_fte3600_fw9369_backend (void)
{
  static const Fte3600Backend backend = {
    .bytes_per_pixel = FTE3600_FW9369_BYTES_PER_PIXEL,
    .frame_overhead = FTE3600_FW9369_IMAGE_OFFSET,
    .prepare_capture = prepare_capture,
    .destroy = destroy,
    .create_init = create_init,
    .create_capture = create_capture,
    .create_wait_release = create_wait_release,
    .create_reset = create_reset,
    .create_shutdown = create_shutdown,
  };

  return &backend;
}
