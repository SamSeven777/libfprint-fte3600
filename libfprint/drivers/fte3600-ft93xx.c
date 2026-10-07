/*
 * FT9365 / FT9769 image acquisition
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Hardware facts are documented in docs/fte3600/ft93xx-protocol.md. The host
 * exposure search and image qualification below are independently designed;
 * no startup image is retained as an empty-sensor baseline.
 */
#define FP_COMPONENT "fte3600"
#include "fte3600-private.h"
#include "fte3600-ft93xx.h"
#include "fte3600-ft93xx-protocol.h"
#include "fte3600-ft93xx-timing.h"

typedef struct
{
  const Fte3600Ft93xxProfile *profile;
  guint16                     chip_id;
  guint8                      dac;
  guint16                    *pixels;
} Ft93xx;

typedef struct
{
  guint16  address;
  guint16  value;
  guint16  mask;
  gboolean sfr;
} RegisterSetting;

typedef struct
{
  guint8   tx[FT93XX_TRANSFER_MAX];
  guint8   rx[FT93XX_TRANSFER_MAX];
  gsize    length;
  gsize    offset;
  gsize    chunk;
  guint    attempts;
  guint    mode_attempts;
  guint    mode_pass;
  gint64   deadline;
  guint16  value;
  guint    position;
  GArray  *settings;
  gboolean calibration_only;
  gboolean wait_release;
  guint    empty_frames;
  guint    frame_poll_ms;
  guint    low_dac;
  guint    high_dac;
} Machine;

static void
machine_free (gpointer data)
{
  Machine *machine = data;

  g_clear_pointer (&machine->settings, g_array_unref);
  fpi_fte3600_secure_clear (machine, sizeof *machine);
  g_free (machine);
}

static FpiSsm *
machine_new (FpiDeviceFte3600 *self, FpiSsmHandlerCallback handler,
             guint states, guint cleanup, const gchar *name)
{
  FpiSsm *ssm = fpi_ssm_new_full (FP_DEVICE (self), handler, states, cleanup, name);

  fpi_ssm_set_data (ssm, g_new0 (Machine, 1), machine_free);
  return ssm;
}

static void
fail_protocol (FpiSsm *ssm, const gchar *message)
{
  fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                      "FT93xx: %s", message));
}

static void
submit (FpiSsm *ssm, gboolean read, gboolean cancellable)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  Machine *machine = fpi_ssm_get_data (ssm);
  FpiSpiTransfer *transfer;

  g_assert (machine->length > 0 && machine->length <= sizeof machine->tx);
  transfer = fpi_spi_transfer_new_with_buffer_size (FP_DEVICE (self), self->spi_fd,
                                                    self->max_transfer);
  fpi_spi_transfer_write (transfer, machine->length);
  memcpy (transfer->buffer_wr, machine->tx, machine->length);
  if (read)
    {
      memset (machine->rx, 0, sizeof machine->rx);
      fpi_spi_transfer_read_full (transfer, machine->rx, machine->length, NULL);
      fpi_spi_transfer_set_full_duplex (transfer, TRUE);
    }
  fpi_spi_transfer_set_sensitive (transfer, TRUE);
  fpi_fte3600_submit_transfer (ssm, transfer, cancellable);
}

static void
command (FpiSsm *ssm, Fte3600Ft93xxCommand command, gboolean cancellable)
{
  Machine *machine = fpi_ssm_get_data (ssm);

  machine->length = fpi_fte3600_ft93xx_command (machine->tx, sizeof machine->tx,
                                                command, NULL);
  submit (ssm, FALSE, cancellable);
}

static void
read_register (FpiSsm *ssm, guint16 address, gboolean sfr, gboolean cancellable)
{
  Machine *machine = fpi_ssm_get_data (ssm);

  machine->length = sfr ?
                    fpi_fte3600_ft93xx_read8 (machine->tx, sizeof machine->tx, address, NULL) :
                    fpi_fte3600_ft93xx_read16 (machine->tx, sizeof machine->tx, address, NULL);
  submit (ssm, TRUE, cancellable);
}

static void
write_register (FpiSsm *ssm, guint16 address, guint16 value,
                gboolean sfr, gboolean cancellable)
{
  Machine *machine = fpi_ssm_get_data (ssm);

  machine->length = sfr ?
                    fpi_fte3600_ft93xx_write8 (machine->tx, sizeof machine->tx, address, value, NULL) :
                    fpi_fte3600_ft93xx_write16 (machine->tx, sizeof machine->tx, address, value, NULL);
  submit (ssm, FALSE, cancellable);
}

static gboolean
result16 (FpiSsm *ssm, guint16 *value)
{
  Machine *machine = fpi_ssm_get_data (ssm);
  GError *error = NULL;

  if (!fpi_fte3600_ft93xx_read16_result (machine->rx, machine->length, value, &error))
    {
      fpi_ssm_mark_failed (ssm, error);
      return FALSE;
    }
  return TRUE;
}

enum { RESET_KEY, RESET_WAKE, RESET_WAKE_WAIT, RESET_IDLE, RESET_IDLE_WAIT,
       RESET_READ, RESET_CHECK, RESET_CLEAR, RESET_DONE, RESET_STATES };

static void
reset_run (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  Machine *machine = fpi_ssm_get_data (ssm);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case RESET_KEY:
      self->armed = FALSE;
      self->idle_verified = FALSE;
      write_register (ssm, 0x9a, 0, TRUE, FALSE);
      break;

    case RESET_WAKE:
      self->armed = FALSE;
      self->idle_verified = FALSE;
      command (ssm, FT93XX_COMMAND_WAKE, FALSE);
      break;

    case RESET_WAKE_WAIT:
    case RESET_IDLE_WAIT:
      fpi_ssm_next_state_delayed (ssm, FT93XX_COMMAND_SETTLE_MS);
      break;

    case RESET_IDLE:
      command (ssm, FT93XX_COMMAND_IDLE, FALSE);
      break;

    case RESET_READ:
      read_register (ssm, 0x80, TRUE, FALSE);
      break;

    case RESET_CHECK:
      if (machine->rx[4] == 0x50)
        fpi_ssm_next_state (ssm);
      else if (++machine->attempts < FT93XX_IDLE_ATTEMPTS)
        fpi_ssm_jump_to_state (ssm, RESET_IDLE);
      else
        fail_protocol (ssm, "sensor did not enter idle");
      break;

    case RESET_CLEAR:
      write_register (ssm, FT93XX_REG_INTERRUPT_CLEAR, 0xffff, FALSE, FALSE);
      break;

    case RESET_DONE:
      self->idle_verified = fpi_ssm_get_error (ssm) == NULL;
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

static FpiSsm *
create_reset (FpiDeviceFte3600 *self)
{
  return machine_new (self, reset_run, RESET_STATES, RESET_WAKE, "FT93xx idle");
}

static void
add_setting (Machine *machine, guint16 address, guint16 value, guint16 mask,
             gboolean sfr)
{
  RegisterSetting setting = { address, value, mask, sfr };

  g_array_append_val (machine->settings, setting);
}

/* These individually described hardware fields are not a vendor initialization
 * blob. Read/modify/write preserves all undocumented bits in AFE registers. */
static void
build_settings (Machine *machine, const Ft93xx *chip)
{
  gboolean wide_dac = chip->profile->sensor == FTE3600_SENSOR_FT9769;

  machine->settings = g_array_new (FALSE, FALSE, sizeof (RegisterSetting));

  if (!wide_dac)
    add_setting (machine, 0x1a8e, 2, 0x0007, FALSE); /* I/O drive strength */
  add_setting (machine, 0x1808, wide_dac ? 0x084b : 0x0807, 0xffff, FALSE);
  add_setting (machine, 0x1815, 0, 0xffff, FALSE);
  add_setting (machine, 0x180d, 1000, 0xffff, FALSE);

  /* AFE channel configuration uses the same protected register window as the
   * reference's cold path; it is independent of its finger-detection bases. */
  add_setting (machine, 0x9a, 0x5a, 0xff, TRUE);
  if (wide_dac)
    {
      const guint channels[] = { 3, 9, 15, 20 };
      for (guint i = 0; i < G_N_ELEMENTS (channels); i++)
        {
          add_setting (machine, 0x00c0 + 2 * channels[i], 0x0103, 0x1fff, FALSE);
          add_setting (machine, 0x00c1 + 2 * channels[i], 0, 0x1fff, FALSE);
        }
    }
  else
    {
      add_setting (machine, 0x00c0, 0x0444, 0x1fff, FALSE);
      add_setting (machine, 0x00c1, 0x0021, 0x003f, FALSE);
      add_setting (machine, 0x00c2, 0x00c0, 0x00c0, FALSE);
    }
  add_setting (machine, 0x9a, 0, 0xff, TRUE);

  /* Four MHz AFE scan timing: delay=4, reset=0, charge=1, sample=2/4. */
  add_setting (machine, 0x1806, 0x023b, 0xffff, FALSE);
  add_setting (machine, 0x180a, 0x0001, 0xffff, FALSE);
  add_setting (machine, 0x180b, 0x0104, 0xffff, FALSE);
  /* Disable baseline subtraction. No unverified startup image becomes a base. */
  add_setting (machine, 0x1812, 0x0100, 0x7fff, FALSE);
  add_setting (machine, 0x1800, chip->profile->scan_window, 0xffff, FALSE);
  add_setting (machine, 0x1816, chip->profile->scan_extension, 0xffff, FALSE);
  add_setting (machine, 0x1807, 0x0fef, 0xffff, FALSE); /* 128 integrations; sample field 15 */
  add_setting (machine, 0x1804, 0x27ca, 0xffff, FALSE); /* image sample mode */
  add_setting (machine, 0x1811, 0x01fe, 0xffff, FALSE);
  add_setting (machine, 0x1a83, 0x0060, 0x0060, FALSE); /* data-ready / AFE interrupts */
  add_setting (machine, 0x8e, 244, 0xff, TRUE); /* 100 ms scaled by 10000/4096 */
  add_setting (machine, 0x8d, 97, 0xff, TRUE); /* 5 ms scaled by 10000/512 */
  add_setting (machine, 0x8f, 0xff, 0xff, TRUE);
  add_setting (machine, 0x90, 0, 0xff, TRUE);
  add_setting (machine, 0x91, 0x07, 0xff, TRUE);
  add_setting (machine, 0x92, 0xd0, 0xff, TRUE);
  add_setting (machine, 0x90, 1, 0xff, TRUE);
  add_setting (machine, 0x1a06, 1, 0xffff, FALSE);
}

enum { SCAN_IDLE, SCAN_WAKE, SCAN_WAKE_WAIT, SCAN_DAC, SCAN_WINDOW, SCAN_CLEAR,
       SCAN_COMMAND, SCAN_COMMAND_WAIT, SCAN_MODE, SCAN_CHECK_MODE,
       SCAN_TRIGGER, SCAN_STATUS, SCAN_CHECK_STATUS, SCAN_FIFO, SCAN_COPY,
       SCAN_DECODE, SCAN_CLEANUP, SCAN_STATES };

static void
scan_run (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  Ft93xx *chip = self->backend_data;
  Machine *machine = fpi_ssm_get_data (ssm);
  guint16 value;
  GError *error = NULL;
  guint state = fpi_ssm_get_cur_state (ssm);

  if (state < SCAN_CLEANUP && fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;
  switch (state)
    {
    case SCAN_IDLE:
      fpi_ssm_start_subsm (ssm, create_reset (self));
      break;

    case SCAN_WAKE:
      self->idle_verified = FALSE;
      command (ssm, FT93XX_COMMAND_WAKE, TRUE);
      break;

    case SCAN_WAKE_WAIT:
      fpi_ssm_next_state_delayed (ssm, FT93XX_COMMAND_SETTLE_MS);
      break;

    case SCAN_DAC:
      value = 0x3000 | ((guint16) chip->profile->gain << 8);
      if (chip->profile->sensor == FTE3600_SENSOR_FT9769)
        value = 0xf000 | ((guint16) chip->profile->gain << 8);
      value |= chip->dac;
      write_register (ssm, 0x1801, value, FALSE, TRUE);
      break;

    case SCAN_CLEAR:
      write_register (ssm, FT93XX_REG_INTERRUPT_CLEAR, 0xffff, FALSE, TRUE);
      break;

    case SCAN_WINDOW:
      /* Clear the trigger bit before each scan, as well as at initialization;
       * its post-scan self-clearing behavior is not assumed. */
      write_register (ssm, 0x1800, chip->profile->scan_window, FALSE, TRUE);
      break;

    case SCAN_COMMAND:
      machine->deadline = g_get_monotonic_time () + FT93XX_SCAN_TIMEOUT_MS * G_TIME_SPAN_MILLISECOND;
      command (ssm, FT93XX_COMMAND_SCAN_IMAGE, TRUE);
      break;

    case SCAN_COMMAND_WAIT:
      fpi_ssm_next_state_delayed (ssm, FT93XX_COMMAND_SETTLE_MS);
      break;

    case SCAN_MODE:
      read_register (ssm, 0x80, TRUE, TRUE);
      break;

    case SCAN_CHECK_MODE:
      if (machine->rx[4] == 0x54)
        fpi_ssm_next_state (ssm);
      else if (++machine->attempts < FT93XX_SCAN_MODE_ATTEMPTS)
        fpi_ssm_jump_to_state_delayed (ssm, SCAN_MODE, FT93XX_COMMAND_SETTLE_MS);
      else
        fail_protocol (ssm, "image scan mode was not acknowledged");
      break;

    case SCAN_TRIGGER:
      write_register (ssm, 0x1800, chip->profile->scan_window | 1, FALSE, TRUE);
      break;

    case SCAN_STATUS:
      read_register (ssm, FT93XX_REG_INTERRUPT_STATUS, FALSE, TRUE);
      break;

    case SCAN_CHECK_STATUS:
      if (!result16 (ssm, &value))
        break;
      if (value & FT93XX_INTERRUPT_FAULT)
        fail_protocol (ssm, "sensor reported reset, ESD or open/short fault");
      else if (value & FT93XX_INTERRUPT_IMAGE_READY)
        fpi_ssm_next_state (ssm);
      else if (g_get_monotonic_time () < machine->deadline)
        fpi_ssm_jump_to_state_delayed (ssm, SCAN_STATUS, FT93XX_COMMAND_SETTLE_MS);
      else
        fpi_ssm_mark_failed (ssm, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                                       "FT93xx image scan timed out"));
      break;

    case SCAN_FIFO:
      machine->chunk = MIN ((gsize) FT93XX_FIFO_PAYLOAD_MAX,
                            self->capture_frame_size - machine->offset);
      machine->length = fpi_fte3600_ft93xx_fifo_read (machine->tx, sizeof machine->tx,
                                                      machine->chunk, &error);
      if (!machine->length)
        fpi_ssm_mark_failed (ssm, error);
      else
        submit (ssm, TRUE, TRUE);
      break;

    case SCAN_COPY:
      memcpy (self->capture_rx + machine->offset, machine->rx + 6, machine->chunk);
      machine->offset += machine->chunk;
      fpi_fte3600_secure_clear (machine->rx, sizeof machine->rx);
      if (machine->offset < self->capture_frame_size)
        fpi_ssm_jump_to_state (ssm, SCAN_FIFO);
      else
        fpi_ssm_next_state (ssm);
      break;

    case SCAN_DECODE:
      if (!fpi_fte3600_ft93xx_decode (chip->profile, chip->chip_id,
                                      self->capture_rx, self->capture_frame_size,
                                      chip->pixels, self->image_size, &error))
        fpi_ssm_mark_failed (ssm, error);
      else
        fpi_ssm_mark_completed (ssm);
      fpi_fte3600_secure_clear (self->capture_rx, self->capture_frame_size);
      break;

    case SCAN_CLEANUP:
      fpi_fte3600_secure_clear (self->capture_rx, self->capture_frame_size);
      fpi_ssm_start_subsm (ssm, create_reset (self));
      break;
    }
}

static FpiSsm *
create_scan (FpiDeviceFte3600 *self)
{
  return machine_new (self, scan_run, SCAN_STATES, SCAN_CLEANUP, "FT93xx raw image");
}

static guint16
quantile (const guint *histogram, gsize count, guint numerator, guint denominator)
{
  gsize target = count * numerator / denominator;
  gsize sum = 0;

  for (guint i = 0; i < 1024; i++)
    {
      sum += histogram[i];
      if (sum > target)
        return i * 4;
    }
  return 4092;
}

enum { IMAGE_BEGIN, IMAGE_SCAN, IMAGE_PROCESS, IMAGE_WAIT, IMAGE_CLEANUP, IMAGE_STATES };

static void
image_run (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  Ft93xx *chip = self->backend_data;
  Machine *machine = fpi_ssm_get_data (ssm);

  if (fpi_ssm_get_cur_state (ssm) < IMAGE_CLEANUP &&
      fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case IMAGE_BEGIN:
      machine->low_dac = 1;
      machine->high_dac = chip->profile->maximum_dac - 1;
      machine->frame_poll_ms = FT93XX_FRAME_POLL_MS;
      fpi_ssm_next_state (ssm);
      break;

    case IMAGE_SCAN:
      fpi_ssm_start_subsm (ssm, create_scan (self));
      break;

    case IMAGE_PROCESS:
      {
        guint histogram[1024] = { 0 };
        guint16 median, lower, upper;
        guint64 gradients = 0;
        for (gsize i = 0; i < self->image_size; i++)
          histogram[chip->pixels[i] >> 2]++;
        median = quantile (histogram, self->image_size, 1, 2);

        /* Increasing SDAC increases the digitized pixel level. Search the
         * documented 10-bit operating region with a fixed iteration budget. */
        if (median < 3200 || median > 3680)
          {
            guint next;
            if (++machine->attempts > FT93XX_EXPOSURE_ADJUSTMENTS)
              {
                fail_protocol (ssm, "ADC exposure did not converge");
                break;
              }
            if (median < 3200)
              machine->low_dac = chip->dac + 1;
            else
              machine->high_dac = chip->dac ? chip->dac - 1 : 0;
            if (machine->low_dac > machine->high_dac)
              {
                fail_protocol (ssm, "ADC exposure is outside the controllable range");
                break;
              }
            next = machine->low_dac + (machine->high_dac - machine->low_dac) / 2;
            chip->dac = next;
            fpi_ssm_jump_to_state (ssm, IMAGE_SCAN);
            break;
          }
        if (machine->calibration_only)
          {
            fpi_fte3600_secure_clear (chip->pixels, self->image_size * sizeof *chip->pixels);
            fpi_ssm_mark_completed (ssm);
            break;
          }

        lower = quantile (histogram, self->image_size, 1, 100);
        upper = quantile (histogram, self->image_size, 99, 100);
        for (gsize i = 1; i < self->image_size; i++)
          if (i % chip->profile->width)
            gradients += ABS ((gint) chip->pixels[i] - (gint) chip->pixels[i - 1]);
        /* This is image qualification, not biometric authentication or liveness
         * detection. Reject uniform ADC levels and wait for useful texture. */
        if (upper - lower < 128 || gradients < self->image_size * 8)
          {
            fpi_fte3600_secure_clear (chip->pixels, self->image_size * sizeof *chip->pixels);
            /* An unsuitable capture is not necessarily an empty sensor.
             * Require both absence-of-texture tests, consecutively, before
             * the release path can accept this weaker image-based evidence. */
            if (machine->wait_release)
              machine->empty_frames = upper - lower < 128 && gradients < self->image_size * 8 ?
                                      machine->empty_frames + 1 : 0;
            if (machine->wait_release && machine->empty_frames >= FT93XX_RELEASE_EMPTY_FRAMES)
              {
                /* This establishes consecutive empty-image evidence, not a
                 * hardware contact bit or a liveness result. Each scan has
                 * already restored and read back the sensor's idle state. */
                fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);
                fpi_ssm_mark_completed (ssm);
                break;
              }
            if (!machine->wait_release)
              fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);
            fpi_ssm_next_state (ssm);
            break;
          }
        if (machine->wait_release)
          {
            machine->empty_frames = 0;
            fpi_device_report_finger_status (dev, FP_FINGER_STATUS_PRESENT);
            fpi_fte3600_secure_clear (chip->pixels, self->image_size * sizeof *chip->pixels);
            fpi_ssm_next_state (ssm);
            break;
          }
        fpi_fte3600_clear_captured_image (self);
        self->captured_image = fp_image_new (chip->profile->width, chip->profile->height);
        /* Physical resolution is unmeasured for these profiles. Preserve
         * FpImage's unknown/default metadata rather than borrowing FT9361. */
        for (gsize i = 0; i < self->image_size; i++)
          {
            guint16 sample = CLAMP (chip->pixels[i], lower, upper);
            self->captured_image->data[i] = (sample - lower) * 255 / (upper - lower);
          }
        fpi_fte3600_secure_clear (chip->pixels, self->image_size * sizeof *chip->pixels);
        fpi_device_report_finger_status (dev, FP_FINGER_STATUS_PRESENT);
        fpi_ssm_mark_completed (ssm);
        break;
      }

    case IMAGE_WAIT:
      {
        guint delay_ms = machine->wait_release ? FT93XX_FRAME_POLL_MS : machine->frame_poll_ms;

        machine->attempts = 0;
        machine->low_dac = 1;
        machine->high_dac = chip->profile->maximum_dac - 1;
        /* Host backoff limits empty capture traffic; it is not a hardware
         * sleep command or a temperature model. Release debounce has an
         * independent fixed cadence and does not inherit this backoff. */
        if (!machine->wait_release)
          machine->frame_poll_ms = MIN (machine->frame_poll_ms * 2, FT93XX_FRAME_POLL_MAX_MS);
        fpi_ssm_jump_to_state_delayed (ssm, IMAGE_SCAN, delay_ms);
        break;
      }

    case IMAGE_CLEANUP:
      fpi_fte3600_secure_clear (chip->pixels, self->image_size * sizeof *chip->pixels);
      /* The core invalidates idle before dispatch. Cancellation can arrive
       * before the first scan, whose child normally performs this cleanup.
       * Establish idle here in that case; do not repeat a verified reset. */
      if (!self->idle_verified)
        fpi_ssm_start_subsm (ssm, create_reset (self));
      else
        fpi_ssm_next_state (ssm);
      break;
    }
}

static FpiSsm *
create_image (FpiDeviceFte3600 *self, gboolean calibration_only)
{
  FpiSsm *ssm = machine_new (self, image_run, IMAGE_STATES, IMAGE_CLEANUP,
                             "FT93xx exposure and image");
  Machine *machine = fpi_ssm_get_data (ssm);

  machine->calibration_only = calibration_only;
  return ssm;
}

enum { INIT_PAD_KEY, INIT_PAD_VOLTAGE, INIT_PAD_WAIT, INIT_PAD_READ, INIT_PAD_CHECK,
       INIT_MODE, INIT_MODE_WAIT, INIT_MODE_READ, INIT_MODE_CHECK,
       INIT_ID, INIT_CHECK_ID, INIT_VARIANT, INIT_CHECK_VARIANT,
       INIT_IDLE, INIT_WAKE, INIT_WAKE_WAIT, INIT_BUILD, INIT_SETTING_READ,
       INIT_SETTING_PREPARE, INIT_SETTING_WRITE, INIT_SETTING_VERIFY,
       INIT_SETTING_CHECK, INIT_CALIBRATE, INIT_CLEANUP, INIT_STATES };

static void
init_run (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  Ft93xx *chip = self->backend_data;
  Machine *machine = fpi_ssm_get_data (ssm);
  RegisterSetting *setting;
  guint16 value;
  guint state = fpi_ssm_get_cur_state (ssm);

  if (state < INIT_CLEANUP && fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;
  switch (state)
    {
    case INIT_PAD_KEY:
      self->idle_verified = FALSE;
      write_register (ssm, 0xfd, 0x0a, TRUE, TRUE);
      break;

    case INIT_PAD_VOLTAGE:
      write_register (ssm, 0xfe, 0x7f, TRUE, TRUE);
      break;

    case INIT_PAD_WAIT:
      fpi_ssm_next_state_delayed (ssm, FT93XX_COMMAND_SETTLE_MS);
      break;

    case INIT_PAD_READ:
      read_register (ssm, 0xfe, TRUE, TRUE);
      break;

    case INIT_PAD_CHECK:
      if (machine->rx[4] == 0x7f)
        fpi_ssm_next_state (ssm);
      else
        fail_protocol (ssm, "chip I/O voltage mode was not acknowledged");
      break;

    case INIT_MODE:
      self->idle_verified = FALSE;
      write_register (ssm, 0xc6, 1, TRUE, TRUE);
      break;

    case INIT_MODE_WAIT:
      fpi_ssm_next_state_delayed (ssm, FT93XX_SFR_PROTOCOL_SETTLE_MS);
      break;

    case INIT_MODE_READ:
      read_register (ssm, 0xc6, TRUE, TRUE);
      break;

    case INIT_MODE_CHECK:
      /* Windows 195D0 allows four attempts and its callers continue to ID
       * validation even when readback never acknowledges the mode. */
      if (machine->rx[4] != 1 && ++machine->mode_attempts < FT93XX_SFR_PROTOCOL_ATTEMPTS)
        fpi_ssm_jump_to_state (ssm, INIT_MODE);
      else if (machine->mode_pass == 0)
        fpi_ssm_next_state (ssm);
      else
        fpi_ssm_jump_to_state (ssm, INIT_VARIANT);
      break;

    case INIT_ID:
      read_register (ssm, FT93XX_REG_CHIP_ID, FALSE, TRUE);
      break;

    case INIT_CHECK_ID:
      if (!result16 (ssm, &chip->chip_id))
        break;
      if (!fpi_fte3600_ft93xx_matches (chip->profile->sensor, chip->chip_id, 0) ||
          chip->chip_id != self->identity.response)
        {
          fail_protocol (ssm, "silicon identity changed during initialization");
        }
      else
        {
          /* The initialization caller repeats 195D0 after its probe (19717). */
          machine->mode_pass = 1;
          machine->mode_attempts = 0;
          fpi_ssm_jump_to_state (ssm, INIT_MODE);
        }
      break;

    case INIT_VARIANT:
      read_register (ssm, FT93XX_REG_VARIANT, FALSE, TRUE);
      break;

    case INIT_CHECK_VARIANT:
      if (!result16 (ssm, &value))
        break;
      if (!fpi_fte3600_ft93xx_matches (chip->profile->sensor, chip->chip_id, value))
        fail_protocol (ssm, "FT9395 silicon is not an FT9769 capture profile");
      else
        fpi_ssm_next_state (ssm);
      break;

    case INIT_IDLE:
      fpi_ssm_start_subsm (ssm, create_reset (self));
      break;

    case INIT_WAKE:
      self->idle_verified = FALSE;
      command (ssm, FT93XX_COMMAND_WAKE, TRUE);
      break;

    case INIT_WAKE_WAIT:
      fpi_ssm_next_state_delayed (ssm, FT93XX_COMMAND_SETTLE_MS);
      break;

    case INIT_BUILD:
      build_settings (machine, chip);
      fpi_ssm_next_state (ssm);
      break;

    case INIT_SETTING_READ:
      if (machine->position == machine->settings->len)
        {
          fpi_ssm_jump_to_state (ssm, INIT_CALIBRATE);
          break;
        }
      setting = &g_array_index (machine->settings, RegisterSetting, machine->position);
      if (setting->sfr || setting->mask == 0xffff)
        {
          machine->value = setting->value;
          fpi_ssm_jump_to_state (ssm, INIT_SETTING_WRITE);
        }
      else
        {
          read_register (ssm, setting->address, FALSE, TRUE);
        }
      break;

    case INIT_SETTING_PREPARE:
      setting = &g_array_index (machine->settings, RegisterSetting, machine->position);
      if (result16 (ssm, &value))
        {
          machine->value = (value & ~setting->mask) | (setting->value & setting->mask);
          fpi_ssm_next_state (ssm);
        }
      break;

    case INIT_SETTING_WRITE:
      setting = &g_array_index (machine->settings, RegisterSetting, machine->position);
      write_register (ssm, setting->address, machine->value, setting->sfr, TRUE);
      break;

    case INIT_SETTING_VERIFY:
      setting = &g_array_index (machine->settings, RegisterSetting, machine->position);
      /* SFR watchdog control and the protected-register key are strobes. */
      if (setting->sfr)
        fpi_ssm_jump_to_state (ssm, INIT_SETTING_CHECK);
      else
        read_register (ssm, setting->address, FALSE, TRUE);
      break;

    case INIT_SETTING_CHECK:
      setting = &g_array_index (machine->settings, RegisterSetting, machine->position);
      if (!setting->sfr)
        {
          if (!result16 (ssm, &value))
            break;
          if ((value & setting->mask) != (machine->value & setting->mask))
            {
              fail_protocol (ssm, "AFE register failed readback verification");
              break;
            }
        }
      machine->position++;
      fpi_ssm_jump_to_state (ssm, INIT_SETTING_READ);
      break;

    case INIT_CALIBRATE:
      fpi_ssm_start_subsm (ssm, create_image (self, TRUE));
      break;

    case INIT_CLEANUP:
      fpi_ssm_start_subsm (ssm, create_reset (self));
      break;
    }
}

static FpiSsm *
create_init (FpiDeviceFte3600 *self)
{
  return machine_new (self, init_run, INIT_STATES, INIT_CLEANUP, "FT93xx initialize");
}

static FpiSsm *
create_capture (FpiDeviceFte3600 *self)
{
  return create_image (self, FALSE);
}

static FpiSsm *
create_wait_release (FpiDeviceFte3600 *self)
{
  FpiSsm *ssm = create_image (self, FALSE);
  Machine *machine = fpi_ssm_get_data (ssm);

  machine->wait_release = TRUE;
  return ssm;
}

static gboolean
prepare_capture (FpiDeviceFte3600 *self, GError **error)
{
  const Fte3600Ft93xxProfile *profile = fpi_fte3600_ft93xx_profile (self->sensor->sensor);
  Ft93xx *chip;

  if (!profile || self->identity.evidence != FTE3600_IDENTITY_SPECIAL_CHIP_ID ||
      !fpi_fte3600_ft93xx_matches (profile->sensor, self->identity.response, 0) ||
      self->image_size != (gsize) profile->width * profile->height ||
      self->capture_frame_size != 2 * (gsize) profile->width *
      (profile->height + profile->extra_rows))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "FT93xx requires positive silicon identity and exact geometry");
      return FALSE;
    }
  chip = g_new0 (Ft93xx, 1);
  chip->profile = profile;
  chip->chip_id = self->identity.response;
  chip->dac = profile->initial_dac;
  chip->pixels = g_new0 (guint16, self->image_size);
  self->backend_data = chip;
  return TRUE;
}

static void
destroy (FpiDeviceFte3600 *self)
{
  Ft93xx *chip = self->backend_data;

  if (!chip)
    return;
  fpi_fte3600_secure_clear (chip->pixels, self->image_size * sizeof *chip->pixels);
  g_free (chip->pixels);
  fpi_fte3600_secure_clear (chip, sizeof *chip);
  g_free (chip);
  self->backend_data = NULL;
}

static const Fte3600Backend ft9365_backend = {
  .bytes_per_pixel = 2,
  .required_transfer_size = FT93XX_TRANSFER_MAX,
  .prepare_capture = prepare_capture,
  .destroy = destroy,
  .create_init = create_init,
  .create_capture = create_capture,
  .create_wait_release = create_wait_release,
  .create_reset = create_reset,
};
static const Fte3600Backend ft9769_backend = {
  .bytes_per_pixel = 2,
  .frame_overhead = 40 * 4 * 2,
  .required_transfer_size = FT93XX_TRANSFER_MAX,
  .prepare_capture = prepare_capture,
  .destroy = destroy,
  .create_init = create_init,
  .create_capture = create_capture,
  .create_wait_release = create_wait_release,
  .create_reset = create_reset,
};

const Fte3600Backend *
fpi_fte3600_ft93xx_backend (Fte3600Sensor sensor)
{
  switch (sensor)
    {
    case FTE3600_SENSOR_FT9365: return &ft9365_backend;

    case FTE3600_SENSOR_FT9769: return &ft9769_backend;

    case FTE3600_SENSOR_UNKNOWN:
    case FTE3600_SENSOR_FT9338:
    case FTE3600_SENSOR_FT9348:
    case FTE3600_SENSOR_FT9361:
    case FTE3600_SENSOR_FT9536:
    case FTE3600_SENSOR_FT9368:
    case FTE3600_SENSOR_FT9369:
    case FTE3600_SENSOR_COUNT:
    default: return NULL;
    }
}
