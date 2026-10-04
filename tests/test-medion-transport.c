/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Synthetic syscall fixture: never opens a SPI device or GPIO controller.
 */
#include "examples/fte3600-medion-transport.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

enum { SPI_FD = 10, RESTORE_FD = 11, RESET_CHIP = 20, IRQ_CHIP = 21,
       RESET_LINE = 30, IRQ_LINE = 31 };

typedef struct
{
  FpiDeviceFte3600 self;
  guint64 open_fds;
  guint32 mode;
  guint32 speed;
  guint8 bits;
  const gchar *bufsiz;
  guint irq_requests;
  guint reset_requests;
  guint reset_value;
  gint request_failure;
  gboolean bits_failure;
  gboolean restore_failure;
  guint32 next_event;
  guint queued_events;
  gboolean truncated_event;
  gboolean interrupt_reads;
  guint read_calls;
} Fixture;

static Fixture *mock;

int __wrap_open64 (const char *path, int flags, ...);
int __wrap_fstat64 (int fd, struct stat64 *st);
int __wrap_stat64 (const char *path, struct stat64 *st);
int __wrap_fcntl64 (int fd, int cmd, ...);
int __wrap_ioctl (int fd, unsigned long cmd, ...);
int __wrap_close (int fd);
ssize_t __wrap_read (int fd, void *buf, size_t size);
ssize_t __wrap___read_chk (int fd, void *buf, size_t size, size_t capacity);
gboolean __wrap_g_file_get_contents (const gchar *filename, gchar **contents,
                                     gsize *length, GError **error);

static void
check_fd (int fd)
{
  g_assert_cmpint (fd, >=, 0);
  g_assert_cmpint (fd, <, 64);
  g_assert_true ((mock->open_fds & (G_GUINT64_CONSTANT (1) << fd)) != 0);
}

static int
new_fd (int fd)
{
  g_assert_true ((mock->open_fds & (G_GUINT64_CONSTANT (1) << fd)) == 0);
  mock->open_fds |= G_GUINT64_CONSTANT (1) << fd;
  return fd;
}

int
__wrap_open64 (const char *path, int flags, ...)
{
  g_assert_true ((flags & O_CLOEXEC) != 0);
  if (g_str_equal (path, "/diagnostic/reset-chip"))
    return new_fd (RESET_CHIP);
  g_assert_cmpstr (path, ==, "/diagnostic/irq-chip");
  return new_fd (IRQ_CHIP);
}

int
__wrap_fstat64 (int fd, struct stat64 *st)
{
  check_fd (fd);
  memset (st, 0, sizeof *st);
  st->st_mode = S_IFCHR | 0600;
  st->st_rdev = fd == SPI_FD ? 123 : 456 + fd;
  return 0;
}

int
__wrap_stat64 (const char *path, struct stat64 *st)
{
  g_assert_cmpstr (path, ==, "/diagnostic/spi");
  memset (st, 0, sizeof *st);
  st->st_mode = S_IFCHR | 0600;
  st->st_rdev = 123;
  return 0;
}

int
__wrap_fcntl64 (int fd, int cmd, ...)
{
  check_fd (fd);
  if (cmd == F_DUPFD_CLOEXEC)
    {
      g_assert_cmpint (fd, ==, SPI_FD);
      return new_fd (RESTORE_FD);
    }
  g_assert_true (fd == RESET_LINE || fd == IRQ_LINE);
  g_assert_true (cmd == F_SETFD || cmd == F_GETFL || cmd == F_SETFL);
  return 0;
}

int
__wrap_ioctl (int fd, unsigned long cmd, ...)
{
  va_list args;
  gpointer value;

  check_fd (fd);
  va_start (args, cmd);
  value = va_arg (args, gpointer);
  va_end (args);
  if (fd == SPI_FD || fd == RESTORE_FD)
    {
      switch (cmd)
        {
        case SPI_IOC_RD_MODE32:
          *(guint32 *) value = mock->mode;
          return 0;
        case SPI_IOC_RD_BITS_PER_WORD:
          *(guint8 *) value = mock->bits;
          return 0;
        case SPI_IOC_RD_MAX_SPEED_HZ:
          *(guint32 *) value = mock->speed;
          return 0;
        case SPI_IOC_WR_BITS_PER_WORD:
          if (mock->bits_failure && fd == SPI_FD)
            {
              errno = EIO;
              return -1;
            }
          mock->bits = *(guint8 *) value;
          return 0;
        case SPI_IOC_WR_MAX_SPEED_HZ:
          if (mock->restore_failure && fd == RESTORE_FD)
            {
              errno = EIO;
              return -1;
            }
          mock->speed = *(guint32 *) value;
          return 0;
        default:
          /* No mode/CS write and no SPI packet are allowed in configuration. */
          g_error ("Unexpected SPI ioctl %lu", cmd);
        }
    }
  if (cmd == GPIO_GET_CHIPINFO_IOCTL)
    {
      struct gpiochip_info *chip = value;

      chip->lines = 64;
      return 0;
    }
  if (cmd == GPIO_V2_GET_LINE_IOCTL)
    {
      struct gpio_v2_line_request *request = value;

      g_assert_cmpuint (request->num_lines, ==, 1);
      if (fd == IRQ_CHIP)
        {
          mock->irq_requests++;
          g_assert_cmpuint (request->offsets[0], ==, 0);
          g_assert_cmpuint (request->config.flags, ==,
                            GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING |
                            GPIO_V2_LINE_FLAG_BIAS_DISABLED);
          g_assert_cmpuint (request->event_buffer_size, ==, 256);
        }
      else
        {
          g_assert_cmpint (fd, ==, RESET_CHIP);
          mock->reset_requests++;
          g_assert_cmpuint (request->offsets[0], ==, 39);
          g_assert_cmpuint (request->config.flags, ==,
                            GPIO_V2_LINE_FLAG_OUTPUT | GPIO_V2_LINE_FLAG_ACTIVE_LOW);
          g_assert_cmpuint (request->config.num_attrs, ==, 1);
          g_assert_cmpuint (request->config.attrs[0].attr.id, ==,
                            GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES);
          g_assert_cmpuint (request->config.attrs[0].mask, ==, 1);
          /* Logical zero deasserts the ACTIVE_LOW reset at acquisition. */
          g_assert_cmpuint (request->config.attrs[0].attr.values, ==, 0);
          mock->reset_value = 0;
        }
      if (fd == mock->request_failure)
        {
          errno = EBUSY;
          return -1;
        }
      request->fd = new_fd (fd == IRQ_CHIP ? IRQ_LINE : RESET_LINE);
      return 0;
    }
  g_assert_cmpint (fd, ==, RESET_LINE);
  g_assert_cmpuint (cmd, ==, GPIO_V2_LINE_SET_VALUES_IOCTL);
  g_assert_cmpuint (((struct gpio_v2_line_values *) value)->mask, ==, 1);
  mock->reset_value = ((struct gpio_v2_line_values *) value)->bits;
  return 0;
}

int
__wrap_close (int fd)
{
  check_fd (fd);
  mock->open_fds &= ~(G_GUINT64_CONSTANT (1) << fd);
  return 0;
}

ssize_t
__wrap_read (int fd, void *buf, size_t size)
{
  struct gpio_v2_line_event *events = buf;
  guint count;

  check_fd (fd);
  g_assert_cmpint (fd, ==, IRQ_LINE);
  mock->read_calls++;
  if (mock->interrupt_reads)
    {
      errno = EINTR;
      return -1;
    }
  if (mock->truncated_event)
    return 3;
  if (!mock->queued_events)
    {
      errno = EAGAIN;
      return -1;
    }
  count = MIN (mock->queued_events, size / sizeof *events);
  for (guint i = 0; i < count; i++)
    {
      events[i] = (struct gpio_v2_line_event) {
        .id = GPIO_V2_LINE_EVENT_RISING_EDGE,
        .offset = 0,
        .seqno = mock->next_event,
        .line_seqno = mock->next_event,
      };
      mock->next_event++;
    }
  mock->queued_events -= count;
  return count * sizeof *events;
}

ssize_t
__wrap___read_chk (int fd, void *buf, size_t size, size_t capacity)
{
  g_assert_cmpuint (size, <=, capacity);
  return __wrap_read (fd, buf, size);
}

gboolean
__wrap_g_file_get_contents (const gchar *filename, gchar **contents,
                            gsize *length, GError **error)
{
  (void) error;
  g_assert_cmpstr (filename, ==, "/sys/module/spidev/parameters/bufsiz");
  *contents = g_strdup (mock->bufsiz);
  if (length)
    *length = strlen (*contents);
  return TRUE;
}

static void
setup (Fixture *f, gconstpointer unused)
{
  const Fte3600MedionTransportConfig config = {
    .spi_path = "/diagnostic/spi",
    .reset_gpiochip = "/diagnostic/reset-chip",
    .irq_gpiochip = "/diagnostic/irq-chip",
  };
  g_autoptr(GError) error = NULL;

  (void) unused;
  mock = f;
  f->self.spi_fd = -1;
  f->speed = 2000000;
  f->bits = 16;
  f->bufsiz = "65536\n";
  f->request_failure = -1;
  f->next_event = 1;
  g_assert_true (fte3600_medion_transport_attach (&f->self, &config, &error));
  g_assert_no_error (error);
  f->self.spi_fd = new_fd (SPI_FD);
}

static void
teardown (Fixture *f, gconstpointer unused)
{
  g_autoptr(GError) error = NULL;

  (void) unused;
  if (f->self.spi_fd >= 0)
    __wrap_close (f->self.spi_fd);
  f->self.spi_fd = -1;
  fte3600_medion_transport_detach (&f->self, &error);
  g_assert_cmpuint (f->open_fds, ==, 0);
  mock = NULL;
}

static void
configured (Fixture *f)
{
  g_autoptr(GError) error = NULL;

  g_assert_true (f->self.transport_ops->configure (&f->self, &error));
  g_assert_no_error (error);
  g_assert_cmpint (f->self.transport_ops->irq_fd (&f->self), ==, IRQ_LINE);
  g_assert_cmpuint (f->self.bridge_capabilities, ==, 0);
}

static void
test_configuration (Fixture *f, gconstpointer unused)
{
  g_autoptr(GError) error = NULL;
  guint32 events = 99;

  (void) unused;
  configured (f);
  g_assert_cmpuint (f->speed, ==, 1000000);
  g_assert_cmpuint (f->bits, ==, 8);
  g_assert_cmpuint (f->self.max_transfer, ==, 32768);
  g_assert_true (f->self.transport_ops->set_reset (&f->self, TRUE, &error));
  g_assert_cmpuint (f->reset_value, ==, 1);
  g_assert_true (f->self.transport_ops->set_reset (&f->self, FALSE, &error));
  g_assert_cmpuint (f->reset_value, ==, 0);
  gboolean drained = f->self.transport_ops->get_events (&f->self, &events, &error);
  g_assert_no_error (error);
  g_assert_true (drained);
  g_assert_cmpuint (events, ==, 0);
  f->queued_events = 3;
  g_assert_true (f->self.transport_ops->get_events (&f->self, &events, &error));
  g_assert_cmpuint (events, ==, 1);
  g_assert_no_error (error);
}

static void
test_probe_reopen (Fixture *f, gconstpointer unused)
{
  (void) unused;
  configured (f);
  __wrap_close (f->self.spi_fd);
  f->self.spi_fd = -1;
  f->self.transport_ops->release (&f->self);
  f->self.transport_ops->release (&f->self);
  g_assert_cmpuint (f->open_fds, ==, 0);
  g_assert_cmpuint (f->speed, ==, 2000000);
  g_assert_cmpuint (f->bits, ==, 16);
  f->self.spi_fd = new_fd (SPI_FD);
  configured (f);
  g_assert_cmpuint (f->irq_requests, ==, 2);
  g_assert_cmpuint (f->reset_requests, ==, 2);
}

static void
test_smaller_limits (Fixture *f, gconstpointer unused)
{
  (void) unused;
  f->bufsiz = "4096\n";
  f->speed = 500000;
  configured (f);
  g_assert_cmpuint (f->self.max_transfer, ==, 4096);
  g_assert_cmpuint (f->speed, ==, 500000);
}

static void
test_setup_failure (Fixture *f, gconstpointer failure)
{
  g_autoptr(GError) error = NULL;

  f->request_failure = GPOINTER_TO_INT (failure);
  if (f->request_failure == SPI_FD)
    f->bits_failure = TRUE;
  g_assert_false (f->self.transport_ops->configure (&f->self, &error));
  g_assert_nonnull (error);
  g_assert_cmpuint (f->open_fds, ==, G_GUINT64_CONSTANT (1) << SPI_FD);
  g_assert_cmpuint (f->speed, ==, 2000000);
  g_assert_cmpuint (f->bits, ==, 16);
  if (f->request_failure != RESET_CHIP)
    g_assert_cmpuint (f->reset_requests, ==, 0);
}

static void
test_bad_mode (Fixture *f, gconstpointer mode)
{
  g_autoptr(GError) error = NULL;

  f->mode = GPOINTER_TO_UINT (mode);
  g_assert_false (f->self.transport_ops->configure (&f->self, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_cmpuint (f->irq_requests, ==, 0);
  g_assert_cmpuint (f->reset_requests, ==, 0);
  g_assert_cmpuint (f->speed, ==, 2000000);
  g_assert_cmpuint (f->bits, ==, 16);
}

static void
test_bad_events (Fixture *f, gconstpointer reason)
{
  g_autoptr(GError) error = NULL;
  guint32 events;

  configured (f);
  switch (GPOINTER_TO_INT (reason))
    {
    case 0:
      f->next_event = 0;
      f->queued_events = 1;
      break;
    case 1:
      f->next_event = 2;
      f->queued_events = 1;
      break;
    case 2:
      f->queued_events = 1;
      g_assert_true (f->self.transport_ops->get_events (&f->self, &events, &error));
      f->next_event = 3;
      f->queued_events = 1;
      break;
    case 3:
      f->truncated_event = TRUE;
      break;
    case 4:
      f->queued_events = 512;
      break;
    case 5:
      f->interrupt_reads = TRUE;
      break;
    default:
      g_assert_not_reached ();
    }
  g_assert_false (f->self.transport_ops->get_events (&f->self, &events, &error));
  g_assert_nonnull (error);
  g_assert_cmpuint (f->read_calls, <=, 256);
}

static void
test_cleanup_error (Fixture *f, gconstpointer unused)
{
  g_autoptr(GError) error = NULL;

  (void) unused;
  configured (f);
  f->restore_failure = TRUE;
  f->self.transport_ops->release (&f->self);
  f->self.transport_ops->release (&f->self);
  g_assert_false (f->self.transport_ops->configure (&f->self, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);
  __wrap_close (f->self.spi_fd);
  f->self.spi_fd = -1;
  g_assert_false (fte3600_medion_transport_detach (&f->self, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_null (f->self.transport_ops);
  g_assert_null (f->self.transport_data);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add ("/medion-transport/configuration", Fixture, NULL, setup, test_configuration, teardown);
  g_test_add ("/medion-transport/probe-reopen", Fixture, NULL, setup, test_probe_reopen, teardown);
  g_test_add ("/medion-transport/smaller-limits", Fixture, NULL, setup, test_smaller_limits, teardown);
  g_test_add ("/medion-transport/fail-spi", Fixture, GINT_TO_POINTER (SPI_FD), setup, test_setup_failure, teardown);
  g_test_add ("/medion-transport/fail-irq", Fixture, GINT_TO_POINTER (IRQ_CHIP), setup, test_setup_failure, teardown);
  g_test_add ("/medion-transport/fail-reset", Fixture, GINT_TO_POINTER (RESET_CHIP), setup, test_setup_failure, teardown);
  g_test_add ("/medion-transport/reject-high-cs", Fixture, GUINT_TO_POINTER (SPI_CS_HIGH), setup, test_bad_mode, teardown);
  g_test_add ("/medion-transport/reject-clock-mode", Fixture, GUINT_TO_POINTER (SPI_MODE_1), setup, test_bad_mode, teardown);
  g_test_add ("/medion-transport/event-zero", Fixture, GINT_TO_POINTER (0), setup, test_bad_events, teardown);
  g_test_add ("/medion-transport/initial-overflow", Fixture, GINT_TO_POINTER (1), setup, test_bad_events, teardown);
  g_test_add ("/medion-transport/event-gap", Fixture, GINT_TO_POINTER (2), setup, test_bad_events, teardown);
  g_test_add ("/medion-transport/truncated-event", Fixture, GINT_TO_POINTER (3), setup, test_bad_events, teardown);
  g_test_add ("/medion-transport/event-budget", Fixture, GINT_TO_POINTER (4), setup, test_bad_events, teardown);
  g_test_add ("/medion-transport/interruption-budget", Fixture, GINT_TO_POINTER (5), setup, test_bad_events, teardown);
  g_test_add ("/medion-transport/cleanup-error", Fixture, NULL, setup, test_cleanup_error, teardown);
  return g_test_run ();
}
