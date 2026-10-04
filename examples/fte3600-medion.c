/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Standalone Medion E3224 experiment. No context enumeration, daemon, policy
 * installation or authentication. The launcher validates the board resources;
 * this process reuses the actual chip discovery, initialization and capture.
 */
#include "fte3600-medion-transport.h"
#include "fte3600-medion-identify.h"

#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <linux/spi/spidev.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct
{
  gboolean done;
  GError  *error;
} ProbeResult;

static void
probe_finished (GObject *object, GAsyncResult *result, gpointer data)
{
  ProbeResult *probe = data;

  g_async_initable_init_finish (G_ASYNC_INITABLE (object), result, &probe->error);
  probe->done = TRUE;
}

static gboolean
cancel_operation (gpointer data)
{
  g_cancellable_cancel (G_CANCELLABLE (data));
  return G_SOURCE_CONTINUE;
}

static const gchar *
evidence_name (Fte3600IdentityEvidence evidence)
{
  switch (evidence)
    {
    case FTE3600_IDENTITY_NONE: return "none";
    case FTE3600_IDENTITY_RUNTIME_GEOMETRY: return "runtime-geometry";
    case FTE3600_IDENTITY_ROM_A8_SPI_OTP: return "ROM-family-and-OTP";
    case FTE3600_IDENTITY_ROM_BOOT_A: return "boot-A-register";
    case FTE3600_IDENTITY_ROM_BOOT_B38_SPI_OTP: return "boot-B38-OTP";
    case FTE3600_IDENTITY_SPECIAL_CHIP_ID: return "chip-ID";
    case FTE3600_IDENTITY_KNOWN_UNMAPPED_ID: return "known-unmapped-ID";
    }
  return "invalid";
}

static void
report_identity (const Fte3600Identity *identity, guint32 max_transfer)
{
  const Fte3600SensorDescriptor *sensor = fpi_fte3600_sensor_get (identity->sensor);

  g_print ("IDENTITY: backend=%s evidence=%s response=0x%04x otp=0x%02x\n",
           sensor ? sensor->name : "unknown", evidence_name (identity->evidence),
           identity->response, identity->otp);
  if (sensor)
    g_print ("IMAGE FORMAT: %u x %u; SPI buffer limit=%u bytes\n",
             sensor->width, sensor->height, max_transfer);
}

typedef struct
{
  FpiDeviceFte3600 *device;
  GCancellable    *cancellable;
} IdentifyIo;

static void
dispatch_signals (void)
{
  /* The standalone synchronous probe runs before any libfprint action. Keep
   * GLib's signal and timeout callbacks live, with a bounded dispatch batch. */
  for (guint i = 0; i < 64 && g_main_context_pending (NULL); i++)
    g_main_context_iteration (NULL, FALSE);
}

static gboolean
identify_cancelled (gpointer user_data, GError **error)
{
  IdentifyIo *io = user_data;

  dispatch_signals ();
  return g_cancellable_set_error_if_cancelled (io->cancellable, error);
}

static gboolean
identify_exchange (gpointer user_data, const guint8 *tx, guint8 *rx,
                   gsize length, GError **error)
{
  IdentifyIo *io = user_data;
  struct spi_ioc_transfer transfer = { 0 };
  g_autofree guint8 *discard = NULL;
  gint result;

  if (!tx || !length || length > io->device->max_transfer || length > G_MAXUINT32)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Invalid Medion identification transaction length");
      return FALSE;
    }
  if (!rx)
    rx = discard = g_malloc0 (length);
  else
    memset (rx, 0, length);
  transfer.tx_buf = (uintptr_t) tx;
  transfer.rx_buf = (uintptr_t) rx;
  transfer.len = length;
  /* Use the adapter's checked speed/bits settings. One full-duplex transaction
   * keeps CS asserted throughout the command and reply. Never replay EINTR:
   * the controller may already have sent a state-changing command. */
  result = ioctl (io->device->spi_fd, SPI_IOC_MESSAGE (1), &transfer);
  if (result < 0)
    {
      gint saved_errno = errno;

      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                   "Medion identification SPI transfer failed: %s", g_strerror (saved_errno));
      return FALSE;
    }
  if ((gsize) result != length)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT,
                   "Medion identification SPI transfer returned %d of %zu bytes", result, length);
      return FALSE;
    }
  return TRUE;
}

static gboolean
identify_reset (gpointer user_data, gboolean asserted, GError **error)
{
  IdentifyIo *io = user_data;

  return io->device->transport_ops->set_reset (io->device, asserted, error);
}

static gboolean
identify_wait (gpointer user_data, guint milliseconds, GError **error)
{
  gint64 deadline = g_get_monotonic_time () + (gint64) milliseconds * 1000;

  /* Record cancellation during a pulse, but let the engine finish that pulse
   * and release reset before honoring it at the next protocol boundary. */
  while (g_get_monotonic_time () < deadline)
    {
      dispatch_signals ();
      gint64 remaining = deadline - g_get_monotonic_time ();

      if (remaining > 0)
        g_usleep (MIN (remaining, 1000));
    }
  dispatch_signals ();
  return TRUE;
}

static void
identify_report (gpointer user_data, const gchar *message)
{
  g_print ("LEGACY IDENTIFY: %s\n", message);
}

static gboolean
identify_legacy (FpiDeviceFte3600 *self, const gchar *path,
                 GCancellable *cancellable, Fte3600Identity *identity,
                 GError **error)
{
  IdentifyIo native = { self, cancellable };
  Fte3600MedionIdentifyIo io = {
    .user_data = &native,
    .exchange = identify_exchange,
    .set_reset = identify_reset,
    .wait = identify_wait,
    .check_cancelled = identify_cancelled,
    .report = identify_report,
  };
  gboolean result = FALSE;

  if (identify_cancelled (&native, error))
    return FALSE;
  self->spi_fd = open (path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  if (self->spi_fd < 0)
    {
      gint saved_errno = errno;

      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                   "Cannot open Medion spidev: %s", g_strerror (saved_errno));
      return FALSE;
    }
  if (self->transport_ops->configure (self, error))
    {
      io.max_transfer = self->max_transfer;
      result = fte3600_medion_identify_legacy (&io, identity, error);
    }
  self->transport_ops->release (self);
  if (close (self->spi_fd) < 0)
    {
      gint saved_errno = errno;

      if (!*error)
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                     "Cannot close Medion spidev: %s", g_strerror (saved_errno));
      else
        g_printerr ("RESTORE FAIL: closing Medion spidev: %s\n", g_strerror (saved_errno));
      result = FALSE;
    }
  self->spi_fd = -1;
  /* detach below reports any latched GPIO/SPI parameter restoration failure.
   * This result is diagnostic only: never place it in self->rom_identity. */
  return result;
}

static gboolean
save_image (GOutputStream *output, FpImage *image, GError **error)
{
  gsize size = 0;
  const guint8 *pixels = fp_image_get_data (image, &size);
  guint width = fp_image_get_width (image), height = fp_image_get_height (image);
  g_autofree gchar *header = NULL;

  if (!width || !height || size != (gsize) width * height)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Capture returned an invalid image layout");
      return FALSE;
    }
  header = g_strdup_printf ("P5\n%u %u\n255\n", width, height);
  return g_output_stream_write_all (output, header, strlen (header), NULL, NULL, error) &&
         g_output_stream_write_all (output, pixels, size, NULL, NULL, error) &&
         g_output_stream_close (output, NULL, error);
}

int
main (int argc, char **argv)
{
  g_autofree gchar *device_path = NULL;
  g_autofree gchar *reset_chip = NULL;
  g_autofree gchar *irq_chip = NULL;
  g_autofree gchar *action = NULL;
  g_autofree gchar *output_path = NULL;
  gint timeout = 60;
  GOptionEntry entries[] = {
    { "device", 0, 0, G_OPTION_ARG_FILENAME, &device_path, "Validated Medion spidev node", "PATH" },
    { "reset-chip", 0, 0, G_OPTION_ARG_FILENAME, &reset_chip, "GPO1 GPIO chip (reset line 39)", "PATH" },
    { "irq-chip", 0, 0, G_OPTION_ARG_FILENAME, &irq_chip, "GPO2 GPIO chip (IRQ line 0)", "PATH" },
    { "action", 0, 0, G_OPTION_ARG_STRING, &action, "Stage: identify-legacy, probe, init or capture", "STAGE" },
    { "output", 0, 0, G_OPTION_ARG_FILENAME, &output_path, "New private PGM file for capture", "PATH" },
    { "timeout", 0, 0, G_OPTION_ARG_INT, &timeout, "Cancellation timeout per stage (1..300 seconds)", "SECONDS" },
    { NULL }
  };
  g_autoptr(GOptionContext) options = g_option_context_new ("- use scripts/medion-spidev.py for resource validation");
  g_autoptr(GError) error = NULL;
  g_autoptr(GError) cleanup_error = NULL;
  g_autoptr(FpiDeviceFte3600) self = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  g_autoptr(FpImage) image = NULL;
  g_autoptr(GFile) output_file = NULL;
  g_autoptr(GFileOutputStream) output = NULL;
  guint sigint_source = 0, sigterm_source = 0, timeout_source = 0;
  gboolean attached = FALSE, saved = FALSE, capture;
  const gchar *stage = "SETUP";
  int status = EXIT_FAILURE;
  ProbeResult probe = { 0 };

  /* Never dump fingerprint frames, firmware or raw bulk SPI buffers to logs,
   * even if an inherited debugging environment requested it. */
  g_unsetenv ("FP_DEBUG_TRANSFER");
  /* This experiment has no persistent firmware-update operation. */
  g_unsetenv ("FTE3600_FT9368_UPDATE");
  g_setenv ("G_MESSAGES_DEBUG", "libfprint-fte3600", TRUE);
  setvbuf (stdout, NULL, _IOLBF, 0);
  g_option_context_add_main_entries (options, entries, NULL);
  if (!g_option_context_parse (options, &argc, &argv, &error))
    goto out;
  capture = g_strcmp0 (action, "capture") == 0;
  if (argc != 1 || !device_path || !reset_chip || !irq_chip || !action ||
      !g_path_is_absolute (device_path) || !g_path_is_absolute (reset_chip) ||
      !g_path_is_absolute (irq_chip) || timeout < 1 || timeout > 300 ||
      (!capture && strcmp (action, "probe") && strcmp (action, "init") &&
       strcmp (action, "identify-legacy")) ||
      (capture != (output_path != NULL)))
    {
      g_set_error_literal (&error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                           "Provide all three absolute device paths and a valid action; only capture requires --output");
      goto out;
    }
  if (geteuid () != 0)
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           "Run the Medion launcher as root");
      goto out;
    }
  if (capture)
    {
      output_file = g_file_new_for_path (output_path);
      output = g_file_create (output_file, G_FILE_CREATE_PRIVATE, NULL, &error);
      if (!output)
        goto out;
    }

  self = g_object_new (fpi_device_fte3600_get_type (),
                        "fpi-udev-data-spidev", device_path, NULL);
  Fte3600MedionTransportConfig config = {
    .spi_path = device_path,
    .reset_gpiochip = reset_chip,
    .irq_gpiochip = irq_chip,
    .skip_irq = strcmp (action, "identify-legacy") == 0,
  };
  if (!fte3600_medion_transport_attach (self, &config, &error))
    goto out;
  attached = TRUE;
  sigint_source = g_unix_signal_add (SIGINT, cancel_operation, cancellable);
  sigterm_source = g_unix_signal_add (SIGTERM, cancel_operation, cancellable);
  if (strcmp (action, "identify-legacy") == 0)
    {
      Fte3600Identity identity = { 0 };

      stage = "IDENTIFY";
      g_print ("IDENTIFY START: dedicated Medion FT9338/FT9348 investigation.\n"
               "Application and ROM/OTP queries; no firmware upload or capture.\n");
      timeout_source = g_timeout_add_seconds (timeout, cancel_operation, cancellable);
      if (!identify_legacy (self, device_path, cancellable, &identity, &error))
        goto out;
      g_clear_handle_id (&timeout_source, g_source_remove);
      if (identity.evidence == FTE3600_IDENTITY_ROM_BOOT_B38_SPI_OTP)
        g_print ("IDENTITY CANDIDATE: %s; Windows B38-style OTP=0x%02x; "
                 "family response=0x%04x. No independent silicon-ID confirmation.\n",
                 fpi_fte3600_sensor_get (identity.sensor)->name, identity.otp, identity.response);
      else
        report_identity (&identity, self->max_transfer);
      status = EXIT_SUCCESS;
      goto out;
    }
  stage = "PROBE";
  g_print ("PROBE START: fixed low-CS mode 0; Windows-derived identity probes.\n"
           "Wake/ROM negotiation may change sensor state. No firmware upload in this stage.\n");
  timeout_source = g_timeout_add_seconds (timeout, cancel_operation, cancellable);
  g_async_initable_init_async (G_ASYNC_INITABLE (self), G_PRIORITY_DEFAULT,
                               cancellable, probe_finished, &probe);
  while (!probe.done)
    g_main_context_iteration (NULL, TRUE);
  g_clear_handle_id (&timeout_source, g_source_remove);
  report_identity (&self->identity, self->max_transfer);
  if (probe.error)
    {
      g_propagate_error (&error, probe.error);
      goto out;
    }
  g_print ("PROBE PASS: identity confirmed; capture backend is available.\n");
  if (strcmp (action, "probe") == 0)
    {
      status = EXIT_SUCCESS;
      goto out;
    }

  stage = "INIT";
  g_print ("INIT START: keep the sensor uncovered until INIT PASS.\n");
  timeout_source = g_timeout_add_seconds (timeout, cancel_operation, cancellable);
  if (!fp_device_open_sync (FP_DEVICE (self), cancellable, &error))
    goto out;
  g_clear_handle_id (&timeout_source, g_source_remove);
  g_print ("INIT PASS: %s initialized by its own backend.\n", self->sensor->name);
  if (!capture)
    {
      status = EXIT_SUCCESS;
      goto out;
    }

  stage = "CAPTURE";
  g_print ("CAPTURE START: place one finger on the sensor now.\n");
  timeout_source = g_timeout_add_seconds (timeout, cancel_operation, cancellable);
  image = fp_device_capture_sync (FP_DEVICE (self), TRUE, cancellable, &error);
  g_clear_handle_id (&timeout_source, g_source_remove);
  if (!image || !save_image (G_OUTPUT_STREAM (output), image, &error))
    goto out;
  saved = TRUE;
  g_print ("CAPTURE PASS: %u x %u image saved locally in %s (private file).\n",
           fp_image_get_width (image), fp_image_get_height (image), output_path);
  status = EXIT_SUCCESS;

out:
  g_clear_handle_id (&timeout_source, g_source_remove);
  if (error)
    g_printerr ("%s FAIL: %s\n", stage, error->message);
  if (self && fp_device_is_open (FP_DEVICE (self)) &&
      !fp_device_close_sync (FP_DEVICE (self), NULL, &cleanup_error))
    {
      g_printerr ("CLOSE FAIL: %s\n", cleanup_error->message);
      g_clear_error (&cleanup_error);
      status = EXIT_FAILURE;
    }
  if (attached && !fte3600_medion_transport_detach (self, &cleanup_error))
    {
      g_printerr ("RESTORE FAIL: %s\n", cleanup_error->message);
      g_clear_error (&cleanup_error);
      status = EXIT_FAILURE;
    }
  if (output && !saved)
    {
      g_output_stream_close (G_OUTPUT_STREAM (output), NULL, NULL);
      if (!g_file_delete (output_file, NULL, &cleanup_error))
        g_printerr ("Cannot remove incomplete output: %s\n", cleanup_error->message);
    }
  if (image)
    {
      gsize size;
      const guint8 *pixels = fp_image_get_data (image, &size);

      fpi_fte3600_secure_clear ((gpointer) pixels, size);
    }
  g_clear_handle_id (&sigint_source, g_source_remove);
  g_clear_handle_id (&sigterm_source, g_source_remove);
  if (status == EXIT_SUCCESS && g_strcmp0 (action, "identify-legacy") == 0)
    g_print ("IDENTIFY PASS: observations repeated and transport restored; "
             "no firmware upload, initialization or capture attempted.\n");
  return status;
}
