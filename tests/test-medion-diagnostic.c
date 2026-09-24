/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exercise the diagnostic's actual helpers without opening any hardware. */
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <gpiod.h>
#include <gudev/gudev.h>
#include <linux/spi/spidev.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/* Keep the independent PM helper's declarations outside the syscall macros.
 * Full CLI tests mock its interface; its filesystem behavior has separate tests. */
#include "../tools/medion-power.h"
#include "../libfprint/drivers/fte3600-gpio.h"

enum MockResult { MOCK_OK, MOCK_ERROR, MOCK_SHORT };
static enum MockResult transfer_result;
static guint transfer_count;
static guint gpio_count;
static guint gpio_fail_at;
static int gpio_readback;
static gsize firmware_size;
static gsize firmware_read;
static GString *events;
static guint8 last_tx[16];
static gsize last_tx_size;
static guint32 last_speed;
static const guint8 *expected_firmware;
static guint firmware_packets;
static GPtrArray *tx_frames;
static guint32 expected_speed;
static gboolean chip_probe_responses;
static gboolean status_observation;
static guint16 mock_mcu_status;
static guint8 mock_boot_edition;
static guint16 mock_chip_id;
static gboolean full_cli;
static gboolean mock_power_active;
static guint mock_config_writes;
static guint mock_config_fail_at;
static gboolean mock_restore_failure;
static gboolean mock_assert_restored;
static guint32 mock_spi_mode;
static guint8 mock_spi_bits;
static guint8 mock_spi_lsb;
static guint mock_cli_expected_transfers;
#define ORIGINAL_MODE (SPI_MODE_3 | SPI_CS_HIGH | SPI_LSB_FIRST | SPI_RX_DUAL)
#define ORIGINAL_BITS 16
#define ORIGINAL_LSB 1
#define MOCK_SPI_SYSFS "/sys/devices/test-controller/spi-FTE3600:00"

static int mock_ioctl (int           fd,
                       unsigned long request,
                       ...);
static int mock_gpio_set (struct gpiod_line_request *request,
                          unsigned int               offset,
                          enum gpiod_line_value      value);
static enum gpiod_line_value mock_gpio_get (struct gpiod_line_request *request,
                                            unsigned int               offset);
static void mock_gpio_release (struct gpiod_line_request *request);
static void mock_sleep (gulong usec);
static int mock_open (const char *path,
                      int         flags,
                      ...);
static int mock_fstat (int          fd,
                       struct stat *st);
static ssize_t mock_read (int    fd,
                          void  *buffer,
                          size_t count);
static int mock_close (int fd);
static gboolean mock_file_get_contents (const gchar *filename,
                                        gchar      **contents,
                                        gsize       *length,
                                        GError     **error);
static GUdevClient *mock_udev_client_new (const gchar * const *subsystems);
static GList *mock_udev_query (GUdevClient *client,
                               const gchar *subsystem);
static GUdevDevice *mock_udev_parent (GUdevDevice *device);
static const gchar *mock_udev_sysfs (GUdevDevice *device);
static const gchar *mock_udev_file (GUdevDevice *device);
static char *mock_realpath (const char *path,
                            char       *resolved);
static gboolean mock_power_prepare (MedionPower *power,
                                    const gchar *device_path,
                                    const gchar *root,
                                    GError     **error);
static gboolean mock_power_restore (MedionPower *power,
                                    GError     **error);
static void mock_power_report (MedionPower *power,
                               const gchar *tag);
static struct gpiod_chip *mock_gpio_open (const char *path);
static int medion_diagnostic_main (int    argc,
                                   char **argv);

#define main medion_diagnostic_main
#define ioctl mock_ioctl
#define gpiod_line_request_set_value mock_gpio_set
#define gpiod_line_request_get_value mock_gpio_get
#define gpiod_line_request_release mock_gpio_release
#define gpiod_chip_open mock_gpio_open
#define g_usleep mock_sleep
#define open mock_open
#define fstat mock_fstat
#define read mock_read
#define close mock_close
#define g_file_get_contents mock_file_get_contents
#define g_udev_client_new mock_udev_client_new
#define g_udev_client_query_by_subsystem mock_udev_query
#define g_udev_device_get_parent mock_udev_parent
#define g_udev_device_get_sysfs_path mock_udev_sysfs
#define g_udev_device_get_device_file mock_udev_file
#define realpath mock_realpath
#define medion_power_prepare mock_power_prepare
#define medion_power_restore mock_power_restore
#define medion_power_report mock_power_report
#include "../tools/test_medion_e3224.c"
#undef main
#undef ioctl
#undef gpiod_line_request_set_value
#undef gpiod_line_request_get_value
#undef gpiod_line_request_release
#undef gpiod_chip_open
#undef g_usleep
#undef open
#undef fstat
#undef read
#undef close
#undef g_file_get_contents
#undef g_udev_client_new
#undef g_udev_client_query_by_subsystem
#undef g_udev_device_get_parent
#undef g_udev_device_get_sysfs_path
#undef g_udev_device_get_device_file
#undef realpath
#undef medion_power_prepare
#undef medion_power_restore
#undef medion_power_report

static void
reset_mocks (void)
{
  transfer_result = MOCK_OK;
  transfer_count = gpio_count = gpio_fail_at = 0;
  gpio_readback = 1;
  firmware_size = FW_SIZE;
  firmware_read = 0;
  interrupted = 0;
  req_gpo1 = NULL;
  spi_fd = -1;
  cleanup_failed = FALSE;
  memset (&saved_spi, 0, sizeof (saved_spi));
  full_cli = mock_power_active = mock_restore_failure = mock_assert_restored = FALSE;
  mock_config_writes = mock_config_fail_at = mock_cli_expected_transfers = 0;
  mock_spi_mode = ORIGINAL_MODE;
  mock_spi_bits = ORIGINAL_BITS;
  mock_spi_lsb = ORIGINAL_LSB;
  g_assert_null (power_state.nodes);
  if (events)
    g_string_free (events, TRUE);
  events = g_string_new (NULL);
  memset (last_tx, 0, sizeof (last_tx));
  last_tx_size = last_speed = 0;
  expected_firmware = NULL;
  firmware_packets = 0;
  cur_speed_hz = 1000000;
  expected_speed = 0;
  chip_probe_responses = FALSE;
  status_observation = FALSE;
  mock_mcu_status = mock_boot_edition = mock_chip_id = 0;
  g_clear_pointer (&tx_frames, g_ptr_array_unref);
  tx_frames = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
}

static int
mock_ioctl (int fd, unsigned long request, ...)
{
  (void) fd;
  va_list args;
  va_start (args, request);
  void *value = va_arg (args, void *);
  va_end (args);
  if (request != SPI_IOC_MESSAGE (1))
    {
      g_assert_true (full_cli);
      g_assert_cmpint (fd, ==, 43);
      /* A per-transfer speed never writes the shared max-speed default. */
      g_assert_cmpuint (request, !=, SPI_IOC_WR_MAX_SPEED_HZ);
      switch (request)
        {
        case SPI_IOC_RD_MODE32: *(guint32 *) value = mock_spi_mode;
          return 0;

        case SPI_IOC_RD_MODE: *(guint8 *) value = mock_spi_mode & 0xff;
          return 0;

        case SPI_IOC_RD_BITS_PER_WORD: *(guint8 *) value = mock_spi_bits;
          return 0;

        case SPI_IOC_RD_LSB_FIRST: *(guint8 *) value = mock_spi_lsb;
          return 0;

        case SPI_IOC_WR_MODE32:
          if (mock_restore_failure)
            {
              errno = EIO;
              return -1;
            }
          mock_spi_mode = *(guint32 *) value;
          mock_spi_lsb = !!(mock_spi_mode & SPI_LSB_FIRST);
          return 0;

        case SPI_IOC_WR_MODE:
          /* spidev treats this byte as the complete user-mode value, not a
           * masked update. In particular, it clears SPI_RX_DUAL above bit 7.
           * Preserve no fixture flags: all ORIGINAL_MODE bits are user bits. */
          mock_spi_mode = *(guint8 *) value;
          mock_spi_lsb = !!(mock_spi_mode & SPI_LSB_FIRST);
          break;

        case SPI_IOC_WR_BITS_PER_WORD: mock_spi_bits = *(guint8 *) value;
          break;

        case SPI_IOC_WR_LSB_FIRST:
          mock_spi_lsb = *(guint8 *) value;
          mock_spi_mode = (mock_spi_mode & ~SPI_LSB_FIRST) |
                          (mock_spi_lsb ? SPI_LSB_FIRST : 0);
          break;

        default: g_assert_not_reached ();
        }
      mock_config_writes++;
      if (mock_config_fail_at && mock_config_writes == mock_config_fail_at)
        {
          errno = EIO;
          return -1;
        }
      return 0;
    }
  struct spi_ioc_transfer *transfer = value;
  if (full_cli)
    {
      /* The observation must use the requested configuration; cleanup must
       * later restore the full original mode, including its high flags. */
      g_assert_cmpint (fd, ==, 43);
      g_assert_cmpuint (mock_spi_mode, ==, SPI_MODE_0);
      g_assert_cmpuint (mock_spi_bits, ==, 8);
      g_assert_cmpuint (mock_spi_lsb, ==, 0);
    }
  transfer_count++;
  g_assert_cmpuint (transfer->bits_per_word, ==, 8);
  last_tx_size = MIN ((gsize) transfer->len, sizeof (last_tx));
  memcpy (last_tx, (const void *) (uintptr_t) transfer->tx_buf, last_tx_size);
  last_speed = transfer->speed_hz;
  if (expected_speed)
    g_assert_cmpuint (last_speed, ==, expected_speed);
  g_ptr_array_add (tx_frames, g_bytes_new ((const void *) (uintptr_t) transfer->tx_buf,
                                           transfer->len));
  g_string_append_printf (events, "spi:%02x/%u;", last_tx[0], transfer->len);
  if (expected_firmware && transfer->len == FW_SIZE + 7)
    {
      const guint8 *packet = (const guint8 *) (uintptr_t) transfer->tx_buf;
      const guint8 header[] = { 0x05, 0xfa, 0, 0, 0x28, 0x9c };
      g_assert_cmpmem (packet, sizeof (header), header, sizeof (header));
      g_assert_cmpmem (packet + sizeof (header), FW_SIZE, expected_firmware, FW_SIZE);
      g_assert_cmpuint (packet[FW_SIZE + 6], ==, 0);
      g_assert_cmpuint (transfer->rx_buf, ==, 0);
      firmware_packets++;
    }
  if (transfer_result == MOCK_ERROR)
    {
      errno = EIO;
      return -1;
    }
  if (transfer_result == MOCK_SHORT)
    return transfer->len - 1;
  if (transfer->rx_buf)
    {
      guint8 *rx = (void *) (uintptr_t) transfer->rx_buf;
      memset (rx, 0x5a, transfer->len);
      if (status_observation)
        {
          g_assert_cmpuint (transfer->len, ==, 6);
          g_assert_cmpuint (last_tx[0], ==, 0x10);
          g_assert_cmpuint (last_tx[1], ==, 0xef);
          g_assert_true (last_tx[2] == 0x20 || last_tx[2] == 0x14);
          memset (rx, 0, transfer->len);
          if (last_tx[2] == 0x20)
            {
              rx[4] = mock_mcu_status >> 8;
              rx[5] = mock_mcu_status & 0xff;
            }
        }
      if (chip_probe_responses)
        {
          if (last_tx[0] == 0x10)
            {
              g_assert_cmpuint (transfer->len, ==, 6);
              g_assert_cmpuint (last_tx[2], ==, 0x20);
              rx[4] = mock_mcu_status >> 8;
              rx[5] = mock_mcu_status & 0xff;
            }
          else if (last_tx[0] == 0x90)
            {
              rx[2] = mock_boot_edition;
            }
          else if (last_tx[0] == 0x04)
            {
              g_assert_cmpuint (transfer->len, ==, 8);
              /* Dummy bytes must not be mistaken for the chip ID. */
              rx[4] = 0xde;
              rx[5] = 0xad;
              rx[6] = mock_chip_id >> 8;
              rx[7] = mock_chip_id & 0xff;
            }
          else
            {
              g_assert_not_reached ();
            }
        }
    }
  return transfer->len;
}

static int
mock_gpio_set (struct gpiod_line_request *request, unsigned int offset,
               enum gpiod_line_value value)
{
  g_assert_nonnull (request);
  g_assert_cmpuint (offset, ==, PIN_GPO1_RESET);
  gpio_count++;
  g_string_append_printf (events, "gpio:%d;", value);
  if (gpio_fail_at && gpio_count == gpio_fail_at)
    {
      errno = EIO;
      return -1;
    }
  return 0;
}

static enum gpiod_line_value
mock_gpio_get (struct gpiod_line_request *request, unsigned int offset)
{
  g_assert_nonnull (request);
  g_assert_cmpuint (offset, ==, PIN_GPO1_RESET);
  if (gpio_readback < 0)
    errno = EIO;
  return gpio_readback;
}

static void
mock_gpio_release (struct gpiod_line_request *request)
{
  g_assert_nonnull (request);
  g_string_append (events, "release;");
}

static void
mock_sleep (gulong usec)
{
  g_string_append_printf (events, "sleep:%lu;", usec);
}

static int
mock_open (const char *path, int flags, ...)
{
  if (full_cli)
    {
      g_assert_cmpstr (path, ==, "/dev/test-spidev");
      g_assert_cmpint (flags & O_ACCMODE, ==, O_RDWR);
      return 43;
    }
  g_assert_cmpstr (path, ==, "/test/invalid-firmware");
  g_assert_cmpint (flags & O_ACCMODE, ==, O_RDONLY);
  return 42;
}

static int
mock_fstat (int fd, struct stat *st)
{
  if (full_cli)
    {
      g_assert_cmpint (fd, ==, 43);
      memset (st, 0, sizeof (*st));
      st->st_mode = S_IFCHR | 0600;
      st->st_rdev = makedev (153, 0);
      return 0;
    }
  g_assert_cmpint (fd, ==, 42);
  memset (st, 0, sizeof (*st));
  st->st_mode = S_IFREG | 0600;
  st->st_size = firmware_size;
  return 0;
}

static ssize_t
mock_read (int fd, void *buffer, size_t count)
{
  g_assert_cmpint (fd, ==, 42);
  gsize size = MIN (count, firmware_size - firmware_read);
  memset (buffer, 0, size);
  firmware_read += size;
  return size;
}

static int
mock_close (int fd)
{
  if (full_cli)
    {
      g_assert_cmpint (fd, ==, 43);
      if (mock_assert_restored)
        {
          if (!mock_restore_failure)
            g_assert_cmpuint (mock_spi_mode, ==, ORIGINAL_MODE);
          g_assert_cmpuint (mock_spi_bits, ==, ORIGINAL_BITS);
          g_assert_cmpuint (mock_spi_lsb, ==, ORIGINAL_LSB);
          g_assert_null (req_gpo1);
          g_assert_cmpuint (gpio_count, ==, 0);
          g_assert_cmpuint (firmware_packets, ==, 0);
          g_assert_cmpuint (transfer_count, ==, mock_cli_expected_transfers);
          for (guint i = 0; i < tx_frames->len; i++)
            {
              gsize size;
              const guint8 *frame = g_bytes_get_data (g_ptr_array_index (tx_frames, i), &size);
              const guint8 expected[] = { 0x10, 0xef, i == 0 ? 0x20 : 0x14, 0, 0, 0 };
              g_assert_cmpmem (frame, size, expected, sizeof (expected));
            }
          puts (mock_restore_failure ? "MOCK REMAINING SPI RESTORES VERIFIED" :
                "MOCK SPI RESTORE VERIFIED");
        }
      return 0;
    }
  g_assert_cmpint (fd, ==, 42);
  return 0;
}

static gboolean
mock_file_get_contents (const gchar *filename, gchar **contents,
                        gsize *length, GError **error)
{
  (void) error;
  if (full_cli && g_str_equal (filename, "/sys/module/spidev/parameters/bufsiz"))
    {
      *contents = g_strdup ("32768\n");
      if (length)
        *length = strlen (*contents);
      return TRUE;
    }
  static const struct { const gchar *file;
                        const gchar *value;
  } dmi[] = {
    { "/sys/class/dmi/id/sys_vendor", "MEDION" },
    { "/sys/class/dmi/id/product_name", "E3224" },
    { "/sys/class/dmi/id/product_version", "FT" },
    { "/sys/class/dmi/id/board_name", "YS13G" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (dmi); i++)
    if (g_str_equal (filename, dmi[i].file))
      {
        *contents = g_strdup (dmi[i].value);
        if (length)
          *length = strlen (*contents);
        return TRUE;
      }
  g_error ("Unexpected hardware/sysfs read: %s", filename);
}

static GUdevClient *
mock_udev_client_new (const gchar * const *subsystems)
{
  if (full_cli)
    {
      g_assert_cmpstr (subsystems[0], ==, "spidev");
      g_assert_null (subsystems[1]);
      return (GUdevClient *) g_object_new (G_TYPE_OBJECT, NULL);
    }
  g_error ("Unexpected hardware discovery before input validation");
  return NULL;
}

static GList *
mock_udev_query (GUdevClient *client, const gchar *subsystem)
{
  g_assert_true (full_cli);
  g_assert_nonnull (client);
  g_assert_cmpstr (subsystem, ==, "spidev");
  return g_list_append (NULL, g_object_new (G_TYPE_OBJECT, NULL));
}

static GUdevDevice *
mock_udev_parent (GUdevDevice *device)
{
  g_assert_true (full_cli);
  g_assert_nonnull (device);
  return (GUdevDevice *) g_object_new (G_TYPE_OBJECT, NULL);
}

static const gchar *
mock_udev_sysfs (GUdevDevice *device)
{
  g_assert_true (full_cli);
  g_assert_nonnull (device);
  return MOCK_SPI_SYSFS;
}

static const gchar *
mock_udev_file (GUdevDevice *device)
{
  g_assert_true (full_cli);
  g_assert_nonnull (device);
  return "/dev/test-spidev";
}

static char *
mock_realpath (const char *path, char *resolved)
{
  g_assert_true (full_cli);
  g_assert_null (resolved);
  g_assert_true (g_str_equal (path, "/sys/bus/spi/devices/spi-FTE3600:00") ||
                 g_str_equal (path, "/sys/dev/char/153:0/device"));
  return g_strdup (MOCK_SPI_SYSFS);
}

static gboolean
mock_power_prepare (MedionPower *power, const gchar *device_path,
                    const gchar *root, GError **error)
{
  (void) error;
  g_assert_true (full_cli);
  g_assert_true (power == &power_state);
  g_assert_cmpstr (device_path, ==, MOCK_SPI_SYSFS);
  g_assert_cmpstr (root, ==, "/sys/devices");
  mock_power_active = TRUE;
  return TRUE;
}

static gboolean
mock_power_restore (MedionPower *power, GError **error)
{
  (void) error;
  g_assert_true (power == &power_state);
  if (mock_power_active)
    {
      g_assert_cmpint (spi_fd, ==, -1);
      mock_power_active = FALSE;
      puts ("MOCK PM RESTORE VERIFIED");
    }
  return TRUE;
}

static void
mock_power_report (MedionPower *power, const gchar *tag)
{
  (void) tag;
  g_assert_true (power == &power_state);
  g_assert_true (full_cli);
}

static struct gpiod_chip *
mock_gpio_open (const char *path)
{
  g_error ("Unexpected hardware GPIO access: %s", path);
  return NULL;
}

static void
test_transfer_success (void)
{
  reset_mocks ();
  guint8 tx[] = { 0x10, 0xef, 0x20, 0, 0, 0 }, rx[6] = { 0 };
  spi_xfer (tx, rx, sizeof (tx), 250000);
  g_assert_cmpuint (transfer_count, ==, 1);
  g_assert_cmpuint (last_speed, ==, 250000);
  g_assert_cmpmem (last_tx, last_tx_size, tx, sizeof (tx));
  for (guint i = 0; i < sizeof (rx); i++)
    g_assert_cmpuint (rx[i], ==, 0x5a);
}

static void
test_transfer_failure (gconstpointer data)
{
  if (g_test_subprocess ())
    {
      reset_mocks ();
      transfer_result = GPOINTER_TO_INT (data);
      probe_status_and_id ("must not print a status", 1000000);
      puts ("UNEXPECTED CONTINUATION");
      exit (0);
    }
  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_failed ();
  g_test_trap_assert_stderr ("*SPI transfer failed: len=6,*");
  g_test_trap_assert_stdout_unmatched ("*Status 0x20*");
  g_test_trap_assert_stdout_unmatched ("*UNEXPECTED CONTINUATION*");
}

static void
test_reset_then_sync (void)
{
  reset_mocks ();
  req_gpo1 = (struct gpiod_line_request *) (uintptr_t) 1;
  windows_reset_pulse ();
  guint8 sync[] = { 0x55, 0xaa };
  spi_xfer (sync, NULL, sizeof (sync), 0);
  g_assert_cmpstr (events->str, ==,
                   "gpio:1;sleep:10000;gpio:0;sleep:20000;gpio:1;spi:55/2;");
  g_assert_cmpuint (last_speed, ==, 1000000);
  g_assert_cmpmem (last_tx, last_tx_size, sync, sizeof (sync));
  req_gpo1 = NULL;
}

static void
test_recovery_sequence (void)
{
  reset_mocks ();
  cur_speed_hz = expected_speed = 250000;
  guint8 firmware[FW_SIZE];
  for (guint i = 0; i < FW_SIZE; i++)
    firmware[i] = (i * 13) & 0xff;
  expected_firmware = firmware;
  req_gpo1 = (struct gpiod_line_request *) (uintptr_t) 1;
  transfer_firmware_and_start (firmware);
  /* Exact command order rejects extra unlock/config writes or any extra
   * delay/register read in the bootloader-entry window. */
  g_assert_cmpstr (events->str, ==,
                   "gpio:1;sleep:10000;gpio:0;sleep:20000;gpio:1;"
                   "spi:55/2;spi:05/10403;sleep:2000;"
                   "gpio:1;sleep:10000;gpio:0;sleep:20000;gpio:1;sleep:10000;"
                   "gpio:1;sleep:10000;gpio:0;sleep:20000;gpio:1;sleep:160000;"
                   "spi:70/1;sleep:5000;spi:70/1;sleep:2000;");
  g_assert_cmpuint (firmware_packets, ==, 1);
  g_assert_cmpuint (transfer_count, ==, 4);
  req_gpo1 = NULL;
  expected_firmware = NULL;
}

static void
test_gpio_failure (void)
{
  if (g_test_subprocess ())
    {
      reset_mocks ();
      req_gpo1 = (struct gpiod_line_request *) (uintptr_t) 1;
      gpio_fail_at = 2;
      windows_reset_pulse ();
      puts ("UNEXPECTED CONTINUATION");
      exit (0);
    }
  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_failed ();
  g_test_trap_assert_stderr ("*set reset GPIO value*");
  g_test_trap_assert_stdout_unmatched ("*UNEXPECTED CONTINUATION*");
}

static void
test_gpio_readback (gconstpointer data)
{
  if (g_test_subprocess ())
    {
      reset_mocks ();
      req_gpo1 = (struct gpiod_line_request *) (uintptr_t) 1;
      gpio_readback = GPOINTER_TO_INT (data);
      report_reset_value (1);
      puts ("UNEXPECTED CONTINUATION");
      exit (0);
    }
  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_failed ();
  g_test_trap_assert_stderr (GPOINTER_TO_INT (data) < 0 ?
                             "*read back reset GPIO value*" :
                             "*Reset GPIO readback does not match*");
  g_test_trap_assert_stdout_unmatched ("*UNEXPECTED CONTINUATION*");
}

static void
test_cli (gconstpointer data)
{
  const gchar *option = data;

  if (g_test_subprocess ())
    {
      reset_mocks ();
      gchar *argv[] = { (gchar *) "diagnostic", (gchar *) option, NULL };
      exit (medion_diagnostic_main (2, argv));
    }
  g_test_trap_subprocess (NULL, 0, 0);
  if (g_str_equal (option, "--help"))
    g_test_trap_assert_passed ();
  else
    g_test_trap_assert_failed ();
  g_test_trap_assert_stdout ("*Usage: diagnostic*");
  g_test_trap_assert_stderr ("");
}

static void
test_invalid_firmware (gconstpointer data)
{
  gboolean wrong_size = GPOINTER_TO_INT (data);

  if (g_test_subprocess ())
    {
      reset_mocks ();
      if (wrong_size)
        firmware_size = FW_SIZE - 1;
      gchar *argv[] = { (gchar *) "diagnostic", (gchar *) "--test-vendor-recovery",
                        (gchar *) "/test/invalid-firmware", NULL };
      exit (medion_diagnostic_main (3, argv));
    }
  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_failed ();
  g_test_trap_assert_stderr (wrong_size ? "*Firmware must be a regular 10396-byte file*" :
                             "*Firmware size or SHA256 mismatch*");
  g_test_trap_assert_stderr_unmatched ("*Unexpected hardware*");
  g_test_trap_assert_stdout_unmatched ("*Successfully claimed*");
}

static void
test_register_framing (void)
{
  reset_mocks ();
  cur_speed_hz = expected_speed = 500000;
  g_assert_cmpuint (query_bootloader_edition (), ==, 0x5a);
  const guint8 query[] = { 0x90, 0, 0 };
  g_assert_cmpmem (last_tx, last_tx_size, query, sizeof (query));
  g_assert_cmpuint (read_mcu_reg (0x20), ==, 0x5a);
  const guint8 read_reg[] = { 0x10, 0xef, 0x20, 0, 0 };
  g_assert_cmpmem (last_tx, last_tx_size, read_reg, sizeof (read_reg));
  write_mcu_reg (0x30, 0xbb);
  const guint8 write_reg[] = { 0x11, 0xee, 0x30, 0xbb, 0 };
  g_assert_cmpmem (last_tx, last_tx_size, write_reg, sizeof (write_reg));
}

static void
assert_tx_frame (guint index, const guint8 *expected, gsize size)
{
  gsize actual_size;
  const guint8 *actual = g_bytes_get_data (g_ptr_array_index (tx_frames, index), &actual_size);

  g_assert_cmpmem (actual, actual_size, expected, size);
}

static void
test_chip_id_sequence (gconstpointer data)
{
  reset_mocks ();
  chip_probe_responses = TRUE;
  mock_chip_id = GPOINTER_TO_UINT (data);
  cur_speed_hz = expected_speed = 125000;
  gboolean known_family = mock_chip_id == 0x2b50 || mock_chip_id == 0x95a8 || mock_chip_id == 0x23dd;
  g_assert_cmpint (probe_chip_id (), ==, known_family ? 0 : 2);
  g_assert_cmpuint (gpio_count, ==, 0);
  g_assert_cmpstr (events->str, ==,
                   "spi:10/6;spi:90/3;spi:06/3;spi:05/11;sleep:2000;spi:09/4;spi:04/8;");
  const guint8 command[] = { 0x06, 0xf9, 0 };
  const guint8 scratch[] = { 0x05, 0xfa, 0x85, 0xc0, 0, 4, 0x11, 0xee, 2, 0, 0 };
  const guint8 trigger[] = { 0x09, 0xf6, 0xa4, 1 };
  const guint8 read_id[] = { 0x04, 0xfb, 0x85, 0xc0, 0, 0, 0, 0 };
  assert_tx_frame (2, command, sizeof (command));
  assert_tx_frame (3, scratch, sizeof (scratch));
  assert_tx_frame (4, trigger, sizeof (trigger));
  assert_tx_frame (5, read_id, sizeof (read_id));
}

static void
test_chip_id_preconditions (gconstpointer data)
{
  reset_mocks ();
  chip_probe_responses = TRUE;
  gboolean edition_a = GPOINTER_TO_UINT (data) == 0;
  mock_mcu_status = GPOINTER_TO_UINT (data);
  mock_boot_edition = 0xef;
  g_assert_cmpint (probe_chip_id (), ==, 2);
  g_assert_cmpuint (gpio_count, ==, 0);
  g_assert_cmpuint (transfer_count, ==, edition_a ? 2 : 1);
  g_assert_cmpstr (events->str, ==, edition_a ? "spi:10/6;spi:90/3;" : "spi:10/6;");
}

static void
test_status_without_reset (gconstpointer data)
{
  reset_mocks ();
  status_observation = TRUE;
  mock_mcu_status = GPOINTER_TO_UINT (data);
  g_assert_cmpint (observe_status_without_reset (), ==,
                   mock_mcu_status == 0xa55a ? 0 : 2);
  g_assert_null (req_gpo1);
  g_assert_cmpuint (gpio_count, ==, 0);
  g_assert_cmpuint (firmware_packets, ==, 0);
  g_assert_cmpuint (transfer_count, ==, 2);
  g_assert_cmpstr (events->str, ==, "spi:10/6;spi:10/6;");
  const guint8 status[] = { 0x10, 0xef, 0x20, 0, 0, 0 };
  const guint8 geometry[] = { 0x10, 0xef, 0x14, 0, 0, 0 };
  assert_tx_frame (0, status, sizeof (status));
  assert_tx_frame (1, geometry, sizeof (geometry));
}

/* Run the real parser, DMI/topology checks, setup, dispatch and cleanup. All
* external interfaces are mocked; unexpected GPIO discovery/access aborts. */
static void
test_status_cli (gconstpointer data)
{
  guint scenario = GPOINTER_TO_UINT (data);

  if (g_test_subprocess ())
    {
      reset_mocks ();
      full_cli = status_observation = mock_assert_restored = TRUE;
      mock_mcu_status = scenario == 2 ? 0 : 0xa55a;
      mock_cli_expected_transfers = 2;
      if (scenario >= 3 && scenario <= 5)
        {
          mock_config_fail_at = scenario - 2;
          mock_cli_expected_transfers = 0;
        }
      if (scenario == 6 || scenario == 7)
        {
          transfer_result = scenario == 6 ? MOCK_ERROR : MOCK_SHORT;
          mock_cli_expected_transfers = 1;
        }
      mock_restore_failure = scenario == 8;
      gchar *argv[] = { (gchar *) "diagnostic", (gchar *) "--status-no-reset", NULL };
      exit (medion_diagnostic_main (scenario == 0 ? 1 : 2, argv));
    }
  g_test_trap_subprocess (NULL, 0, 0);
  if (scenario < 2)
    g_test_trap_assert_passed ();
  else
    g_test_trap_assert_failed ();
  g_test_trap_assert_stdout (scenario == 8 ? "*MOCK REMAINING SPI RESTORES VERIFIED*" :
                             "*MOCK SPI RESTORE VERIFIED*");
  g_test_trap_assert_stdout ("*MOCK PM RESTORE VERIFIED*");
  if (scenario == 2)
    g_test_trap_assert_stdout ("*Diagnostic exit=2;*");
  if (scenario == 8)
    g_test_trap_assert_stderr ("*ERROR: restoring SPI mode*");
  else if (scenario == 6 || scenario == 7)
    g_test_trap_assert_stderr ("*SPI transfer failed: len=6,*");
  else if (scenario >= 3 && scenario <= 5)
    g_test_trap_assert_stderr ("*ERROR: set SPI*");
  else
    g_test_trap_assert_stderr ("");
  g_test_trap_assert_stdout_unmatched ("*Sending dual*");
  g_test_trap_assert_stdout_unmatched ("*Successfully claimed*");
}

static void
test_speed_invalid (gconstpointer data)
{
  if (g_test_subprocess ())
    {
      reset_mocks ();
      gchar *argv[] = { (gchar *) "diagnostic", (gchar *) "--speed", (gchar *) data, NULL };
      exit (medion_diagnostic_main (3, argv));
    }
  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_failed ();
  g_test_trap_assert_stderr ("*--speed requires an integer from 1 to 1000000 Hz*");
  g_test_trap_assert_stderr_unmatched ("*Unexpected hardware*");
}

static void
test_speed_valid (void)
{
  g_assert_cmpuint (parse_spi_speed ("1"), ==, 1);
  g_assert_cmpuint (parse_spi_speed ("250000"), ==, 250000);
  g_assert_cmpuint (parse_spi_speed ("1000000"), ==, 1000000);
}

static void
test_chip_id_exclusive (gconstpointer data)
{
  if (g_test_subprocess ())
    {
      reset_mocks ();
      gchar *argv[] = { (gchar *) "diagnostic", (gchar *) "--chip-id", (gchar *) data,
                        (gchar *) "/test/invalid-firmware", NULL };
      int argc = g_str_equal (data, "--test-vendor-recovery") ? 4 : 3;
      exit (medion_diagnostic_main (argc, argv));
    }
  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_failed ();
  g_test_trap_assert_stderr ("*Choose exactly one*");
  g_test_trap_assert_stderr_unmatched ("*Unexpected hardware*");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/medion-diagnostic/transfer/success", test_transfer_success);
  g_test_add_data_func ("/medion-diagnostic/transfer/ioctl-error",
                        GINT_TO_POINTER (MOCK_ERROR), test_transfer_failure);
  g_test_add_data_func ("/medion-diagnostic/transfer/short",
                        GINT_TO_POINTER (MOCK_SHORT), test_transfer_failure);
  g_test_add_func ("/medion-diagnostic/reset/immediate-sync", test_reset_then_sync);
  g_test_add_func ("/medion-diagnostic/recovery-sequence", test_recovery_sequence);
  g_test_add_func ("/medion-diagnostic/reset/write-error", test_gpio_failure);
  g_test_add_data_func ("/medion-diagnostic/reset/read-error",
                        GINT_TO_POINTER (-1), test_gpio_readback);
  g_test_add_data_func ("/medion-diagnostic/reset/read-mismatch",
                        GINT_TO_POINTER (0), test_gpio_readback);
  g_test_add_data_func ("/medion-diagnostic/cli/help", "--help", test_cli);
  g_test_add_data_func ("/medion-diagnostic/cli/unknown", "--unknown", test_cli);
  g_test_add_data_func ("/medion-diagnostic/cli/missing-firmware",
                        "--test-vendor-recovery", test_cli);
  g_test_add_data_func ("/medion-diagnostic/firmware/size",
                        GINT_TO_POINTER (TRUE), test_invalid_firmware);
  g_test_add_data_func ("/medion-diagnostic/firmware/hash",
                        GINT_TO_POINTER (FALSE), test_invalid_firmware);
  g_test_add_func ("/medion-diagnostic/register-framing", test_register_framing);
  g_test_add_data_func ("/medion-diagnostic/chip-id/family-2b50",
                        GUINT_TO_POINTER (0x2b50), test_chip_id_sequence);
  g_test_add_data_func ("/medion-diagnostic/chip-id/family-95a8",
                        GUINT_TO_POINTER (0x95a8), test_chip_id_sequence);
  g_test_add_data_func ("/medion-diagnostic/chip-id/family-23dd",
                        GUINT_TO_POINTER (0x23dd), test_chip_id_sequence);
  g_test_add_data_func ("/medion-diagnostic/chip-id/all-zero",
                        GUINT_TO_POINTER (0), test_chip_id_sequence);
  g_test_add_data_func ("/medion-diagnostic/chip-id/all-ff",
                        GUINT_TO_POINTER (0xffff), test_chip_id_sequence);
  g_test_add_data_func ("/medion-diagnostic/chip-id/unknown",
                        GUINT_TO_POINTER (0x3800), test_chip_id_sequence);
  g_test_add_data_func ("/medion-diagnostic/chip-id/edition-a",
                        GUINT_TO_POINTER (0), test_chip_id_preconditions);
  g_test_add_data_func ("/medion-diagnostic/chip-id/runtime-idle",
                        GUINT_TO_POINTER (0xa55a), test_chip_id_preconditions);
  g_test_add_data_func ("/medion-diagnostic/chip-id/runtime-other",
                        GUINT_TO_POINTER (0x1234), test_chip_id_preconditions);
  g_test_add_data_func ("/medion-diagnostic/chip-id/exclusive-probe", "--probe", test_chip_id_exclusive);
  g_test_add_data_func ("/medion-diagnostic/chip-id/exclusive-observation", "--status-no-reset", test_chip_id_exclusive);
  g_test_add_data_func ("/medion-diagnostic/chip-id/exclusive-reset", "--reset", test_chip_id_exclusive);
  g_test_add_data_func ("/medion-diagnostic/chip-id/exclusive-recovery",
                        "--test-vendor-recovery", test_chip_id_exclusive);
  g_test_add_func ("/medion-diagnostic/speed/valid", test_speed_valid);
  g_test_add_data_func ("/medion-diagnostic/speed/zero", "0", test_speed_invalid);
  g_test_add_data_func ("/medion-diagnostic/speed/too-high", "8000000", test_speed_invalid);
  g_test_add_data_func ("/medion-diagnostic/speed/negative", "-1", test_speed_invalid);
  g_test_add_data_func ("/medion-diagnostic/speed/sign", "+1", test_speed_invalid);
  g_test_add_data_func ("/medion-diagnostic/speed/whitespace", " 1", test_speed_invalid);
  g_test_add_data_func ("/medion-diagnostic/speed/trailing", "100k", test_speed_invalid);
  g_test_add_data_func ("/medion-diagnostic/speed/overflow", "18446744073709551616", test_speed_invalid);
  g_test_add_data_func ("/medion-diagnostic/speed/empty", "", test_speed_invalid);
  g_test_add_data_func ("/medion-diagnostic/cli/missing-speed", "--speed", test_cli);
  g_test_add_data_func ("/medion-diagnostic/status-no-reset/idle",
                        GUINT_TO_POINTER (0xa55a), test_status_without_reset);
  g_test_add_data_func ("/medion-diagnostic/status-no-reset/zero",
                        GUINT_TO_POINTER (0), test_status_without_reset);
  g_test_add_data_func ("/medion-diagnostic/status-no-reset/all-ff",
                        GUINT_TO_POINTER (0xffff), test_status_without_reset);
  const gchar *status_scenarios[] = {
    "default", "explicit", "not-idle", "mode-setup-error", "bits-setup-error",
    "lsb-setup-error", "transfer-error", "transfer-short", "restore-error",
  };
  for (guint i = 0; i < G_N_ELEMENTS (status_scenarios); i++)
    {
      g_autofree gchar *path = g_strdup_printf ("/medion-diagnostic/status-cli/%s",
                                                status_scenarios[i]);
      g_test_add_data_func (path, GUINT_TO_POINTER (i), test_status_cli);
    }
  int result = g_test_run ();
  if (events)
    g_string_free (events, TRUE);
  g_clear_pointer (&tx_frames, g_ptr_array_unref);
  return result;
}
