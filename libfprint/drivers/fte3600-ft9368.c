/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* FT9368 state machines, independently expressed from protocol observations. */
#define FP_COMPONENT "fte3600"
#include "fte3600-ft9368.h"
#include "fte3600-ft9368-protocol.h"
#include "fte3600-ft9368-update.h"
#include "fte3600-ft9368-timing.h"
#include <string.h>

typedef struct
{
  guint8            rx[FTE3600_FT9368_INFO_SIZE + FTE3600_FT9368_HEADER];
  gboolean          rx_valid;
  gboolean          identity_lost;
  Fte3600Ft9368Info info;
} Ft9368;

enum {
  INIT_WAKE, INIT_UPDATE,
  INIT_START, INIT_START_WAIT, INIT_RESET, INIT_DONE, INIT_NSTATES,
};
enum {
  RESET_WAKE, RESET_WAIT, RESET_INFO1, RESET_CHECK_ID1,
  RESET_INFO2, RESET_CHECK_ID2, RESET_CLEAR,
  RESET_VERIFY, RESET_CHECK, RESET_DONE, RESET_NSTATES,
};
enum {
  CAPTURE_DRAIN, CAPTURE_WAKE, CAPTURE_CLEAR,
  CAPTURE_INFO, CAPTURE_CHECK, CAPTURE_WAIT_IRQ, CAPTURE_IRQ_INFO,
  CAPTURE_IRQ_CHECK, CAPTURE_IMAGE_WAKE,
  CAPTURE_IMAGE, CAPTURE_PROCESS, CAPTURE_CLEANUP, CAPTURE_DONE,
  CAPTURE_NSTATES,
};

static FpiSsm *ft9368_reset_new (FpiDeviceFte3600 *self);

static void
ft9368_read_done (FpiSpiTransfer *transfer, FpDevice *dev,
                  gpointer user_data, GError *error)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  Ft9368 *state = self->backend_data;

  state->rx_valid = error == NULL;
  fpi_ssm_spi_transfer_cb (transfer, dev, user_data, error);
}

static void
ft9368_read (FpiSsm *ssm, guint16 command, gsize length, gboolean cancellable)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  Ft9368 *state = self->backend_data;
  FpiSpiTransfer *transfer;
  gsize frame = length ? length + FTE3600_FT9368_HEADER : 4;

  if (frame > sizeof state->rx || frame > self->max_transfer)
    {
      fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                             FP_DEVICE_ERROR_PROTO, "FT9368 command exceeds its transport buffer"));
      return;
    }
  state->rx_valid = FALSE;
  memset (state->rx, 0, sizeof state->rx);
  transfer = fpi_spi_transfer_new_with_buffer_size (FP_DEVICE (self), self->spi_fd,
                                                    self->max_transfer);
  fpi_spi_transfer_write (transfer, frame);
  fpi_fte3600_ft9368_read (transfer->buffer_wr, frame, command, length);
  fpi_spi_transfer_read_full (transfer, state->rx, frame, NULL);
  fpi_spi_transfer_set_full_duplex (transfer, TRUE);
  fpi_spi_transfer_set_sensitive (transfer, command == FTE3600_FT9368_IMAGE);
  transfer->ssm = ssm;
  fpi_spi_transfer_submit (transfer,
                           cancellable ? fpi_device_get_cancellable (FP_DEVICE (self)) : NULL,
                           ft9368_read_done, NULL);
}

static gboolean
ft9368_check_identity (FpiSsm *ssm)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  Ft9368 *state = self->backend_data;
  const guint8 *info = state->rx + FTE3600_FT9368_HEADER;
  guint16 id = ((guint16) info[19] << 8) | info[20];

  /* Blank responses and a temporarily unhealthy application do not establish
   * a different chip. A positive conflicting identity invalidates the session
   * before even the cleanup protocol may send another family-specific write. */
  if (state->rx_valid && id != 0 && id != 0xffff && id != 0x9368)
    {
      state->identity_lost = TRUE;
      self->session_failed = TRUE;
      self->armed = FALSE;
      self->idle_verified = FALSE;
      fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                             FP_DEVICE_ERROR_PROTO,
                             "FT9368 application identity changed to %04x", id));
      return FALSE;
    }
  return TRUE;
}

static gboolean
ft9368_check_info (FpiSsm *ssm)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  Ft9368 *state = self->backend_data;

  if (!ft9368_check_identity (ssm))
    return FALSE;

  if (!state->rx_valid ||
      !fpi_fte3600_ft9368_parse_info (state->rx + FTE3600_FT9368_HEADER,
                                      FTE3600_FT9368_INFO_SIZE, &state->info))
    {
      fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                             FP_DEVICE_ERROR_PROTO,
                             "FT9368 information does not confirm a healthy 64x80 application"));
      return FALSE;
    }
  return TRUE;
}

enum { WAKE_COMMAND, WAKE_WAIT, WAKE_READ, WAKE_CHECK, WAKE_INFO, WAKE_VALIDATE, WAKE_NSTATES };

static void
ft9368_wake_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  Ft9368 *state = self->backend_data;
  guint *attempts = fpi_ssm_get_data (ssm);

  if (fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case WAKE_COMMAND:
      if (state->identity_lost || self->session_failed)
        {
          fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                 FP_DEVICE_ERROR_PROTO, "FT9368 identity was lost; reopen the device"));
          break;
        }
      (*attempts)++;
      ft9368_read (ssm, FTE3600_FT9368_WAKE, 0, TRUE);
      break;

    case WAKE_WAIT:
      fpi_ssm_next_state_delayed (ssm, FTE3600_FT9368_WAKE_MS);
      break;

    case WAKE_READ:
      ft9368_read (ssm, FTE3600_FT9368_INFO, FTE3600_FT9368_WAKE_CHECK_SIZE, TRUE);
      break;

    case WAKE_CHECK:
      if (state->rx_valid && fpi_fte3600_ft9368_wake_ready (
            state->rx + FTE3600_FT9368_HEADER, FTE3600_FT9368_WAKE_CHECK_SIZE))
        fpi_ssm_next_state (ssm);
      else if (*attempts < FTE3600_FT9368_WAKE_ATTEMPTS)
        fpi_ssm_jump_to_state (ssm, WAKE_COMMAND);
      else
        fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                               FP_DEVICE_ERROR_PROTO, "FT9368 did not respond after three wake attempts"));
      break;

    case WAKE_INFO:
      ft9368_read (ssm, FTE3600_FT9368_INFO, FTE3600_FT9368_INFO_SIZE, TRUE);
      break;

    case WAKE_VALIDATE:
      if (ft9368_check_info (ssm))
        fpi_ssm_mark_completed (ssm);
      break;
    }
}

static FpiSsm *
ft9368_wake_new (FpiDeviceFte3600 *self)
{
  FpiSsm *ssm = fpi_ssm_new (FP_DEVICE (self), ft9368_wake_handler, WAKE_NSTATES);

  fpi_ssm_set_data (ssm, g_new0 (guint, 1), g_free);
  return ssm;
}

static void
ft9368_init_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);

  if (fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case INIT_WAKE:
      fpi_ssm_start_subsm (ssm, ft9368_wake_new (self));
      return;

    case INIT_UPDATE:
      if (g_strcmp0 (g_getenv ("FTE3600_FT9368_UPDATE"), "1") == 0)
        fpi_ssm_start_subsm (ssm, fpi_fte3600_ft9368_update_new (self));
      else
        fpi_ssm_next_state (ssm);
      return;

    case INIT_START:
      ft9368_read (ssm, FTE3600_FT9368_START, 4, TRUE);
      return;

    case INIT_START_WAIT:
      fpi_ssm_next_state_delayed (ssm, FTE3600_FT9368_START_MS);
      return;

    case INIT_RESET:
      fpi_ssm_start_subsm (ssm, ft9368_reset_new (self));
      return;

    case INIT_DONE:
      fpi_ssm_mark_completed (ssm);
      return;

    default:
      g_assert_not_reached ();
    }
}

static void
ft9368_reset_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  Ft9368 *state = self->backend_data;

  if ((state->identity_lost || self->session_failed) &&
      fpi_ssm_get_cur_state (ssm) != RESET_DONE)
    {
      if (fpi_ssm_get_error (ssm))
        fpi_ssm_jump_to_state (ssm, RESET_DONE);
      else
        fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                               FP_DEVICE_ERROR_PROTO, "FT9368 identity was lost; reopen the device"));
      return;
    }

  /* Cleanup is bounded and uncancellable. Its first failure is retained by
   * the SSM; later states may still clear a pending image and verify identity. */
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case RESET_WAKE:
      self->idle_verified = FALSE;
      self->armed = FALSE;
      fpi_fte3600_clear_irq_source (self);
      ft9368_read (ssm, FTE3600_FT9368_WAKE, 0, FALSE);
      return;

    case RESET_WAIT:
      fpi_ssm_next_state_delayed (ssm, FTE3600_FT9368_WAKE_MS);
      return;

    case RESET_INFO1:
    case RESET_INFO2:
      ft9368_read (ssm, FTE3600_FT9368_INFO, FTE3600_FT9368_INFO_SIZE, FALSE);
      return;

    case RESET_CHECK_ID1:
    case RESET_CHECK_ID2:
      if (ft9368_check_identity (ssm))
        fpi_ssm_next_state (ssm);
      return;

    case RESET_CLEAR:
      ft9368_read (ssm, FTE3600_FT9368_IMAGE, FTE3600_FT9368_CLEAR_SIZE, FALSE);
      return;

    case RESET_VERIFY:
      ft9368_read (ssm, FTE3600_FT9368_INFO, FTE3600_FT9368_INFO_SIZE, FALSE);
      return;

    case RESET_CHECK:
      if (ft9368_check_info (ssm))
        {
          g_autoptr(GError) error = NULL;

          if (!fpi_fte3600_drain_irq_events (self, &error))
            {
              fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
            }
          else
            {
              /* Here idle means an acknowledged capture with a responsive
               * application, not the legacy a5/5a MCU state or powered off. */
              self->idle_verified = fpi_ssm_get_error (ssm) == NULL;
              fpi_ssm_mark_completed (ssm);
            }
        }
      return;

    case RESET_DONE:
      fpi_ssm_mark_completed (ssm);
      return;

    default:
      g_assert_not_reached ();
    }
}

static void
ft9368_capture_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  Ft9368 *state = self->backend_data;
  guint step = fpi_ssm_get_cur_state (ssm);

  if (step < CAPTURE_CLEANUP && fpi_fte3600_fail_if_cancelled (ssm, dev))
    return;
  switch (step)
    {
    case CAPTURE_DRAIN:
      {
        g_autoptr(GError) error = NULL;
        self->idle_verified = FALSE;
        fpi_fte3600_clear_captured_image (self);
        if (!fpi_fte3600_drain_irq_events (self, &error))
          fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
        else
          fpi_ssm_next_state (ssm);
        return;
      }

    case CAPTURE_WAKE:
    case CAPTURE_IMAGE_WAKE:
      fpi_ssm_start_subsm (ssm, ft9368_wake_new (self));
      return;

    case CAPTURE_CLEAR:
      ft9368_read (ssm, FTE3600_FT9368_IMAGE, FTE3600_FT9368_CLEAR_SIZE, TRUE);
      return;

    case CAPTURE_INFO:
    case CAPTURE_IRQ_INFO:
      ft9368_read (ssm, FTE3600_FT9368_INFO, FTE3600_FT9368_INFO_SIZE, TRUE);
      return;

    case CAPTURE_CHECK:
    case CAPTURE_IRQ_CHECK:
      if (!ft9368_check_info (ssm))
        return;
      if (state->info.finger_present)
        {
          self->armed = FALSE;
          fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED | FP_FINGER_STATUS_PRESENT);
          fpi_ssm_jump_to_state (ssm, CAPTURE_IMAGE_WAKE);
        }
      else if (step == CAPTURE_IRQ_CHECK && ++self->false_irq_count >= FTE3600_FT9368_MAX_FALSE_IRQS)
        {
          fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                 FP_DEVICE_ERROR_PROTO, "FT9368 repeatedly interrupted without a valid finger status"));
        }
      else
        {
          fpi_ssm_jump_to_state (ssm, CAPTURE_WAIT_IRQ);
        }
      return;

    case CAPTURE_WAIT_IRQ:
      self->armed = TRUE;
      fpi_fte3600_wait_for_irq (ssm);
      return;

    case CAPTURE_IMAGE:
      {
        FpiSpiTransfer *transfer = fpi_spi_transfer_new_with_buffer_size (
          dev, self->spi_fd, self->max_transfer);
        memset (self->capture_rx, 0, self->capture_frame_size);
        fpi_spi_transfer_write_full (transfer, self->capture_tx, self->capture_frame_size, NULL);
        fpi_spi_transfer_read_full (transfer, self->capture_rx, self->capture_frame_size, NULL);
        fpi_spi_transfer_set_full_duplex (transfer, TRUE);
        fpi_spi_transfer_set_sensitive (transfer, TRUE);
        fpi_fte3600_submit_transfer (ssm, transfer, TRUE);
        return;
      }

    case CAPTURE_PROCESS:
      fpi_fte3600_clear_captured_image (self);
      self->captured_image = fp_image_new (self->sensor->width, self->sensor->height);
      self->captured_image->flags |= FPI_IMAGE_PARTIAL;
      memcpy (self->captured_image->data, self->capture_rx + FTE3600_FT9368_HEADER,
              self->image_size);
      fpi_fte3600_secure_clear (self->capture_rx, self->capture_frame_size);
      fpi_ssm_next_state (ssm);
      return;

    case CAPTURE_CLEANUP:
      self->armed = FALSE;
      fpi_fte3600_clear_irq_source (self);
      fpi_fte3600_secure_clear (self->capture_rx, self->capture_frame_size);
      if (state->identity_lost)
        fpi_ssm_jump_to_state (ssm, CAPTURE_DONE);
      else
        fpi_ssm_start_subsm (ssm, ft9368_reset_new (self));
      return;

    case CAPTURE_DONE:
      if (fpi_ssm_get_error (ssm))
        fpi_fte3600_clear_captured_image (self);
      fpi_ssm_mark_completed (ssm);
      return;

    default:
      g_assert_not_reached ();
    }
}

static gboolean
ft9368_prepare (FpiDeviceFte3600 *self, GError **error)
{
  if (self->sensor->width != FTE3600_FT9368_WIDTH ||
      self->sensor->height != FTE3600_FT9368_HEIGHT ||
      self->image_size != FTE3600_FT9368_PIXELS ||
      fpi_fte3600_ft9368_read (self->capture_tx, self->capture_frame_size,
                               FTE3600_FT9368_IMAGE, self->image_size) != self->capture_frame_size)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "FT9368 requires the verified 64x80 byte image frame");
      return FALSE;
    }
  self->backend_data = g_new0 (Ft9368, 1);
  return TRUE;
}

static void
ft9368_destroy (FpiDeviceFte3600 *self)
{
  if (self->backend_data)
    fpi_fte3600_secure_clear (self->backend_data, sizeof (Ft9368));
  g_clear_pointer (&self->backend_data, g_free);
}

static FpiSsm *
ft9368_init_new (FpiDeviceFte3600 *self)
{
  return fpi_ssm_new (FP_DEVICE (self), ft9368_init_handler, INIT_NSTATES);
}

static FpiSsm *
ft9368_capture_new (FpiDeviceFte3600 *self)
{
  return fpi_ssm_new_full (FP_DEVICE (self), ft9368_capture_handler,
                           CAPTURE_NSTATES, CAPTURE_CLEANUP, "FT9368 capture");
}

static FpiSsm *
ft9368_reset_new (FpiDeviceFte3600 *self)
{
  return fpi_ssm_new_full (FP_DEVICE (self), ft9368_reset_handler,
                           RESET_NSTATES, RESET_WAIT, "FT9368 clear capture");
}

const Fte3600Backend *
fpi_fte3600_ft9368_backend (void)
{
  static const Fte3600Backend backend = {
    .bytes_per_pixel = 1,
    .frame_overhead = FTE3600_FT9368_HEADER,
    .prepare_capture = ft9368_prepare,
    .destroy = ft9368_destroy,
    .create_init = ft9368_init_new,
    .create_capture = ft9368_capture_new,
    .create_reset = ft9368_reset_new,
  };

  return &backend;
}
