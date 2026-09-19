/*
 * Dedicated Hardware Diagnostic & Firmware Recovery Test for Medion Akoya E3224
 * Comprehensive pin matrix, power rail, and SPI bus probe
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
#include <unistd.h>

#define FW_SIZE 10396
#define FW_SHA "027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f"

#define PIN_GPO1_RESET 0x27  /* Pin 39 on \\_SB.GPO1 */
#define PIN_GPO2_AUX   0x00  /* Pin 0 on \\_SB.GPO2 */

static struct gpiod_line_request *req_gpo1 = NULL;
static struct gpiod_line_request *req_gpo2 = NULL;
static int spi_fd = -1;
static int cur_pin39_val = 1;
static int cur_pin0_val = 1;

static void cleanup (void)
{
  if (req_gpo1)
    {
      gpiod_line_request_release (req_gpo1);
      req_gpo1 = NULL;
    }
  if (req_gpo2)
    {
      gpiod_line_request_release (req_gpo2);
      req_gpo2 = NULL;
    }
  if (spi_fd >= 0)
    {
      close (spi_fd);
      spi_fd = -1;
    }
}

static void sig_handler (int sig)
{
  cleanup ();
  _exit (128 + sig);
}

static void require (int ok, const char *what)
{
  if (!ok)
    {
      fprintf (stderr, "ERROR: %s: %s\n", what, strerror (errno));
      exit (1);
    }
}

static void check_dmi (gboolean force)
{
  gchar *vendor = NULL;
  gchar *product = NULL;

  g_file_get_contents ("/sys/class/dmi/id/sys_vendor", &vendor, NULL, NULL);
  g_file_get_contents ("/sys/class/dmi/id/product_name", &product, NULL, NULL);

  if (vendor)
    g_strstrip (vendor);
  if (product)
    g_strstrip (product);

  printf ("Platform DMI: Vendor='%s', Product='%s'\n", vendor ? vendor : "",
          product ? product : "");

  if (g_strcmp0 (vendor, "MEDION") != 0 || g_strcmp0 (product, "E3224") != 0)
    {
      if (!force)
        {
          fprintf (stderr,
                   "ERROR: Not running on Medion Akoya E3224. Use --force to override.\n");
          exit (1);
        }
      printf ("WARNING: --force specified; proceeding despite DMI mismatch.\n");
    }
  g_free (vendor);
  g_free (product);
}

static void wake_device_power (const char *path)
{
  gchar *ctrl_path = g_build_filename (path, "power", "control", NULL);
  gchar *stat_path = g_build_filename (path, "power", "runtime_status", NULL);
  gchar *status = NULL;

  if (g_file_test (ctrl_path, G_FILE_TEST_EXISTS))
    {
      g_file_set_contents (ctrl_path, "on\n", -1, NULL);
      g_file_get_contents (stat_path, &status, NULL, NULL);
      if (status)
        g_strstrip (status);
      printf ("  Power: %s -> 'on' (status: %s)\n", path, status ? status : "unknown");
      g_free (status);
    }
  g_free (ctrl_path);
  g_free (stat_path);
}

static void awaken_spi_subsystem (void)
{
  printf ("--- Checking and Awakening SPI Power Management ---\n");
  /* Intel LPSS SPI controller PCI nodes */
  wake_device_power ("/sys/bus/pci/devices/0000:00:19.0");
  wake_device_power ("/sys/devices/pci0000:00/0000:00:19.0/pxa2xx-spi.12");
  wake_device_power ("/sys/devices/pci0000:00/0000:00:19.0/pxa2xx-spi.12/spi_master/spi1/spi-FTE3600:00");
  wake_device_power ("/sys/bus/spi/devices/spi-FTE3600:00");
  printf ("----------------------------------------------------\n\n");
}

static gboolean acpi_path_matches (const gchar *node_path, const gchar *target)
{
  if (!node_path || !target)
    return FALSE;

  const gchar *target_suffix = strrchr (target, '.');
  target_suffix = target_suffix ? target_suffix + 1 : target;

  if (g_str_has_suffix (node_path, target_suffix))
    return TRUE;

  if (g_strcmp0 (node_path, target) == 0)
    return TRUE;

  return FALSE;
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
          if (acpi_path_matches (node_path, target_acpi_path))
            {
              result = g_strdup (dev_file);
              break;
            }
        }
    }
  g_list_free_full (gpio_devices, g_object_unref);
  return result;
}

static void spi_xfer (const void *tx, void *rx, size_t len, uint32_t speed_hz)
{
  struct spi_ioc_transfer t = {
    .tx_buf = (uintptr_t) tx,
    .rx_buf = (uintptr_t) rx,
    .len = len,
    .speed_hz = speed_hz ? speed_hz : 1000000,
    .bits_per_word = 8,
  };
  int rc = ioctl (spi_fd, SPI_IOC_MESSAGE (1), &t);
  if (rc != (int) len)
    {
      fprintf (stderr, "SPI transfer failed: len=%zu, rc=%d (%s)\n", len, rc,
               rc < 0 ? strerror (errno) : "short transfer");
    }
}

static void set_spi_mode (uint8_t mode)
{
  require (ioctl (spi_fd, SPI_IOC_WR_MODE, &mode) == 0, "set SPI mode");
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

  return has_nonzero || (rx_id[4] == 0x40 && rx_id[5] == 0x50);
}

static void set_pin39 (int high)
{
  cur_pin39_val = high;
  if (req_gpo1)
    {
      enum gpiod_line_value val = high ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
      gpiod_line_request_set_value (req_gpo1, PIN_GPO1_RESET, val);
    }
}

static void set_pin0 (int high)
{
  cur_pin0_val = high;
  if (req_gpo2)
    {
      enum gpiod_line_value val = high ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
      gpiod_line_request_set_value (req_gpo2, PIN_GPO2_AUX, val);
    }
}

static void pulse_pin39 (int current_level, unsigned hold_ms)
{
  set_pin39 (!current_level);
  g_usleep (hold_ms * 1000);
  set_pin39 (current_level);
}

static void pulse_pin0 (int current_level, unsigned hold_ms)
{
  set_pin0 (!current_level);
  g_usleep (hold_ms * 1000);
  set_pin0 (current_level);
}

static void write_bootloader_reg (uint8_t reg, uint8_t val)
{
  uint8_t tx[4] = { 0x09, 0xf6, reg, val };
  spi_xfer (tx, NULL, sizeof (tx), 1000000);
}

static void dump_system_gpio_info (void)
{
  printf ("\n=== System GPIO Information ===\n");
  for (int i = 0; i < 8; i++)
    {
      g_autofree gchar *chip_dev = g_strdup_printf ("/dev/gpiochip%d", i);
      if (!g_file_test (chip_dev, G_FILE_TEST_EXISTS))
        continue;
      struct gpiod_chip *c = gpiod_chip_open (chip_dev);
      if (!c)
        continue;
      struct gpiod_chip_info *info = gpiod_chip_get_info (c);
      if (info)
        {
          printf ("  %s: label='%s', num_lines=%zu\n", chip_dev,
                  gpiod_chip_info_get_label (info),
                  gpiod_chip_info_get_num_lines (info));
          gpiod_chip_info_free (info);
        }
      gpiod_chip_close (c);
    }
  printf ("===============================\n\n");
}

int main (int argc, char **argv)
{
  gboolean force = FALSE;
  gboolean probe_matrix = FALSE;
  gboolean vendor_recover = FALSE;
  gboolean a1_recover = FALSE;
  const char *firmware_file = NULL;
  const char *spi_dev = "/dev/spidev1.0";
  int override_p39 = 1;
  int override_p0 = 1;

  for (int i = 1; i < argc; i++)
    {
      if (!strcmp (argv[i], "--force"))
        force = TRUE;
      else if (!strcmp (argv[i], "--probe-matrix") || !strcmp (argv[i], "--probe-polarities"))
        probe_matrix = TRUE;
      else if (!strcmp (argv[i], "--test-vendor-recovery") && i + 1 < argc)
        {
          vendor_recover = TRUE;
          firmware_file = argv[++i];
        }
      else if (!strcmp (argv[i], "--test-a1-recovery") && i + 1 < argc)
        {
          a1_recover = TRUE;
          firmware_file = argv[++i];
        }
      else if (!strcmp (argv[i], "--spi") && i + 1 < argc)
        spi_dev = argv[++i];
      else if (!strcmp (argv[i], "--pin39") && i + 1 < argc)
        override_p39 = atoi (argv[++i]);
      else if (!strcmp (argv[i], "--pin0") && i + 1 < argc)
        override_p0 = atoi (argv[++i]);
      else
        {
          printf ("Usage: %s [OPTIONS]\n", argv[0]);
          printf ("Options:\n");
          printf ("  --probe-matrix               Test all GPIO pin combinations (Pin39 and Pin0), SPI modes and speeds\n");
          printf ("  --test-vendor-recovery <fw>  Run official Windows sequence (5 writes + 10KB upload + autonomous boot)\n");
          printf ("  --test-a1-recovery <fw>      Run A1 sequence (upload + dual hardware reset pulses)\n");
          printf ("  --pin39 <0|1>                Set GPO1 Pin 39 baseline level (default: 1)\n");
          printf ("  --pin0 <0|1>                 Set GPO2 Pin 0 baseline level (default: 1)\n");
          printf ("  --spi <device>               SPI device path (default: /dev/spidev1.0)\n");
          printf ("  --force                      Bypass DMI verification\n");
          return 1;
        }
    }

  if (!probe_matrix && !vendor_recover && !a1_recover)
    probe_matrix = TRUE;

  signal (SIGINT, sig_handler);
  signal (SIGTERM, sig_handler);
  atexit (cleanup);

  printf ("=== Medion Akoya E3224 Comprehensive Hardware Probe & Diagnostic Tool ===\n");
  check_dmi (force);
  awaken_spi_subsystem ();
  dump_system_gpio_info ();

  /* Check SPI bufsiz */
  gchar *bufsiz_str = NULL;
  if (g_file_get_contents ("/sys/module/spidev/parameters/bufsiz", &bufsiz_str, NULL, NULL))
    {
      printf ("spidev bufsiz: %s", bufsiz_str);
      if (g_ascii_strtoull (bufsiz_str, NULL, 10) < 10403 && (vendor_recover || a1_recover))
        {
          fprintf (stderr, "ERROR: spidev bufsiz must be at least 10403 (current: %s)\n", bufsiz_str);
          return 1;
        }
      g_free (bufsiz_str);
    }

  /* Resolve GPIO controllers */
  g_autofree gchar *chip_gpo1 = find_gpiochip_for_acpi ("\\_SB_.GPO1");
  g_autofree gchar *chip_gpo2 = find_gpiochip_for_acpi ("\\_SB_.GPO2");

  printf ("Resolved GPO1 (Pin 39): %s\n", chip_gpo1 ? chip_gpo1 : "NOT FOUND (will fallback)");
  printf ("Resolved GPO2 (Pin 0) : %s\n", chip_gpo2 ? chip_gpo2 : "NOT FOUND (will fallback)");

  /* Fallback: if not found, use /dev/gpiochip0 and /dev/gpiochip2 */
  if (!chip_gpo1 && g_file_test ("/dev/gpiochip0", G_FILE_TEST_EXISTS))
    chip_gpo1 = g_strdup ("/dev/gpiochip0");
  if (!chip_gpo2 && g_file_test ("/dev/gpiochip2", G_FILE_TEST_EXISTS))
    chip_gpo2 = g_strdup ("/dev/gpiochip2");

  /* Open SPI device */
  spi_fd = open (spi_dev, O_RDWR | O_CLOEXEC);
  require (spi_fd >= 0, "open SPI device");
  set_spi_mode (SPI_MODE_0);
  uint8_t bits = 8, lsb = 0;
  require (ioctl (spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) == 0, "set SPI 8-bit");
  require (ioctl (spi_fd, SPI_IOC_WR_LSB_FIRST, &lsb) == 0, "set SPI MSB-first");
  printf ("SPI device %s opened successfully\n\n", spi_dev);

  /* Request GPO1 Pin 39 */
  if (chip_gpo1)
    {
      struct gpiod_chip *chip = gpiod_chip_open (chip_gpo1);
      if (chip)
        {
          struct gpiod_line_settings *s = gpiod_line_settings_new ();
          struct gpiod_line_config *c = gpiod_line_config_new ();
          gpiod_line_settings_set_direction (s, GPIOD_LINE_DIRECTION_OUTPUT);
          gpiod_line_settings_set_output_value (s, GPIOD_LINE_VALUE_ACTIVE);
          unsigned off = PIN_GPO1_RESET;
          if (gpiod_line_config_add_line_settings (c, &off, 1, s) == 0)
            req_gpo1 = gpiod_chip_request_lines (chip, NULL, c);
          gpiod_line_settings_free (s);
          gpiod_line_config_free (c);
          gpiod_chip_close (chip);
          if (req_gpo1)
            printf ("Successfully claimed Pin 39 on %s\n", chip_gpo1);
          else
            fprintf (stderr, "WARNING: Could not claim Pin 39 on %s: %s\n", chip_gpo1, strerror (errno));
        }
    }

  /* Request GPO2 Pin 0 */
  if (chip_gpo2)
    {
      struct gpiod_chip *chip = gpiod_chip_open (chip_gpo2);
      if (chip)
        {
          struct gpiod_line_settings *s = gpiod_line_settings_new ();
          struct gpiod_line_config *c = gpiod_line_config_new ();
          gpiod_line_settings_set_direction (s, GPIOD_LINE_DIRECTION_OUTPUT);
          gpiod_line_settings_set_output_value (s, GPIOD_LINE_VALUE_ACTIVE);
          unsigned off = PIN_GPO2_AUX;
          if (gpiod_line_config_add_line_settings (c, &off, 1, s) == 0)
            req_gpo2 = gpiod_chip_request_lines (chip, NULL, c);
          gpiod_line_settings_free (s);
          gpiod_line_config_free (c);
          gpiod_chip_close (chip);
          if (req_gpo2)
            printf ("Successfully claimed Pin 0 on %s\n", chip_gpo2);
          else
            fprintf (stderr, "WARNING: Could not claim Pin 0 on %s: %s\n", chip_gpo2, strerror (errno));
        }
    }

  /* --- MODE 1: MATRIX SWEEP --- */
  if (probe_matrix)
    {
      printf ("\n============================================================\n");
      printf ("PHASE 1: GPIO Matrix Level Sweep (Pin 39 & Pin 0)\n");
      printf ("============================================================\n");

      int states[4][2] = {
        { 1, 1 },  /* Default active */
        { 1, 0 },  /* Pin 39 high, Pin 0 low */
        { 0, 1 },  /* Pin 39 low,  Pin 0 high */
        { 0, 0 },  /* Both low */
      };

      for (int s = 0; s < 4; s++)
        {
          int p39 = states[s][0];
          int p0 = states[s][1];
          printf ("\n------------------------------------------------------------\n");
          printf ("State [%d/4]: Pin 39 = %d, Pin 0 = %d\n", s + 1, p39, p0);
          printf ("------------------------------------------------------------\n");

          set_pin39 (p39);
          set_pin0 (p0);
          g_usleep (30000); /* 30ms power/settle */

          set_spi_mode (SPI_MODE_0);
          probe_status_and_id ("Mode 0 (1MHz)", 1000000);

          set_spi_mode (SPI_MODE_3);
          probe_status_and_id ("Mode 3 (1MHz)", 1000000);

          set_spi_mode (SPI_MODE_0);
          probe_status_and_id ("Mode 0 (500kHz)", 500000);

          /* Test pulse on Pin 39 */
          pulse_pin39 (p39, 10);
          g_usleep (20000);
          probe_status_and_id ("After Pulse Pin 39", 1000000);

          /* Test pulse on Pin 0 */
          pulse_pin0 (p0, 10);
          g_usleep (20000);
          probe_status_and_id ("After Pulse Pin 0", 1000000);

          /* Soft-reset commands */
          uint8_t soft = 0x70;
          spi_xfer (&soft, NULL, 1, 1000000);
          g_usleep (5000);
          spi_xfer (&soft, NULL, 1, 1000000);
          g_usleep (5000);
          probe_status_and_id ("After Soft Reset (0x70)", 1000000);
        }
    }

  /* --- MODE 2: VENDOR RECOVERY SEQUENCE --- */
  if (vendor_recover)
    {
      printf ("\n============================================================\n");
      printf ("Running Vendor Recovery (Baseline Pin39=%d, Pin0=%d)\n", override_p39, override_p0);
      printf ("============================================================\n");

      set_pin39 (override_p39);
      set_pin0 (override_p0);
      g_usleep (30000);

      g_autofree gchar *fw_buf = NULL;
      gsize fw_len = 0;
      require (g_file_get_contents (firmware_file, &fw_buf, &fw_len, NULL), "read firmware");
      require (fw_len == FW_SIZE, "verify firmware size (10396 bytes)");

      /* 1. Pulse reset to enter bootloader */
      pulse_pin39 (override_p39, 10);
      g_usleep (20000);
      probe_status_and_id ("Pre-Sync Status", 1000000);

      /* 2. Sync */
      uint8_t sync[2] = { 0x55, 0xaa };
      spi_xfer (sync, NULL, sizeof (sync), 1000000);
      printf ("Sent Bootloader Sync (0x55 0xaa)\n");

      /* 3. Five vendor register writes */
      printf ("Sending 5 vendor pre-upload register writes...\n");
      write_bootloader_reg (0xc8, 0xff);
      write_bootloader_reg (0xca, 0xff);
      write_bootloader_reg (0xcb, 0xff);
      write_bootloader_reg (0xb9, 0xbf);
      write_bootloader_reg (0xb9, 0xff);
      g_usleep (20000);

      /* 4. Upload 10,403-byte firmware packet */
      printf ("Uploading 10,403-byte firmware payload in single SPI transaction...\n");
      uint8_t *packet = g_malloc (FW_SIZE + 7);
      packet[0] = 0x05;
      packet[1] = 0xfa;
      packet[2] = 0x00;
      packet[3] = 0x00;
      packet[4] = (FW_SIZE >> 8) & 0xff;
      packet[5] = FW_SIZE & 0xff;
      memcpy (packet + 6, fw_buf, FW_SIZE);
      packet[6 + FW_SIZE] = 0x00;
      spi_xfer (packet, NULL, FW_SIZE + 7, 1000000);
      g_free (packet);

      /* 5. Wait 50ms without hardware reset */
      printf ("Firmware uploaded. Waiting 50ms for autonomous MCU SRAM boot...\n");
      g_usleep (50000);

      /* 6. Poll for idle (0xa5 0x5a) */
      gboolean success = FALSE;
      for (unsigned attempt = 1; attempt <= 20; attempt++)
        {
          uint8_t tx_poll[6] = { 0x10, 0xef, 0x20, 0, 0, 0 };
          uint8_t rx_poll[6] = { 0 };
          spi_xfer (tx_poll, rx_poll, sizeof (tx_poll), 1000000);
          printf ("  Poll #%02u: Status = %02x %02x\n", attempt, rx_poll[4], rx_poll[5]);
          if (rx_poll[4] == 0xa5 && rx_poll[5] == 0x5a)
            {
              success = TRUE;
              break;
            }
          g_usleep (20000);
        }

      if (success)
        printf ("\n>>> SUCCESS! FT9361 MCU returned idle (a5 5a) under Vendor Recovery! <<<\n");
      else
        printf ("\n>>> FAILED: FT9361 MCU did not return idle under Vendor Recovery <<<\n");
    }

  /* --- MODE 3: A1-STYLE RECOVERY --- */
  if (a1_recover)
    {
      printf ("\n============================================================\n");
      printf ("Running A1-style Recovery (Baseline Pin39=%d, Pin0=%d)\n", override_p39, override_p0);
      printf ("============================================================\n");

      set_pin39 (override_p39);
      set_pin0 (override_p0);
      g_usleep (30000);

      g_autofree gchar *fw_buf = NULL;
      gsize fw_len = 0;
      require (g_file_get_contents (firmware_file, &fw_buf, &fw_len, NULL), "read firmware");

      pulse_pin39 (override_p39, 10);
      g_usleep (20000);

      uint8_t sync[2] = { 0x55, 0xaa };
      spi_xfer (sync, NULL, sizeof (sync), 1000000);

      uint8_t *packet = g_malloc (FW_SIZE + 7);
      packet[0] = 0x05;
      packet[1] = 0xfa;
      packet[2] = 0x00;
      packet[3] = 0x00;
      packet[4] = (FW_SIZE >> 8) & 0xff;
      packet[5] = FW_SIZE & 0xff;
      memcpy (packet + 6, fw_buf, FW_SIZE);
      packet[6 + FW_SIZE] = 0x00;
      spi_xfer (packet, NULL, FW_SIZE + 7, 1000000);
      g_free (packet);

      printf ("Firmware uploaded. Executing A1 dual reset pulses...\n");
      pulse_pin39 (override_p39, 5);
      g_usleep (10000);
      pulse_pin39 (override_p39, 5);
      g_usleep (160000);

      uint8_t soft = 0x70;
      spi_xfer (&soft, NULL, 1, 1000000);
      g_usleep (5000);
      spi_xfer (&soft, NULL, 1, 1000000);
      g_usleep (5000);

      gboolean success = FALSE;
      for (unsigned attempt = 1; attempt <= 20; attempt++)
        {
          uint8_t tx_poll[6] = { 0x10, 0xef, 0x20, 0, 0, 0 };
          uint8_t rx_poll[6] = { 0 };
          spi_xfer (tx_poll, rx_poll, sizeof (tx_poll), 1000000);
          printf ("  Poll #%02u: Status = %02x %02x\n", attempt, rx_poll[4], rx_poll[5]);
          if (rx_poll[4] == 0xa5 && rx_poll[5] == 0x5a)
            {
              success = TRUE;
              break;
            }
          g_usleep (20000);
        }

      if (success)
        printf ("\n>>> SUCCESS! FT9361 MCU returned idle (a5 5a) under A1 Recovery! <<<\n");
      else
        printf ("\n>>> FAILED: FT9361 MCU did not return idle under A1 Recovery <<<\n");
    }

  printf ("\nDiagnostic test complete. GPIO lines released.\n");
  return 0;
}
