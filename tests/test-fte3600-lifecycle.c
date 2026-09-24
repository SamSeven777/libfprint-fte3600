/*
 * Hardware-independent FTE3600 lifecycle and fault-injection tests.
 * SPDX-FileCopyrightText: 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Linker wrappers replace only the OS/device boundary. The production driver,
 * SPI worker threads, state machines, cancellables and public FpDevice API run
 * unchanged. No real sensor, GPIO, firmware or biometric fixture is needed.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/spi/spidev.h>
#include <gpiod.h>
#include <gudev/gudev.h>

#include "drivers/fte3600.h"

#ifndef FTE3600_ENABLE_PERSONAL_AUTH
#define FTE3600_ENABLE_PERSONAL_AUTH 0
#endif

GType fpi_device_fte3600_get_type (void);

/* Declarations also check wrapper signatures against the platform headers. */
#define WRAPPED(name) __typeof__ (name) __wrap_ ## name
WRAPPED (open);
WRAPPED (close);
WRAPPED (ioctl);
WRAPPED (g_file_get_contents);
WRAPPED (g_udev_client_new);
WRAPPED (g_udev_client_query_by_subsystem);
WRAPPED (g_udev_device_get_device_file);
WRAPPED (g_udev_device_get_sysfs_path);
WRAPPED (gpiod_chip_open);
WRAPPED (gpiod_chip_close);
WRAPPED (gpiod_chip_request_lines);
WRAPPED (gpiod_line_request_release);
WRAPPED (gpiod_line_request_set_value);
WRAPPED (gpiod_line_request_get_fd);
WRAPPED (gpiod_line_request_wait_edge_events);
WRAPPED (gpiod_line_request_read_edge_events);
WRAPPED (gpiod_edge_event_buffer_get_event);
WRAPPED (gpiod_edge_event_get_event_type);
WRAPPED (gpiod_edge_event_get_line_offset);
#undef WRAPPED

__typeof__ (open) __real_open;
__typeof__ (close) __real_close;
__typeof__ (g_file_get_contents) __real_g_file_get_contents;
int __wrap_open64 (const char *path,
                   int         flags,
                   ...);

typedef enum {
  DELIVER_FINGER,
  CANCEL_WAIT,
} IrqAction;

typedef struct
{
  const gchar *vendor;
  const gchar *product;
  const gchar *controller_path;
  const gchar *controller_hid;
  guint        reset_offset;
  guint        irq_offset;
} TestPlatform;

static const TestPlatform platforms[] = {
  { "ONE-NETBOOK TECHNOLOGY CO., LTD.", "A1", "\\_SB_.PCI0.GPI0", NULL, 0x55, 0x56 },
  { "GPD", "Pocket 3", "\\_SB_.GPI0", "INT34C8", 211, 56 },
  { "GPD", "Pocket 3", "\\_SB_.GPI0", "INT3455", 179, 24 },
  { "GPD", "GPD Pocket 3", "\\_SB_.GPI0", "INT34C8", 211, 56 },
  { "GPD", "GPD Pocket 3", "\\_SB_.GPI0", "INT3455", 179, 24 },
};

static struct
{
  GMutex              lock;
  const TestPlatform *platform;
  gint                spi_fd;
  gint                irq_pipe[2];
  guint               opens;
  guint               closes;
  guint               claims;
  guint               chip_opens;
  guint               releases;
  guint               resets;
  guint               hardware_asserts;
  guint               hardware_deasserts;
  guint               images;
  guint               irq_source;
  guint8              registers[256];
  gboolean            claimed;
  gboolean            armed;
  gboolean            finger_ready;
  gboolean            bad_id;
  gboolean            fail_config;
  gboolean            fail_claim;
  gboolean            fail_image;
  gboolean            fail_reset;
  gboolean            cancel_image;
  gboolean            hardware_recovery;
  gboolean            cold_start;
  gboolean            reset_asserted;
  gboolean            cancel_hardware_reset;
  gboolean            fail_hardware_reset;
  IrqAction           irq_action;
  GCancellable       *cancellable;
} sensor;

int
__wrap_open (const char *path, int flags, ...)
{
  /* Fail closed: an unexpected firmware/hardware open is a test failure. */
  g_assert_cmpstr (path, ==, "/mock/fte3600-spi");
  g_assert_cmpint (sensor.spi_fd, ==, -1);
  sensor.spi_fd = __real_open ("/dev/null", O_RDWR | O_CLOEXEC);
  g_assert_cmpint (sensor.spi_fd, >=, 0);
  sensor.opens++;
  return sensor.spi_fd;
}

int
__wrap_open64 (const char *path, int flags, ...)
{
  return __wrap_open (path, flags);
}

int
__wrap_close (int fd)
{
  g_assert_cmpint (fd, ==, sensor.spi_fd);
  sensor.spi_fd = -1;
  sensor.closes++;
  return __real_close (fd);
}

gboolean
__wrap_g_file_get_contents (const gchar *path, gchar **contents,
                            gsize *length, GError **error)
{
  const gchar *value = NULL;

  if (g_str_equal (path, "/sys/class/dmi/id/sys_vendor"))
    {
      value = sensor.platform->vendor;
    }
  else if (g_str_equal (path, "/sys/class/dmi/id/product_name"))
    {
      value = sensor.platform->product;
    }
  else if (g_str_equal (path, "/mock/gpio/firmware_node/path"))
    {
      value = sensor.platform->controller_path;
    }
  else if (g_str_equal (path, "/mock/gpio/firmware_node/hid"))
    {
      value = sensor.platform->controller_hid;
      if (!value)
        {
          g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                               "Mock controller has no HID attribute");
          return FALSE;
        }
    }
  else if (g_str_equal (path, "/sys/module/spidev/parameters/bufsiz"))
    {
      value = "32768\n";
    }
  else
    {
      return __real_g_file_get_contents (path, contents, length, error);
    }

  *contents = g_strdup (value);
  if (length)
    *length = strlen (value);
  return TRUE;
}

GUdevClient *
__wrap_g_udev_client_new (const gchar * const *subsystems)
{
  return (GUdevClient *) g_object_new (G_TYPE_OBJECT, NULL);
}

GList *
__wrap_g_udev_client_query_by_subsystem (GUdevClient *client,
                                         const gchar *subsystem)
{
  g_assert_cmpstr (subsystem, ==, "gpio");
  return g_list_append (NULL, g_object_new (G_TYPE_OBJECT, NULL));
}

const gchar *
__wrap_g_udev_device_get_device_file (GUdevDevice *device)
{
  return "/mock/gpiochip";
}

const gchar *
__wrap_g_udev_device_get_sysfs_path (GUdevDevice *device)
{
  return "/mock/gpio";
}

struct gpiod_chip *
__wrap_gpiod_chip_open (const char *path)
{
  g_assert_cmpstr (path, ==, "/mock/gpiochip");
  sensor.chip_opens++;
  return (struct gpiod_chip *) &sensor;
}

void
__wrap_gpiod_chip_close (struct gpiod_chip *chip)
{
  g_assert_true (chip == (struct gpiod_chip *) &sensor);
}

struct gpiod_line_request *
__wrap_gpiod_chip_request_lines (struct gpiod_chip           *chip,
                                 struct gpiod_request_config *request_config,
                                 struct gpiod_line_config    *line_config)
{
  unsigned int offsets[2];
  struct gpiod_line_settings *reset_settings;
  struct gpiod_line_settings *irq_settings;

  /* Inspect the real libgpiod configuration, not just driver-side metadata. */
  g_assert_cmpuint (gpiod_line_config_get_num_configured_offsets (line_config), ==, 2);
  g_assert_cmpuint (gpiod_line_config_get_configured_offsets (line_config, offsets, 2), ==, 2);
  g_assert_true ((offsets[0] == sensor.platform->reset_offset &&
                  offsets[1] == sensor.platform->irq_offset) ||
                 (offsets[1] == sensor.platform->reset_offset &&
                  offsets[0] == sensor.platform->irq_offset));
  reset_settings = gpiod_line_config_get_line_settings (line_config,
                                                        sensor.platform->reset_offset);
  irq_settings = gpiod_line_config_get_line_settings (line_config,
                                                      sensor.platform->irq_offset);
  g_assert_nonnull (reset_settings);
  g_assert_nonnull (irq_settings);
  g_assert_cmpint (gpiod_line_settings_get_direction (reset_settings), ==,
                   GPIOD_LINE_DIRECTION_OUTPUT);
  g_assert_cmpint (gpiod_line_settings_get_output_value (reset_settings), ==,
                   GPIOD_LINE_VALUE_ACTIVE);
  g_assert_false (gpiod_line_settings_get_active_low (reset_settings));
  g_assert_cmpint (gpiod_line_settings_get_direction (irq_settings), ==,
                   GPIOD_LINE_DIRECTION_INPUT);
  g_assert_cmpint (gpiod_line_settings_get_edge_detection (irq_settings), ==,
                   GPIOD_LINE_EDGE_RISING);
  gpiod_line_settings_free (reset_settings);
  gpiod_line_settings_free (irq_settings);

  if (sensor.fail_claim)
    {
      errno = EBUSY;
      return NULL;
    }
  g_assert_false (sensor.claimed);
  sensor.claimed = TRUE;
  sensor.claims++;
  return (struct gpiod_line_request *) &sensor;
}

void
__wrap_gpiod_line_request_release (struct gpiod_line_request *request)
{
  g_assert_true (sensor.claimed);
  g_assert_false (sensor.reset_asserted);
  sensor.claimed = FALSE;
  sensor.releases++;
}

int
__wrap_gpiod_line_request_set_value (struct gpiod_line_request *request,
                                     unsigned int               offset,
                                     enum gpiod_line_value      value)
{
  g_assert_true (sensor.claimed);
  g_assert_cmpuint (offset, ==, sensor.platform->reset_offset);
  if (value == GPIOD_LINE_VALUE_INACTIVE)
    {
      g_assert_true (sensor.hardware_recovery);
      sensor.hardware_asserts++;
      if (sensor.fail_hardware_reset)
        {
          errno = EIO;
          return -1;
        }
      sensor.reset_asserted = TRUE;
      if (sensor.cancel_hardware_reset)
        g_cancellable_cancel (sensor.cancellable);
    }
  else
    {
      g_assert_cmpint (value, ==, GPIOD_LINE_VALUE_ACTIVE);
      if (sensor.reset_asserted)
        {
          sensor.hardware_deasserts++;
          sensor.reset_asserted = FALSE;
          sensor.cold_start = FALSE;
        }
    }
  return 0;
}

static gboolean
deliver_irq (gpointer unused)
{
  sensor.irq_source = 0;
  if (sensor.irq_action == CANCEL_WAIT)
    {
      g_cancellable_cancel (sensor.cancellable);
    }
  else
    {
      g_mutex_lock (&sensor.lock);
      sensor.armed = FALSE;
      sensor.finger_ready = TRUE;
      g_mutex_unlock (&sensor.lock);
      g_assert_cmpint (write (sensor.irq_pipe[1], "x", 1), ==, 1);
    }
  return G_SOURCE_REMOVE;
}

int
__wrap_gpiod_line_request_get_fd (struct gpiod_line_request *request)
{
  /* Called once to attach the wait, then again to check the delivered IRQ. */
  if (sensor.armed && sensor.irq_source == 0)
    sensor.irq_source = g_idle_add (deliver_irq, NULL);
  return sensor.irq_pipe[0];
}

int
__wrap_gpiod_line_request_wait_edge_events (struct gpiod_line_request *request,
                                            int64_t                    timeout_ns)
{
  struct pollfd fd = { .fd = sensor.irq_pipe[0], .events = POLLIN };

  g_assert_cmpint (timeout_ns, ==, 0);
  return poll (&fd, 1, 0);
}

int
__wrap_gpiod_line_request_read_edge_events (struct gpiod_line_request      *request,
                                            struct gpiod_edge_event_buffer *buffer,
                                            size_t                          max_events)
{
  char byte;

  g_assert_cmpint (read (sensor.irq_pipe[0], &byte, 1), ==, 1);
  return 1;
}

struct gpiod_edge_event *
__wrap_gpiod_edge_event_buffer_get_event (struct gpiod_edge_event_buffer *buffer,
                                          unsigned long                   index)
{
  g_assert_cmpuint (index, ==, 0);
  return (struct gpiod_edge_event *) &sensor;
}

enum gpiod_edge_event_type
__wrap_gpiod_edge_event_get_event_type (struct gpiod_edge_event *event)
{
  return GPIOD_EDGE_EVENT_RISING_EDGE;
}

unsigned int
__wrap_gpiod_edge_event_get_line_offset (struct gpiod_edge_event *event)
{
  return sensor.platform->irq_offset;
}

int
__wrap_ioctl (int fd, unsigned long operation, ...)
{
  struct spi_ioc_transfer *transfer;
  const guint8 *tx;
  guint8 *rx;
  va_list args;
  int result;

  g_assert_cmpint (fd, ==, sensor.spi_fd);
  if (operation == SPI_IOC_WR_MODE || operation == SPI_IOC_WR_BITS_PER_WORD ||
      operation == SPI_IOC_WR_MAX_SPEED_HZ)
    {
      if (sensor.fail_config)
        {
          errno = EIO;
          return -1;
        }
      return 0;
    }

  g_assert_cmpuint (operation, ==, SPI_IOC_MESSAGE (1));
  va_start (args, operation);
  transfer = va_arg (args, struct spi_ioc_transfer *);
  va_end (args);
  tx = (const guint8 *) (guintptr) transfer->tx_buf;
  rx = (guint8 *) (guintptr) transfer->rx_buf;
  result = transfer->len;
  g_mutex_lock (&sensor.lock);
  switch (tx[0])
    {
    case 0x70:
      sensor.resets++;
      sensor.armed = FALSE;
      sensor.finger_ready = FALSE;
      if (sensor.fail_reset)
        result = -1;
      break;

    case 0x11:
      g_assert_cmpuint (transfer->len, ==, 5);
      sensor.registers[tx[2]] = tx[3];
      if (tx[2] == FT9361_REG_CAPTURE_START && tx[3] == 1)
        sensor.armed = TRUE;
      break;

    case 0x10:
      g_assert_nonnull (rx);
      memset (rx, 0, transfer->len);
      if (tx[2] == FT9361_REG_MCU_STATUS)
        {
          if (!sensor.armed && !sensor.cold_start)
            {
              rx[4] = 0xa5;
              rx[5] = 0x5a;
            }
        }
      else if (tx[2] == FT9361_REG_FINGER_STATUS)
        {
          rx[4] = sensor.finger_ready ? 1 : 0;
        }
      else if (tx[2] == FT9361_REG_SENSOR_ID_HIGH && sensor.bad_id)
        {
          rx[4] = 0xff;
        }
      else
        {
          rx[4] = sensor.registers[tx[2]];
        }
      break;

    case 0x04:
      sensor.images++;
      g_assert_cmpuint (transfer->len, ==, FT9361_CAPTURE_FRAME_SIZE);
      g_assert_nonnull (rx);
      for (guint i = 0; i < FT9361_IMAGE_SIZE; i++)
        rx[FT9361_CAPTURE_DATA_OFFSET + i] = (i * 37 + 11) & 0xff;
      if (sensor.fail_image)
        result = -1;
      if (sensor.cancel_image)
        g_cancellable_cancel (sensor.cancellable);
      break;

    default:
      g_assert_not_reached ();
    }
  g_mutex_unlock (&sensor.lock);
  if (result < 0)
    errno = EIO;
  return result;
}

typedef struct
{
  gboolean complete;
  GError  *error;
} DeviceInit;

static void
init_complete (GObject *object, GAsyncResult *result, gpointer data)
{
  DeviceInit *initialized = data;
  gboolean success;

  success = g_async_initable_init_finish (G_ASYNC_INITABLE (object), result,
                                          &initialized->error);
  g_assert_cmpint (success, ==, initialized->error == NULL);
  initialized->complete = TRUE;
}

static FpDevice *
new_device_for_platform (const TestPlatform *platform, GError **error)
{
  DeviceInit initialized = { 0 };
  FpDevice *device;

  memset (&sensor, 0, sizeof (sensor));
  g_mutex_init (&sensor.lock);
  sensor.platform = platform;
  sensor.spi_fd = -1;
  sensor.registers[FT9361_REG_SENSOR_ID_HIGH] = FT9361_SENSOR_ID_HIGH;
  sensor.registers[FT9361_REG_SENSOR_ID_LOW] = FT9361_SENSOR_ID_LOW;
  sensor.registers[FT9361_REG_FW_VERSION] = FT9361_FW_VERSION;
  sensor.registers[FT9361_REG_AGC_VERSION] = FT9361_AGC_VERSION;
  g_assert_cmpint (pipe (sensor.irq_pipe), ==, 0);
  sensor.cancellable = g_cancellable_new ();
  device = g_object_new (fpi_device_fte3600_get_type (),
                         "fpi-udev-data-spidev", "/mock/fte3600-spi", NULL);
  g_async_initable_init_async (G_ASYNC_INITABLE (device), G_PRIORITY_DEFAULT,
                               NULL, init_complete, &initialized);
  while (!initialized.complete)
    g_main_context_iteration (NULL, TRUE);
  if (initialized.error)
    g_propagate_error (error, initialized.error);
  return device;
}

static FpDevice *
new_device (void)
{
  g_autoptr(GError) error = NULL;
  FpDevice *device = new_device_for_platform (&platforms[0], &error);

  g_assert_no_error (error);
  return device;
}

static void
finish_device (FpDevice *device)
{
  GError *error = NULL;

  if (fp_device_is_open (device))
    {
      g_assert_true (fp_device_close_sync (device, NULL, &error));
      g_assert_no_error (error);
    }
  g_object_unref (device);
  g_assert_cmpint (sensor.spi_fd, ==, -1);
  g_assert_false (sensor.claimed);
  g_assert_cmpuint (sensor.opens, ==, sensor.closes);
  g_assert_cmpuint (sensor.claims, ==, sensor.releases);
  g_assert_cmpuint (sensor.irq_source, ==, 0);
  __real_close (sensor.irq_pipe[0]);
  __real_close (sensor.irq_pipe[1]);
  g_clear_object (&sensor.cancellable);
  g_mutex_clear (&sensor.lock);
}

static void
open_device (FpDevice *device)
{
  GError *error = NULL;

  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (fp_device_is_open (device));
}

static void
test_capture_reopen (gconstpointer data)
{
  g_autoptr(GError) init_error = NULL;
  FpDevice *device = new_device_for_platform (data, &init_error);

  g_assert_no_error (init_error);

  for (guint round = 0; round < 2; round++)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(FpImage) image = NULL;
      const guint8 *pixels;
      gsize size;
      guint resets;

      open_device (device);
      image = fp_device_capture_sync (device, TRUE, NULL, &error);
      g_assert_no_error (error);
      g_assert_nonnull (image);
      g_assert_cmpuint (fp_image_get_width (image), ==, FT9361_IMAGE_WIDTH);
      g_assert_cmpuint (fp_image_get_height (image), ==, FT9361_IMAGE_HEIGHT);
      pixels = fp_image_get_data (image, &size);
      g_assert_cmpuint (size, ==, FT9361_IMAGE_SIZE);
      for (guint i = 0; i < size; i++)
        g_assert_cmpuint (pixels[i], ==, (guint8) ~((i * 37 + 11) & 0xff));
      g_assert_cmpuint (sensor.resets, ==, (round + 1) * 4);
      resets = sensor.resets;
      g_assert_true (fp_device_close_sync (device, NULL, &error));
      g_assert_no_error (error);
      /* Terminal cleanup already reset the sensor; close must not repeat it. */
      g_assert_cmpuint (sensor.resets, ==, resets);
      g_assert_false (sensor.claimed);
    }
  g_assert_cmpuint (sensor.images, ==, 2);
  finish_device (device);
}

static void
test_unknown_controller_hid (gconstpointer data)
{
  TestPlatform platform = platforms[1];

  g_autoptr(GError) error = NULL;
  FpDevice *device;

  platform.controller_hid = data;
  device = new_device_for_platform (&platform, &error);
  g_assert_no_error (error);
  g_assert_false (fp_device_open_sync (device, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_cmpuint (sensor.chip_opens, ==, 0);
  g_assert_cmpuint (sensor.claims, ==, 0);
  g_assert_cmpuint (sensor.resets, ==, 0);
  g_assert_false (fp_device_is_open (device));
  finish_device (device);
}

static void
test_unknown_dmi (void)
{
  TestPlatform platform = platforms[0];

  g_autoptr(GError) error = NULL;
  FpDevice *device;

  platform.vendor = "UNVERIFIED";
  device = new_device_for_platform (&platform, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_cmpuint (sensor.opens, ==, 0);
  g_assert_cmpuint (sensor.chip_opens, ==, 0);
  g_assert_cmpuint (sensor.claims, ==, 0);
  finish_device (device);
}

static void
test_hardware_reset (gconstpointer data)
{
  guint scenario = GPOINTER_TO_UINT (data);
  FpDevice *device = new_device ();

  g_autoptr(GError) error = NULL;

  sensor.hardware_recovery = TRUE;
  sensor.cold_start = TRUE;
  sensor.cancel_hardware_reset = scenario == 1;
  sensor.fail_hardware_reset = scenario == 2;
  if (scenario == 2)
    g_test_expect_message ("libfprint-fte3600", G_LOG_LEVEL_WARNING,
                           "*Sensor reset after open failure also failed:*");

  if (scenario == 0)
    {
      g_assert_true (fp_device_open_sync (device, sensor.cancellable, &error));
      g_assert_no_error (error);
    }
  else
    {
      g_assert_false (fp_device_open_sync (device, sensor.cancellable, &error));
      g_assert_error (error, G_IO_ERROR,
                      (scenario == 1 ? G_IO_ERROR_CANCELLED : G_IO_ERROR_FAILED));
      g_assert_false (fp_device_is_open (device));
      g_assert_cmpint (sensor.spi_fd, ==, -1);
      g_assert_false (sensor.claimed);
    }

  if (scenario == 2)
    {
      g_test_assert_expected_messages ();
      g_assert_cmpuint (sensor.hardware_asserts, ==, 1);
      g_assert_cmpuint (sensor.hardware_deasserts, ==, 0);
    }
  else
    {
      /* Cancellation during the first pulse must not truncate either pulse. */
      g_assert_cmpuint (sensor.hardware_asserts, ==, 2);
      g_assert_cmpuint (sensor.hardware_deasserts, ==, 2);
    }
  finish_device (device);
}

static void
test_capture_error (gconstpointer data)
{
  guint scenario = GPOINTER_TO_UINT (data);
  gboolean cancel = scenario != 0;
  gboolean cancel_wait = scenario == 1 || scenario == 3;
  FpDevice *device = new_device ();

  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;

  open_device (device);
  sensor.irq_action = cancel_wait ? CANCEL_WAIT : DELIVER_FINGER;
  sensor.fail_image = !cancel;
  sensor.cancel_image = scenario == 2;
  sensor.fail_reset = scenario == 3;
  image = fp_device_capture_sync (device, TRUE, sensor.cancellable, &error);
  g_assert_null (image);
  g_assert_error (error, G_IO_ERROR, (cancel ? G_IO_ERROR_CANCELLED : G_IO_ERROR_FAILED));
  g_assert_cmpuint (sensor.resets, ==, 4);
  g_assert_cmpuint (sensor.images, ==, cancel_wait ? 0 : 1);
  g_assert_cmpint (fpi_device_get_current_action (device), ==, FPI_DEVICE_ACTION_NONE);

  /* Cancellation/error must leave the same open device usable again. */
  g_clear_error (&error);
  g_cancellable_reset (sensor.cancellable);
  sensor.irq_action = DELIVER_FINGER;
  sensor.fail_image = FALSE;
  sensor.cancel_image = FALSE;
  sensor.fail_reset = FALSE;
  image = fp_device_capture_sync (device, TRUE, sensor.cancellable, &error);
  g_assert_no_error (error);
  g_assert_nonnull (image);
  finish_device (device);
}

#if FTE3600_ENABLE_PERSONAL_AUTH
static void
test_enroll_cancel (void)
{
  FpDevice *device = new_device ();

  g_autoptr(FpPrint) print = g_object_ref_sink (fp_print_new (device));
  g_autoptr(FpPrint) enrolled = NULL;
  g_autoptr(GError) error = NULL;

  open_device (device);
  sensor.irq_action = CANCEL_WAIT;
  enrolled = fp_device_enroll_sync (device, print, sensor.cancellable,
                                    NULL, NULL, &error);
  g_assert_null (enrolled);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint (sensor.images, ==, 0);
  g_assert_cmpuint (sensor.resets, ==, 4);
  g_assert_cmpint (fpi_device_get_current_action (device), ==, FPI_DEVICE_ACTION_NONE);
  finish_device (device);
}
#endif

static void
test_open_error (gconstpointer data)
{
  guint failure = GPOINTER_TO_UINT (data);
  FpDevice *device = new_device ();

  g_autoptr(GError) error = NULL;

  sensor.fail_config = failure == 0;
  sensor.fail_claim = failure == 1;
  sensor.bad_id = failure == 2;
  g_assert_false (fp_device_open_sync (device, NULL, &error));
  g_assert_nonnull (error);
  g_assert_false (fp_device_is_open (device));
  g_assert_cmpint (sensor.spi_fd, ==, -1);
  g_assert_false (sensor.claimed);
  g_assert_cmpuint (sensor.claims, ==, sensor.releases);
  if (sensor.bad_id)
    {
      g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
      g_assert_cmpuint (sensor.resets, ==, 4);
    }

  sensor.fail_config = sensor.fail_claim = sensor.bad_id = FALSE;
  open_device (device);
  finish_device (device);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  /* Former production overrides must not bypass DMI or change requested GPIOs.
   * Set these before any worker threads are created, for the entire test run. */
  g_setenv ("FTE3600_FORCE_PROBE", "1", TRUE);
  g_setenv ("FTE3600_RESET_GPIO_OFFSET", "7", TRUE);
  g_setenv ("FTE3600_IRQ_GPIO_OFFSET", "8", TRUE);
  g_test_add_data_func ("/fte3600-lifecycle/capture-reopen", &platforms[0],
                        test_capture_reopen);
  g_test_add_data_func ("/fte3600-lifecycle/gpio/jasper-lake", &platforms[1],
                        test_capture_reopen);
  g_test_add_data_func ("/fte3600-lifecycle/gpio/tiger-lake", &platforms[2],
                        test_capture_reopen);
  g_test_add_data_func ("/fte3600-lifecycle/gpio/jasper-lake-product-alias", &platforms[3],
                        test_capture_reopen);
  g_test_add_data_func ("/fte3600-lifecycle/gpio/tiger-lake-product-alias", &platforms[4],
                        test_capture_reopen);
  g_test_add_data_func ("/fte3600-lifecycle/gpio/unknown-hid", "UNKNOWN",
                        test_unknown_controller_hid);
  g_test_add_data_func ("/fte3600-lifecycle/gpio/missing-hid", NULL,
                        test_unknown_controller_hid);
  g_test_add_data_func ("/fte3600-lifecycle/gpio/empty-hid", "",
                        test_unknown_controller_hid);
  g_test_add_func ("/fte3600-lifecycle/probe/unknown-dmi", test_unknown_dmi);
  g_test_add_data_func ("/fte3600-lifecycle/hardware-reset/recovery", GUINT_TO_POINTER (0),
                        test_hardware_reset);
  g_test_add_data_func ("/fte3600-lifecycle/hardware-reset/cancel", GUINT_TO_POINTER (1),
                        test_hardware_reset);
  g_test_add_data_func ("/fte3600-lifecycle/hardware-reset/error", GUINT_TO_POINTER (2),
                        test_hardware_reset);
  g_test_add_data_func ("/fte3600-lifecycle/cancel-wait", GINT_TO_POINTER (TRUE),
                        test_capture_error);
  g_test_add_data_func ("/fte3600-lifecycle/capture-error", GINT_TO_POINTER (FALSE),
                        test_capture_error);
  g_test_add_data_func ("/fte3600-lifecycle/cancel-transfer", GUINT_TO_POINTER (2),
                        test_capture_error);
  g_test_add_data_func ("/fte3600-lifecycle/cancel-cleanup-failure", GUINT_TO_POINTER (3),
                        test_capture_error);
#if FTE3600_ENABLE_PERSONAL_AUTH
  g_test_add_func ("/fte3600-lifecycle/enroll-cancel", test_enroll_cancel);
#endif
  g_test_add_data_func ("/fte3600-lifecycle/open/spi-error", GUINT_TO_POINTER (0),
                        test_open_error);
  g_test_add_data_func ("/fte3600-lifecycle/open/gpio-error", GUINT_TO_POINTER (1),
                        test_open_error);
  g_test_add_data_func ("/fte3600-lifecycle/open/id-error", GUINT_TO_POINTER (2),
                        test_open_error);
  return g_test_run ();
}
