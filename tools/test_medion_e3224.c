/*
 * Dedicated Hardware Diagnostic & Firmware Recovery Test for Medion Akoya E3224
 * Clean-room implementation based strictly on official Windows driver decompilation
 * (ftWbioUmdfDriverV2.dll v2.0.3.102)
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
#include <unistd.h>

#define FW_SIZE 10396
#define FW_SHA "027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f"

#define PIN_GPO1_RESET 0x27  /* Pin 39 on \\_SB.GPO1 - sole reset output pin */

static struct gpiod_line_request *req_gpo1 = NULL;
static int spi_fd = -1;
static int cur_pin39_val = 1;

static void cleanup (void)
{
  if (req_gpo1)
    {
      gpiod_line_request_release (req_gpo1);
      req_gpo1 = NULL;
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

static void set_pin39 (int high)
{
  cur_pin39_val = high;
  if (req_gpo1)
    {
      enum gpiod_line_value val = high ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
      gpiod_line_request_set_value (req_gpo1, PIN_GPO1_RESET, val);
    }
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
  g_usleep (10000);
}

/* Write MCU Register: 0x11 0xee <reg> <val> 0x00 */
static void write_mcu_reg (uint8_t reg, uint8_t val)
{
  uint8_t tx[5] = { 0x11, 0xee, reg, val, 0x00 };
  spi_xfer (tx, NULL, sizeof (tx), 1000000);
  g_usleep (1000);  /* Sleep(1) in Windows InitMcuConfig */
}

/* Read MCU Register: TX 0x10 0xef <reg> 0x00 0x00 -> RX byte index 4 */
static uint8_t read_mcu_reg (uint8_t reg)
{
  uint8_t tx[5] = { 0x10, 0xef, reg, 0x00, 0x00 };
  uint8_t rx[5] = { 0 };
  spi_xfer (tx, rx, sizeof (tx), 1000000);
  return rx[4];
}

/* Write Bootloader Register (0x09 0xf6 <reg> <val>) */
static void write_bootloader_reg (uint8_t reg, uint8_t val)
{
  uint8_t tx[4] = { 0x09, 0xf6, reg, val };
  spi_xfer (tx, NULL, sizeof (tx), 1000000);
}

/* Query Bootloader ROM Edition (0x90 0x00 0x00 -> byte index 2 is 0xef for Edition A) */
static uint8_t query_bootloader_edition (void)
{
  uint8_t tx[3] = { 0x90, 0x00, 0x00 };
  uint8_t rx[3] = { 0 };
  spi_xfer (tx, rx, sizeof (tx), 1000000);
  return rx[2];
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

int main (int argc, char **argv)
{
  gboolean force = FALSE;
  gboolean probe_only = FALSE;
  gboolean vendor_recover = FALSE;
  gboolean do_reset = FALSE;
  const char *firmware_file = NULL;
  const char *spi_dev = "/dev/spidev1.0";

  for (int i = 1; i < argc; i++)
    {
      if (!strcmp (argv[i], "--force"))
        force = TRUE;
      else if (!strcmp (argv[i], "--probe"))
        probe_only = TRUE;
      else if (!strcmp (argv[i], "--reset"))
        do_reset = TRUE;
      else if (!strcmp (argv[i], "--test-vendor-recovery") && i + 1 < argc)
        {
          vendor_recover = TRUE;
          firmware_file = argv[++i];
        }
      else if (!strcmp (argv[i], "--spi") && i + 1 < argc)
        spi_dev = argv[++i];
      else
        {
          printf ("Usage: %s [OPTIONS]\n", argv[0]);
          printf ("Options:\n");
          printf ("  --probe                      Probe chip status and bootloader edition\n");
          printf ("  --test-vendor-recovery <fw>  Run official decompiled Windows sequence:\n");
          printf ("                                 1. Soft reset & idle check\n");
          printf ("                                 2. Bootloader 0x90 check & HW reset pulse\n");
          printf ("                                 3. 0x55 0xaa Sync + 5 unlock register writes\n");
          printf ("                                 4. 10,403-byte firmware upload\n");
          printf ("                                 5. InitMcuConfig (0x01=1, 0x41=0xf, 0x30=0xbb)\n");
          printf ("                                 6. MCU status poll for idle (a5 5a)\n");
          printf ("                                 7. Sensor ID check (0x40 0x50)\n");
          printf ("  --reset                      Perform Windows reset pulse on Pin 39\n");
          printf ("  --spi <device>               SPI device path (default: /dev/spidev1.0)\n");
          printf ("  --force                      Bypass DMI verification\n");
          return 1;
        }
    }

  if (!probe_only && !vendor_recover && !do_reset)
    probe_only = TRUE;

  signal (SIGINT, sig_handler);
  signal (SIGTERM, sig_handler);
  atexit (cleanup);

  printf ("=== Medion Akoya E3224 Hardware Diagnostic & Recovery Tool ===\n");
  printf ("    (Pure Clean-Room Logic from ftWbioUmdfDriverV2.dll v2.0.3.102)\n");
  check_dmi (force);
  awaken_spi_subsystem ();

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
  g_autofree gchar *chip_gpo1 = find_gpiochip_for_acpi ("\\_SB_.GPO1");
  printf ("Resolved GPO1 (Pin 39): %s\n", chip_gpo1 ? chip_gpo1 : "NOT FOUND (fallback to /dev/gpiochip0)");
  if (!chip_gpo1 && g_file_test ("/dev/gpiochip0", G_FILE_TEST_EXISTS))
    chip_gpo1 = g_strdup ("/dev/gpiochip0");

  /* Open SPI device */
  spi_fd = open (spi_dev, O_RDWR | O_CLOEXEC);
  require (spi_fd >= 0, "open SPI device");
  set_spi_mode (SPI_MODE_0);
  uint8_t bits = 8, lsb = 0;
  require (ioctl (spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) == 0, "set SPI 8-bit");
  require (ioctl (spi_fd, SPI_IOC_WR_LSB_FIRST, &lsb) == 0, "set SPI MSB-first");
  printf ("SPI device %s opened successfully\n\n", spi_dev);

  /* Request GPO1 Pin 39 (Active-Low reset line, idle = 1) */
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
            printf ("Successfully claimed Reset Line (GPO1 Pin 39) on %s\n", chip_gpo1);
          else
            fprintf (stderr, "WARNING: Could not claim Pin 39 on %s: %s\n", chip_gpo1, strerror (errno));
        }
    }

  if (do_reset)
    {
      printf ("Executing official Windows reset pulse (1 -> 10ms -> 0 -> 20ms -> 1)...\n");
      windows_reset_pulse ();
      printf ("Reset complete.\n");
      return 0;
    }

  if (probe_only)
    {
      printf ("============================================================\n");
      printf ("Probing Sensor & Bootloader Status\n");
      printf ("============================================================\n");

      /* 1. Soft reset */
      printf ("Sending dual 0x70 soft reset commands...\n");
      uint8_t soft = 0x70;
      spi_xfer (&soft, NULL, 1, 1000000);
      g_usleep (5000);
      spi_xfer (&soft, NULL, 1, 1000000);
      g_usleep (5000);

      gboolean is_idle = probe_status_and_id ("Post Soft-Reset", 1000000);
      if (is_idle)
        {
          printf ("\n>>> Sensor MCU is RUNNING and IDLE! <<<\n");
          uint8_t id_hi = read_mcu_reg (0x14);
          uint8_t id_lo = read_mcu_reg (0x15);
          printf ("    Sensor ID: 0x%02x 0x%02x\n", id_hi, id_lo);
        }
      else
        {
          printf ("MCU is not in runtime idle state. Querying bootloader edition...\n");
          uint8_t ed = query_bootloader_edition ();
          printf ("Reg 0x90 response: 0x%02x %s\n", ed,
                  (ed == 0xef) ? "(Bootloader Edition A DETECTED)" : "(Not in Bootloader Edition A)");
        }
    }

  if (vendor_recover)
    {
      printf ("============================================================\n");
      printf ("Running Official Windows Firmware Recovery Sequence\n");
      printf ("============================================================\n");

      g_autofree gchar *fw_buf = NULL;
      gsize fw_len = 0;
      require (g_file_get_contents (firmware_file, &fw_buf, &fw_len, NULL), "read firmware");
      require (fw_len == FW_SIZE, "verify firmware size (10396 bytes)");
      printf ("Firmware loaded: %s (%zu bytes)\n", firmware_file, fw_len);

      /* Step 1: Soft reset check */
      printf ("\n[1/6] Soft reset & status probe...\n");
      uint8_t soft = 0x70;
      spi_xfer (&soft, NULL, 1, 1000000);
      g_usleep (5000);
      spi_xfer (&soft, NULL, 1, 1000000);
      g_usleep (5000);

      if (probe_status_and_id ("Initial Probe", 1000000))
        {
          printf ("  MCU is ALREADY running and idle (0xa5 0x5a)! Firmware already loaded.\n");
        }

      /* Step 2: Bootloader check & Hardware reset pulse */
      printf ("\n[2/6] Checking bootloader & pulsing hardware reset...\n");
      uint8_t boot_ed = query_bootloader_edition ();
      printf ("  Pre-reset Reg 0x90: 0x%02x\n", boot_ed);

      printf ("  Executing official reset pulse (High 10ms -> Low 20ms -> High)...\n");
      windows_reset_pulse ();

      boot_ed = query_bootloader_edition ();
      printf ("  Post-reset Reg 0x90: 0x%02x %s\n", boot_ed,
              (boot_ed == 0xef) ? "(0xef Bootloader Edition A CONFIRMED)" : "");

      /* Step 3: Bootloader Sync */
      printf ("\n[3/6] Sending Bootloader Sync (0x55 0xaa)...\n");
      uint8_t sync[2] = { 0x55, 0xaa };
      spi_xfer (sync, NULL, sizeof (sync), 1000000);

      /* Step 4: Five unlock register writes */
      printf ("\n[4/6] Sending 5 unlock register writes...\n");
      write_bootloader_reg (0xc8, 0xff);
      write_bootloader_reg (0xca, 0xff);
      write_bootloader_reg (0xcb, 0xff);
      write_bootloader_reg (0xb9, 0xbf);
      write_bootloader_reg (0xb9, 0xff);
      printf ("  Waiting 20ms unlock settle delay...\n");
      g_usleep (20000);

      /* Step 5: Upload 10,403-byte firmware packet */
      printf ("\n[5/6] Uploading 10,403-byte firmware payload in single SPI transaction...\n");
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
      g_usleep (2000);

      /* Step 6: Trigger MCU SRAM boot via official InitMcuConfig (0x180036a30) */
      printf ("\n[6/6] Executing official InitMcuConfig (triggering MCU SRAM execution)...\n");
      write_mcu_reg (0x01, 0x01);
      write_mcu_reg (0x41, 0x0f);
      write_mcu_reg (0x30, 0xbb);

      uint8_t marker = read_mcu_reg (0x30);
      printf ("  Config marker Reg 0x30 readback: 0x%02x (expected 0xbb)\n", marker);
      if (marker != 0xbb)
        fprintf (stderr, "  WARNING: Config marker mismatch (got 0x%02x, expected 0xbb)!\n", marker);

      /* Step 7: Poll for MCU idle (0xa5 0x5a) */
      printf ("  Polling MCU status (Reg 0x20) for idle (0xa5 0x5a)...\n");
      gboolean success = FALSE;
      for (unsigned attempt = 1; attempt <= 20; attempt++)
        {
          uint8_t tx_poll[6] = { 0x10, 0xef, 0x20, 0, 0, 0 };
          uint8_t rx_poll[6] = { 0 };
          spi_xfer (tx_poll, rx_poll, sizeof (tx_poll), 1000000);
          printf ("    Poll #%02u: Status = %02x %02x\n", attempt, rx_poll[4], rx_poll[5]);
          if (rx_poll[4] == 0xa5 && rx_poll[5] == 0x5a)
            {
              success = TRUE;
              break;
            }
          g_usleep (2000);
        }

      if (success)
        {
          printf ("\n>>> SUCCESS! FT9361 MCU returned idle (a5 5a) under official Windows sequence! <<<\n");
          uint8_t id_hi = read_mcu_reg (0x14);
          uint8_t id_lo = read_mcu_reg (0x15);
          printf ("    Sensor ID: 0x%02x 0x%02x\n", id_hi, id_lo);
          if (id_hi == 0x40 && id_lo == 0x50)
            printf ("    >>> FT9361 Sensor Hardware Identified (0x40 0x50)! <<<\n");
        }
      else
        {
          printf ("\n>>> FAILED: FT9361 MCU did not return idle <<<\n");
        }
    }

  printf ("\nDiagnostic complete. GPIO line released safely.\n");
  return 0;
}
