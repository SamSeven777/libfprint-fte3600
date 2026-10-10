/*
 * FocalTech FTE3600 sensor family driver
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "fte3600"

#include "fte3600-private.h"
#include "fte3600-timing.h"

#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

void
fpi_fte3600_secure_clear (gpointer data,
                          gsize    size)
{
  if (data == NULL || size == 0)
    return;

  explicit_bzero (data, size);
}

void
fpi_fte3600_clear_irq_source (FpiDeviceFte3600 *self)
{
  self->irq_wait_ssm = NULL;

  if (self->irq_source)
    {
      g_source_destroy (self->irq_source);
      g_clear_pointer (&self->irq_source, g_source_unref);
    }
  if (self->irq_guard_source)
    {
      g_source_destroy (self->irq_guard_source);
      g_clear_pointer (&self->irq_guard_source, g_source_unref);
    }
}

gboolean
fpi_fte3600_transport_check (FpDevice *device, GError **error)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (device);

  /* Used by the SPI worker. Configuration is changed only between completed
   * transfers; resource metadata remains immutable until the last worker ends. */
  if (self->spi_configuration_invalid)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
                           "FTE3600 SPI configuration is unverified; reopen the device");
      return FALSE;
    }
  return fpi_fte3600_resources_check (&self->resources, error);
}

static gboolean
write_reset_value (FpiDeviceFte3600 *self, gboolean asserted, GError **error)
{
  struct gpio_v2_line_values value = { .mask = 1, .bits = !!asserted };

  if (ioctl (self->reset_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &value) < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Cannot %s FTE3600 reset: %s", asserted ? "assert" : "release",
                   g_strerror (errno));
      return FALSE;
    }
  return TRUE;
}

static gboolean
set_reset_value (FpiDeviceFte3600 *self, gboolean asserted, GError **error)
{
  return fpi_fte3600_resources_check (&self->resources, error) &&
         write_reset_value (self, asserted, error) &&
         fpi_fte3600_resources_check (&self->resources, error);
}

static gboolean
release_reset_for_sync (FpDevice *device, GError **error)
{
  /* The SPI worker has validated the message and session. Its next operation
   * is the prepared SPI ioctl, followed by the normal completion checks. */
  return write_reset_value (FPI_DEVICE_FTE3600 (device), FALSE, error);
}

void
fpi_fte3600_release_reset_and_sync (FpiSsm *ssm)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  FpiSpiTransfer *transfer;

  self->idle_verified = FALSE;
  transfer = fpi_spi_transfer_new_with_buffer_size (FP_DEVICE (self), self->spi_fd,
                                                    self->max_transfer);
  fpi_spi_transfer_write (transfer, FTE3600_BOOT_SYNC_SIZE);
  fpi_fte3600_build_command (transfer->buffer_wr, FTE3600_BOOT_SYNC_SIZE,
                             FTE3600_COMMAND_BOOT_SYNC, NULL);
  fpi_spi_transfer_read (transfer, FTE3600_BOOT_SYNC_SIZE);
  fpi_spi_transfer_set_full_duplex (transfer, TRUE);
  fpi_spi_transfer_set_sensitive (transfer, TRUE);
  fpi_spi_transfer_set_prepare (transfer, release_reset_for_sync);
  /* Finish the pulse/handshake once reset is asserted. The next SSM state
   * observes cancellation; no main-context work runs between release/sync. */
  fpi_fte3600_submit_transfer (ssm, transfer, FALSE);
}

void
fpi_fte3600_deassert_hardware_reset_best_effort (FpiDeviceFte3600 *self,
                                                 const gchar      *context)
{
  g_autoptr(GError) error = NULL;

  if (self->reset_fd >= 0 && !set_reset_value (self, FALSE, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE))
    fp_warn ("Failed to release FTE3600 reset while %s: %s", context, error->message);
}

void
fpi_fte3600_release_transport (FpiDeviceFte3600 *self)
{
  /* Resource descriptors are closed by transport_close, after protocol I/O. */
  self->idle_verified = FALSE;
  self->enroll_needs_release = FALSE;
  self->waiting_for_release = FALSE;
  fpi_fte3600_clear_irq_source (self);
  if (self->backend && self->backend->destroy)
    self->backend->destroy (self);
  self->backend_data = NULL;
  fpi_fte3600_secure_clear (self->capture_rx, self->capture_frame_size);
  fpi_fte3600_secure_clear (self->capture_tx, self->capture_frame_size);
}

static gboolean
fte3600_get_irq_events (FpiDeviceFte3600 *self, guint32 *events, GError **error)
{
  guint32 event_counter;

  *events = 0;
  if (!fpi_fte3600_transport_check (FP_DEVICE (self), error))
    return FALSE;
  /* Bound draining even when a disconnected sensor generates an IRQ storm. */
  for (guint i = 0; i < 1024; i++)
    {
      ssize_t size = read (self->irq_fd, &event_counter, sizeof (event_counter));

      if (size < 0 && errno == EINTR)
        continue;
      if (size < 0 && errno == EAGAIN)
        return fpi_fte3600_transport_check (FP_DEVICE (self), error);
      if (size < 0)
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                       "Cannot read FTE3600 UIO interrupt counter: %s", g_strerror (errno));
          return FALSE;
        }
      if (size != sizeof (event_counter))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                               "Invalid FTE3600 UIO interrupt counter");
          return FALSE;
        }
      /* UIO counters may coalesce edges and wrap to zero. Any complete read
       * means a notification; the backend validates actual sensor state. */
      *events = 1;
    }
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                       "FTE3600 UIO interrupt queue did not become idle");
  return FALSE;
}

gboolean
fpi_fte3600_drain_irq_events (FpiDeviceFte3600 *self, GError **error)
{
  guint32 events;

  /* Events are wakeups only: each backend verifies sensor state afterwards. */
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
  g_assert_cmpint (fd, ==, self->irq_fd);
  if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL))
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
                                 "FTE3600 IRQ companion disappeared; reopen the device");
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

static gboolean
fte3600_irq_guard_cb (gpointer user_data)
{
  FpiDeviceFte3600 *self = user_data;
  FpiSsm *ssm = self->irq_wait_ssm;

  g_autoptr(GError) error = NULL;

  if (fpi_fte3600_transport_check (FP_DEVICE (self), &error))
    return G_SOURCE_CONTINUE;
  self->armed = FALSE;
  fpi_fte3600_clear_irq_source (self);
  fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
  return G_SOURCE_REMOVE;
}

void
fpi_fte3600_wait_for_irq (FpiSsm *ssm)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  gint fd;
  GError *error = NULL;

  g_assert (self->spi_fd >= 0);
  g_assert (self->irq_source == NULL);
  g_assert (self->irq_wait_ssm == NULL);

  if (!fpi_fte3600_transport_check (FP_DEVICE (self), &error))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  fd = self->irq_fd;
  self->irq_source = g_unix_fd_source_new (
    fd, G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL);
  self->irq_wait_ssm = ssm;
  g_source_set_name (self->irq_source, "FTE3600 finger IRQ");
  g_source_set_callback (self->irq_source,
                         G_SOURCE_FUNC (fte3600_irq_ready_cb), self, NULL);
  g_source_attach (self->irq_source, g_main_context_get_thread_default ());
  self->irq_guard_source = g_timeout_source_new (200);
  g_source_set_name (self->irq_guard_source, "FTE3600 ACPI session guard");
  g_source_set_callback (self->irq_guard_source, fte3600_irq_guard_cb, self, NULL);
  g_source_attach (self->irq_guard_source, g_main_context_get_thread_default ());
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
  /* Only callers with a bounded fallback/poll loop opt in. A failed read
   * never exposes stale bytes as a valid reply. Lifecycle errors propagate. */
  if (GPOINTER_TO_INT (user_data) &&
      (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED) ||
       g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)))
    {
      fp_dbg ("Retryable register read: %s", error->message);
      g_clear_error (&error);
    }
  fpi_ssm_spi_transfer_cb (transfer, device, user_data, error);
}

static void
submit_reg_read (FpiSsm *ssm, guint8 reg, gsize result_len,
                 gboolean cancellable, gboolean allow_unavailable)
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
    fte3600_reg_read_cb, GINT_TO_POINTER (allow_unavailable));
}

void
fpi_fte3600_submit_reg_read (FpiSsm *ssm, guint8 reg, gsize result_len,
                             gboolean cancellable)
{
  submit_reg_read (ssm, reg, result_len, cancellable, FALSE);
}

void
fpi_fte3600_try_reg_read (FpiSsm *ssm, guint8 reg, gsize result_len,
                          gboolean cancellable)
{
  submit_reg_read (ssm, reg, result_len, cancellable, TRUE);
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
  GError *error = NULL;

  g_assert (self->spi_fd >= 0);
  self->idle_verified = FALSE;
  if (!set_reset_value (self, asserted, &error))
    {
      fpi_ssm_mark_failed (ssm, error);
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

static gboolean
spi_configuration (FpiDeviceFte3600 *self, guint32 mode, guint8 bits,
                   guint32 speed, GError **error)
{
  guint32 read_mode, read_speed;
  guint8 read_bits;

  if ((self->resources.cs_control &&
       ioctl (self->spi_fd, SPI_IOC_WR_MODE32, &mode) < 0) ||
      ioctl (self->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
      ioctl (self->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0 ||
      ioctl (self->spi_fd, SPI_IOC_RD_MODE32, &read_mode) < 0 ||
      ioctl (self->spi_fd, SPI_IOC_RD_BITS_PER_WORD, &read_bits) < 0 ||
      ioctl (self->spi_fd, SPI_IOC_RD_MAX_SPEED_HZ, &read_speed) < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Cannot configure FTE3600 spidev: %s", g_strerror (errno));
      return FALSE;
    }
  if (read_mode != mode || read_bits != bits || read_speed != speed)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "The SPI controller did not retain the requested configuration");
      return FALSE;
    }
  self->spi_mode = mode;
  return TRUE;
}

static gint
open_character_device (const gchar *path, gint flags, struct stat *st, GError **error)
{
  gint fd;

  if (!path || !g_path_is_absolute (path))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "FTE3600 requires absolute SPI, GPIO and UIO device paths");
      return -1;
    }
  fd = open (path, flags | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Cannot open %s: %s", path, g_strerror (errno));
      return -1;
    }
  if (fstat (fd, st) < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Cannot inspect %s: %s", path, g_strerror (errno));
      close (fd);
      return -1;
    }
  if (!S_ISCHR (st->st_mode))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "%s is not a character device", path);
      close (fd);
      return -1;
    }
  return fd;
}

gboolean
fpi_fte3600_transport_open (FpiDeviceFte3600 *self, GError **error)
{
  FpDevice *device = FP_DEVICE (self);
  const gchar *spi_path = fpi_device_get_udev_data (device, FPI_DEVICE_UDEV_SUBTYPE_FTE3600);
  const gchar *gpio_path = fpi_device_get_udev_data (device, FPI_DEVICE_UDEV_SUBTYPE_GPIO);
  const gchar *irq_path = fpi_device_get_udev_data (device, FPI_DEVICE_UDEV_SUBTYPE_UIO);
  struct stat spi_stat, gpio_stat, irq_stat, opened_irq_stat;
  struct gpiochip_info info = { 0 };
  struct gpio_v2_line_request reset = {
    .offsets = { 0 }, .num_lines = 1, .consumer = "libfprint-fte3600-reset",
    .config = {
      .flags = GPIO_V2_LINE_FLAG_OUTPUT | GPIO_V2_LINE_FLAG_ACTIVE_LOW,
      .num_attrs = 1,
      .attrs = {{ .attr = { .id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES, .values = 0 }, .mask = 1 }},
    },
  };
  gint gpio_fd = -1, irq_path_fd = -1;
  guint32 speed, mode;

  g_return_val_if_fail (self->spi_fd < 0 && self->reset_fd < 0 && self->irq_fd < 0, FALSE);
  self->spi_fd = open_character_device (spi_path, O_RDWR, &spi_stat, error);
  if (self->spi_fd < 0)
    return FALSE;
  /* Cooperative exclusion. Stock spidev still permits non-cooperating users. */
  if (flock (self->spi_fd, LOCK_EX | LOCK_NB) < 0)
    goto system_error;
  gpio_fd = open_character_device (gpio_path, O_RDONLY, &gpio_stat, error);
  if (gpio_fd < 0)
    goto failed;
  /* O_PATH does not invoke UIO open or acquire an interrupt. Validate its
   * identity before acquiring the reset lease required for the real open. */
  irq_path_fd = open_character_device (irq_path, O_PATH, &irq_stat, error);
  if (irq_path_fd < 0)
    goto failed;
  if (!fpi_fte3600_resources_resolve ("/sys", spi_stat.st_rdev, gpio_stat.st_rdev, irq_stat.st_rdev,
                                      &self->resources, error) ||
      !fpi_fte3600_resources_buffer_size ("/sys", &self->max_transfer, error))
    goto failed;
  if (ioctl (gpio_fd, GPIO_GET_CHIPINFO_IOCTL, &info) < 0)
    goto system_error;
  if (info.lines != 1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "FTE3600 GPIO companion must expose exactly one reset line");
      goto failed;
    }
  if (ioctl (gpio_fd, GPIO_V2_GET_LINE_IOCTL, &reset) < 0)
    goto system_error;
  self->reset_fd = reset.fd;
  self->irq_fd = open_character_device (irq_path, O_RDONLY | O_NONBLOCK, &opened_irq_stat, error);
  if (self->irq_fd < 0)
    goto failed;
  if (opened_irq_stat.st_rdev != irq_stat.st_rdev)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "FTE3600 IRQ node changed while opening");
      goto failed;
    }
  close (irq_path_fd);
  irq_path_fd = -1;
  if (close (gpio_fd) < 0)
    {
      gpio_fd = -1;
      goto system_error;
    }
  gpio_fd = -1;
  if (!fpi_fte3600_transport_check (device, error))
    goto failed;
  if (ioctl (self->spi_fd, SPI_IOC_RD_BITS_PER_WORD, &self->original_bits) < 0 ||
      ioctl (self->spi_fd, SPI_IOC_RD_MAX_SPEED_HZ, &self->original_speed) < 0)
    goto system_error;
  if (self->resources.cs_control)
    {
      /* Native CS can recover an ACPI baseline after a killed trial session. */
      mode = self->resources.acpi_mode;
    }
  else
    {
      /* Stock spidev hides CS_HIGH for GPIO CS and forces it on WR_MODE32.
       * Do not write mode here or on close: retain the controller's effective
       * polarity, including while spi_setup applies word length and speed. */
      if (ioctl (self->spi_fd, SPI_IOC_RD_MODE32, &mode) < 0)
        goto system_error;
      if ((mode & ~SPI_CS_HIGH) != SPI_MODE_0)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                               "FTE3600 controller-managed CS requires an existing SPI mode 0 configuration");
          goto failed;
        }
    }
  self->spi_mode = mode;
  self->spi_configured = TRUE;
  self->spi_configuration_invalid = FALSE;
  speed = self->resources.acpi_speed_hz;
  if (self->original_speed)
    speed = MIN (self->original_speed, speed);
  if (!speed)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "FTE3600 spidev has no valid transfer speed");
      goto failed;
    }
  if (!spi_configuration (self, mode, 8, speed, error) ||
      !fpi_fte3600_transport_check (device, error))
    goto failed;
  self->transport_capabilities = self->resources.cs_control ? FTE3600_TRANSPORT_CAP_CS_POLARITY : 0;
  fpi_spi_transfer_set_device_guard (device, fpi_fte3600_transport_check);
  return TRUE;

system_error:
  g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
               "Cannot acquire FTE3600 spidev/reset/UIO resources: %s", g_strerror (errno));
failed:
  if (irq_path_fd >= 0)
    close (irq_path_fd);
  if (gpio_fd >= 0)
    close (gpio_fd);
  {
    g_autoptr(GError) cleanup = NULL;
    if (!fpi_fte3600_transport_close (self, &cleanup) &&
        !g_error_matches (cleanup, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE))
      fp_warn ("FTE3600 transport cleanup also failed: %s", cleanup->message);
  }
  return FALSE;
}

static void
remember_error (GError **first, GError *next)
{
  if (!*first)
    *first = next;
  else
    g_clear_error (&next);
}

static gboolean
restore_spi_configuration (FpiDeviceFte3600 *self, GError **error)
{
  guint32 expected_mode = self->resources.cs_control ? self->resources.acpi_mode : self->spi_mode;
  guint32 mode = expected_mode;
  guint32 speed = self->original_speed;
  guint8 bits = self->original_bits;
  const gulong operations[] = { SPI_IOC_WR_MODE32, SPI_IOC_WR_BITS_PER_WORD,
                                SPI_IOC_WR_MAX_SPEED_HZ };
  gpointer values[] = { &mode, &bits, &speed };
  GError *first = NULL;

  /* Attempt each independent restoration even if an earlier setting failed. */
  for (guint i = self->resources.cs_control ? 0 : 1; i < G_N_ELEMENTS (operations); i++)
    if (ioctl (self->spi_fd, operations[i], values[i]) < 0)
      remember_error (&first, g_error_new (G_IO_ERROR, g_io_error_from_errno (errno),
                                           "Cannot restore FTE3600 SPI configuration: %s", g_strerror (errno)));
  if (ioctl (self->spi_fd, SPI_IOC_RD_MODE32, &mode) < 0 ||
      ioctl (self->spi_fd, SPI_IOC_RD_BITS_PER_WORD, &bits) < 0 ||
      ioctl (self->spi_fd, SPI_IOC_RD_MAX_SPEED_HZ, &speed) < 0)
    remember_error (&first, g_error_new (G_IO_ERROR, g_io_error_from_errno (errno),
                                         "Cannot verify restored FTE3600 SPI configuration: %s", g_strerror (errno)));
  else if (mode != expected_mode || bits != self->original_bits || speed != self->original_speed)
    remember_error (&first, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                                 "FTE3600 SPI configuration was not restored"));
  if (first)
    {
      g_propagate_error (error, first);
      return FALSE;
    }
  self->spi_mode = mode;
  return TRUE;
}

gboolean
fpi_fte3600_transport_close (FpiDeviceFte3600 *self, GError **error)
{
  GError *first = NULL, *next = NULL;
  gint *descriptors[] = { &self->irq_fd, &self->reset_fd, &self->spi_fd };

  /* Called only after the last SPI worker completed. */
  fpi_spi_transfer_set_device_guard (FP_DEVICE (self), NULL);
  fpi_fte3600_clear_irq_source (self);
  if (self->reset_fd >= 0 && !set_reset_value (self, FALSE, &next))
    remember_error (&first, g_steal_pointer (&next));
  if (self->spi_configured)
    {
      if (!fpi_fte3600_resources_check (&self->resources, &next) ||
          !restore_spi_configuration (self, &next))
        remember_error (&first, g_steal_pointer (&next));
    }
  for (guint i = 0; i < G_N_ELEMENTS (descriptors); i++)
    if (*descriptors[i] >= 0)
      {
        if (close (*descriptors[i]) < 0)
          {
            next = g_error_new (G_IO_ERROR, g_io_error_from_errno (errno),
                                "Cannot close FTE3600 resource: %s", g_strerror (errno));
            remember_error (&first, g_steal_pointer (&next));
          }
        *descriptors[i] = -1;
      }
  self->spi_configured = FALSE;
  /* This flag describes the released session. The next open establishes its
   * ACPI configuration before installing a new SPI guard. */
  self->spi_configuration_invalid = FALSE;
  self->transport_capabilities = 0;
  fpi_fte3600_resources_clear (&self->resources);
  if (first)
    {
      g_propagate_error (error, first);
      return FALSE;
    }
  return TRUE;
}

gboolean
fpi_fte3600_set_cs_polarity (FpiDeviceFte3600 *self, gboolean active_high, GError **error)
{
  guint32 value = !!active_high;
  guint32 old_mode = self->spi_mode;
  guint32 mode = (old_mode & ~SPI_CS_HIGH) | (value ? SPI_CS_HIGH : 0);
  guint32 read_mode;
  gint saved_errno;

  if (self->spi_configuration_invalid)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
                           "FTE3600 SPI configuration is unverified; reopen the device");
      return FALSE;
    }
  if (!fpi_fte3600_transport_check (FP_DEVICE (self), error))
    return FALSE;

  if (!!(self->spi_mode & SPI_CS_HIGH) == value)
    return TRUE;
  if (!(self->transport_capabilities & FTE3600_TRANSPORT_CAP_CS_POLARITY))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "This FTE3600 transport cannot negotiate chip-select polarity");
      return FALSE;
    }
  if (ioctl (self->spi_fd, SPI_IOC_WR_MODE32, &mode) < 0)
    {
      saved_errno = errno;
    }
  else if (ioctl (self->spi_fd, SPI_IOC_RD_MODE32, &read_mode) < 0)
    {
      saved_errno = errno;
    }
  else if (read_mode != mode)
    {
      saved_errno = EIO;
    }
  else
    {
      self->spi_mode = mode;
      return fpi_fte3600_transport_check (FP_DEVICE (self), error);
    }
  self->spi_configuration_invalid =
    ioctl (self->spi_fd, SPI_IOC_WR_MODE32, &old_mode) < 0 ||
    ioctl (self->spi_fd, SPI_IOC_RD_MODE32, &read_mode) < 0 || read_mode != old_mode;
  g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
               "Cannot configure FTE3600 chip-select polarity: %s", g_strerror (saved_errno));
  return FALSE;
}
