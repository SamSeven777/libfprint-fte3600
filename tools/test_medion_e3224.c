/*
 * Dedicated Hardware Diagnostic & Firmware Recovery Test for Medion Akoya E3224
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
#define MEDION_RESET_PIN 0x27  /* Pin 39 decimal on \\_SB.GPO1 */

static struct gpiod_line_request *reset_request = NULL;
static int spi_fd = -1;
static int cleanup_raw_level = 1;

static void cleanup (void)
{
  if (reset_request)
    {
      gpiod_line_request_set_value (
          reset_request, MEDION_RESET_PIN,
          cleanup_raw_level ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
      gpiod_line_request_release (reset_request);
      reset_request = NULL;
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
          if (g_str_has_suffix (node_path, "GPO1") ||
              g_strcmp0 (node_path, target_acpi_path) == 0)
            {
              result = g_strdup (dev_file);
              break;
            }
        }
    }
  g_list_free_full (gpio_devices, g_object_unref);
  return result;
}

static void spi_xfer (const void *tx, void *rx, size_t len)
{
  struct spi_ioc_transfer t = {
    .tx_buf = (uintptr_t) tx,
    .rx_buf = (uintptr_t) rx,
    .len = len,
    .speed_hz = 1000000,
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

static void read_status_and_id (const char *tag)
{
  uint8_t tx_status[6] = { 0x10, 0xef, 0x20, 0x00, 0x00, 0x00 };
  uint8_t rx_status[6] = { 0 };
  uint8_t tx_id[6] = { 0x10, 0xef, 0x14, 0x00, 0x00, 0x00 };
  uint8_t rx_id[6] = { 0 };

  spi_xfer (tx_status, rx_status, sizeof (tx_status));
  spi_xfer (tx_id, rx_id, sizeof (tx_id));

  printf ("  [%-18s] Status 0x20 RX: %02x %02x %02x %02x [%02x %02x]  |  ID 0x14 RX: %02x %02x %02x %02x [%02x %02x]\n",
          tag,
          rx_status[0], rx_status[1], rx_status[2], rx_status[3], rx_status[4], rx_status[5],
          rx_id[0], rx_id[1], rx_id[2], rx_id[3], rx_id[4], rx_id[5]);
}

static void set_raw_reset (int high)
{
  enum gpiod_line_value val = high ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
  require (gpiod_line_request_set_value (reset_request, MEDION_RESET_PIN, val) == 0,
           "set reset line level");
}

static void pulse_reset (int release_level, int assert_level, unsigned hold_ms)
{
  set_raw_reset (assert_level);
  g_usleep (hold_ms * 1000);
  set_raw_reset (release_level);
  cleanup_raw_level = release_level;
}

static void write_bootloader_reg (uint8_t reg, uint8_t val)
{
  uint8_t tx[4] = { 0x09, 0xf6, reg, val };
  spi_xfer (tx, NULL, sizeof (tx));
}

int main (int argc, char **argv)
{
  gboolean force = FALSE;
  gboolean probe_polarities = FALSE;
  gboolean vendor_recover = FALSE;
  gboolean a1_recover = FALSE;
  const char *firmware_file = NULL;
  const char *spi_dev = "/dev/spidev1.0";
  int polarity_mode = 0; /* 0 = probe both, 1 = active-low (release 1), 2 = active-high (release 0) */

  for (int i = 1; i < argc; i++)
    {
      if (!strcmp (argv[i], "--force"))
        force = TRUE;
      else if (!strcmp (argv[i], "--probe-polarities"))
        probe_polarities = TRUE;
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
      else if (!strcmp (argv[i], "--active-low"))
        polarity_mode = 1;
      else if (!strcmp (argv[i], "--active-high"))
        polarity_mode = 2;
      else if (!strcmp (argv[i], "--spi") && i + 1 < argc)
        spi_dev = argv[++i];
      else
        {
          printf ("Usage: %s [OPTIONS]\n", argv[0]);
          printf ("Options:\n");
          printf ("  --probe-polarities           Test raw line levels (1 vs 0) and observe MISO response\n");
          printf ("  --test-vendor-recovery <fw>  Run Windows sequence: 5 unlock writes + 10KB upload + wait 50ms (no reset)\n");
          printf ("  --test-a1-recovery <fw>      Run A1 sequence: direct upload + dual reset pulses\n");
          printf ("  --active-low                 Assume Active-Low (1 = normal/release, 0 = reset pulse)\n");
          printf ("  --active-high                Assume Active-High (0 = normal/release, 1 = reset pulse)\n");
          printf ("  --spi <device>               SPI device path (default: /dev/spidev1.0)\n");
          printf ("  --force                      Bypass DMI verification\n");
          return 1;
        }
    }

  if (!probe_polarities && !vendor_recover && !a1_recover)
    probe_polarities = TRUE;

  signal (SIGINT, sig_handler);
  signal (SIGTERM, sig_handler);
  atexit (cleanup);

  printf ("=== Medion Akoya E3224 FTE3600 Hardware Diagnostic Tool ===\n");
  check_dmi (force);

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

  /* Resolve GPO1 controller */
  g_autofree gchar *gpiochip = find_gpiochip_for_acpi ("\\_SB_.GPO1");
  if (!gpiochip)
    {
      fprintf (stderr, "ERROR: Could not find GPIO controller for \\_SB_.GPO1\n");
      return 1;
    }
  printf ("Resolved Reset GPIO controller: %s (Pin 0x27 / 39)\n", gpiochip);

  /* Open SPI device */
  spi_fd = open (spi_dev, O_RDWR | O_CLOEXEC);
  require (spi_fd >= 0, "open SPI device");
  uint8_t mode = SPI_MODE_0, bits = 8, lsb = 0;
  require (ioctl (spi_fd, SPI_IOC_WR_MODE, &mode) == 0, "set SPI mode 0");
  require (ioctl (spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) == 0, "set SPI 8-bit");
  require (ioctl (spi_fd, SPI_IOC_WR_LSB_FIRST, &lsb) == 0, "set SPI MSB-first");
  printf ("SPI device %s configured successfully (Mode 0, 8-bit, 1MHz)\n\n", spi_dev);

  /* Claim GPIO reset line */
  struct gpiod_chip *chip = gpiod_chip_open (gpiochip);
  require (chip != NULL, "open gpiochip");
  struct gpiod_line_settings *settings = gpiod_line_settings_new ();
  struct gpiod_line_config *config = gpiod_line_config_new ();
  require (settings && config, "allocate GPIO settings");

  gpiod_line_settings_set_direction (settings, GPIOD_LINE_DIRECTION_OUTPUT);
  gpiod_line_settings_set_output_value (settings, GPIOD_LINE_VALUE_ACTIVE); /* default high */
  unsigned offset = MEDION_RESET_PIN;
  require (gpiod_line_config_add_line_settings (config, &offset, 1, settings) == 0, "configure pin 39");

  reset_request = gpiod_chip_request_lines (chip, NULL, config);
  require (reset_request != NULL, "request line 39 on GPO1");
  gpiod_line_settings_free (settings);
  gpiod_line_config_free (config);
  gpiod_chip_close (chip);

  /* --- MODE 1: PROBE POLARITIES --- */
  if (probe_polarities)
    {
      printf ("------------------------------------------------------------\n");
      printf ("TEST 1: Hypothesis A (Active-Low: Release=HIGH, Reset=LOW)\n");
      printf ("------------------------------------------------------------\n");
      set_raw_reset (1);
      g_usleep (20000);
      read_status_and_id ("Raw High (Release)");

      printf ("Pulsing reset low for 10ms...\n");
      pulse_reset (1, 0, 10);
      g_usleep (50000);
      read_status_and_id ("After Pulse to Low");

      printf ("\n------------------------------------------------------------\n");
      printf ("TEST 2: Hypothesis B (Active-High: Release=LOW, Reset=HIGH)\n");
      printf ("------------------------------------------------------------\n");
      set_raw_reset (0);
      g_usleep (20000);
      read_status_and_id ("Raw Low (Release)");

      printf ("Pulsing reset high for 10ms...\n");
      pulse_reset (0, 1, 10);
      g_usleep (50000);
      read_status_and_id ("After Pulse to High");

      /* Soft-reset test */
      printf ("\n------------------------------------------------------------\n");
      printf ("TEST 3: Soft Reset Commands (0x70) under both states\n");
      printf ("------------------------------------------------------------\n");
      set_raw_reset (1);
      uint8_t cmd_soft = 0x70;
      spi_xfer (&cmd_soft, NULL, 1);
      g_usleep (5000);
      spi_xfer (&cmd_soft, NULL, 1);
      g_usleep (5000);
      read_status_and_id ("Soft-reset (Raw=1)");

      set_raw_reset (0);
      spi_xfer (&cmd_soft, NULL, 1);
      g_usleep (5000);
      spi_xfer (&cmd_soft, NULL, 1);
      g_usleep (5000);
      read_status_and_id ("Soft-reset (Raw=0)");
    }

  /* --- MODE 2: VENDOR RECOVERY SEQUENCE --- */
  if (vendor_recover)
    {
      int release_lvl = (polarity_mode == 2) ? 0 : 1;
      int assert_lvl = (polarity_mode == 2) ? 1 : 0;
      printf ("\n============================================================\n");
      printf ("Running Vendor Recovery (Release=%d, Assert=%d)\n", release_lvl, assert_lvl);
      printf ("============================================================\n");

      g_autofree gchar *fw_buf = NULL;
      gsize fw_len = 0;
      require (g_file_get_contents (firmware_file, &fw_buf, &fw_len, NULL), "read firmware");
      require (fw_len == FW_SIZE, "verify firmware size (10396 bytes)");

      /* 1. Pulse reset to enter bootloader */
      pulse_reset (release_lvl, assert_lvl, 10);
      g_usleep (20000);
      read_status_and_id ("Pre-Sync Status");

      /* 2. Sync */
      uint8_t sync[2] = { 0x55, 0xaa };
      spi_xfer (sync, NULL, sizeof (sync));
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
      spi_xfer (packet, NULL, FW_SIZE + 7);
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
          spi_xfer (tx_poll, rx_poll, sizeof (tx_poll));
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
      int release_lvl = (polarity_mode == 2) ? 0 : 1;
      int assert_lvl = (polarity_mode == 2) ? 1 : 0;
      printf ("\n============================================================\n");
      printf ("Running A1-style Recovery (Release=%d, Assert=%d)\n", release_lvl, assert_lvl);
      printf ("============================================================\n");

      g_autofree gchar *fw_buf = NULL;
      gsize fw_len = 0;
      require (g_file_get_contents (firmware_file, &fw_buf, &fw_len, NULL), "read firmware");

      pulse_reset (release_lvl, assert_lvl, 10);
      g_usleep (20000);

      uint8_t sync[2] = { 0x55, 0xaa };
      spi_xfer (sync, NULL, sizeof (sync));

      uint8_t *packet = g_malloc (FW_SIZE + 7);
      packet[0] = 0x05;
      packet[1] = 0xfa;
      packet[2] = 0x00;
      packet[3] = 0x00;
      packet[4] = (FW_SIZE >> 8) & 0xff;
      packet[5] = FW_SIZE & 0xff;
      memcpy (packet + 6, fw_buf, FW_SIZE);
      packet[6 + FW_SIZE] = 0x00;
      spi_xfer (packet, NULL, FW_SIZE + 7);
      g_free (packet);

      printf ("Firmware uploaded. Executing A1 dual reset pulses...\n");
      pulse_reset (release_lvl, assert_lvl, 5);
      g_usleep (10000);
      pulse_reset (release_lvl, assert_lvl, 5);
      g_usleep (160000);

      uint8_t soft = 0x70;
      spi_xfer (&soft, NULL, 1);
      g_usleep (5000);
      spi_xfer (&soft, NULL, 1);
      g_usleep (5000);

      gboolean success = FALSE;
      for (unsigned attempt = 1; attempt <= 20; attempt++)
        {
          uint8_t tx_poll[6] = { 0x10, 0xef, 0x20, 0, 0, 0 };
          uint8_t rx_poll[6] = { 0 };
          spi_xfer (tx_poll, rx_poll, sizeof (tx_poll));
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

  printf ("\nDiagnostic test complete. GPIO line released safely.\n");
  return 0;
}
