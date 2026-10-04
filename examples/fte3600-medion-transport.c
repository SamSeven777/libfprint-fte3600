/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Medion E3224 experimental diagnostic transport. This file is linked only
 * into the explicit diagnostic executable, never the installed libfprint.
 * GPIO paths and ACPI resources are validated by its launcher. No model-based
 * admission rule or spidev fallback is added to normal device enumeration.
 */

#include "fte3600-medion-transport.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define MEDION_RESET_OFFSET 39U
#define MEDION_IRQ_OFFSET 0U
#define MEDION_EVENT_BATCH 16U
#define MEDION_EVENT_LIMIT 256U

typedef struct
{
  gchar *spi_path;
  gchar *reset_gpiochip;
  gchar *irq_gpiochip;
  gboolean skip_irq;
  gint restore_fd;
  gint reset_fd;
  gint irq_fd;
  guint32 original_speed;
  guint8 original_bits;
  gboolean restore_parameters;
  gboolean configured;
  guint32 event_seqno;
  guint32 line_seqno;
  GError *cleanup_error;
} MedionTransport;

static const Fte3600TransportOps medion_ops;

static gboolean
system_error (GError **error, const gchar *operation)
{
  gint saved_errno = errno;

  g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
               "%s: %s", operation, g_strerror (saved_errno));
  return FALSE;
}

static void
save_cleanup_error (MedionTransport *data, const gchar *operation)
{
  if (!data->cleanup_error)
    system_error (&data->cleanup_error, operation);
}

static gboolean
set_reset_value (MedionTransport *data, gboolean asserted, GError **error)
{
  struct gpio_v2_line_values values = { .mask = 1, .bits = !!asserted };

  if (data->reset_fd < 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                           "Medion reset line is not requested");
      return FALSE;
    }
  /* Logical 0 with ACTIVE_LOW is physical high; logical 1 is physical low.
   * This matches the documented H/L/H reset waveform. */
  if (ioctl (data->reset_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &values) < 0)
    return system_error (error, "Cannot set Medion reset line");
  return TRUE;
}

static void
close_session (MedionTransport *data)
{
  if (data->reset_fd >= 0)
    {
      g_autoptr(GError) error = NULL;

      if (!set_reset_value (data, FALSE, &error) && !data->cleanup_error)
        data->cleanup_error = g_steal_pointer (&error);
      if (close (data->reset_fd) < 0)
        save_cleanup_error (data, "Cannot close Medion reset request");
      data->reset_fd = -1;
    }
  if (data->irq_fd >= 0)
    {
      if (close (data->irq_fd) < 0)
        save_cleanup_error (data, "Cannot close Medion IRQ request");
      data->irq_fd = -1;
    }
  if (data->restore_fd >= 0)
    {
      /* The core may already have closed spi_fd. Only this owned duplicate
       * is used here. CS/CPOL/CPHA were never changed by this adapter. */
      if (data->restore_parameters)
        {
          if (ioctl (data->restore_fd, SPI_IOC_WR_BITS_PER_WORD, &data->original_bits) < 0)
            save_cleanup_error (data, "Cannot restore SPI word size");
          if (ioctl (data->restore_fd, SPI_IOC_WR_MAX_SPEED_HZ, &data->original_speed) < 0)
            save_cleanup_error (data, "Cannot restore SPI speed");
        }
      if (close (data->restore_fd) < 0)
        save_cleanup_error (data, "Cannot close SPI restoration descriptor");
      data->restore_fd = -1;
    }
  data->restore_parameters = FALSE;
  data->configured = FALSE;
  data->event_seqno = 0;
  data->line_seqno = 0;
  /* After a GPIO request is closed the kernel controls its default state;
   * deasserting first is best effort, not a guarantee about later levels. */
}

static gboolean
request_line (const gchar *path, gboolean reset, gint *line_fd, GError **error)
{
  struct gpiochip_info chip_info = { 0 };
  struct gpio_v2_line_request request = { 0 };
  struct stat st;
  guint offset = reset ? MEDION_RESET_OFFSET : MEDION_IRQ_OFFSET;
  gint chip_fd;
  gboolean result = FALSE;

  chip_fd = open (path, O_RDONLY | O_CLOEXEC);
  if (chip_fd < 0)
    return system_error (error, "Cannot open validated GPIO controller");
  if (fstat (chip_fd, &st) < 0)
    {
      system_error (error, "Cannot inspect GPIO controller");
      goto out;
    }
  if (!S_ISCHR (st.st_mode))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "GPIO controller path is not a character device");
      goto out;
    }
  if (ioctl (chip_fd, GPIO_GET_CHIPINFO_IOCTL, &chip_info) < 0)
    {
      system_error (error, "Cannot query GPIO controller");
      goto out;
    }
  if (offset >= chip_info.lines)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Validated Medion GPIO offset is outside controller");
      goto out;
    }
  request.offsets[0] = offset;
  request.num_lines = 1;
  g_strlcpy (request.consumer, "fte3600-medion-diagnostic", sizeof request.consumer);
  if (reset)
    {
      request.config.flags = GPIO_V2_LINE_FLAG_OUTPUT | GPIO_V2_LINE_FLAG_ACTIVE_LOW;
      request.config.num_attrs = 1;
      request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
      request.config.attrs[0].attr.values = 0;
      request.config.attrs[0].mask = 1;
    }
  else
    {
      /* The recorded IRQ resource is active-high/rising with NoPull. */
      request.config.flags = GPIO_V2_LINE_FLAG_INPUT |
                             GPIO_V2_LINE_FLAG_EDGE_RISING |
                             GPIO_V2_LINE_FLAG_BIAS_DISABLED;
      request.event_buffer_size = MEDION_EVENT_LIMIT;
    }
  if (ioctl (chip_fd, GPIO_V2_GET_LINE_IOCTL, &request) < 0)
    {
      system_error (error, reset ? "Cannot request Medion reset GPIO v2 line" :
                    "Cannot request Medion IRQ GPIO v2 line");
      goto out;
    }
  *line_fd = request.fd;
  if (fcntl (*line_fd, F_SETFD, FD_CLOEXEC) < 0)
    {
      system_error (error, "Cannot protect GPIO request descriptor");
      goto out;
    }
  if (!reset)
    {
      gint flags = fcntl (*line_fd, F_GETFL);

      if (flags < 0 || fcntl (*line_fd, F_SETFL, flags | O_NONBLOCK) < 0)
        {
          system_error (error, "Cannot make GPIO IRQ nonblocking");
          goto out;
        }
    }
  result = TRUE;
out:
  if (close (chip_fd) < 0 && result)
    result = system_error (error, "Cannot close GPIO controller descriptor");
  return result;
}

static gboolean
configure (FpiDeviceFte3600 *self, GError **error)
{
  MedionTransport *data = self->transport_data;
  g_autofree gchar *buffer_text = NULL;
  guint64 buffer_size;
  struct stat opened, requested;
  guint32 mode, speed;
  guint8 bits = 8;

  if (!data || self->spi_fd < 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                           "Medion diagnostic transport is not attached/open");
      return FALSE;
    }
  if (data->cleanup_error)
    {
      g_propagate_error (error, g_error_copy (data->cleanup_error));
      return FALSE;
    }
  if (data->configured)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                           "Medion diagnostic transport is already configured");
      return FALSE;
    }
  if (fstat (self->spi_fd, &opened) < 0 || stat (data->spi_path, &requested) < 0)
    return system_error (error, "Cannot verify opened SPI device");
  if (!S_ISCHR (opened.st_mode) || !S_ISCHR (requested.st_mode) ||
      opened.st_rdev != requested.st_rdev)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Opened SPI fd does not match the validated spidev node");
      return FALSE;
    }
  if (!g_file_get_contents ("/sys/module/spidev/parameters/bufsiz", &buffer_text, NULL, error) ||
      !g_ascii_string_to_unsigned (g_strstrip (buffer_text), 10,
                                  FTE3600_COMMAND_MAX_SIZE, G_MAXUINT32,
                                  &buffer_size, error))
    return FALSE;
  data->restore_fd = fcntl (self->spi_fd, F_DUPFD_CLOEXEC, 0);
  if (data->restore_fd < 0)
    return system_error (error, "Cannot retain SPI restoration descriptor");
  if (ioctl (self->spi_fd, SPI_IOC_RD_MODE32, &mode) < 0 ||
      ioctl (self->spi_fd, SPI_IOC_RD_BITS_PER_WORD, &data->original_bits) < 0 ||
      ioctl (self->spi_fd, SPI_IOC_RD_MAX_SPEED_HZ, &data->original_speed) < 0)
    {
      system_error (error, "Cannot read original SPI configuration");
      goto fail;
    }
  if (mode != SPI_MODE_0 || !data->original_speed)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Medion diagnostic requires existing MODE0, active-low CS and a nonzero SPI speed; no polarity change was attempted");
      goto fail;
    }
  speed = MIN (data->original_speed, FTE3600_SPI_SPEED_HZ);
  /* No transactions have been issued. Even a failed setup ioctl may have
   * reached the controller, so schedule restoration before making the call. */
  data->restore_parameters = TRUE;
  if (ioctl (self->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
      ioctl (self->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0 ||
      ioctl (self->spi_fd, SPI_IOC_RD_BITS_PER_WORD, &bits) < 0 ||
      ioctl (self->spi_fd, SPI_IOC_RD_MAX_SPEED_HZ, &speed) < 0 ||
      ioctl (self->spi_fd, SPI_IOC_RD_MODE32, &mode) < 0)
    {
      system_error (error, "Cannot configure diagnostic SPI parameters");
      goto fail;
    }
  if (mode != SPI_MODE_0 || bits != 8 || !speed || speed > FTE3600_SPI_SPEED_HZ)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "SPI configuration readback does not satisfy diagnostic limits");
      goto fail;
    }
  /* Capture needs an IRQ, so acquire its input before changing reset. Explicit
   * synchronous identification does not need or claim the interrupt line. */
  if ((!data->skip_irq && !request_line (data->irq_gpiochip, FALSE, &data->irq_fd, error)) ||
      !request_line (data->reset_gpiochip, TRUE, &data->reset_fd, error))
    goto fail;
  self->spi_mode = mode;
  self->bridge_capabilities = 0; /* Fixed CS; no alternate-polarity discovery. */
  self->max_transfer = MIN (buffer_size, FTE3600_BRIDGE_MAX_TRANSFER);
  /* This is a spidev buffer bound, not a query of the controller's maximum.
   * Existing full-duplex transfers stay unsplit and propagate EMSGSIZE. */
  data->configured = TRUE;
  g_print ("Medion diagnostic transport: mode=0x%08x bits=%u configured_speed_limit_hz=%u "
           "spidev_bufsiz=%" G_GUINT64_FORMAT " max_transfer=%u "
           "reset=%s:39 active_low logical0=physical_high "
           "irq=%s:0 %s\n",
           mode, bits, speed, buffer_size, self->max_transfer,
           data->reset_gpiochip, data->irq_gpiochip,
           data->skip_irq ? "not_requested (synchronous diagnostic)" : "active_high rising");
  return TRUE;
fail:
  close_session (data);
  return FALSE;
}

static gboolean
set_reset (FpiDeviceFte3600 *self, gboolean asserted, GError **error)
{
  MedionTransport *data = self->transport_data;

  if (!data || !data->configured)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                           "Medion diagnostic transport is not configured");
      return FALSE;
    }
  return set_reset_value (data, asserted, error);
}

static gboolean
get_events (FpiDeviceFte3600 *self, guint32 *events, GError **error)
{
  MedionTransport *data = self->transport_data;
  struct gpio_v2_line_event batch[MEDION_EVENT_BATCH];
  guint processed = 0;

  *events = 0;
  if (!data || !data->configured || data->irq_fd < 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                           "Medion diagnostic IRQ is not configured");
      return FALSE;
    }
  /* Drain only a bounded batch. Sequence numbers begin at 1 for a request;
   * unsigned subtraction also handles their wraparound. A gap includes loss
   * before the very first read, and must not become an invented finger event. */
  while (processed < MEDION_EVENT_LIMIT)
    {
      ssize_t size = read (data->irq_fd, batch, sizeof batch);

      if (size < 0)
        {
          if (errno == EAGAIN)
            return TRUE;
          if (errno == EINTR)
            {
              /* Bound even a continuously interrupted stream. */
              processed++;
              continue;
            }
          return system_error (error, "Cannot read Medion GPIO IRQ");
        }
      if (!size || (size_t) size % sizeof batch[0])
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                               "Truncated or closed Medion GPIO event stream");
          return FALSE;
        }
      for (guint i = 0; i < (size_t) size / sizeof batch[0]; i++)
        {
          if (batch[i].offset != MEDION_IRQ_OFFSET ||
              batch[i].id != GPIO_V2_LINE_EVENT_RISING_EDGE ||
              (guint32) (batch[i].seqno - data->event_seqno) != 1 ||
              (guint32) (batch[i].line_seqno - data->line_seqno) != 1)
            {
              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                   "Medion GPIO event identity/sequence mismatch; events may have been lost");
              return FALSE;
            }
          data->event_seqno = batch[i].seqno;
          data->line_seqno = batch[i].line_seqno;
          processed++;
          *events = 1; /* Match the bridge's coalesced notification contract. */
        }
    }
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                       "Medion GPIO event drain exceeded its bounded limit");
  return FALSE;
}

static gint
irq_fd (FpiDeviceFte3600 *self)
{
  MedionTransport *data = self->transport_data;

  return data && data->configured ? data->irq_fd : -1;
}

static void
release (FpiDeviceFte3600 *self)
{
  MedionTransport *data = self->transport_data;

  if (data)
    close_session (data);
}

static const Fte3600TransportOps medion_ops = {
  .configure = configure,
  .set_reset = set_reset,
  .get_events = get_events,
  .irq_fd = irq_fd,
  .release = release,
};

gboolean
fte3600_medion_transport_attach (FpiDeviceFte3600                    *self,
                                const Fte3600MedionTransportConfig *config,
                                GError                           **error)
{
  MedionTransport *data;

  if (!config || !config->spi_path || !g_path_is_absolute (config->spi_path) ||
      !config->reset_gpiochip || !g_path_is_absolute (config->reset_gpiochip) ||
      !config->irq_gpiochip || !g_path_is_absolute (config->irq_gpiochip) ||
      g_str_equal (config->reset_gpiochip, config->irq_gpiochip))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Explicit validated spidev and separate GPIO controller paths are required");
      return FALSE;
    }
  if (self->transport_ops || self->transport_data || self->spi_fd >= 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                           "Attach the diagnostic transport before opening the device");
      return FALSE;
    }
  data = g_new0 (MedionTransport, 1);
  data->spi_path = g_strdup (config->spi_path);
  data->reset_gpiochip = g_strdup (config->reset_gpiochip);
  data->irq_gpiochip = g_strdup (config->irq_gpiochip);
  data->skip_irq = config->skip_irq;
  data->restore_fd = data->reset_fd = data->irq_fd = -1;
  self->transport_data = data;
  self->transport_ops = &medion_ops;
  return TRUE;
}

gboolean
fte3600_medion_transport_detach (FpiDeviceFte3600 *self, GError **error)
{
  MedionTransport *data;
  gboolean result = TRUE;

  if (!self->transport_ops && !self->transport_data)
    return TRUE;
  if (self->transport_ops != &medion_ops || self->spi_fd >= 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                           "Close the Medion diagnostic device before detaching its transport");
      return FALSE;
    }
  data = self->transport_data;
  if (data)
    {
      close_session (data);
      if (data->cleanup_error)
        {
          g_propagate_error (error, g_steal_pointer (&data->cleanup_error));
          result = FALSE;
        }
      g_free (data->spi_path);
      g_free (data->reset_gpiochip);
      g_free (data->irq_gpiochip);
      g_free (data);
    }
  self->transport_data = NULL;
  self->transport_ops = NULL;
  return result;
}
