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
static guint gpio_read_count;
static guint gpio_read_fail_at;
static guint gpio_request_count;
static guint gpio_release_count;
static gboolean gpio_request_failure;
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
static gboolean soft_reset_comparison;
static gboolean legacy_id_observation;
static gboolean legacy_historical_observation;
static gboolean legacy_ctfdavis_observation;
static guint8 legacy_id_response[4];
static guint legacy_c6_read_count;
static guint legacy_c6_success_at;
static guint16 mock_mcu_status;
static guint16 mock_after_status;
static guint8 mock_boot_edition;
static guint16 mock_chip_id;
static gboolean full_cli;
static gboolean mock_power_active;
static guint mock_config_writes;
static guint mock_config_fail_at;
static guint mock_config_reads;
static guint mock_config_read_fail_at;
static guint mock_transfer_fail_at;
static guint mock_signal_at;
static int mock_signal_number;
static gboolean mock_signal_eintr;
static guint mock_sleep_count;
static guint mock_sleep_signal_at;
static gint mock_expected_exit;
static guint mock_power_checks;
static guint mock_power_check_fail_at;
static gboolean mock_restore_failure;
static gboolean mock_speed_restore_failure;
static guint mock_speed_write_count;
static gboolean mock_zero_speed_refusal;
static gboolean mock_assert_restored;
static guint32 mock_spi_mode;
static guint32 mock_spi_max_speed;
static guint32 mock_original_spi_max_speed;
static guint8 mock_spi_bits;
static guint8 mock_spi_lsb;
static guint mock_cli_expected_transfers;
static unsigned long mock_config_requests[32];
static guint mock_config_request_count;
#define ORIGINAL_MODE (SPI_MODE_3 | SPI_CS_HIGH | SPI_LSB_FIRST | SPI_RX_DUAL)
#define ORIGINAL_BITS 16
#define ORIGINAL_LSB 1
#define ORIGINAL_MAX_SPEED 777777
#define MOCK_SPI_SYSFS "/sys/devices/test-controller/spi-FTE3600:00"

typedef enum {
  MOCK_DISCOVERY_NONE,
  MOCK_DISCOVERY_SPIDEV,
  MOCK_DISCOVERY_GPIO,
} MockDiscovery;

static MockDiscovery mock_discovery;
static enum gpiod_line_value mock_gpio_initial_value;

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
static gboolean mock_power_verify_medion_active (MedionPower *power,
                                                 GError     **error);
static void mock_power_report (MedionPower *power,
                               const gchar *tag);
static struct gpiod_chip *mock_gpio_open (const char *path);
static struct gpiod_line_info *mock_gpio_get_line_info (struct gpiod_chip *chip,
                                                        unsigned int       offset);
static const char *mock_gpio_info_name (struct gpiod_line_info *info);
static const char *mock_gpio_info_consumer (struct gpiod_line_info *info);
static bool mock_gpio_info_is_used (struct gpiod_line_info *info);
static void mock_gpio_info_free (struct gpiod_line_info *info);
static struct gpiod_line_settings *mock_gpio_settings_new (void);
static void mock_gpio_settings_free (struct gpiod_line_settings *settings);
static int mock_gpio_set_direction (struct gpiod_line_settings *settings,
                                    enum gpiod_line_direction   direction);
static int mock_gpio_set_output_value (struct gpiod_line_settings *settings,
                                       enum gpiod_line_value       value);
static struct gpiod_line_config *mock_gpio_config_new (void);
static void mock_gpio_config_free (struct gpiod_line_config *config);
static int mock_gpio_add_settings (struct gpiod_line_config   *config,
                                   const unsigned int         *offsets,
                                   size_t                      num_offsets,
                                   struct gpiod_line_settings *settings);
static struct gpiod_line_request *mock_gpio_request_lines (struct gpiod_chip           *chip,
                                                           struct gpiod_request_config *request_config,
                                                           struct gpiod_line_config    *line_config);
static void mock_gpio_chip_close (struct gpiod_chip *chip);
static const gchar *mock_udev_driver (GUdevDevice *device);
static int medion_diagnostic_main (int    argc,
                                   char **argv);
static void mock_exit (int status) G_GNUC_NORETURN;

#define main medion_diagnostic_main
#define exit mock_exit
#define ioctl mock_ioctl
#define gpiod_line_request_set_value mock_gpio_set
#define gpiod_line_request_get_value mock_gpio_get
#define gpiod_line_request_release mock_gpio_release
#define gpiod_chip_open mock_gpio_open
#define gpiod_chip_get_line_info mock_gpio_get_line_info
#define gpiod_line_info_get_name mock_gpio_info_name
#define gpiod_line_info_get_consumer mock_gpio_info_consumer
#define gpiod_line_info_is_used mock_gpio_info_is_used
#define gpiod_line_info_free mock_gpio_info_free
#define gpiod_line_settings_new mock_gpio_settings_new
#define gpiod_line_settings_free mock_gpio_settings_free
#define gpiod_line_settings_set_direction mock_gpio_set_direction
#define gpiod_line_settings_set_output_value mock_gpio_set_output_value
#define gpiod_line_config_new mock_gpio_config_new
#define gpiod_line_config_free mock_gpio_config_free
#define gpiod_line_config_add_line_settings mock_gpio_add_settings
#define gpiod_chip_request_lines mock_gpio_request_lines
#define gpiod_chip_close mock_gpio_chip_close
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
#define g_udev_device_get_driver mock_udev_driver
#define realpath mock_realpath
#define medion_power_prepare mock_power_prepare
#define medion_power_restore mock_power_restore
#define medion_power_verify_medion_active mock_power_verify_medion_active
#define medion_power_report mock_power_report
#include "../tools/test_medion_e3224.c"
#undef main
#undef exit
#undef ioctl
#undef gpiod_line_request_set_value
#undef gpiod_line_request_get_value
#undef gpiod_line_request_release
#undef gpiod_chip_open
#undef gpiod_chip_get_line_info
#undef gpiod_line_info_get_name
#undef gpiod_line_info_get_consumer
#undef gpiod_line_info_is_used
#undef gpiod_line_info_free
#undef gpiod_line_settings_new
#undef gpiod_line_settings_free
#undef gpiod_line_settings_set_direction
#undef gpiod_line_settings_set_output_value
#undef gpiod_line_config_new
#undef gpiod_line_config_free
#undef gpiod_line_config_add_line_settings
#undef gpiod_chip_request_lines
#undef gpiod_chip_close
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
#undef g_udev_device_get_driver
#undef realpath
#undef medion_power_prepare
#undef medion_power_restore
#undef medion_power_verify_medion_active
#undef medion_power_report

static void
reset_mocks (void)
{
  transfer_result = MOCK_OK;
  transfer_count = gpio_count = gpio_fail_at = 0;
  gpio_read_count = gpio_read_fail_at = gpio_request_count = gpio_release_count = 0;
  gpio_request_failure = FALSE;
  gpio_readback = 1;
  firmware_size = FW_SIZE;
  firmware_read = 0;
  interrupted = 0;
  req_gpo1 = NULL;
  spi_fd = -1;
  cleanup_failed = FALSE;
  memset (&saved_spi, 0, sizeof (saved_spi));
  full_cli = mock_power_active = mock_restore_failure = mock_assert_restored = FALSE;
  mock_speed_restore_failure = mock_zero_speed_refusal = FALSE;
  mock_speed_write_count = 0;
  mock_config_writes = mock_config_fail_at = mock_cli_expected_transfers = 0;
  mock_config_reads = mock_config_read_fail_at = mock_transfer_fail_at = 0;
  mock_signal_at = mock_power_checks = mock_power_check_fail_at = 0;
  mock_signal_eintr = FALSE;
  mock_signal_number = SIGTERM;
  mock_sleep_count = mock_sleep_signal_at = 0;
  mock_expected_exit = -1;
  mock_spi_mode = ORIGINAL_MODE;
  mock_original_spi_max_speed = ORIGINAL_MAX_SPEED;
  mock_spi_max_speed = mock_original_spi_max_speed;
  mock_spi_bits = ORIGINAL_BITS;
  mock_spi_lsb = ORIGINAL_LSB;
  memset (mock_config_requests, 0, sizeof (mock_config_requests));
  mock_config_request_count = 0;
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
  soft_reset_comparison = FALSE;
  legacy_id_observation = FALSE;
  legacy_historical_observation = FALSE;
  legacy_ctfdavis_observation = FALSE;
  legacy_c6_read_count = 0;
  legacy_c6_success_at = 1;
  mock_discovery = MOCK_DISCOVERY_NONE;
  mock_gpio_initial_value = GPIOD_LINE_VALUE_ERROR;
  memset (legacy_id_response, 0, sizeof (legacy_id_response));
  mock_after_status = 0;
  mock_mcu_status = mock_boot_edition = mock_chip_id = 0;
  g_clear_pointer (&tx_frames, g_ptr_array_unref);
  tx_frames = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
}

static guint32
mock_ctfdavis_speed (void)
{
  return MIN (mock_original_spi_max_speed, 1000000);
}

static void
mock_exit (int status)
{
  if (mock_expected_exit >= 0)
    {
      g_assert_cmpint (status, ==, mock_expected_exit);
      printf ("MOCK EXIT STATUS VERIFIED: %d\n", status);
    }
  exit (status);
}

static int
mock_ioctl (int fd, unsigned long request, ...)
{
  (void) fd;
  va_list args;
  va_start (args, request);
  void *value = va_arg (args, void *);
  va_end (args);
  if (request == SPI_IOC_MESSAGE (2))
    {
      struct spi_ioc_transfer *transfers = value;
      const struct spi_ioc_transfer *tx = &transfers[0];
      const struct spi_ioc_transfer *rx = &transfers[1];
      const guint8 identity_command[] = { 0x04, 0xfb, 0x9a, 0x8b, 0, 0 };
      const guint8 c6_command[] = { 0x08, 0xf7, 0xc6, 0 };
      gboolean c6_read = tx->len == sizeof (c6_command);
      guint32 speed = legacy_historical_observation ?
                      FTE3600_LEGACY_SPI_SPEED_HZ :
                      legacy_ctfdavis_observation ? mock_ctfdavis_speed () :
                      1000000;

      g_assert_true (legacy_id_observation || legacy_historical_observation ||
                     legacy_ctfdavis_observation);
      g_assert_cmpint (fd, ==, full_cli ? 43 : spi_fd);
      if (full_cli)
        {
          g_assert_cmpuint (mock_spi_mode, ==, SPI_MODE_0);
          g_assert_cmpuint (mock_spi_bits, ==, 8);
          g_assert_cmpuint (mock_spi_lsb, ==, 0);
          g_assert_true (mock_power_active);
          if (legacy_historical_observation || legacy_ctfdavis_observation)
            {
              g_assert_cmpuint (mock_spi_max_speed, ==,
                                legacy_historical_observation ?
                                FTE3600_LEGACY_SPI_SPEED_HZ :
                                mock_ctfdavis_speed ());
              if (legacy_historical_observation)
                {
                  g_assert_nonnull (req_gpo1);
                }
              else
                {
                  g_assert_null (req_gpo1);
                  g_assert_cmpuint (gpio_count, ==, 0);
                }
            }
          else
            {
              g_assert_null (req_gpo1);
              g_assert_cmpuint (gpio_count, ==, 0);
            }
        }
      g_assert_cmpuint (tx->tx_buf, !=, 0);
      g_assert_cmpuint (tx->rx_buf, ==, 0);
      g_assert_cmpuint (tx->len, ==,
                        c6_read ? sizeof (c6_command) :
                        sizeof (identity_command));
      g_assert_cmpuint (rx->tx_buf, ==, 0);
      g_assert_cmpuint (rx->rx_buf, !=, 0);
      g_assert_cmpuint (rx->len, ==,
                        c6_read ? 1 : sizeof (legacy_id_response));
      for (guint i = 0; i < 2; i++)
        {
          g_assert_cmpuint (transfers[i].speed_hz, ==, speed);
          g_assert_cmpuint (transfers[i].bits_per_word, ==, 8);
          g_assert_cmpuint (transfers[i].cs_change, ==, 0);
          g_assert_cmpuint (transfers[i].delay_usecs, ==, 0);
          g_assert_cmpuint (transfers[i].tx_nbits, ==, 0);
          g_assert_cmpuint (transfers[i].rx_nbits, ==, 0);
        }
      g_assert_cmpmem ((const void *) (uintptr_t) tx->tx_buf, tx->len,
                       c6_read ? c6_command : identity_command,
                       c6_read ? sizeof (c6_command) :
                       sizeof (identity_command));
      transfer_count++;
      last_tx_size = tx->len;
      memcpy (last_tx, (const void *) (uintptr_t) tx->tx_buf, tx->len);
      last_speed = tx->speed_hz;
      g_ptr_array_add (tx_frames, g_bytes_new ((const void *) (uintptr_t) tx->tx_buf,
                                               tx->len));
      g_string_append_printf (events, "spi2:%02x/%u+%u;",
                              ((const guint8 *) (uintptr_t) tx->tx_buf)[0],
                              tx->len, rx->len);
      if (mock_signal_at == transfer_count)
        {
          sig_handler (mock_signal_number);
          if (mock_signal_eintr)
            {
              errno = EINTR;
              return -1;
            }
        }
      if (transfer_result == MOCK_ERROR &&
          (!mock_transfer_fail_at || mock_transfer_fail_at == transfer_count))
        {
          errno = EIO;
          return -1;
        }
      if (transfer_result == MOCK_SHORT &&
          (!mock_transfer_fail_at || mock_transfer_fail_at == transfer_count))
        return tx->len + rx->len - 1;
      if (c6_read)
        {
          legacy_c6_read_count++;
          *(guint8 *) (uintptr_t) rx->rx_buf =
            legacy_c6_success_at != 0 &&
            legacy_c6_read_count >= legacy_c6_success_at ? 1 : 0;
        }
      else
        {
          memcpy ((void *) (uintptr_t) rx->rx_buf, legacy_id_response,
                  sizeof (legacy_id_response));
        }
      return tx->len + rx->len;
    }
  if (request != SPI_IOC_MESSAGE (1))
    {
      g_assert_true (full_cli);
      g_assert_cmpint (fd, ==, 43);
      g_assert_cmpuint (mock_config_request_count, <,
                        G_N_ELEMENTS (mock_config_requests));
      mock_config_requests[mock_config_request_count++] = request;
      if (request == SPI_IOC_RD_MODE32 || request == SPI_IOC_RD_MODE ||
          request == SPI_IOC_RD_BITS_PER_WORD || request == SPI_IOC_RD_LSB_FIRST ||
          request == SPI_IOC_RD_MAX_SPEED_HZ)
        {
          mock_config_reads++;
          if (mock_config_read_fail_at == mock_config_reads)
            {
              errno = EIO;
              return -1;
            }
        }
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

        case SPI_IOC_RD_MAX_SPEED_HZ: *(guint32 *) value = mock_spi_max_speed;
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

        case SPI_IOC_WR_MAX_SPEED_HZ:
          mock_speed_write_count++;
          /* Linux rejects zero rather than restoring it as a default. */
          if (*(guint32 *) value == 0)
            {
              errno = EINVAL;
              return -1;
            }
          if (mock_speed_restore_failure && mock_speed_write_count == 2)
            {
              errno = EIO;
              return -1;
            }
          mock_spi_max_speed = *(guint32 *) value;
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
      g_assert_true (mock_power_active);
      if (legacy_historical_observation || legacy_ctfdavis_observation)
        {
          g_assert_cmpuint (mock_spi_max_speed, ==,
                            legacy_historical_observation ?
                            FTE3600_LEGACY_SPI_SPEED_HZ :
                            mock_ctfdavis_speed ());
          if (legacy_historical_observation)
            {
              g_assert_nonnull (req_gpo1);
            }
          else
            {
              g_assert_null (req_gpo1);
              g_assert_cmpuint (gpio_count, ==, 0);
            }
        }
      else
        {
          g_assert_null (req_gpo1);
          g_assert_cmpuint (gpio_count, ==, 0);
        }
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
  if (soft_reset_comparison)
    {
      g_assert_cmpuint (transfer_count, <=, 6);
      g_assert_cmpuint (transfer->speed_hz, ==, 1000000);
      g_assert_cmpuint (transfer->cs_change, ==, 0);
      g_assert_cmpuint (transfer->delay_usecs, ==, 0);
      if (transfer_count == 3 || transfer_count == 4)
        {
          g_assert_cmpuint (transfer->len, ==, 1);
          g_assert_cmpuint (last_tx[0], ==, 0x70);
          g_assert_cmpuint (transfer->rx_buf, ==, 0);
        }
      else
        {
          const guint8 expected[] = { 0x10, 0xef,
                                      transfer_count == 1 || transfer_count == 5 ? 0x20 : 0x14,
                                      0, 0, 0 };
          g_assert_cmpmem (last_tx, transfer->len, expected, sizeof (expected));
          g_assert_cmpuint (transfer->rx_buf, !=, 0);
        }
    }
  if (legacy_historical_observation || legacy_ctfdavis_observation)
    {
      const guint8 expected[] = { 0x09, 0xf6, 0xc6, 0x01 };

      g_assert_cmpuint (transfer->speed_hz, ==,
                        legacy_historical_observation ?
                        FTE3600_LEGACY_SPI_SPEED_HZ :
                        mock_ctfdavis_speed ());
      g_assert_cmpuint (transfer->len, ==, sizeof (expected));
      g_assert_cmpuint (transfer->rx_buf, ==, 0);
      g_assert_cmpmem (last_tx, transfer->len, expected, sizeof (expected));
    }
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
  if (mock_signal_at == transfer_count)
    {
      sig_handler (mock_signal_number);
      if (mock_signal_eintr)
        {
          errno = EINTR;
          return -1;
        }
    }
  if (transfer_result == MOCK_ERROR && (!mock_transfer_fail_at || mock_transfer_fail_at == transfer_count))
    {
      errno = EIO;
      return -1;
    }
  if (transfer_result == MOCK_SHORT && (!mock_transfer_fail_at || mock_transfer_fail_at == transfer_count))
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
              guint16 status = soft_reset_comparison && transfer_count >= 5 ? mock_after_status : mock_mcu_status;
              rx[4] = status >> 8;
              rx[5] = status & 0xff;
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
  gpio_readback = value;
  return 0;
}

static enum gpiod_line_value
mock_gpio_get (struct gpiod_line_request *request, unsigned int offset)
{
  g_assert_nonnull (request);
  g_assert_cmpuint (offset, ==, PIN_GPO1_RESET);
  gpio_read_count++;
  if (gpio_read_fail_at == gpio_read_count)
    {
      errno = EIO;
      return GPIOD_LINE_VALUE_ERROR;
    }
  if (gpio_readback < 0)
    errno = EIO;
  return gpio_readback;
}

static void
mock_gpio_release (struct gpiod_line_request *request)
{
  g_assert_nonnull (request);
  gpio_release_count++;
  g_string_append (events, "release;");
}

static void
mock_sleep (gulong usec)
{
  g_string_append_printf (events, "sleep:%lu;", usec);
  mock_sleep_count++;
  if (mock_sleep_signal_at == mock_sleep_count)
    sig_handler (mock_signal_number);
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
          if (mock_speed_restore_failure)
            {
              g_assert_cmpuint (mock_spi_max_speed, ==, cur_speed_hz);
              g_assert_cmpuint (mock_speed_write_count, ==, 2);
              g_assert_cmpuint (mock_config_request_count, >=, 3);
              g_assert_cmpuint (mock_config_requests[mock_config_request_count - 3], ==,
                                SPI_IOC_WR_LSB_FIRST);
              g_assert_cmpuint (mock_config_requests[mock_config_request_count - 2], ==,
                                SPI_IOC_WR_BITS_PER_WORD);
              g_assert_cmpuint (mock_config_requests[mock_config_request_count - 1], ==,
                                SPI_IOC_WR_MODE32);
            }
          else
            {
              g_assert_cmpuint (mock_spi_max_speed, ==,
                                mock_original_spi_max_speed);
            }
          if (mock_zero_speed_refusal)
            {
              g_assert_cmpuint (mock_config_writes, ==, 0);
              g_assert_cmpuint (mock_speed_write_count, ==, 0);
              g_assert_cmpuint (gpio_request_count, ==, 0);
              g_assert_cmpuint (gpio_count, ==, 0);
              g_assert_cmpuint (transfer_count, ==, 0);
              g_assert_cmpstr (events->str, ==, "");
              puts ("MOCK ZERO SPEED REFUSED BEFORE MUTATION");
            }
          g_assert_null (req_gpo1);
          if (legacy_historical_observation && gpio_request_count && !gpio_request_failure)
            {
              /* Even interrupted and failed pulses must attempt raw high
               * before releasing the successfully claimed line. */
              g_assert_cmpuint (gpio_release_count, ==, 1);
              g_assert_true (g_str_has_suffix (events->str, "gpio:1;release;"));
              puts ("MOCK GPIO CLEANUP VERIFIED");
            }
          else
            {
              g_assert_cmpuint (gpio_release_count, ==, 0);
            }
          if (!legacy_historical_observation)
            g_assert_cmpuint (gpio_count, ==, 0);
          g_assert_cmpuint (firmware_packets, ==, 0);
          g_assert_cmpuint (transfer_count, ==, mock_cli_expected_transfers);
          for (guint i = 0; i < tx_frames->len; i++)
            {
              gsize size;
              const guint8 *frame = g_bytes_get_data (g_ptr_array_index (tx_frames, i), &size);
              if (legacy_historical_observation || legacy_ctfdavis_observation)
                {
                  static const guint8 c6_write[] = {
                    0x09, 0xf6, 0xc6, 0x01,
                  };
                  static const guint8 c6_read[] = {
                    0x08, 0xf7, 0xc6, 0x00,
                  };
                  static const guint8 identity[] = {
                    0x04, 0xfb, 0x9a, 0x8b, 0x00, 0x00,
                  };

                  if (size == sizeof (identity))
                    {
                      g_assert_cmpuint (i + 1, ==, tx_frames->len);
                      g_assert_cmpmem (frame, size, identity,
                                       sizeof (identity));
                    }
                  else if ((i & 1) == 0)
                    {
                      g_assert_cmpmem (frame, size, c6_write,
                                       sizeof (c6_write));
                    }
                  else
                    {
                      g_assert_cmpmem (frame, size, c6_read,
                                       sizeof (c6_read));
                    }
                }
              else if (legacy_id_observation)
                {
                  const guint8 expected[] = { 0x04, 0xfb, 0x9a, 0x8b, 0, 0 };
                  g_assert_cmpmem (frame, size, expected, sizeof (expected));
                }
              else if (soft_reset_comparison && (i == 2 || i == 3))
                {
                  const guint8 expected[] = { 0x70 };
                  g_assert_cmpmem (frame, size, expected, sizeof (expected));
                }
              else
                {
                  const guint8 expected[] = { 0x10, 0xef, i == 0 || i == 4 ? 0x20 : 0x14, 0, 0, 0 };
                  g_assert_cmpmem (frame, size, expected, sizeof (expected));
                }
            }
          puts (mock_restore_failure || mock_speed_restore_failure ? "MOCK REMAINING SPI RESTORES VERIFIED" :
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
  if (legacy_historical_observation &&
      g_str_equal (filename,
                   "/sys/class/gpio/gpiochip-test/device/firmware_node/path"))
    {
      *contents = g_strdup ("\\_SB_.GPO1\n");
      if (length)
        *length = strlen (*contents);
      return TRUE;
    }
  if (legacy_historical_observation &&
      g_str_equal (filename,
                   "/sys/class/gpio/gpiochip-test/device/firmware_node/hid"))
    {
      *contents = g_strdup ("INT3453\n");
      if (length)
        *length = strlen (*contents);
      return TRUE;
    }
  if (legacy_historical_observation &&
      g_str_equal (filename,
                   "/sys/class/gpio/gpiochip-test/device/firmware_node/uid"))
    {
      *contents = g_strdup ("1\n");
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
      if (g_str_equal (subsystems[0], "spidev"))
        mock_discovery = MOCK_DISCOVERY_SPIDEV;
      else if (legacy_historical_observation &&
               g_str_equal (subsystems[0], "gpio"))
        mock_discovery = MOCK_DISCOVERY_GPIO;
      else
        g_assert_not_reached ();
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
  g_assert_cmpstr (subsystem, ==,
                   mock_discovery == MOCK_DISCOVERY_SPIDEV ? "spidev" :
                   "gpio");
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
  return mock_discovery == MOCK_DISCOVERY_SPIDEV ?
         MOCK_SPI_SYSFS : "/sys/class/gpio/gpiochip-test";
}

static const gchar *
mock_udev_file (GUdevDevice *device)
{
  g_assert_true (full_cli);
  g_assert_nonnull (device);
  return mock_discovery == MOCK_DISCOVERY_SPIDEV ?
         "/dev/test-spidev" : "/dev/gpiochip-test";
}

static const gchar *
mock_udev_driver (GUdevDevice *device)
{
  g_assert_true (full_cli);
  g_assert_nonnull (device);
  g_assert_cmpint (mock_discovery, ==, MOCK_DISCOVERY_GPIO);
  return "mock-gpio";
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
mock_power_verify_medion_active (MedionPower *power, GError **error)
{
  g_assert_true (power == &power_state);
  g_assert_true (mock_power_active);
  mock_power_checks++;
  if (mock_power_check_fail_at == mock_power_checks)
    {
      g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_IO, "comparison controller no longer active");
      return FALSE;
    }
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
  g_assert_true (legacy_historical_observation);
  g_assert_cmpstr (path, ==, "/dev/gpiochip-test");
  return (struct gpiod_chip *) (uintptr_t) 0x11;
}

static struct gpiod_line_info *
mock_gpio_get_line_info (struct gpiod_chip *chip,
                         unsigned int       offset)
{
  g_assert_true (chip == (struct gpiod_chip *) (uintptr_t) 0x11);
  g_assert_cmpuint (offset, ==, PIN_GPO1_RESET);
  return (struct gpiod_line_info *) (uintptr_t) 0x12;
}

static const char *
mock_gpio_info_name (struct gpiod_line_info *info)
{
  g_assert_true (info == (struct gpiod_line_info *) (uintptr_t) 0x12);
  return "legacy-control";
}

static const char *
mock_gpio_info_consumer (struct gpiod_line_info *info)
{
  g_assert_true (info == (struct gpiod_line_info *) (uintptr_t) 0x12);
  return NULL;
}

static bool
mock_gpio_info_is_used (struct gpiod_line_info *info)
{
  g_assert_true (info == (struct gpiod_line_info *) (uintptr_t) 0x12);
  return false;
}

static void
mock_gpio_info_free (struct gpiod_line_info *info)
{
  g_assert_true (info == (struct gpiod_line_info *) (uintptr_t) 0x12);
}

static struct gpiod_line_settings *
mock_gpio_settings_new (void)
{
  return (struct gpiod_line_settings *) (uintptr_t) 0x13;
}

static void
mock_gpio_settings_free (struct gpiod_line_settings *settings)
{
  g_assert_true (settings == (struct gpiod_line_settings *) (uintptr_t) 0x13);
}

static int
mock_gpio_set_direction (struct gpiod_line_settings *settings,
                         enum gpiod_line_direction   direction)
{
  g_assert_true (settings == (struct gpiod_line_settings *) (uintptr_t) 0x13);
  g_assert_cmpint (direction, ==, GPIOD_LINE_DIRECTION_OUTPUT);
  return 0;
}

static int
mock_gpio_set_output_value (struct gpiod_line_settings *settings,
                            enum gpiod_line_value       value)
{
  g_assert_true (settings == (struct gpiod_line_settings *) (uintptr_t) 0x13);
  mock_gpio_initial_value = value;
  return 0;
}

static struct gpiod_line_config *
mock_gpio_config_new (void)
{
  return (struct gpiod_line_config *) (uintptr_t) 0x14;
}

static void
mock_gpio_config_free (struct gpiod_line_config *config)
{
  g_assert_true (config == (struct gpiod_line_config *) (uintptr_t) 0x14);
}

static int
mock_gpio_add_settings (struct gpiod_line_config   *config,
                        const unsigned int         *offsets,
                        size_t                      num_offsets,
                        struct gpiod_line_settings *settings)
{
  g_assert_true (config == (struct gpiod_line_config *) (uintptr_t) 0x14);
  g_assert_true (settings == (struct gpiod_line_settings *) (uintptr_t) 0x13);
  g_assert_cmpuint (num_offsets, ==, 1);
  g_assert_cmpuint (offsets[0], ==, PIN_GPO1_RESET);
  return 0;
}

static struct gpiod_line_request *
mock_gpio_request_lines (struct gpiod_chip           *chip,
                         struct gpiod_request_config *request_config,
                         struct gpiod_line_config    *line_config)
{
  g_assert_true (chip == (struct gpiod_chip *) (uintptr_t) 0x11);
  g_assert_null (request_config);
  g_assert_true (line_config == (struct gpiod_line_config *) (uintptr_t) 0x14);
  g_assert_cmpint (mock_gpio_initial_value, ==, GPIOD_LINE_VALUE_INACTIVE);
  gpio_request_count++;
  if (gpio_request_failure)
    {
      errno = EBUSY;
      return NULL;
    }
  gpio_readback = mock_gpio_initial_value;
  return (struct gpiod_line_request *) (uintptr_t) 0x15;
}

static void
mock_gpio_chip_close (struct gpiod_chip *chip)
{
  g_assert_true (chip == (struct gpiod_chip *) (uintptr_t) 0x11);
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
test_legacy_transfer (void)
{
  reset_mocks ();
  legacy_id_observation = TRUE;
  spi_fd = 7;
  legacy_id_response[0] = 0x93;
  legacy_id_response[1] = 0x62;
  legacy_id_response[2] = 0x1c;
  legacy_id_response[3] = 0x53;
  const guint8 command[] = { 0x04, 0xfb, 0x9a, 0x8b, 0, 0 };
  guint8 response[4] = { 0 };

  spi_write_then_read_once (command, sizeof (command), response, sizeof (response),
                            1000000);
  g_assert_cmpuint (transfer_count, ==, 1);
  g_assert_cmpuint (last_speed, ==, 1000000);
  g_assert_cmpmem (last_tx, last_tx_size, command, sizeof (command));
  g_assert_cmpmem (response, sizeof (response), legacy_id_response,
                   sizeof (legacy_id_response));
  g_assert_cmpstr (events->str, ==, "spi2:04/6+4;");
  spi_fd = -1;
}

static void
test_legacy_transfer_failure (gconstpointer data)
{
  if (g_test_subprocess ())
    {
      reset_mocks ();
      legacy_id_observation = TRUE;
      spi_fd = 7;
      transfer_result = GPOINTER_TO_INT (data);
      const guint8 command[] = { 0x04, 0xfb, 0x9a, 0x8b, 0, 0 };
      guint8 response[4] = { 0 };
      spi_write_then_read_once (command, sizeof (command), response,
                                sizeof (response), 1000000);
      puts ("UNEXPECTED CONTINUATION");
      exit (0);
    }
  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_failed ();
  g_test_trap_assert_stderr ("*SPI write-then-read failed: tx=6, rx=4,*");
  g_test_trap_assert_stdout_unmatched ("*UNEXPECTED CONTINUATION*");
}

static void
test_vendor_crc (void)
{
  static const struct
  {
    guint8  data[2];
    guint16 expected;
  } cases[] = {
    { { 0x93, 0x62 }, 0x1c53 },
    { { 0x93, 0x91 }, 0xc32f },
    { { 0x93, 0x61 }, 0x2c30 },
    { { 0x00, 0x00 }, 0x1d0f },
    { { 0xff, 0xff }, 0x0000 },
    { { 0x12, 0x34 }, 0x0ec9 },
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    g_assert_cmphex (vendor_crc16 (cases[i].data, sizeof (cases[i].data)), ==,
                     cases[i].expected);
  const guint8 standard[] = "123456789";
  g_assert_cmphex (vendor_crc16 (standard, sizeof (standard) - 1), ==, 0x29b1);
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
test_legacy_id_cli (gconstpointer data)
{
  guint scenario = GPOINTER_TO_UINT (data);

  if (g_test_subprocess ())
    {
      reset_mocks ();
      full_cli = legacy_id_observation = mock_assert_restored = TRUE;
      mock_cli_expected_transfers = 1;
      switch (scenario)
        {
        case 0: /* recognized identity and matching non-zero CRC */
          legacy_id_response[0] = 0x93;
          legacy_id_response[1] = 0x62;
          legacy_id_response[2] = 0x1c;
          legacy_id_response[3] = 0x53;
          break;

        case 1: /* recognized identity, zero trailer is not validation */
          legacy_id_response[0] = 0x93;
          legacy_id_response[1] = 0x62;
          break;

        case 2: /* all zero */
          break;

        case 3: /* all ff */
          memset (legacy_id_response, 0xff, sizeof (legacy_id_response));
          break;

        case 4: /* unknown identity with internally consistent CRC */
          legacy_id_response[0] = 0x12;
          legacy_id_response[1] = 0x34;
          legacy_id_response[2] = 0x0e;
          legacy_id_response[3] = 0xc9;
          break;

        case 5: /* recognized identity with a bad CRC */
          legacy_id_response[0] = 0x93;
          legacy_id_response[1] = 0x62;
          legacy_id_response[2] = 0x12;
          legacy_id_response[3] = 0x34;
          break;

        case 6: /* 9361 is not in this archived library's accepted ID set */
          legacy_id_response[0] = 0x93;
          legacy_id_response[1] = 0x61;
          legacy_id_response[2] = 0x2c;
          legacy_id_response[3] = 0x30;
          break;

        case 7: /* identity ff with the helper's zero-trailer bypass */
          legacy_id_response[0] = 0xff;
          legacy_id_response[1] = 0xff;
          break;

        case 8:
          transfer_result = MOCK_ERROR;
          break;

        case 9:
          transfer_result = MOCK_SHORT;
          break;

        case 10:
          mock_power_check_fail_at = 1;
          mock_cli_expected_transfers = 0;
          break;

        case 11:
          mock_power_check_fail_at = 2;
          break;

        case 12:
        case 13:
          mock_signal_at = 1;
          mock_signal_eintr = scenario == 12;
          mock_signal_number = scenario == 12 ? SIGTERM : SIGINT;
          mock_expected_exit = 128 + mock_signal_number;
          break;

        case 14:
          mock_config_read_fail_at = 1;
          mock_cli_expected_transfers = 0;
          break;

        case 15:
          mock_config_fail_at = 1;
          mock_cli_expected_transfers = 0;
          break;

        case 16:
          mock_restore_failure = TRUE;
          break;

        default:
          g_assert_not_reached ();
        }
      gchar *argv[] = { (gchar *) "diagnostic", (gchar *) "--legacy-id-no-init", NULL };
      exit (medion_diagnostic_main (2, argv));
    }
  g_test_trap_subprocess (NULL, 0, 0);
  if (scenario == 0)
    g_test_trap_assert_passed ();
  else
    g_test_trap_assert_failed ();
  g_test_trap_assert_stdout (scenario == 16 ? "*MOCK REMAINING SPI RESTORES VERIFIED*" :
                             "*MOCK SPI RESTORE VERIFIED*");
  g_test_trap_assert_stdout ("*MOCK PM RESTORE VERIFIED*");
  g_test_trap_assert_stdout_unmatched ("*Successfully claimed*");
  g_test_trap_assert_stdout_unmatched ("*Soft reset*");
  g_test_trap_assert_stdout_unmatched ("*ROM edition*");
  if (scenario <= 7)
    {
      g_test_trap_assert_stdout ("*Legacy raw response:*");
      g_test_trap_assert_stdout (scenario == 0 ? "*Diagnostic exit=0;*" :
                                 "*Diagnostic exit=2;*");
      g_test_trap_assert_stderr ("");
    }
  else if (scenario == 8 || scenario == 9)
    {
      g_test_trap_assert_stdout_unmatched ("*Legacy raw response:*");
      g_test_trap_assert_stderr ("*SPI write-then-read failed: tx=6, rx=4,*");
    }
  else if (scenario == 10 || scenario == 11)
    {
      g_test_trap_assert_stderr ("*comparison controller no longer active*");
      if (scenario == 10)
        g_test_trap_assert_stdout_unmatched ("*Legacy raw response:*");
    }
  else if (scenario == 12 || scenario == 13)
    {
      g_test_trap_assert_stderr ("*Interrupted during SPI transfer*");
      g_test_trap_assert_stdout_unmatched ("*Legacy raw response:*");
    }
  else if (scenario == 14 || scenario == 15)
    {
      g_test_trap_assert_stderr ("*ERROR:*");
      g_test_trap_assert_stdout_unmatched ("*Legacy-protocol candidate read:*");
    }
  else
    {
      g_test_trap_assert_stdout ("*Diagnostic exit=1;*FAILED*");
      g_test_trap_assert_stderr ("*ERROR: restoring SPI mode*");
    }
}

static void
test_legacy_candidate_cli (gconstpointer data,
                           gboolean      ctfdavis)
{
  guint scenario = GPOINTER_TO_UINT (data);

  if (g_test_subprocess ())
    {
      reset_mocks ();
      full_cli = mock_assert_restored = TRUE;
      if (ctfdavis)
        legacy_ctfdavis_observation = TRUE;
      else
        legacy_historical_observation = TRUE;
      legacy_id_response[0] = 0x93;
      legacy_id_response[1] = 0x62;
      mock_cli_expected_transfers = 3;

      switch (scenario)
        {
        case 0: /* C6 succeeds on attempt 1. */
          break;

        case 1:
        case 2:
        case 3: /* C6 succeeds on attempt 2, 3, or 4. */
          legacy_c6_success_at = scenario + 1;
          mock_cli_expected_transfers = 3 + scenario * 2;
          break;

        case 4: /* Four failed C6 readbacks must not accept an ID. */
          legacy_c6_success_at = 0;
          mock_cli_expected_transfers = 9;
          break;

        case 5:
          legacy_id_response[0] = 0x26;
          legacy_id_response[1] = 0xc4;
          break;

        case 6:
          memset (legacy_id_response, 0, sizeof (legacy_id_response));
          break;

        case 7:
          transfer_result = MOCK_ERROR;
          mock_cli_expected_transfers = 1;
          break;

        case 8:
          transfer_result = MOCK_SHORT;
          mock_cli_expected_transfers = 1;
          break;

        case 9:
        case 10: /* Error/short return from the C6 read MESSAGE(2). */
          transfer_result = scenario == 9 ? MOCK_ERROR : MOCK_SHORT;
          mock_transfer_fail_at = 2;
          mock_cli_expected_transfers = 2;
          break;

        case 11:
        case 12: /* Error/short return from the identity MESSAGE(2). */
          transfer_result = scenario == 11 ? MOCK_ERROR : MOCK_SHORT;
          mock_transfer_fail_at = 3;
          mock_cli_expected_transfers = 3;
          break;

        case 13: /* Zero cannot be restored by the real spidev ioctl. */
          mock_zero_speed_refusal = TRUE;
          mock_original_spi_max_speed = 0;
          mock_spi_max_speed = mock_original_spi_max_speed;
          mock_cli_expected_transfers = 0;
          mock_expected_exit = 1;
          break;

        case 14: /* A device maximum above 1 MHz is capped. */
          g_assert_true (ctfdavis);
          mock_original_spi_max_speed = 4000000;
          mock_spi_max_speed = mock_original_spi_max_speed;
          break;

        case 15: /* Failed speed restore must not skip remaining cleanup. */
          mock_speed_restore_failure = TRUE;
          /* Make the selected and original values observably different. */
          mock_original_spi_max_speed = 3000000;
          mock_spi_max_speed = mock_original_spi_max_speed;
          break;

        case 16:
        case 17:
        case 18:
        case 19: /* First 10 ms wait and C6 4 ms wait, for both signals. */
          mock_sleep_signal_at = (scenario & 1) ? (ctfdavis ? 2 : 3) : 1;
          mock_signal_number = scenario < 18 ? SIGTERM : SIGINT;
          mock_expected_exit = 128 + mock_signal_number;
          mock_cli_expected_transfers = (scenario & 1) ? 1 : 0;
          break;

        case 20:
        case 21: /* The attachment also has a second 10 ms GPIO pulse. */
          g_assert_false (ctfdavis);
          mock_sleep_signal_at = 2;
          mock_signal_number = scenario == 20 ? SIGTERM : SIGINT;
          mock_expected_exit = 128 + mock_signal_number;
          mock_cli_expected_transfers = 0;
          break;

        case 22:
          g_assert_false (ctfdavis);
          gpio_request_failure = TRUE;
          mock_expected_exit = 1;
          mock_cli_expected_transfers = 0;
          break;

        case 23:
          g_assert_false (ctfdavis);
          gpio_fail_at = 1;
          mock_expected_exit = 1;
          mock_cli_expected_transfers = 0;
          break;

        case 24:
          g_assert_false (ctfdavis);
          gpio_read_fail_at = 1;
          mock_expected_exit = 1;
          mock_cli_expected_transfers = 0;
          break;

        case 25: /* Failure to leave raw high still releases the line. */
          g_assert_false (ctfdavis);
          gpio_fail_at = 6;
          break;

        case 26:
          g_assert_false (ctfdavis);
          gpio_fail_at = 2;
          mock_expected_exit = 1;
          mock_cli_expected_transfers = 0;
          break;

        case 27:
          g_assert_false (ctfdavis);
          gpio_read_fail_at = 2;
          mock_expected_exit = 1;
          mock_cli_expected_transfers = 0;
          break;

        default:
          g_assert_not_reached ();
        }

      gchar *argv[] = { (gchar *) "diagnostic",
                        (gchar *) (ctfdavis ? "--legacy-ctfdavis-id" :
                                   "--legacy-historical-id"),
                        NULL };
      int result = medion_diagnostic_main (2, argv);

      if (scenario == 15 || scenario == 25)
        {
          g_assert_cmpint (result, ==, 1);
          g_assert_true (cleanup_failed);
        }
      if (scenario == 0 || scenario == 14)
        {
          static const unsigned long expected_config[] = {
            SPI_IOC_RD_MODE32,
            SPI_IOC_RD_BITS_PER_WORD,
            SPI_IOC_RD_LSB_FIRST,
            SPI_IOC_RD_MAX_SPEED_HZ,
            SPI_IOC_WR_MODE,
            SPI_IOC_WR_BITS_PER_WORD,
            SPI_IOC_WR_LSB_FIRST,
            SPI_IOC_WR_MAX_SPEED_HZ,
            SPI_IOC_RD_MODE,
            SPI_IOC_RD_BITS_PER_WORD,
            SPI_IOC_RD_LSB_FIRST,
            SPI_IOC_RD_MAX_SPEED_HZ,
            SPI_IOC_WR_MAX_SPEED_HZ,
            SPI_IOC_WR_LSB_FIRST,
            SPI_IOC_WR_BITS_PER_WORD,
            SPI_IOC_WR_MODE32,
          };

          g_assert_cmpint (result, ==, 0);
          if (scenario == 14)
            g_assert_cmpuint (cur_speed_hz, ==, 1000000);
          g_assert_cmpuint (mock_power_checks, ==, 2);
          g_assert_cmpuint (gpio_count, ==, ctfdavis ? 0 : 6);
          if (ctfdavis)
            {
              g_assert_cmpstr (events->str, ==,
                               "sleep:10000;spi:09/4;sleep:4000;"
                               "spi2:08/4+1;spi2:04/6+4;");
            }
          else
            {
              g_assert_cmpstr (events->str, ==,
                               "gpio:0;gpio:0;sleep:10000;gpio:1;"
                               "gpio:0;sleep:10000;gpio:1;"
                               "spi:09/4;sleep:4000;spi2:08/4+1;"
                               "spi2:04/6+4;gpio:1;release;");
            }
          g_assert_cmpuint (mock_config_request_count, ==,
                            G_N_ELEMENTS (expected_config));
          g_assert_cmpmem (mock_config_requests,
                           mock_config_request_count *
                           sizeof (mock_config_requests[0]),
                           expected_config, sizeof (expected_config));
        }
      exit (result);
    }

  g_test_trap_subprocess (NULL, 0, 0);
  if (scenario <= 3 || scenario == 14)
    g_test_trap_assert_passed ();
  else
    g_test_trap_assert_failed ();
  if (scenario != 13 && !(scenario >= 22 && scenario <= 24))
    g_test_trap_assert_stdout (ctfdavis ?
                               "*ctfdavis candidate prefix:*speed capped at 1 MHz*no GPIO request*" :
                               "*Later proposed attachment prefix:*Mode 0 / 4 MHz*Pin 39*");
  if (ctfdavis || scenario == 13 || scenario == 22)
    g_test_trap_assert_stdout_unmatched ("*Successfully claimed Reset Line*");
  else
    g_test_trap_assert_stdout ("*Successfully claimed Reset Line*");
  g_test_trap_assert_stdout (scenario == 15 ? "*MOCK REMAINING SPI RESTORES VERIFIED*" :
                             "*MOCK SPI RESTORE VERIFIED*");
  g_test_trap_assert_stdout ("*MOCK PM RESTORE VERIFIED*");
  if (!ctfdavis && scenario != 13 && scenario != 22)
    g_test_trap_assert_stdout ("*MOCK GPIO CLEANUP VERIFIED*");
  if (scenario == 0 || scenario == 14)
    {
      g_test_trap_assert_stdout ("*Accepted archived FW9362-branch identity 9362*");
    }
  else if (scenario >= 1 && scenario <= 3)
    {
      g_test_trap_assert_stdout (scenario == 1 ? "*C6 readback attempt 2/4: 01*" :
                                 scenario == 2 ? "*C6 readback attempt 3/4: 01*" :
                                 "*C6 readback attempt 4/4: 01*");
    }
  else if (scenario == 4)
    {
      g_test_trap_assert_stdout ("*C6 readback attempt 4/4: 00*");
      g_test_trap_assert_stdout ("*C6 never read back 01*");
    }
  else if (scenario == 5)
    {
      g_test_trap_assert_stdout ("*shifted-ID condition*");
    }
  else if (scenario == 6)
    {
      g_test_trap_assert_stdout ("*No supported FW9362-branch identity*");
    }
  else if (scenario == 7 || scenario == 8)
    {
      g_test_trap_assert_stderr ("*SPI transfer failed: len=4,*");
    }
  else if (scenario == 9 || scenario == 10)
    {
      g_test_trap_assert_stderr ("*SPI write-then-read failed: tx=4, rx=1,*");
    }
  else if (scenario == 11 || scenario == 12)
    {
      g_test_trap_assert_stderr ("*SPI write-then-read failed: tx=6, rx=4,*");
    }
  else if (scenario == 13)
    {
      g_test_trap_assert_stderr ("*Cannot safely restore a zero SPI maximum speed; refusing before SPI configuration or sensor traffic*");
      g_test_trap_assert_stdout ("*MOCK ZERO SPEED REFUSED BEFORE MUTATION*");
    }
  else if (scenario == 15)
    {
      g_test_trap_assert_stderr ("*ERROR: restoring SPI maximum speed*");
      g_test_trap_assert_stdout ("*Diagnostic exit=1;*FAILED*");
    }
  else if (scenario >= 16 && scenario <= 21)
    {
      g_test_trap_assert_stdout (scenario == 16 || scenario == 17 || scenario == 20 ?
                                 "*MOCK EXIT STATUS VERIFIED: 143*" :
                                 "*MOCK EXIT STATUS VERIFIED: 130*");
      g_test_trap_assert_stdout_unmatched ("*FW9362-branch raw identity response:*");
    }
  else if (scenario == 22)
    {
      g_test_trap_assert_stderr ("*Could not claim reset GPIO*");
    }
  else if (scenario == 23 || scenario == 26)
    {
      g_test_trap_assert_stderr ("*set reset GPIO value*");
    }
  else if (scenario == 24 || scenario == 27)
    {
      g_test_trap_assert_stderr ("*read back reset GPIO value*");
    }
  else if (scenario == 25)
    {
      g_test_trap_assert_stderr ("*ERROR: release reset high*");
      g_test_trap_assert_stdout ("*Diagnostic exit=1;*FAILED*");
    }
}

static void
test_legacy_historical_cli (gconstpointer data)
{
  test_legacy_candidate_cli (data, FALSE);
}

static void
test_legacy_ctfdavis_cli (gconstpointer data)
{
  test_legacy_candidate_cli (data, TRUE);
}

static void
test_comparison_cli (gconstpointer data)
{
  guint scenario = GPOINTER_TO_UINT (data);

  if (g_test_subprocess ())
    {
      reset_mocks ();
      full_cli = status_observation = soft_reset_comparison = mock_assert_restored = TRUE;
      mock_cli_expected_transfers = 6;
      mock_mcu_status = scenario == 2 || scenario == 3 ? 0xa55a :
                        scenario == 4 ? 0xffff : scenario == 5 ? 0x1234 : 0;
      mock_after_status = scenario == 1 || scenario == 3 ? 0xa55a :
                          scenario == 4 ? 0xffff : scenario == 5 ? 0x5678 : 0;
      if (scenario >= 10 && scenario <= 25)
        {
          transfer_result = scenario < 20 ? MOCK_ERROR : MOCK_SHORT;
          mock_transfer_fail_at = scenario % 10 + 1;
          mock_cli_expected_transfers = mock_transfer_fail_at;
        }
      if (scenario >= 30 && scenario <= 45)
        {
          mock_signal_at = scenario % 10 + 1;
          mock_signal_eintr = scenario < 40;
          mock_signal_number = scenario < 40 ? SIGTERM : SIGINT;
          mock_expected_exit = 128 + mock_signal_number;
          mock_cli_expected_transfers = mock_signal_at;
        }
      if (scenario >= 50 && scenario <= 55)
        {
          mock_config_read_fail_at = scenario - 49;
          mock_cli_expected_transfers = 0;
        }
      if (scenario >= 60 && scenario <= 62)
        {
          mock_config_fail_at = scenario - 59;
          mock_cli_expected_transfers = 0;
        }
      if (scenario >= 70 && scenario <= 73)
        {
          mock_power_check_fail_at = scenario - 69;
          mock_cli_expected_transfers = (scenario - 70) * 2;
        }
      mock_restore_failure = scenario == 80;
      if (scenario >= 90 && scenario <= 93)
        {
          mock_sleep_signal_at = scenario % 2 + 1;
          mock_signal_number = scenario < 92 ? SIGTERM : SIGINT;
          mock_expected_exit = 128 + mock_signal_number;
          mock_cli_expected_transfers = mock_sleep_signal_at + 2;
        }
      gchar *argv[] = { (gchar *) "diagnostic", (gchar *) "--compare-soft-reset", NULL };
      int result = medion_diagnostic_main (2, argv);
      g_assert_cmpint (result, ==, scenario == 80 ? 1 :
                       mock_after_status == 0xa55a ? 0 : 2);
      g_assert_cmpuint (mock_power_checks, ==, 4);
      g_assert_cmpstr (events->str, ==,
                       "spi:10/6;spi:10/6;spi:70/1;sleep:5000;spi:70/1;sleep:2000;spi:10/6;spi:10/6;");
      exit (result);
    }
  g_test_trap_subprocess (NULL, 0, 0);
  if (scenario == 1 || scenario == 3)
    g_test_trap_assert_passed ();
  else
    g_test_trap_assert_failed ();
  g_test_trap_assert_stdout (scenario == 80 ? "*MOCK REMAINING SPI RESTORES VERIFIED*" :
                             "*MOCK SPI RESTORE VERIFIED*");
  g_test_trap_assert_stdout ("*MOCK PM RESTORE VERIFIED*");
  g_test_trap_assert_stdout_unmatched ("*Successfully claimed*");
  g_test_trap_assert_stdout_unmatched ("*ROM edition response*");
  if (scenario < 6)
    {
      g_test_trap_assert_stdout ("*A: before soft reset*B: after soft reset*Comparison: A_idle=*B_idle=*");
      g_test_trap_assert_stdout (scenario == 1 || scenario == 3 ? "*Diagnostic exit=0;*" :
                                 "*Diagnostic exit=2;*");
      g_test_trap_assert_stderr ("");
    }
  else if (scenario == 80)
    {
      g_test_trap_assert_stdout ("*Diagnostic exit=1;*FAILED*");
      g_test_trap_assert_stderr ("*ERROR: restoring SPI mode*");
    }
  else
    {
      g_test_trap_assert_stdout_unmatched ("*Comparison: A_idle=*");
      if (scenario < 30)
        {
          g_test_trap_assert_stderr ("*SPI transfer failed:*");
        }
      else if (scenario < 50)
        {
          g_test_trap_assert_stderr ("*Interrupted during SPI transfer*");
          g_test_trap_assert_stdout (scenario < 40 ? "*MOCK EXIT STATUS VERIFIED: 143*" :
                                     "*MOCK EXIT STATUS VERIFIED: 130*");
        }
      else if (scenario < 70)
        {
          g_test_trap_assert_stderr ("*ERROR:*");
        }
      else if (scenario < 90)
        {
          g_test_trap_assert_stderr ("*comparison controller no longer active*");
        }
      else
        {
          g_test_trap_assert_stderr ("");
          g_test_trap_assert_stdout (scenario < 92 ? "*MOCK EXIT STATUS VERIFIED: 143*" :
                                     "*MOCK EXIT STATUS VERIFIED: 130*");
        }
    }
}

static void
test_comparison_exclusive (gconstpointer data)
{
  if (g_test_subprocess ())
    {
      reset_mocks ();
      gchar *argv[] = { (gchar *) "diagnostic", (gchar *) "--compare-soft-reset", (gchar *) data,
                        (gchar *) "/test/invalid-firmware", NULL };
      int argc = g_str_equal (data, "--test-vendor-recovery") ? 4 : 3;
      exit (medion_diagnostic_main (argc, argv));
    }
  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_failed ();
  g_test_trap_assert_stderr ("*Choose exactly one*");
  g_test_trap_assert_stderr_unmatched ("*Unexpected hardware*");
}

static void
test_comparison_speed (void)
{
  if (g_test_subprocess ())
    {
      reset_mocks ();
      gchar *argv[] = { (gchar *) "diagnostic", (gchar *) "--compare-soft-reset",
                        (gchar *) "--speed", (gchar *) "250000", NULL };
      exit (medion_diagnostic_main (4, argv));
    }
  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_failed ();
  g_test_trap_assert_stderr ("*fixes the SPI speed at 1000000 Hz*");
  g_test_trap_assert_stderr_unmatched ("*Unexpected hardware*");
}

static void
test_legacy_id_speed (void)
{
  if (g_test_subprocess ())
    {
      reset_mocks ();
      gchar *argv[] = { (gchar *) "diagnostic", (gchar *) "--legacy-id-no-init",
                        (gchar *) "--speed", (gchar *) "250000", NULL };
      exit (medion_diagnostic_main (4, argv));
    }
  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_failed ();
  g_test_trap_assert_stderr ("*--legacy-id-no-init fixes the SPI speed at 1000000 Hz*");
  g_test_trap_assert_stderr_unmatched ("*Unexpected hardware*");
}

static void
test_legacy_historical_speed (void)
{
  if (g_test_subprocess ())
    {
      reset_mocks ();
      gchar *argv[] = { (gchar *) "diagnostic",
                        (gchar *) "--legacy-historical-id",
                        (gchar *) "--speed", (gchar *) "1000000", NULL };
      exit (medion_diagnostic_main (4, argv));
    }
  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_failed ();
  g_test_trap_assert_stderr ("*--legacy-historical-id fixes the historical SPI speed at 4000000 Hz*");
  g_test_trap_assert_stderr_unmatched ("*Unexpected hardware*");
}

static void
test_legacy_ctfdavis_speed (void)
{
  if (g_test_subprocess ())
    {
      reset_mocks ();
      gchar *argv[] = { (gchar *) "diagnostic",
                        (gchar *) "--legacy-ctfdavis-id",
                        (gchar *) "--speed", (gchar *) "1000000", NULL };
      exit (medion_diagnostic_main (4, argv));
    }
  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_failed ();
  g_test_trap_assert_stderr ("*--legacy-ctfdavis-id derives speed from the device maximum, capped at 1000000 Hz*");
  g_test_trap_assert_stderr_unmatched ("*Unexpected hardware*");
}

static void
test_legacy_ctfdavis_exclusive (void)
{
  if (g_test_subprocess ())
    {
      reset_mocks ();
      gchar *argv[] = { (gchar *) "diagnostic",
                        (gchar *) "--legacy-ctfdavis-id",
                        (gchar *) "--legacy-historical-id",
                        NULL };
      exit (medion_diagnostic_main (3, argv));
    }
  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_failed ();
  g_test_trap_assert_stderr ("*Choose exactly one*");
  g_test_trap_assert_stderr_unmatched ("*Unexpected hardware*");
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
  g_test_add_func ("/medion-diagnostic/legacy-id/transfer", test_legacy_transfer);
  g_test_add_data_func ("/medion-diagnostic/legacy-id/transfer-error",
                        GINT_TO_POINTER (MOCK_ERROR), test_legacy_transfer_failure);
  g_test_add_data_func ("/medion-diagnostic/legacy-id/transfer-short",
                        GINT_TO_POINTER (MOCK_SHORT), test_legacy_transfer_failure);
  g_test_add_func ("/medion-diagnostic/legacy-id/crc", test_vendor_crc);
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
  const gchar *legacy_scenarios[] = {
    "recognized-crc", "recognized-zero-trailer", "all-zero", "all-ff",
    "unknown-crc", "recognized-bad-crc", "9361-not-recognized", "ffff-zero-trailer",
    "transfer-error", "transfer-short", "pre-power-failure", "post-power-failure",
    "sigterm-eintr", "sigint-completed-ioctl", "config-read-failure",
    "config-write-failure", "restore-failure",
  };
  for (guint i = 0; i < G_N_ELEMENTS (legacy_scenarios); i++)
    {
      g_autofree gchar *path = g_strdup_printf ("/medion-diagnostic/legacy-id/cli/%s",
                                                legacy_scenarios[i]);
      g_test_add_data_func (path, GUINT_TO_POINTER (i), test_legacy_id_cli);
    }
  const gchar *historical_scenarios[] = {
    "fw9362-c6-attempt-1", "fw9362-c6-attempt-2",
    "fw9362-c6-attempt-3", "fw9362-c6-attempt-4", "c6-fails",
    "shifted-id", "all-zero", "c6-write-error", "c6-write-short",
    "c6-read-error", "c6-read-short", "identity-read-error",
    "identity-read-short", "zero-max-speed-refused",
  };
  for (guint i = 0; i < G_N_ELEMENTS (historical_scenarios); i++)
    {
      g_autofree gchar *path = g_strdup_printf (
        "/medion-diagnostic/legacy-historical/cli/%s",
        historical_scenarios[i]);
      g_test_add_data_func (path, GUINT_TO_POINTER (i),
                            test_legacy_historical_cli);
    }
  const gchar *ctfdavis_scenarios[] = {
    "fw9362-c6-attempt-1", "fw9362-c6-attempt-2",
    "fw9362-c6-attempt-3", "fw9362-c6-attempt-4", "c6-fails",
    "shifted-id", "all-zero", "c6-write-error", "c6-write-short",
    "c6-read-error", "c6-read-short", "identity-read-error",
    "identity-read-short", "zero-max-speed-refused", "high-max-speed-cap",
  };
  for (guint i = 0; i < G_N_ELEMENTS (ctfdavis_scenarios); i++)
    {
      g_autofree gchar *path = g_strdup_printf (
        "/medion-diagnostic/legacy-ctfdavis/cli/%s",
        ctfdavis_scenarios[i]);
      g_test_add_data_func (path, GUINT_TO_POINTER (i),
                            test_legacy_ctfdavis_cli);
    }
  const gchar *legacy_failure_scenarios[] = {
    "speed-restore-error", "sigterm-first-10ms", "sigterm-c6-4ms",
    "sigint-first-10ms", "sigint-c6-4ms", "sigterm-second-10ms",
    "sigint-second-10ms", "gpio-request-error", "gpio-initial-set-error",
    "gpio-initial-read-error", "gpio-cleanup-high-error",
    "gpio-pulse-set-error", "gpio-pulse-read-error",
  };
  for (guint i = 0; i < G_N_ELEMENTS (legacy_failure_scenarios); i++)
    {
      g_autofree gchar *historical_path = g_strdup_printf (
        "/medion-diagnostic/legacy-historical/cli/%s",
        legacy_failure_scenarios[i]);
      g_test_add_data_func (historical_path, GUINT_TO_POINTER (15 + i),
                            test_legacy_historical_cli);
      if (i <= 4)
        {
          g_autofree gchar *ctfdavis_path = g_strdup_printf (
            "/medion-diagnostic/legacy-ctfdavis/cli/%s",
            legacy_failure_scenarios[i]);
          g_test_add_data_func (ctfdavis_path, GUINT_TO_POINTER (15 + i),
                                test_legacy_ctfdavis_cli);
        }
    }
  const gchar *comparison_results[] = { "neither-idle", "becomes-idle", "loses-idle",
                                        "both-idle", "all-ff", "unknown-data" };
  for (guint i = 0; i < G_N_ELEMENTS (comparison_results); i++)
    {
      g_autofree gchar *path = g_strdup_printf ("/medion-diagnostic/comparison/%s", comparison_results[i]);
      g_test_add_data_func (path, GUINT_TO_POINTER (i), test_comparison_cli);
    }
  const struct { const gchar *name;
                 guint        start;
                 guint        count;
  } comparison_failures[] = {
    { "transfer-error", 10, 6 }, { "transfer-short", 20, 6 },
    { "sigterm-eintr", 30, 6 }, { "sigint-completed-ioctl", 40, 6 },
    { "config-read", 50, 6 }, { "config-write", 60, 3 },
    { "controller-state", 70, 4 }, { "restore-error", 80, 1 },
    { "signal-during-sleep", 90, 4 },
  };
  for (guint i = 0; i < G_N_ELEMENTS (comparison_failures); i++)
    for (guint j = 0; j < comparison_failures[i].count; j++)
      {
        g_autofree gchar *path = g_strdup_printf ("/medion-diagnostic/comparison/%s/%u",
                                                  comparison_failures[i].name, j + 1);
        g_test_add_data_func (path, GUINT_TO_POINTER (comparison_failures[i].start + j), test_comparison_cli);
      }
  const gchar *comparison_conflicts[] = { "--status-no-reset", "--probe", "--reset",
                                          "--chip-id", "--test-vendor-recovery",
                                          "--legacy-id-no-init" };
  for (guint i = 0; i < G_N_ELEMENTS (comparison_conflicts); i++)
    {
      g_autofree gchar *path = g_strdup_printf ("/medion-diagnostic/comparison/exclusive/%s",
                                                comparison_conflicts[i] + 2);
      g_test_add_data_func (path, comparison_conflicts[i], test_comparison_exclusive);
    }
  g_test_add_func ("/medion-diagnostic/comparison/fixed-speed", test_comparison_speed);
  g_test_add_func ("/medion-diagnostic/legacy-id/fixed-speed", test_legacy_id_speed);
  g_test_add_func ("/medion-diagnostic/legacy-historical/fixed-speed",
                   test_legacy_historical_speed);
  g_test_add_func ("/medion-diagnostic/legacy-ctfdavis/fixed-speed",
                   test_legacy_ctfdavis_speed);
  g_test_add_func ("/medion-diagnostic/legacy-ctfdavis/exclusive",
                   test_legacy_ctfdavis_exclusive);
  int result = g_test_run ();
  if (events)
    g_string_free (events, TRUE);
  g_clear_pointer (&tx_frames, g_ptr_array_unref);
  return result;
}
