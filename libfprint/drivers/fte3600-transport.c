/*
 * FocalTech FTE3600 sensor family driver
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "fte3600"

#include "fte3600-private.h"
#include "fte3600-timing.h"

#include <errno.h>
#include <glib-unix.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>

void
fpi_fte3600_secure_clear (gpointer data,
                          gsize    size)
{
  volatile guint8 *bytes = data;

  if (data == NULL)
    return;

  while (size-- > 0)
    *bytes++ = 0;
}

void
fpi_fte3600_clear_irq_source (FpiDeviceFte3600 *self)
{
  self->irq_wait_ssm = NULL;

  if (!self->irq_source)
    return;

  g_source_destroy (self->irq_source);
  g_clear_pointer (&self->irq_source, g_source_unref);
}

void
fpi_fte3600_deassert_hardware_reset_best_effort (FpiDeviceFte3600 *self,
                                                 const gchar      *context)
{
  guint32 asserted = 0;

  if (self->transport_ops)
    {
      g_autoptr(GError) error = NULL;

      if (self->spi_fd >= 0 && !self->transport_ops->set_reset (self, FALSE, &error))
        fp_warn ("Failed to release FTE3600 reset while %s: %s", context, error->message);
      return;
    }
  if (self->spi_fd >= 0 && ioctl (self->spi_fd, FTE3600_IOC_SET_RESET, &asserted) < 0)
    fp_warn ("Failed to release FTE3600 reset while %s: %s", context, g_strerror (errno));
}

void
fpi_fte3600_release_transport (FpiDeviceFte3600 *self)
{
  /* The bridge releases the reset line when the descriptor is closed. */
  self->idle_verified = FALSE;
  self->enroll_needs_release = FALSE;
  self->waiting_for_release = FALSE;
  fpi_fte3600_clear_irq_source (self);
  if (self->transport_ops)
    self->transport_ops->release (self);
  if (self->backend && self->backend->destroy)
    self->backend->destroy (self);
  self->backend_data = NULL;
  fpi_fte3600_secure_clear (self->capture_rx, self->capture_frame_size);
  fpi_fte3600_secure_clear (self->capture_tx, self->capture_frame_size);
}

static gboolean
fte3600_get_irq_events (FpiDeviceFte3600 *self, guint32 *events, GError **error)
{
  if (self->transport_ops)
    return self->transport_ops->get_events (self, events, error);
  if (ioctl (self->spi_fd, FTE3600_IOC_GET_EVENTS, events) == 0)
    return TRUE;
  g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
               "Failed to read FTE3600 IRQ events: %s", g_strerror (errno));
  return FALSE;
}

gboolean
fpi_fte3600_drain_irq_events (FpiDeviceFte3600 *self, GError **error)
{
  guint32 events;

  /* The bridge coalesces interrupts. One atomic exchange drains stale events. */
  return fte3600_get_irq_events (self, &events, error);
}

static gboolean
fte3600_irq_ready_cb (gint fd, GIOCondition condition, gpointer user_data)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (user_data);
  FpiSsm *ssm = self->irq_wait_ssm;

  g_autoptr(GError) error = NULL;
  guint32 events = 0;

  g_assert (ssm != NULL);
  g_assert_cmpint (fd, ==, self->transport_ops ? self->transport_ops->irq_fd (self) : self->spi_fd);
  if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL))
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
                                 "FTE3600 interrupt transport was lost; reopen the device");
  else if (fte3600_get_irq_events (self, &events, &error) && !events)
    return G_SOURCE_CONTINUE;

  self->armed = FALSE;
  self->capture_ready_deadline =
    g_get_monotonic_time () + FTE3600_CAPTURE_READY_TIMEOUT_MS * 1000;
  fpi_fte3600_clear_irq_source (self);
  if (error)
    fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
  else
    fpi_ssm_next_state (ssm);
  return G_SOURCE_REMOVE;
}

void
fpi_fte3600_wait_for_irq (FpiSsm *ssm)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  gint fd;

  g_assert (self->spi_fd >= 0);
  g_assert (self->irq_source == NULL);
  g_assert (self->irq_wait_ssm == NULL);

  fd = self->transport_ops ? self->transport_ops->irq_fd (self) : self->spi_fd;
  if (fd < 0)
    {
      fpi_ssm_mark_failed (ssm, g_error_new_literal (
                             G_IO_ERROR, G_IO_ERROR_CLOSED, "FTE3600 interrupt source is closed"));
      return;
    }
  self->irq_source = g_unix_fd_source_new (
    fd, G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL);
  self->irq_wait_ssm = ssm;
  g_source_set_name (self->irq_source, "FTE3600 finger IRQ");
  g_source_set_callback (self->irq_source,
                         G_SOURCE_FUNC (fte3600_irq_ready_cb), self, NULL);
  g_source_attach (self->irq_source, g_main_context_get_thread_default ());
}

void
fpi_fte3600_submit_transfer (FpiSsm *ssm, FpiSpiTransfer *transfer,
                             gboolean cancellable)
{
  FpDevice *dev = fpi_ssm_get_device (ssm);

  transfer->ssm = ssm;
  fpi_spi_transfer_submit (
    transfer, cancellable ? fpi_device_get_cancellable (dev) : NULL,
    fpi_ssm_spi_transfer_cb, NULL);
}

void
fpi_fte3600_submit_reg_write (FpiSsm *ssm, guint8 reg, guint8 value,
                              gboolean cancellable)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  FpiSpiTransfer *transfer;
  GError *error = NULL;

  transfer = fpi_spi_transfer_new_with_buffer_size (FP_DEVICE (self), self->spi_fd,
                                                    self->max_transfer);
  fpi_spi_transfer_write (transfer, FTE3600_REG_WRITE_SIZE);
  if (!fpi_fte3600_build_app_write (transfer->buffer_wr, FTE3600_REG_WRITE_SIZE,
                                    reg, value, &error))
    {
      fpi_spi_transfer_unref (transfer);
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  fpi_fte3600_submit_transfer (ssm, transfer, cancellable);
}

static void
fte3600_reg_read_cb (FpiSpiTransfer *transfer,
                     FpDevice       *device,
                     gpointer        user_data,
                     GError         *error)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (device);

  self->small_rx_valid = error == NULL;
  fpi_ssm_spi_transfer_cb (transfer, device, user_data, error);
}

void
fpi_fte3600_submit_reg_read (FpiSsm *ssm, guint8 reg, gsize result_len,
                             gboolean cancellable)
{
  FpDevice *device;
  FpiDeviceFte3600 *self;
  FpiSpiTransfer *transfer;
  gsize frame_len = FTE3600_REG_READ_HEADER_SIZE + result_len;
  GError *error = NULL;

  g_return_if_fail (ssm != NULL);

  device = fpi_ssm_get_device (ssm);
  if (G_UNLIKELY (!FPI_IS_DEVICE_FTE3600 (device)))
    {
      fpi_ssm_mark_failed (
        ssm, fpi_device_error_new_msg (
          FP_DEVICE_ERROR_GENERAL,
          "FTE3600 register-read state machine has no valid device"));
      return;
    }
  self = FPI_DEVICE_FTE3600 (device);

  g_assert (result_len > 0);
  g_assert (frame_len <= sizeof (self->small_rx));

  memset (self->small_rx, 0, frame_len);
  self->small_rx_valid = FALSE;
  transfer = fpi_spi_transfer_new_with_buffer_size (FP_DEVICE (self), self->spi_fd,
                                                    self->max_transfer);
  fpi_spi_transfer_write (transfer, frame_len);
  if (!fpi_fte3600_build_app_read (transfer->buffer_wr, frame_len,
                                   reg, result_len, &error))
    {
      fpi_spi_transfer_unref (transfer);
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  fpi_spi_transfer_read_full (transfer, self->small_rx, frame_len, NULL);
  fpi_spi_transfer_set_full_duplex (transfer, TRUE);
  transfer->ssm = ssm;
  fpi_spi_transfer_submit (
    transfer, cancellable ? fpi_device_get_cancellable (device) : NULL,
    fte3600_reg_read_cb, NULL);
}

guint8
fpi_fte3600_read_result_byte (FpiDeviceFte3600 *self)
{
  return self->small_rx[FTE3600_REG_RESULT_OFFSET];
}

gboolean
fpi_fte3600_mcu_is_idle (FpiDeviceFte3600 *self)
{
  return self->small_rx_valid &&
         self->small_rx[FTE3600_REG_RESULT_OFFSET] == FTE3600_MCU_IDLE_HIGH &&
         self->small_rx[FTE3600_REG_RESULT_OFFSET + 1] == FTE3600_MCU_IDLE_LOW;
}

void
fpi_fte3600_set_hardware_reset (FpiSsm           *ssm,
                                FpiDeviceFte3600 *self,
                                gboolean          asserted)
{
  guint32 value = asserted;

  g_assert (self->spi_fd >= 0);
  self->idle_verified = FALSE;
  if (self->transport_ops)
    {
      GError *error = NULL;

      if (!self->transport_ops->set_reset (self, asserted, &error))
        fpi_ssm_mark_failed (ssm, error);
      else
        fpi_ssm_next_state (ssm);
      return;
    }
  if (ioctl (self->spi_fd, FTE3600_IOC_SET_RESET, &value) < 0)
    {
      fpi_ssm_mark_failed (
        ssm, g_error_new (G_IO_ERROR, g_io_error_from_errno (errno),
                          "Failed to %s the FTE3600 hardware reset line: %s",
                          asserted ? "assert" : "deassert",
                          g_strerror (errno)));
      return;
    }

  fpi_ssm_next_state (ssm);
}

gboolean
fpi_fte3600_fail_if_cancelled (FpiSsm *ssm, FpDevice *dev)
{
  GCancellable *cancellable;
  GError *error = NULL;

  if (!fpi_device_action_is_cancelled (dev))
    return FALSE;

  cancellable = fpi_device_get_cancellable (dev);
  if (!cancellable ||
      !g_cancellable_set_error_if_cancelled (cancellable, &error))
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                 "Fingerprint operation was cancelled");

  fpi_ssm_mark_failed (ssm, error);
  return TRUE;
}

gboolean
fpi_fte3600_configure_spi (FpiDeviceFte3600 *self, GError **error)
{
  struct fte3600_bridge_info info = { 0 };

  if (self->transport_ops)
    return self->transport_ops->configure (self, error);
  if (ioctl (self->spi_fd, FTE3600_IOC_GET_INFO, &info) < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Cannot query FTE3600 resource bridge: %s", g_strerror (errno));
      return FALSE;
    }
  if (info.abi_version != FTE3600_BRIDGE_ABI || info.bits_per_word != 8 ||
      (info.mode & ~SPI_CS_HIGH) != SPI_MODE_0 ||
      !info.speed_hz || info.speed_hz > FTE3600_SPI_SPEED_HZ ||
      info.max_transfer < FTE3600_COMMAND_MAX_SIZE ||
      info.max_transfer > FTE3600_BRIDGE_MAX_TRANSFER)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "FTE3600 bridge ABI or SPI resources are incompatible with the FTE3600 transport");
      return FALSE;
    }
  self->max_transfer = info.max_transfer;
  self->spi_mode = info.mode;
  self->bridge_capabilities = info.capabilities;
  return TRUE;
}

gboolean
fpi_fte3600_set_cs_polarity (FpiDeviceFte3600 *self, gboolean active_high, GError **error)
{
  guint32 value = !!active_high;

  if (!!(self->spi_mode & SPI_CS_HIGH) == value)
    return TRUE;
  if (!(self->bridge_capabilities & FTE3600_BRIDGE_CAP_CS_POLARITY))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "This FTE3600 bridge cannot negotiate chip-select polarity");
      return FALSE;
    }
  if (ioctl (self->spi_fd, FTE3600_IOC_SET_CS_POLARITY, &value) < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Cannot configure FTE3600 chip-select polarity: %s", g_strerror (errno));
      return FALSE;
    }
  self->spi_mode = (self->spi_mode & ~SPI_CS_HIGH) | (value ? SPI_CS_HIGH : 0);
  return TRUE;
}
