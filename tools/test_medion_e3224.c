/*
 * Dedicated Hardware Diagnostic & Firmware Recovery Test for Medion Akoya E3224
 * Experimental recovery diagnostic. Successful host transfers do not prove
 * that the sensor accepted a command or that its power rail is enabled.
 *
 * SPDX-FileCopyrightText: 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <gpiod.h>
#include <gudev/gudev.h>
#include <linux/spi/spidev.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "medion-power.h"
#include "../libfprint/drivers/fte3600-gpio.h"

#define FW_SIZE 10396
#define FW_SHA "027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f"

#define PIN_GPO1_RESET 0x27  /* Pin 39 on \\_SB.GPO1 - sole reset output pin */

static struct gpiod_line_request *req_gpo1 = NULL;
static int spi_fd = -1;
static MedionPower power_state;
static volatile sig_atomic_t interrupted;
static gboolean cleanup_failed;
static uint32_t cur_speed_hz = 1000000;

static void fail (const char *what)
{
  fprintf (stderr, "ERROR: %s\n", what);
  exit (1);
}

static uint32_t parse_spi_speed (const char *value)
{
  if (!value || !*value)
    fail ("--speed requires an integer from 1 to 1000000 Hz");
  for (const char *p = value; *p; p++)
    if (!g_ascii_isdigit (*p))
      fail ("--speed requires an integer from 1 to 1000000 Hz");
  errno = 0;
  guint64 speed = g_ascii_strtoull (value, NULL, 10);
  if (errno == ERANGE || speed == 0 || speed > 1000000)
    fail ("--speed requires an integer from 1 to 1000000 Hz");
  return (uint32_t) speed;
}

static void cleanup (void)
{
  if (req_gpo1)
    {
      if (gpiod_line_request_set_value (req_gpo1, PIN_GPO1_RESET,
                                        GPIOD_LINE_VALUE_ACTIVE) < 0)
        {
          fprintf (stderr, "ERROR: release reset high: %s\n", strerror (errno));
          cleanup_failed = TRUE;
        }
      gpiod_line_request_release (req_gpo1);
      req_gpo1 = NULL;
    }
  if (spi_fd >= 0)
    {
      close (spi_fd);
      spi_fd = -1;
    }
  g_autoptr (GError) error = NULL;
  if (!medion_power_restore (&power_state, &error))
    {
      fprintf (stderr, "ERROR: restoring runtime PM: %s\n", error->message);
      cleanup_failed = TRUE;
    }
  medion_power_clear (&power_state);
}

static void sig_handler (int sig)
{
  /* Do not call GLib or libgpiod from an asynchronous signal handler. */
  interrupted = sig;
}

static void require (int ok, const char *what)
{
  if (!ok)
    {
      fprintf (stderr, "ERROR: %s: %s\n", what, strerror (errno));
      exit (1);
    }
}

static void check_dmi (void)
{
  gchar *vendor = NULL;
  gchar *product = NULL;
  g_autofree gchar *version = NULL;
  g_autofree gchar *board = NULL;

  g_file_get_contents ("/sys/class/dmi/id/sys_vendor", &vendor, NULL, NULL);
  g_file_get_contents ("/sys/class/dmi/id/product_name", &product, NULL, NULL);
  g_file_get_contents ("/sys/class/dmi/id/product_version", &version, NULL, NULL);
  g_file_get_contents ("/sys/class/dmi/id/board_name", &board, NULL, NULL);

  if (vendor)
    g_strstrip (vendor);
  if (product)
    g_strstrip (product);
  if (version)
    g_strstrip (version);
  if (board)
    g_strstrip (board);

  printf ("Platform DMI: Vendor='%s', Product='%s'\n", vendor ? vendor : "",
          product ? product : "");

  printf ("Profile: version='%s', board='%s'\n", version ? version : "",
          board ? board : "");
  if (g_strcmp0 (vendor, "MEDION") != 0 || g_strcmp0 (product, "E3224") != 0 ||
      g_strcmp0 (version, "FT") != 0 || g_strcmp0 (board, "YS13G") != 0)
    fail ("Expected MEDION / E3224 / FT / YS13G; GPIO access refused");
  g_free (vendor);
  g_free (product);
}

static gchar *find_gpiochip_for_acpi (const gchar *target_acpi_path)
{
  const gchar *subsystems[] = { "gpio", NULL };
  g_autoptr (GUdevClient) client = g_udev_client_new (subsystems);
  GList *gpio_devices = g_udev_client_query_by_subsystem (client, "gpio");
  gchar *result = NULL;

  for (GList *iter = gpio_devices; iter; iter = iter->next)
    {
      GUdevDevice *dev = iter->data;
      const gchar *sysfs = g_udev_device_get_sysfs_path (dev);
      const gchar *dev_file = g_udev_device_get_device_file (dev);
      g_autofree gchar *node_path = NULL;
      g_autofree gchar *path_file = NULL;

      if (!sysfs || !dev_file)
        continue;

      path_file = g_build_filename (sysfs, "firmware_node", "path", NULL);
      if (!g_file_test (path_file, G_FILE_TEST_EXISTS))
        {
          g_clear_pointer (&path_file, g_free);
          path_file = g_build_filename (sysfs, "device", "firmware_node", "path", NULL);
        }
      if (g_file_get_contents (path_file, &node_path, NULL, NULL))
        {
          g_strchomp (node_path);
          if (fte3600_acpi_path_equal (node_path, target_acpi_path))
            {
              if (result)
                fail ("Multiple GPIO controllers match the reset ACPI path");
              result = g_strdup (dev_file);
              printf ("GPIO controller: %s ACPI=%s\n", sysfs, node_path);
              g_autofree gchar *firmware_node = g_path_get_dirname (path_file);
              const gchar *attributes[] = { "hid", "uid" };
              for (guint i = 0; i < G_N_ELEMENTS (attributes); i++)
                {
                  g_autofree gchar *attribute_path = g_build_filename (firmware_node, attributes[i], NULL);
                  g_autofree gchar *value = NULL;
                  if (g_file_get_contents (attribute_path, &value, NULL, NULL))
                    printf ("  GPIO %s=%s\n", attributes[i], g_strstrip (value));
                }
              g_autoptr (GUdevDevice) parent = g_udev_device_get_parent (dev);
              if (parent)
                printf ("  GPIO parent=%s driver=%s\n", g_udev_device_get_sysfs_path (parent),
                        g_udev_device_get_driver (parent) ? g_udev_device_get_driver (parent) : "(none)");
            }
        }
    }
  g_list_free_full (gpio_devices, g_object_unref);
  return result;
}

static gchar *resolve_spi_device (const gchar *requested, gchar **sysfs_path)
{
  const gchar *subsystems[] = { "spidev", NULL };
  g_autoptr (GUdevClient) client = g_udev_client_new (subsystems);
  GList *devices = g_udev_client_query_by_subsystem (client, "spidev");
  g_autofree gchar *sensor = realpath ("/sys/bus/spi/devices/spi-FTE3600:00", NULL);
  gchar *result = NULL;

  require (sensor != NULL, "resolve FTE3600 SPI sysfs device");
  for (GList *item = devices; item; item = item->next)
    {
      GUdevDevice *dev = item->data;
      g_autoptr (GUdevDevice) parent = g_udev_device_get_parent (dev);
      if (parent && g_strcmp0 (g_udev_device_get_sysfs_path (parent), sensor) == 0)
        {
          if (result)
            fail ("Multiple spidev nodes for FTE3600");
          result = g_strdup (g_udev_device_get_device_file (dev));
        }
    }
  g_list_free_full (devices, g_object_unref);
  if (!result)
    fail ("No spidev character node for ACPI FTE3600");
  if (requested)
    {
      struct stat expected, supplied;
      require (stat (result, &expected) == 0, "stat discovered SPI node");
      require (stat (requested, &supplied) == 0, "stat requested SPI node");
      if (!S_ISCHR (supplied.st_mode) || expected.st_rdev != supplied.st_rdev)
        fail ("--spi must refer to the discovered FTE3600 character device");
    }
  printf ("SPI topology: %s -> %s\n", result, sensor);
  *sysfs_path = g_steal_pointer (&sensor);
  return result;
}

static guint8 *load_verified_firmware (const gchar *path)
{
  guint8 *data = g_malloc (FW_SIZE + 1);
  struct stat st;
  gsize length = 0;
  int fd = open (path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  require (fd >= 0, "open firmware");
  require (fstat (fd, &st) == 0, "stat firmware");
  if (!S_ISREG (st.st_mode) || st.st_size != FW_SIZE)
    fail ("Firmware must be a regular 10396-byte file");
  while (length < FW_SIZE + 1)
    {
      ssize_t n = read (fd, data + length, FW_SIZE + 1 - length);
      if (n < 0 && errno == EINTR)
        continue;
      require (n >= 0, "read firmware");
      if (n == 0)
        break;
      length += n;
    }
  require (close (fd) == 0, "close firmware");
  g_autofree gchar *sha = g_compute_checksum_for_data (G_CHECKSUM_SHA256, data, length);
  if (length != FW_SIZE || g_strcmp0 (sha, FW_SHA) != 0)
    fail ("Firmware size or SHA256 mismatch");
  printf ("Firmware: %u bytes, SHA256=%s (verified)\n", FW_SIZE, sha);
  return data;
}

static void spi_xfer (const void *tx, void *rx, size_t len, uint32_t speed_hz)
{
  if (interrupted)
    exit (128 + interrupted);
  struct spi_ioc_transfer t = {
    .tx_buf = (uintptr_t) tx,
    .rx_buf = (uintptr_t) rx,
    .len = len,
    .speed_hz = speed_hz ? speed_hz : cur_speed_hz,
    .bits_per_word = 8,
  };
  int rc = ioctl (spi_fd, SPI_IOC_MESSAGE (1), &t);
  if (rc != (int) len)
    {
      fprintf (stderr, "SPI transfer failed: len=%zu, rc=%d (%s)\n", len, rc,
               rc < 0 ? strerror (errno) : "short transfer");
      exit (1);
    }
}

static void set_spi_mode (uint8_t mode)
{
  require (ioctl (spi_fd, SPI_IOC_WR_MODE, &mode) == 0, "set SPI mode");
}

static void set_pin39 (int high)
{
  if (interrupted)
    exit (128 + interrupted);
  if (!req_gpo1)
    fail ("Reset GPIO is not claimed");
  enum gpiod_line_value val = high ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
  require (gpiod_line_request_set_value (req_gpo1, PIN_GPO1_RESET, val) == 0,
           "set reset GPIO value");
}

static void report_reset_value (int high)
{
  int actual = gpiod_line_request_get_value (req_gpo1, PIN_GPO1_RESET);
  require (actual >= 0, "read back reset GPIO value");
  printf ("  Reset GPIO requested=%d readback=%d (not an electrical measurement)\n",
          high, actual);
  if (actual != high)
    fail ("Reset GPIO readback does not match requested value");
}

/*
 * Official Windows reset pulse from clsSpiDev::ft_interface_spi_ResetDevice (0x18002f5e0):
 * 1. Write 1 (High / Deasserted)
 * 2. Sleep 10ms
 * 3. Write 0 (Low / Asserted)
 * 4. Sleep 20ms
 * 5. Write 1 (High / Deasserted)
 */
static void windows_reset_pulse (void)
{
  set_pin39 (1);
  g_usleep (10000);
  set_pin39 (0);
  g_usleep (20000);
  set_pin39 (1);
}

/* Write MCU Register: 0x11 0xee <reg> <val> 0x00 */
static void write_mcu_reg (uint8_t reg, uint8_t val)
{
  uint8_t tx[5] = { 0x11, 0xee, reg, val, 0x00 };
  spi_xfer (tx, NULL, sizeof (tx), cur_speed_hz);
  g_usleep (2000);
}

/* Read MCU Register: TX 0x10 0xef <reg> 0x00 0x00 -> RX byte index 4 */
static uint8_t read_mcu_reg (uint8_t reg)
{
  uint8_t tx[5] = { 0x10, 0xef, reg, 0x00, 0x00 };
  uint8_t rx[5] = { 0 };
  spi_xfer (tx, rx, sizeof (tx), cur_speed_hz);
  return rx[4];
}

/* Query Bootloader ROM Edition (0x90 0x00 0x00 -> byte index 2 is 0xef for Edition A) */
static uint8_t query_bootloader_edition (void)
{
  uint8_t tx[3] = { 0x90, 0x00, 0x00 };
  uint8_t rx[3] = { 0 };
  spi_xfer (tx, rx, sizeof (tx), cur_speed_hz);
  return rx[2];
}

/* JudgeByChipId in Windows 2.0.3.102 uses scratch address 0x85c0 only for
 * non-Edition-A bootloaders. This is an explicit mutating diagnostic, not
 * part of the ordinary status probe or FT9361 firmware recovery. */
static int probe_chip_id (void)
{
  uint8_t tx_status[6] = { 0x10, 0xef, 0x20, 0, 0, 0 };
  uint8_t rx_status[6] = { 0 };
  spi_xfer (tx_status, rx_status, sizeof (tx_status), cur_speed_hz);
  printf ("MCU status before chip-ID probe: %02x %02x\n", rx_status[4], rx_status[5]);
  if (!((rx_status[4] == 0 && rx_status[5] == 0) ||
        (rx_status[4] == 0xff && rx_status[5] == 0xff)))
    {
      fprintf (stderr, "Chip-ID scratch writes refused: MCU returned runtime/status data.\n");
      return 2;
    }

  uint8_t edition = query_bootloader_edition ();
  printf ("ROM edition response: %02x\n", edition);
  if (edition == 0xef)
    {
      fprintf (stderr, "Chip-ID scratch writes refused: Edition A requires a different OTP probe.\n");
      return 2;
    }

  uint8_t command[] = { 0x06, 0xf9, 0x00 };
  uint8_t scratch[] = { 0x05, 0xfa, 0x85, 0xc0, 0x00, 0x04,
                        0x11, 0xee, 0x02, 0x00, 0x00 };
  uint8_t trigger[] = { 0x09, 0xf6, 0xa4, 0x01 };
  /* Two opcode bytes + two address bytes + two dummy bytes + two RX bytes. */
  uint8_t tx_read[] = { 0x04, 0xfb, 0x85, 0xc0, 0, 0, 0, 0 };
  uint8_t rx_read[8] = { 0 };
  printf ("Testing bootloader chip ID (writes scratch address 85c0; no hardware reset)...\n");
  spi_xfer (command, NULL, sizeof (command), cur_speed_hz);
  spi_xfer (scratch, NULL, sizeof (scratch), cur_speed_hz);
  g_usleep (2000);
  spi_xfer (trigger, NULL, sizeof (trigger), cur_speed_hz);
  spi_xfer (tx_read, rx_read, sizeof (tx_read), cur_speed_hz);

  uint16_t chip_id = ((uint16_t) rx_read[6] << 8) | rx_read[7];
  if (chip_id == 0x2b50 || chip_id == 0x95a8 || chip_id == 0x23dd)
    {
      printf ("Bootloader chip ID=%04x: FT95A8 family response (not an exact FT9361 identification).\n",
              chip_id);
      return 0;
    }
  printf ("Bootloader chip ID=%04x: %s; this does not establish sensor power.\n", chip_id,
          chip_id == 0 || chip_id == 0xffff ? "inconclusive response" : "unknown family");
  return 2;
}

static gboolean probe_status_and_id (const char *tag, uint32_t speed_hz)
{
  uint8_t tx_status[6] = { 0x10, 0xef, 0x20, 0x00, 0x00, 0x00 };
  uint8_t rx_status[6] = { 0 };
  uint8_t tx_id[6] = { 0x10, 0xef, 0x14, 0x00, 0x00, 0x00 };
  uint8_t rx_id[6] = { 0 };

  spi_xfer (tx_status, rx_status, sizeof (tx_status), speed_hz);
  spi_xfer (tx_id, rx_id, sizeof (tx_id), speed_hz);

  gboolean has_nonzero = FALSE;
  for (int i = 0; i < 6; i++)
    {
      if (rx_status[i] != 0x00 && rx_status[i] != 0xff)
        has_nonzero = TRUE;
      if (rx_id[i] != 0x00 && rx_id[i] != 0xff)
        has_nonzero = TRUE;
    }

  const char *mark = "";
  if (rx_id[4] == 0x40 && rx_id[5] == 0x50)
    mark = " >>> [MATCH! FT9361 SENSOR ID 0x40 0x50 DETECTED!] <<<";
  else if (rx_status[4] == 0xa5 && rx_status[5] == 0x5a)
    mark = " >>> [MATCH! FT9361 MCU IDLE (a5 5a) DETECTED!] <<<";
  else if (has_nonzero)
    mark = " *** [NON-ZERO SPI DATA RECEIVED!] ***";

  printf ("  [%-24s] Status 0x20: %02x %02x %02x %02x [%02x %02x]  |  ID 0x14: %02x %02x %02x %02x [%02x %02x]%s\n",
          tag,
          rx_status[0], rx_status[1], rx_status[2], rx_status[3], rx_status[4], rx_status[5],
          rx_id[0], rx_id[1], rx_id[2], rx_id[3], rx_id[4], rx_id[5],
          mark);

  return (rx_status[4] == 0xa5 && rx_status[5] == 0x5a);
}

static void transfer_firmware_and_start (const guint8 *firmware)
{
  g_autofree guint8 *packet = g_malloc (FW_SIZE + 7);
  packet[0] = 0x05;
  packet[1] = 0xfa;
  packet[2] = 0x00;
  packet[3] = 0x00;
  packet[4] = (FW_SIZE >> 8) & 0xff;
  packet[5] = FW_SIZE & 0xff;
  memcpy (packet + 6, firmware, FW_SIZE);
  packet[6 + FW_SIZE] = 0x00;

  /* No extra probe, log or delay between reset release and download sync. */
  printf ("\n[3/7] Entry reset (High 10ms -> Low 20ms -> High), then sync 55 aa...\n");
  windows_reset_pulse ();
  guint8 sync[2] = { 0x55, 0xaa };
  spi_xfer (sync, NULL, sizeof (sync), cur_speed_hz);

  printf ("\n[4/7] Sending 10,403-byte firmware packet in one SPI transaction...\n");
  spi_xfer (packet, NULL, FW_SIZE + 7, cur_speed_hz);
  g_usleep (2000);

  printf ("\n[5/7] ReturnIdleByReset: two hardware pulses, 160ms boot, dual 70...\n");
  windows_reset_pulse ();
  g_usleep (10000);
  windows_reset_pulse ();
  g_usleep (160000);
  guint8 soft = 0x70;
  spi_xfer (&soft, NULL, 1, cur_speed_hz);
  g_usleep (5000);
  spi_xfer (&soft, NULL, 1, cur_speed_hz);
  g_usleep (2000);
}

int main (int argc, char **argv)
{
  gboolean probe_only = FALSE;
  gboolean vendor_recover = FALSE;
  gboolean do_reset = FALSE;
  gboolean chip_id_only = FALSE;
  int result = 0;
  const char *firmware_file = NULL;
  const char *requested_spi = NULL;
  g_autofree gchar *spi_sysfs = NULL;
  g_autofree guint8 *firmware = NULL;

  for (int i = 1; i < argc; i++)
    {
      if (!strcmp (argv[i], "--probe"))
        probe_only = TRUE;
      else if (!strcmp (argv[i], "--reset"))
        do_reset = TRUE;
      else if (!strcmp (argv[i], "--chip-id"))
        chip_id_only = TRUE;
      else if (!strcmp (argv[i], "--test-vendor-recovery") && i + 1 < argc)
        {
          vendor_recover = TRUE;
          firmware_file = argv[++i];
        }
      else if (!strcmp (argv[i], "--spi") && i + 1 < argc)
        requested_spi = argv[++i];
      else if (!strcmp (argv[i], "--speed") && i + 1 < argc)
        cur_speed_hz = parse_spi_speed (argv[++i]);
      else
        {
          printf ("Usage: %s [OPTIONS]\n", argv[0]);
          printf ("Options:\n");
          printf ("  --probe                      Probe chip status and bootloader edition\n");
          printf ("  --chip-id                    Explicit scratch-RAM chip-family probe; no GPIO/reset\n");
          printf ("  --test-vendor-recovery <fw>  Test the experimental Medion recovery sequence:\n");
          printf ("                                 1. Soft reset & idle check\n");
          printf ("                                 2. ROM query before entry reset pulse\n");
          printf ("                                 3. Reset -> immediate 0x55 0xaa Sync\n");
          printf ("                                 4. 10,403-byte firmware upload\n");
          printf ("                                 5. Double hardware reset, 160ms boot, dual 0x70\n");
          printf ("                                 6. MCU idle poll, sensor ID/version checks\n");
          printf ("                                 7. Runtime configuration and idle check\n");
          printf ("  --reset                      Perform Windows reset pulse on Pin 39\n");
          printf ("  --spi <device>               Verify node against discovered FTE3600 device\n");
          printf ("  --speed <hz>                 SPI speed 1..1000000 Hz (default: ACPI's 1000000)\n");
          printf ("  --help                       Show usage without accessing hardware\n");
          printf ("Runtime PM overrides are temporary and restored on exit.\n");
          return !strcmp (argv[i], "--help") ? 0 : 1;
        }
    }

  if (!probe_only && !vendor_recover && !do_reset && !chip_id_only)
    probe_only = TRUE;
  if (probe_only + vendor_recover + do_reset + chip_id_only != 1)
    fail ("Choose exactly one of --probe, --reset, --chip-id, or --test-vendor-recovery");

  setvbuf (stdout, NULL, _IOLBF, 0);
  signal (SIGINT, sig_handler);
  signal (SIGTERM, sig_handler);
  atexit (cleanup);

  printf ("=== Medion Akoya E3224 Hardware Diagnostic & Recovery Tool ===\n");
  printf ("Diagnostic revision: 2026-09-23.1 (checked runtime PM and transfers)\n");
  check_dmi ();
  if (vendor_recover)
    firmware = load_verified_firmware (firmware_file);
  g_autofree gchar *spi_dev = resolve_spi_device (requested_spi, &spi_sysfs);
  g_autoptr (GError) power_error = NULL;
  if (!medion_power_prepare (&power_state, spi_sysfs, "/sys/devices", &power_error))
    fail (power_error->message);

  /* Check SPI bufsiz */
  gchar *bufsiz_str = NULL;
  if (g_file_get_contents ("/sys/module/spidev/parameters/bufsiz", &bufsiz_str, NULL, NULL))
    {
      printf ("spidev bufsiz: %s", bufsiz_str);
      if (g_ascii_strtoull (bufsiz_str, NULL, 10) < 10403 && vendor_recover)
        {
          fprintf (stderr, "ERROR: spidev bufsiz must be at least 10403 (current: %s)\n", bufsiz_str);
          return 1;
        }
      g_free (bufsiz_str);
    }

  /* Resolve GPO1 (Pin 39 reset) */
  g_autofree gchar *chip_gpo1 = NULL;
  if (!chip_id_only)
    {
      chip_gpo1 = find_gpiochip_for_acpi ("\\_SB_.GPO1");
      printf ("Resolved GPO1 (Pin 39): %s\n", chip_gpo1 ? chip_gpo1 : "NOT FOUND");
    }
  if (!chip_gpo1 && (do_reset || vendor_recover))
    fail ("Reset controller ACPI path was not found; refusing to guess gpiochip0");

  /* Open SPI device */
  spi_fd = open (spi_dev, O_RDWR | O_CLOEXEC);
  require (spi_fd >= 0, "open SPI device");
  struct stat spi_stat;
  require (fstat (spi_fd, &spi_stat) == 0, "stat opened SPI device");
  g_autofree gchar *char_link = g_strdup_printf ("/sys/dev/char/%u:%u/device",
                                               major (spi_stat.st_rdev), minor (spi_stat.st_rdev));
  g_autofree gchar *opened_sysfs = realpath (char_link, NULL);
  if (!S_ISCHR (spi_stat.st_mode) || g_strcmp0 (opened_sysfs, spi_sysfs) != 0)
    fail ("Opened SPI node does not match FTE3600 sysfs device");
  set_spi_mode (SPI_MODE_0);
  uint8_t bits = 8, lsb = 0;
  require (ioctl (spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) == 0, "set SPI 8-bit");
  require (ioctl (spi_fd, SPI_IOC_WR_LSB_FIRST, &lsb) == 0, "set SPI MSB-first");
  uint8_t mode_read = 0, bits_read = 0, lsb_read = 0;
  require (ioctl (spi_fd, SPI_IOC_RD_MODE, &mode_read) == 0, "read SPI mode");
  require (ioctl (spi_fd, SPI_IOC_RD_BITS_PER_WORD, &bits_read) == 0, "read SPI word size");
  require (ioctl (spi_fd, SPI_IOC_RD_LSB_FIRST, &lsb_read) == 0, "read SPI bit order");
  printf ("SPI %s: mode=%u bits=%u lsb_first=%u; each transfer requests %u Hz\n",
          spi_dev, mode_read, bits_read, lsb_read, cur_speed_hz);
  if (mode_read != SPI_MODE_0 || bits_read != 8 || lsb_read != 0)
    fail ("SPI settings did not read back as requested");

  /* Request GPO1 Pin 39 (Active-Low reset line, idle = 1) */
  if (do_reset || vendor_recover)
    {
      struct gpiod_chip *chip = gpiod_chip_open (chip_gpo1);
      require (chip != NULL, "open reset GPIO controller");
        {
          struct gpiod_line_info *info = gpiod_chip_get_line_info (chip, PIN_GPO1_RESET);
          require (info != NULL, "get reset GPIO line info");
          printf ("Reset line name=%s consumer=%s used=%d\n",
                  gpiod_line_info_get_name (info) ? gpiod_line_info_get_name (info) : "(unnamed)",
                  gpiod_line_info_get_consumer (info) ? gpiod_line_info_get_consumer (info) : "(none)",
                  gpiod_line_info_is_used (info));
          gpiod_line_info_free (info);
          struct gpiod_line_settings *s = gpiod_line_settings_new ();
          struct gpiod_line_config *c = gpiod_line_config_new ();
          require (s != NULL && c != NULL, "allocate reset GPIO settings");
          require (gpiod_line_settings_set_direction (s, GPIOD_LINE_DIRECTION_OUTPUT) == 0,
                   "set reset GPIO direction");
          require (gpiod_line_settings_set_output_value (s, GPIOD_LINE_VALUE_ACTIVE) == 0,
                   "set reset GPIO initial high");
          unsigned off = PIN_GPO1_RESET;
          if (gpiod_line_config_add_line_settings (c, &off, 1, s) == 0)
            req_gpo1 = gpiod_chip_request_lines (chip, NULL, c);
          gpiod_line_settings_free (s);
          gpiod_line_config_free (c);
          gpiod_chip_close (chip);
          if (req_gpo1)
            printf ("Successfully claimed Reset Line (GPO1 Pin 39) on %s\n", chip_gpo1);
          else
            fail ("Could not claim reset GPIO; stop fprintd before testing");
          set_pin39 (1);
          report_reset_value (1);
        }
    }

  if (chip_id_only)
    {
      result = probe_chip_id ();
      goto done;
    }

  if (do_reset)
    {
      printf ("Executing official Windows reset pulse (1 -> 10ms -> 0 -> 20ms -> 1)...\n");
      windows_reset_pulse ();
      report_reset_value (1);
      printf ("Reset complete.\n");
      goto done;
    }

  if (probe_only)
    {
      printf ("============================================================\n");
      printf ("Probing Sensor & Bootloader Status\n");
      printf ("============================================================\n");

      /* 1. Soft reset */
      printf ("Sending dual 0x70 soft reset commands...\n");
      uint8_t soft = 0x70;
      spi_xfer (&soft, NULL, 1, cur_speed_hz);
      g_usleep (5000);
      spi_xfer (&soft, NULL, 1, cur_speed_hz);
      g_usleep (5000);

      gboolean is_idle = probe_status_and_id ("Post Soft-Reset", cur_speed_hz);
      if (is_idle)
        {
          printf ("\n>>> Sensor MCU is RUNNING and IDLE! <<<\n");
          uint8_t id_hi = read_mcu_reg (0x14);
          uint8_t id_lo = read_mcu_reg (0x15);
          printf ("    Sensor ID: 0x%02x 0x%02x\n", id_hi, id_lo);
          if (id_hi != 0x40 || id_lo != 0x50)
            fail ("Idle response received, but sensor ID does not match FT9361");
        }
      else
        {
          printf ("MCU is not in runtime idle state. Querying bootloader edition...\n");
          uint8_t ed = query_bootloader_edition ();
          printf ("Reg 0x90 response: 0x%02x %s\n", ed,
                  (ed == 0xef) ? "(Bootloader Edition A DETECTED)" : "(Not in Bootloader Edition A)");
          result = 2;
        }
    }

  if (vendor_recover)
    {
      printf ("============================================================\n");
      printf ("Testing FT9361 recovery (2.0.3.102 FT95a8 download / ReturnIdleByReset)\n");
      printf ("============================================================\n");

      /* Step 1: Soft reset check */
      printf ("\n[1/7] Soft reset & status probe...\n");
      uint8_t soft = 0x70;
      spi_xfer (&soft, NULL, 1, cur_speed_hz);
      g_usleep (5000);
      spi_xfer (&soft, NULL, 1, cur_speed_hz);
      g_usleep (2000);

      if (probe_status_and_id ("Initial Probe", cur_speed_hz))
        {
          printf ("  MCU is already idle; skipping firmware upload.\n");
          goto verify_identity;
        }

      printf ("\n[2/7] Querying bootloader edition before entry reset...\n");
      uint8_t boot_ed = query_bootloader_edition ();
      printf ("  ROM edition response: 0x%02x (00 alone does not prove loss of power)\n", boot_ed);

      transfer_firmware_and_start (firmware);

      printf ("\n[6/7] Polling MCU idle (a5 5a), then checking identity/version...\n");
      gboolean success = FALSE;
      for (unsigned attempt = 1; attempt <= 20; attempt++)
        {
          uint8_t tx_poll[6] = { 0x10, 0xef, 0x20, 0, 0, 0 };
          uint8_t rx_poll[6] = { 0 };
          spi_xfer (tx_poll, rx_poll, sizeof (tx_poll), cur_speed_hz);
          printf ("    Poll #%02u: Status = %02x %02x\n", attempt, rx_poll[4], rx_poll[5]);
          if (rx_poll[4] == 0xa5 && rx_poll[5] == 0x5a)
            {
              success = TRUE;
              break;
            }
          g_usleep (2000);
        }

      if (!success)
        {
          fprintf (stderr, "FAILED: host SPI transfers completed, but no MCU idle response.\n"
                   "This does not establish whether the sensor rail is powered.\n");
          result = 2;
          goto done;
        }

verify_identity:;
      uint8_t id_hi = read_mcu_reg (0x14);
      uint8_t id_lo = read_mcu_reg (0x15);
      uint8_t fw_version = read_mcu_reg (0x1a);
      uint8_t agc_version = read_mcu_reg (0x3c);
      printf ("Sensor ID=%02x %02x, FW=%02x, AGC=%02x\n",
              id_hi, id_lo, fw_version, agc_version);
      if (id_hi != 0x40 || id_lo != 0x50 || fw_version != 0x30 || agc_version != 0x31)
        fail ("Unexpected sensor ID or application versions");

      printf ("\n[7/7] Runtime configuration (after MCU idle, not an SRAM jump command)...\n");
      if (read_mcu_reg (0x30) != 0xbb)
        {
          write_mcu_reg (0x01, 0x01);
          write_mcu_reg (0x41, 0x0f);
          write_mcu_reg (0x30, 0xbb);
          if (read_mcu_reg (0x30) != 0xbb)
            fail ("Configuration marker did not read back as bb");
          write_mcu_reg (0x22, 0x00);
          write_mcu_reg (0x23, 0x0e);
        }
      if (!probe_status_and_id ("Final MCU idle", cur_speed_hz))
        {
          result = 2;
          goto done;
        }
      printf ("SUCCESS: verified FT9361 identity and MCU idle after configuration.\n");
    }

done:
  if (req_gpo1)
    report_reset_value (1);
  medion_power_report (&power_state, "after SPI operations");
  cleanup ();
  printf ("\nDiagnostic exit=%d; runtime PM restore=%s\n", result,
          cleanup_failed ? "FAILED (see errors)" : "complete");
  return cleanup_failed ? 1 : (interrupted ? 128 + interrupted : result);
}
