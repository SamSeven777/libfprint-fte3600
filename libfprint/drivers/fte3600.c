/*
 * FocalTech FTE3600/FT9361 SPI fingerprint driver
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define FP_COMPONENT "fte3600"

#include "fte3600.h"
#include "drivers_api.h"
#include "fte3600-template.h"

#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <glib-unix.h>
#include <gudev/gudev.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define FT9361_REG_READ_HEADER_SIZE 4
#define FT9361_REG_WRITE_SIZE 5
#define FT9361_SMALL_FRAME_SIZE 6
#define FT9361_RESET_DELAY_MS 5
#define FT9361_RESET_SETTLE_MS 2
#define FT9361_HARD_RESET_PULSE_MS 5
#define FT9361_HARD_RESET_INTERVAL_MS 10
#define FT9361_HARD_RESET_BOOT_MS 200
#define FT9361_CONFIG_DELAY_MS 2
#define FT9361_ARM_DELAY_MS 10
#define FT9361_ARM_TIMEOUT_MS 1000
#define FT9361_ARM_MAX_ATTEMPTS 3
#define FT9361_POLL_DELAY_MS 20
#define FT9361_CAPTURE_READY_TIMEOUT_MS 5000
#define FT9361_MAX_FALSE_IRQS 8
#define FT9361_MAX_IRQ_DRAIN_BATCHES 8
#define FT9361_IMAGE_PPMM 20.0

/*
 * spidev does not expose the GPIO descriptors from its ACPI companion to
 * userspace.  Keep verified DMI-specific ACPI resource mappings in a table,
 * while resolving the gpiochip device dynamically so gpiochip numbering is
 * never assumed.  The A1 values below are the output-only, active-low reset
 * GpioIo pin 0x55 and the GpioInt pin 0x56 (Edge, ActiveHigh), both on
 * \_SB.PCI0.GPI0.
 */
typedef struct
{
  const gchar *sys_vendor;
  const gchar *product_name;
  const gchar *product_version;
  const gchar *board_name;
  const gchar *reset_controller_acpi_path;
  guint        reset_offset;
  gboolean     reset_active_low;
  gboolean     allow_hardware_reset;
  const gchar *irq_controller_acpi_path;
  guint        irq_offset;
} Fte3600GpioProfile;

static const Fte3600GpioProfile fte3600_gpio_profiles[] = {
  {
    .sys_vendor = "ONE-NETBOOK TECHNOLOGY CO., LTD.",
    .product_name = "A1",
    .product_version = NULL,
    .board_name = NULL,
    .reset_controller_acpi_path = "\\_SB_.PCI0.GPI0",
    .reset_offset = 0x55,
    .reset_active_low = TRUE,
    .allow_hardware_reset = TRUE,
    .irq_controller_acpi_path = "\\_SB_.PCI0.GPI0",
    .irq_offset = 0x56,
  },
  {
    .sys_vendor = "MEDION",
    .product_name = "E3224",
    .product_version = "FT",
    .board_name = "YS13G",
    .reset_controller_acpi_path = "\\_SB_.GPO1",
    .reset_offset = 0x27,
    .reset_active_low = TRUE,
    .allow_hardware_reset = FALSE, /* Safety gate: disabled until polarity is confirmed */
    .irq_controller_acpi_path = "\\_SB_.GPO2",
    .irq_offset = 0x00,
  },
};

static inline enum gpiod_line_value
fte3600_reset_line_value (const Fte3600GpioProfile *profile,
                          gboolean                  asserted)
{
  g_assert (profile != NULL);

  if (profile->reset_active_low)
    return asserted ? GPIOD_LINE_VALUE_INACTIVE : GPIOD_LINE_VALUE_ACTIVE;
  else
    return asserted ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
}

G_STATIC_ASSERT (FT9361_IMAGE_SIZE == FTE3600_BRISK_IMAGE_SIZE);

struct _FpiDeviceFte3600
{
  FpDevice parent;

  gint spi_fd;
  gboolean capturing;
  gboolean armed;
  gboolean idle_verified;
  gboolean init_hardware_reset_attempted;
  guint enroll_stages_passed;

  struct gpiod_line_request *reset_request;
  struct gpiod_line_request *irq_request;
  struct gpiod_edge_event_buffer *irq_event_buffer;
  GSource *irq_source;
  FpiSsm *irq_wait_ssm;
  gint64 arm_deadline;
  guint arm_attempts;
  gint64 capture_ready_deadline;
  guint false_irq_count;
  const Fte3600GpioProfile *gpio_profile;

  Fte3600Template *enroll_template;
  Fte3600Template *verify_template;
  FpImage *captured_image;

  guint8 small_rx[FT9361_SMALL_FRAME_SIZE];
  gboolean small_rx_valid;
  guint8 *capture_tx;
  guint8 *capture_rx;
};

G_DECLARE_FINAL_TYPE (FpiDeviceFte3600, fpi_device_fte3600, FPI,
                      DEVICE_FTE3600, FpDevice);
G_DEFINE_TYPE (FpiDeviceFte3600, fpi_device_fte3600, FP_TYPE_DEVICE);

enum fte3600_init_state
{
  FTE3600_INIT_RESET_1,
  FTE3600_INIT_RESET_DELAY,
  FTE3600_INIT_RESET_2,
  FTE3600_INIT_RESET_SETTLE,
  FTE3600_INIT_READ_MCU_STATUS,
  FTE3600_INIT_CHECK_MCU_STATUS,
  FTE3600_INIT_HARD_RESET_ASSERT_1,
  FTE3600_INIT_HARD_RESET_HOLD_1,
  FTE3600_INIT_HARD_RESET_DEASSERT_1,
  FTE3600_INIT_HARD_RESET_INTERVAL,
  FTE3600_INIT_HARD_RESET_ASSERT_2,
  FTE3600_INIT_HARD_RESET_HOLD_2,
  FTE3600_INIT_HARD_RESET_DEASSERT_2,
  FTE3600_INIT_HARD_RESET_BOOT,
  FTE3600_INIT_READ_ID_HIGH,
  FTE3600_INIT_CHECK_ID_HIGH,
  FTE3600_INIT_READ_ID_LOW,
  FTE3600_INIT_CHECK_ID_LOW,
  FTE3600_INIT_READ_FW_VERSION,
  FTE3600_INIT_CHECK_FW_VERSION,
  FTE3600_INIT_READ_AGC_VERSION,
  FTE3600_INIT_CHECK_AGC_VERSION,
  FTE3600_INIT_READ_CONFIG_MARKER,
  FTE3600_INIT_CHECK_CONFIG_MARKER,
  FTE3600_INIT_WRITE_CONFIG_01,
  FTE3600_INIT_CONFIG_01_DELAY,
  FTE3600_INIT_WRITE_CONFIG_41,
  FTE3600_INIT_CONFIG_41_DELAY,
  FTE3600_INIT_WRITE_CONFIG_MARKER,
  FTE3600_INIT_CONFIG_MARKER_DELAY,
  FTE3600_INIT_VERIFY_CONFIG_MARKER,
  FTE3600_INIT_CHECK_CONFIG_VERIFY,
  FTE3600_INIT_WRITE_CONFIG_22,
  FTE3600_INIT_CONFIG_22_DELAY,
  FTE3600_INIT_WRITE_CONFIG_23,
  FTE3600_INIT_CONFIG_23_DELAY,
  FTE3600_INIT_FINAL_READ_MCU_STATUS,
  FTE3600_INIT_FINAL_CHECK_MCU_STATUS,
  FTE3600_INIT_DONE,
  FTE3600_INIT_NSTATES,
};

enum fte3600_arm_state
{
  FTE3600_ARM_READ_MCU_STATUS,
  FTE3600_ARM_CHECK_MCU_STATUS,
  FTE3600_ARM_RECOVERY_RESET_1,
  FTE3600_ARM_RECOVERY_RESET_DELAY,
  FTE3600_ARM_RECOVERY_RESET_2,
  FTE3600_ARM_READ_MODE,
  FTE3600_ARM_CHECK_MODE,
  FTE3600_ARM_STOP_START,
  FTE3600_ARM_STOP_ENABLE,
  FTE3600_ARM_STOP_DELAY,
  FTE3600_ARM_WRITE_MODE,
  FTE3600_ARM_WRITE_ENABLE,
  FTE3600_ARM_WRITE_START,
  FTE3600_ARM_DELAY,
  FTE3600_ARM_DRAIN_FINGER_STATUS,
  FTE3600_ARM_READ_ARMED_MCU_STATUS,
  FTE3600_ARM_CHECK_ARMED_MCU_STATUS,
  FTE3600_ARM_DONE,
  FTE3600_ARM_NSTATES,
};

enum fte3600_capture_state
{
  FTE3600_CAPTURE_PREPARE_ARM,
  FTE3600_CAPTURE_WAIT_FINGER_IRQ,
  FTE3600_CAPTURE_POLL_MCU_STATUS,
  FTE3600_CAPTURE_CHECK_MCU_STATUS,
  FTE3600_CAPTURE_READ_FINGER_STATUS,
  FTE3600_CAPTURE_CHECK_FINGER_STATUS,
  FTE3600_CAPTURE_QUICK_READ_MCU_STATUS,
  FTE3600_CAPTURE_QUICK_CHECK_MCU_STATUS,
  FTE3600_CAPTURE_QUICK_WRITE_MODE,
  FTE3600_CAPTURE_QUICK_WRITE_TRIGGER,
  FTE3600_CAPTURE_QUICK_READ_ARMED_MCU_STATUS,
  FTE3600_CAPTURE_QUICK_CHECK_ARMED_MCU_STATUS,
  FTE3600_CAPTURE_READ_IMAGE,
  FTE3600_CAPTURE_PROCESS_IMAGE,
  FTE3600_CAPTURE_CLEANUP_DISPATCH,
  FTE3600_CAPTURE_CLEANUP_REARM,
  FTE3600_CAPTURE_CLEANUP_REARM_DONE,
  FTE3600_CAPTURE_CLEANUP_RESET_1,
  FTE3600_CAPTURE_CLEANUP_RESET_DELAY,
  FTE3600_CAPTURE_CLEANUP_RESET_2,
  FTE3600_CAPTURE_CLEANUP_RESET_SETTLE,
  FTE3600_CAPTURE_CLEANUP_RESET_READ_MCU_STATUS,
  FTE3600_CAPTURE_CLEANUP_RESET_CHECK_MCU_STATUS,
  FTE3600_CAPTURE_DONE,
  FTE3600_CAPTURE_NSTATES,
};

enum fte3600_reset_state
{
  FTE3600_RESET_1,
  FTE3600_RESET_DELAY,
  FTE3600_RESET_2,
  FTE3600_RESET_SETTLE,
  FTE3600_RESET_READ_MCU_STATUS,
  FTE3600_RESET_CHECK_MCU_STATUS,
  FTE3600_RESET_NSTATES,
};

typedef enum
{
  FTE3600_RESET_FOR_OPEN_ERROR,
  FTE3600_RESET_FOR_CLOSE,
  FTE3600_RESET_FOR_ACTION_ERROR,
} Fte3600ResetPurpose;

typedef struct
{
  Fte3600ResetPurpose purpose;
  GError *operation_error;
} Fte3600ResetData;

typedef struct
{
  guint8 image[FT9361_IMAGE_SIZE];
  Fte3600Template *enroll_template;
  Fte3600TemplateStatus status;
  Fte3600TemplateStatus encode_status;
  Fte3600BriskStatus extract_status;
  GBytes *encoded_template;
} Fte3600EnrollJob;

#if FTE3600_ENABLE_PERSONAL_AUTH
typedef struct
{
  guint8 image[FT9361_IMAGE_SIZE];
  Fte3600Template *verify_template;
  Fte3600BriskStatus extract_status;
  Fte3600TemplateStatus compare_status;
  Fte3600TemplateCompareResult comparison;
} Fte3600VerifyJob;
#endif

static void fte3600_start_reset (FpiDeviceFte3600 *self,
                                 Fte3600ResetPurpose purpose,
                                 GError *operation_error);
static void fte3600_complete_action_error (FpiDeviceFte3600 *self,
                                           GError *error);

static void
fte3600_secure_clear (gpointer data,
                      gsize    size)
{
  volatile guint8 *bytes = data;

  if (data == NULL)
    return;

  while (size-- > 0)
    *bytes++ = 0;
}

static void
fte3600_clear_captured_image (FpiDeviceFte3600 *self)
{
  gsize image_size;

  if (self->captured_image == NULL)
    return;

  image_size = (gsize) self->captured_image->width *
               self->captured_image->height;
  if (self->captured_image->data != NULL)
    fte3600_secure_clear (self->captured_image->data, image_size);
  g_clear_object (&self->captured_image);
}

static void
fte3600_clear_irq_source (FpiDeviceFte3600 *self)
{
  self->irq_wait_ssm = NULL;

  if (!self->irq_source)
    return;

  g_source_destroy (self->irq_source);
  g_clear_pointer (&self->irq_source, g_source_unref);
}

static void
fte3600_deassert_hardware_reset_best_effort (FpiDeviceFte3600 *self,
                                             const gchar      *context)
{
  if (self->reset_request && self->gpio_profile &&
      self->gpio_profile->allow_hardware_reset &&
      gpiod_line_request_set_value (
          self->reset_request, self->gpio_profile->reset_offset,
          fte3600_reset_line_value (self->gpio_profile, FALSE)) < 0)
    fp_warn ("Failed to leave the FTE3600 hardware reset line deasserted while %s: "
             "%s", context, g_strerror (errno));
}

static void
fte3600_release_gpio (FpiDeviceFte3600 *self)
{
  self->idle_verified = FALSE;
  fte3600_clear_irq_source (self);
  g_clear_pointer (&self->irq_event_buffer,
                   gpiod_edge_event_buffer_free);
  fte3600_deassert_hardware_reset_best_effort (self, "releasing GPIOs");
  g_clear_pointer (&self->reset_request, gpiod_line_request_release);
  g_clear_pointer (&self->irq_request, gpiod_line_request_release);
}

static gboolean
fte3600_read_dmi_value (const gchar  *name,
                        gchar       **value,
                        GError      **error)
{
  g_autofree gchar *path = NULL;
  g_autofree gchar *contents = NULL;
  g_autoptr (GError) read_error = NULL;

  path = g_build_filename ("/sys/class/dmi/id", name, NULL);
  if (!g_file_get_contents (path, &contents, NULL, &read_error))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Could not read DMI %s for FTE3600 platform matching: %s",
                   name, read_error->message);
      return FALSE;
    }

  g_strstrip (contents);
  if (contents[0] == '\0')
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "DMI %s was empty for FTE3600 platform matching", name);
      return FALSE;
    }

  *value = g_steal_pointer (&contents);
  return TRUE;
}

static gboolean
fte3600_select_gpio_profile (FpiDeviceFte3600 *self,
                             GError          **error)
{
  g_autofree gchar *sys_vendor = NULL;
  g_autofree gchar *product_name = NULL;
  g_autofree gchar *product_version = NULL;
  g_autofree gchar *board_name = NULL;

  self->gpio_profile = NULL;
  if (!fte3600_read_dmi_value ("sys_vendor", &sys_vendor, error) ||
      !fte3600_read_dmi_value ("product_name", &product_name, error))
    return FALSE;

  fte3600_read_dmi_value ("product_version", &product_version, NULL);
  fte3600_read_dmi_value ("board_name", &board_name, NULL);

  for (guint i = 0; i < G_N_ELEMENTS (fte3600_gpio_profiles); i++)
    {
      const Fte3600GpioProfile *profile = &fte3600_gpio_profiles[i];

      if (!g_str_equal (sys_vendor, profile->sys_vendor) ||
          !g_str_equal (product_name, profile->product_name))
        continue;

      if (profile->product_version != NULL &&
          (!product_version || !g_str_equal (product_version, profile->product_version)))
        continue;

      if (profile->board_name != NULL &&
          (!board_name || !g_str_equal (board_name, profile->board_name)))
        continue;

      self->gpio_profile = profile;
      return TRUE;
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "FTE3600 GPIO routing is not verified for DMI system '%s' '%s' "
               "(version: '%s', board: '%s')",
               sys_vendor, product_name,
               product_version ? product_version : "unknown",
               board_name ? board_name : "unknown");
  return FALSE;
}

static gboolean
fte3600_acpi_path_equal (const gchar *path_a,
                         const gchar *path_b)
{
  g_auto (GStrv) parts_a = NULL;
  g_auto (GStrv) parts_b = NULL;
  guint len_a, len_b;

  if (g_strcmp0 (path_a, path_b) == 0)
    return TRUE;
  if (!path_a || !path_b)
    return FALSE;

  while (*path_a == '\\')
    path_a++;
  while (*path_b == '\\')
    path_b++;

  parts_a = g_strsplit (path_a, ".", -1);
  parts_b = g_strsplit (path_b, ".", -1);
  len_a = g_strv_length (parts_a);
  len_b = g_strv_length (parts_b);
  if (len_a != len_b)
    return FALSE;

  for (guint i = 0; i < len_a; i++)
    {
      gchar *s_a = parts_a[i];
      gchar *s_b = parts_b[i];
      gsize slen_a = strlen (s_a);
      gsize slen_b = strlen (s_b);

      while (slen_a > 0 && s_a[slen_a - 1] == '_')
        {
          s_a[slen_a - 1] = '\0';
          slen_a--;
        }
      while (slen_b > 0 && s_b[slen_b - 1] == '_')
        {
          s_b[slen_b - 1] = '\0';
          slen_b--;
        }

      if (g_strcmp0 (s_a, s_b) != 0)
        return FALSE;
    }

  return TRUE;
}

static gchar *
fte3600_find_gpiochip (const gchar  *target_acpi_path,
                       GError      **error)
{
  const gchar *subsystems[] = { "gpio", NULL };
  g_autoptr (GUdevClient) client = NULL;
  g_autofree gchar *result = NULL;
  GList *gpio_devices;

  client = g_udev_client_new (subsystems);
  gpio_devices = g_udev_client_query_by_subsystem (client, "gpio");

  for (GList *iter = gpio_devices; iter; iter = iter->next)
    {
      GUdevDevice *gpio_device = iter->data;
      const gchar *device_file;
      const gchar *sysfs_path;
      g_autofree gchar *controller_path_file = NULL;
      g_autofree gchar *controller_path = NULL;
      g_autoptr (GError) read_error = NULL;

      device_file = g_udev_device_get_device_file (gpio_device);
      sysfs_path = g_udev_device_get_sysfs_path (gpio_device);
      if (!device_file || !sysfs_path)
        continue;

      controller_path_file =
          g_build_filename (sysfs_path, "firmware_node", "path", NULL);
      if (!g_file_test (controller_path_file, G_FILE_TEST_EXISTS))
        {
          g_clear_pointer (&controller_path_file, g_free);
          controller_path_file =
              g_build_filename (sysfs_path, "device", "firmware_node", "path", NULL);
        }
      if (!g_file_get_contents (controller_path_file, &controller_path, NULL,
                                &read_error))
        continue;

      g_strchomp (controller_path);
      if (fte3600_acpi_path_equal (controller_path, target_acpi_path))
        {
          result = g_strdup (device_file);
          break;
        }
    }

  g_list_free_full (gpio_devices, g_object_unref);

  if (!result)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                 "Could not find the GPIO controller %s required by the "
                 "FTE3600 ACPI resource profile",
                 target_acpi_path);

  return g_steal_pointer (&result);
}

static gboolean
fte3600_request_gpio (FpiDeviceFte3600 *self, GError **error)
{
  g_autofree gchar *reset_gpiochip_path = NULL;
  g_autofree gchar *irq_gpiochip_path = NULL;
  struct gpiod_request_config *reset_req_config = NULL;
  struct gpiod_request_config *irq_req_config = NULL;
  struct gpiod_line_settings *reset_settings = NULL;
  struct gpiod_line_settings *irq_settings = NULL;
  struct gpiod_line_config *reset_line_config = NULL;
  struct gpiod_line_config *irq_line_config = NULL;
  struct gpiod_edge_event_buffer *event_buffer = NULL;
  struct gpiod_chip *reset_chip = NULL;
  struct gpiod_chip *irq_chip = NULL;
  unsigned int irq_offset;
  unsigned int reset_offset;
  gint saved_errno = 0;

  g_assert (self->gpio_profile != NULL);
  reset_offset = self->gpio_profile->reset_offset;
  irq_offset = self->gpio_profile->irq_offset;

  if (fte3600_acpi_path_equal (self->gpio_profile->reset_controller_acpi_path,
                               self->gpio_profile->irq_controller_acpi_path))
    g_assert_cmpuint (irq_offset, !=, reset_offset);

  /* Resolve required GPIO controllers first before requesting any lines */
  irq_gpiochip_path = fte3600_find_gpiochip (
      self->gpio_profile->irq_controller_acpi_path, error);
  if (!irq_gpiochip_path)
    return FALSE;

  if (self->gpio_profile->allow_hardware_reset
      && self->gpio_profile->reset_controller_acpi_path != NULL)
    {
      reset_gpiochip_path = fte3600_find_gpiochip (
          self->gpio_profile->reset_controller_acpi_path, error);
      if (!reset_gpiochip_path)
        return FALSE;

      /* Reset line configuration */
      reset_chip = gpiod_chip_open (reset_gpiochip_path);
      reset_settings = gpiod_line_settings_new ();
      reset_line_config = gpiod_line_config_new ();
      reset_req_config = gpiod_request_config_new ();
      if (!reset_chip || !reset_settings || !reset_line_config || !reset_req_config)
        goto fail;

      gpiod_request_config_set_consumer (reset_req_config, "libfprint-fte3600-reset");
      if (gpiod_line_settings_set_direction (
              reset_settings, GPIOD_LINE_DIRECTION_OUTPUT) < 0
          || gpiod_line_settings_set_output_value (
                 reset_settings,
                 fte3600_reset_line_value (self->gpio_profile, FALSE)) < 0
          || gpiod_line_config_add_line_settings (
                 reset_line_config, &reset_offset, 1, reset_settings) < 0)
        goto fail;

      self->reset_request = gpiod_chip_request_lines (
          reset_chip, reset_req_config, reset_line_config);
      if (!self->reset_request)
        goto fail;

      gpiod_request_config_free (reset_req_config);
      gpiod_line_config_free (reset_line_config);
      gpiod_line_settings_free (reset_settings);
      gpiod_chip_close (reset_chip);
      reset_req_config = NULL;
      reset_line_config = NULL;
      reset_settings = NULL;
      reset_chip = NULL;
    }
  else
    {
      fp_dbg ("FTE3600 hardware reset line not claimed (allow_hardware_reset=FALSE)");
    }

  /* IRQ line configuration */
  irq_chip = gpiod_chip_open (irq_gpiochip_path);
  irq_settings = gpiod_line_settings_new ();
  irq_line_config = gpiod_line_config_new ();
  irq_req_config = gpiod_request_config_new ();
  if (!irq_chip || !irq_settings || !irq_line_config || !irq_req_config)
    goto fail;

  gpiod_request_config_set_consumer (irq_req_config, "libfprint-fte3600-irq");
  if (gpiod_line_settings_set_direction (
          irq_settings, GPIOD_LINE_DIRECTION_INPUT) < 0
      || gpiod_line_settings_set_edge_detection (
             irq_settings, GPIOD_LINE_EDGE_RISING) < 0
      || gpiod_line_config_add_line_settings (
             irq_line_config, &irq_offset, 1, irq_settings) < 0)
    goto fail;

  self->irq_request = gpiod_chip_request_lines (
      irq_chip, irq_req_config, irq_line_config);
  if (!self->irq_request)
    goto fail;

  event_buffer = gpiod_edge_event_buffer_new (8);
  if (!event_buffer)
    goto fail;

  self->irq_event_buffer = event_buffer;
  event_buffer = NULL;

  gpiod_request_config_free (irq_req_config);
  gpiod_line_config_free (irq_line_config);
  gpiod_line_settings_free (irq_settings);
  gpiod_chip_close (irq_chip);

  fp_dbg ("Using reset line %s:%u (claimed=%d) and finger IRQ line %s:%u for FTE3600",
          reset_gpiochip_path ? reset_gpiochip_path : "unclaimed",
          reset_offset,
          self->reset_request != NULL,
          irq_gpiochip_path, irq_offset);
  return TRUE;

fail:
  saved_errno = errno ? errno : ENOMEM;
  g_clear_pointer (&event_buffer, gpiod_edge_event_buffer_free);
  g_clear_pointer (&self->irq_request, gpiod_line_request_release);
  fte3600_deassert_hardware_reset_best_effort (self, "GPIO request failure");
  g_clear_pointer (&self->reset_request, gpiod_line_request_release);
  if (reset_req_config)
    gpiod_request_config_free (reset_req_config);
  if (reset_line_config)
    gpiod_line_config_free (reset_line_config);
  if (reset_settings)
    gpiod_line_settings_free (reset_settings);
  if (reset_chip)
    gpiod_chip_close (reset_chip);
  if (irq_req_config)
    gpiod_request_config_free (irq_req_config);
  if (irq_line_config)
    gpiod_line_config_free (irq_line_config);
  if (irq_settings)
    gpiod_line_settings_free (irq_settings);
  if (irq_chip)
    gpiod_chip_close (irq_chip);

  g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
               "Failed to claim FTE3600 GPIO lines: %s",
               g_strerror (saved_errno));
  return FALSE;
}

static gboolean
fte3600_drain_irq_events (FpiDeviceFte3600 *self, GError **error)
{
  gsize drained = 0;
  guint batches = 0;

  while (batches < FT9361_MAX_IRQ_DRAIN_BATCHES)
    {
      gint ready;
      gint count;

      do
        ready = gpiod_line_request_wait_edge_events (self->irq_request, 0);
      while (ready < 0 && errno == EINTR);

      if (ready == 0)
        break;
      if (ready < 0)
        goto fail;

      do
        count = gpiod_line_request_read_edge_events (
            self->irq_request, self->irq_event_buffer,
            gpiod_edge_event_buffer_get_capacity (self->irq_event_buffer));
      while (count < 0 && errno == EINTR);

      if (count < 0)
        goto fail;
      drained += count;
      batches++;
    }

  if (batches == FT9361_MAX_IRQ_DRAIN_BATCHES)
    {
      gint ready;

      /* Consuming exactly the batch limit is not itself evidence that the
       * queue is still active.  Probe once more without consuming anything;
       * fail only if an additional event is actually pending. */
      do
        ready = gpiod_line_request_wait_edge_events (self->irq_request, 0);
      while (ready < 0 && errno == EINTR);
      if (ready < 0)
        goto fail;
      if (ready > 0)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                               "FTE3600 IRQ remained continuously active "
                               "while draining stale events");
          return FALSE;
        }
    }

  if (drained > 0)
    fp_dbg ("Drained %" G_GSIZE_FORMAT " stale FTE3600 IRQ event(s)",
            drained);
  return TRUE;

fail:
  g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
               "Failed to drain FTE3600 finger IRQ events: %s",
               g_strerror (errno));
  return FALSE;
}

static gboolean
fte3600_irq_ready_cb (gint fd, GIOCondition condition, gpointer user_data)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (user_data);
  FpiSsm *ssm = self->irq_wait_ssm;
  g_autoptr (GError) error = NULL;
  gboolean have_rising_edge = FALSE;
  gint count;

  g_assert (ssm != NULL);
  g_assert_cmpint (fd, ==, gpiod_line_request_get_fd (self->irq_request));

  if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL))
    {
      error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
                                   "FTE3600 finger IRQ descriptor failed");
      goto out;
    }

  do
    count = gpiod_line_request_read_edge_events (
        self->irq_request, self->irq_event_buffer,
        gpiod_edge_event_buffer_get_capacity (self->irq_event_buffer));
  while (count < 0 && errno == EINTR);

  if (count < 0)
    {
      error = g_error_new (G_IO_ERROR, g_io_error_from_errno (errno),
                           "Failed to read FTE3600 finger IRQ: %s",
                           g_strerror (errno));
      goto out;
    }

  for (gint i = 0; i < count; i++)
    {
      struct gpiod_edge_event *event =
          gpiod_edge_event_buffer_get_event (self->irq_event_buffer, i);

      if (gpiod_edge_event_get_event_type (event)
              == GPIOD_EDGE_EVENT_RISING_EDGE
          && gpiod_edge_event_get_line_offset (event)
                 == self->gpio_profile->irq_offset)
        have_rising_edge = TRUE;
    }

  if (!have_rising_edge)
    return G_SOURCE_CONTINUE;

  self->armed = FALSE;
  self->capture_ready_deadline =
      g_get_monotonic_time () + FT9361_CAPTURE_READY_TIMEOUT_MS * 1000;

out:
  fte3600_clear_irq_source (self);
  if (error)
    fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
  else
    fpi_ssm_next_state (ssm);
  return G_SOURCE_REMOVE;
}

static void
fte3600_wait_for_irq (FpiSsm *ssm)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  gint fd;

  g_assert (self->irq_request != NULL);
  g_assert (self->irq_source == NULL);
  g_assert (self->irq_wait_ssm == NULL);

  fd = gpiod_line_request_get_fd (self->irq_request);
  self->irq_source = g_unix_fd_source_new (
      fd, G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL);
  self->irq_wait_ssm = ssm;
  g_source_set_name (self->irq_source, "FTE3600 finger IRQ");
  g_source_set_callback (self->irq_source,
                         G_SOURCE_FUNC (fte3600_irq_ready_cb), self, NULL);
  g_source_attach (self->irq_source, g_main_context_get_thread_default ());
}

static void
fte3600_submit_transfer (FpiSsm *ssm, FpiSpiTransfer *transfer,
                         gboolean cancellable)
{
  FpDevice *dev = fpi_ssm_get_device (ssm);

  transfer->ssm = ssm;
  fpi_spi_transfer_submit (
      transfer, cancellable ? fpi_device_get_cancellable (dev) : NULL,
      fpi_ssm_spi_transfer_cb, NULL);
}

static void
fte3600_submit_command (FpiSsm *ssm, guint8 command, gboolean cancellable)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  FpiSpiTransfer *transfer;

  transfer = fpi_spi_transfer_new (FP_DEVICE (self), self->spi_fd);
  fpi_spi_transfer_write (transfer, 1);
  transfer->buffer_wr[0] = command;
  fte3600_submit_transfer (ssm, transfer, cancellable);
}

static void
fte3600_submit_reg_write (FpiSsm *ssm, guint8 reg, guint8 value,
                          gboolean cancellable)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  FpiSpiTransfer *transfer;

  transfer = fpi_spi_transfer_new (FP_DEVICE (self), self->spi_fd);
  fpi_spi_transfer_write (transfer, FT9361_REG_WRITE_SIZE);
  transfer->buffer_wr[0] = 0x11;
  transfer->buffer_wr[1] = 0xee;
  transfer->buffer_wr[2] = reg;
  transfer->buffer_wr[3] = value;
  transfer->buffer_wr[4] = 0x00;
  fte3600_submit_transfer (ssm, transfer, cancellable);
}

static void
fte3600_reg_read_cb (FpiSpiTransfer *transfer,
                     FpDevice       *device,
                     gpointer        user_data,
                     GError         *error)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (device);

  self->small_rx_valid = error == NULL;
  fpi_ssm_spi_transfer_cb (transfer, device, user_data, error);
}

static void
fte3600_submit_reg_read (FpiSsm *ssm, guint8 reg, gsize result_len,
                         gboolean cancellable)
{
  FpDevice *device;
  FpiDeviceFte3600 *self;
  FpiSpiTransfer *transfer;
  gsize frame_len = FT9361_REG_READ_HEADER_SIZE + result_len;

  g_return_if_fail (ssm != NULL);

  device = fpi_ssm_get_device (ssm);
  if (G_UNLIKELY (!FPI_IS_DEVICE_FTE3600 (device)))
    {
      fpi_ssm_mark_failed (
          ssm, fpi_device_error_new_msg (
                   FP_DEVICE_ERROR_GENERAL,
                   "FTE3600 register-read state machine has no valid device"));
      return;
    }
  self = FPI_DEVICE_FTE3600 (device);

  g_assert (result_len > 0);
  g_assert (frame_len <= sizeof (self->small_rx));

  memset (self->small_rx, 0, frame_len);
  self->small_rx_valid = FALSE;
  transfer = fpi_spi_transfer_new (FP_DEVICE (self), self->spi_fd);
  fpi_spi_transfer_write (transfer, frame_len);
  transfer->buffer_wr[0] = 0x10;
  transfer->buffer_wr[1] = 0xef;
  transfer->buffer_wr[2] = reg;
  transfer->buffer_wr[3] = 0x00;
  fpi_spi_transfer_read_full (transfer, self->small_rx, frame_len, NULL);
  fpi_spi_transfer_set_full_duplex (transfer, TRUE);
  transfer->ssm = ssm;
  fpi_spi_transfer_submit (
      transfer, cancellable ? fpi_device_get_cancellable (device) : NULL,
      fte3600_reg_read_cb, NULL);
}

static void
fte3600_submit_capture (FpiSsm *ssm)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  FpiSpiTransfer *transfer;

  memset (self->capture_rx, 0, FT9361_CAPTURE_FRAME_SIZE);
  transfer = fpi_spi_transfer_new (FP_DEVICE (self), self->spi_fd);
  fpi_spi_transfer_write_full (transfer, self->capture_tx,
                               FT9361_CAPTURE_FRAME_SIZE, NULL);
  fpi_spi_transfer_read_full (transfer, self->capture_rx,
                              FT9361_CAPTURE_FRAME_SIZE, NULL);
  fpi_spi_transfer_set_full_duplex (transfer, TRUE);
  fpi_spi_transfer_set_sensitive (transfer, TRUE);
  fte3600_submit_transfer (ssm, transfer, TRUE);
}

static guint8
fte3600_read_result_byte (FpiDeviceFte3600 *self)
{
  return self->small_rx[FT9361_REG_READ_HEADER_SIZE];
}

static gboolean
fte3600_mcu_is_idle (FpiDeviceFte3600 *self)
{
  return self->small_rx_valid
         && self->small_rx[FT9361_REG_READ_HEADER_SIZE] == 0xa5
         && self->small_rx[FT9361_REG_READ_HEADER_SIZE + 1] == 0x5a;
}

static void
fte3600_set_hardware_reset (FpiSsm            *ssm,
                            FpiDeviceFte3600  *self,
                            gboolean           asserted)
{
  enum gpiod_line_value value;

  if (!self->gpio_profile || !self->gpio_profile->allow_hardware_reset
      || !self->reset_request)
    {
      fpi_ssm_mark_failed (
          ssm, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                    "Hardware reset recovery is not enabled or verified for this platform"));
      return;
    }

  value = fte3600_reset_line_value (self->gpio_profile, asserted);
  self->idle_verified = FALSE;
  if (gpiod_line_request_set_value (
          self->reset_request, self->gpio_profile->reset_offset, value) < 0)
    {
      fpi_ssm_mark_failed (
          ssm, g_error_new (G_IO_ERROR, g_io_error_from_errno (errno),
                            "Failed to %s the FTE3600 hardware reset line: %s",
                            asserted ? "assert" : "deassert",
                            g_strerror (errno)));
      return;
    }

  fpi_ssm_next_state (ssm);
}

static gboolean
fte3600_fail_if_cancelled (FpiSsm *ssm, FpDevice *dev)
{
  GCancellable *cancellable;
  GError *error = NULL;

  if (!fpi_device_action_is_cancelled (dev))
    return FALSE;

  cancellable = fpi_device_get_cancellable (dev);
  if (!cancellable
      || !g_cancellable_set_error_if_cancelled (cancellable, &error))
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                 "Fingerprint operation was cancelled");

  fpi_ssm_mark_failed (ssm, error);
  return TRUE;
}

static void
fte3600_init_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  guint state = fpi_ssm_get_cur_state (ssm);
  guint8 value;

  /* Once reset has been asserted, always finish the complete pulse train and
   * return the active-low line high before observing cancellation. */
  if ((state < FTE3600_INIT_HARD_RESET_ASSERT_1
       || state > FTE3600_INIT_HARD_RESET_BOOT)
      && fte3600_fail_if_cancelled (ssm, dev))
    return;

  switch (state)
    {
    case FTE3600_INIT_RESET_1:
    case FTE3600_INIT_RESET_2:
      fte3600_submit_command (ssm, 0x70, TRUE);
      return;

    case FTE3600_INIT_RESET_DELAY:
      fpi_ssm_next_state_delayed (ssm, FT9361_RESET_DELAY_MS);
      return;

    case FTE3600_INIT_RESET_SETTLE:
      fpi_ssm_next_state_delayed (ssm, FT9361_RESET_SETTLE_MS);
      return;

    case FTE3600_INIT_READ_MCU_STATUS:
      fte3600_submit_reg_read (ssm, FT9361_REG_MCU_STATUS, 2, TRUE);
      return;

    case FTE3600_INIT_CHECK_MCU_STATUS:
      if (!fte3600_mcu_is_idle (self))
        {
          if (!self->init_hardware_reset_attempted
              && self->gpio_profile
              && self->gpio_profile->allow_hardware_reset)
            {
              self->init_hardware_reset_attempted = TRUE;
              self->armed = FALSE;
              fte3600_clear_irq_source (self);
              fp_warn ("FT9361 soft reset returned %02x %02x; attempting "
                       "one DMI-verified hardware recovery",
                       self->small_rx[4], self->small_rx[5]);
              fpi_ssm_jump_to_state (
                  ssm, FTE3600_INIT_HARD_RESET_ASSERT_1);
              return;
            }

          fpi_ssm_mark_failed (
            ssm, fpi_device_error_new_msg (
                   FP_DEVICE_ERROR_PROTO,
                   self->init_hardware_reset_attempted
                     ? "FT9361 MCU did not return to idle after hardware recovery (%02x %02x)"
                     : "FT9361 MCU did not return to idle (%02x %02x)",
                   self->small_rx[4], self->small_rx[5]));
          return;
        }
      fpi_ssm_jump_to_state (ssm, FTE3600_INIT_READ_ID_HIGH);
      return;

    case FTE3600_INIT_HARD_RESET_ASSERT_1:
    case FTE3600_INIT_HARD_RESET_ASSERT_2:
      fte3600_set_hardware_reset (ssm, self, TRUE);
      return;

    case FTE3600_INIT_HARD_RESET_HOLD_1:
    case FTE3600_INIT_HARD_RESET_HOLD_2:
      fpi_ssm_next_state_delayed (ssm, FT9361_HARD_RESET_PULSE_MS);
      return;

    case FTE3600_INIT_HARD_RESET_DEASSERT_1:
    case FTE3600_INIT_HARD_RESET_DEASSERT_2:
      fte3600_set_hardware_reset (ssm, self, FALSE);
      return;

    case FTE3600_INIT_HARD_RESET_INTERVAL:
      fpi_ssm_next_state_delayed (ssm, FT9361_HARD_RESET_INTERVAL_MS);
      return;

    case FTE3600_INIT_HARD_RESET_BOOT:
      fpi_ssm_jump_to_state_delayed (
          ssm, FTE3600_INIT_RESET_1, FT9361_HARD_RESET_BOOT_MS);
      return;

    case FTE3600_INIT_READ_ID_HIGH:
      fte3600_submit_reg_read (ssm, FT9361_REG_SENSOR_ID_HIGH, 1, TRUE);
      return;

    case FTE3600_INIT_CHECK_ID_HIGH:
      value = fte3600_read_result_byte (self);
      if (value != FT9361_SENSOR_ID_HIGH)
        {
          fpi_ssm_mark_failed (
              ssm, fpi_device_error_new_msg (
                       FP_DEVICE_ERROR_NOT_SUPPORTED,
                       "Unexpected FTE3600 sensor ID high byte %02x", value));
          return;
        }
      fpi_ssm_next_state (ssm);
      return;

    case FTE3600_INIT_READ_ID_LOW:
      fte3600_submit_reg_read (ssm, FT9361_REG_SENSOR_ID_LOW, 1, TRUE);
      return;

    case FTE3600_INIT_CHECK_ID_LOW:
      value = fte3600_read_result_byte (self);
      if (value != FT9361_SENSOR_ID_LOW)
        {
          fpi_ssm_mark_failed (
              ssm, fpi_device_error_new_msg (
                       FP_DEVICE_ERROR_NOT_SUPPORTED,
                       "Unexpected FTE3600 sensor ID low byte %02x", value));
          return;
        }
      fpi_ssm_next_state (ssm);
      return;

    case FTE3600_INIT_READ_FW_VERSION:
      fte3600_submit_reg_read (ssm, FT9361_REG_FW_VERSION, 1, TRUE);
      return;

    case FTE3600_INIT_CHECK_FW_VERSION:
      value = fte3600_read_result_byte (self);
      if (value != FT9361_FW_VERSION)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (
                                   FP_DEVICE_ERROR_PROTO,
                                   "Unexpected FT9361 firmware version %02x; "
                                   "firmware upload is disabled",
                                   value));
          return;
        }
      fpi_ssm_next_state (ssm);
      return;

    case FTE3600_INIT_READ_AGC_VERSION:
      fte3600_submit_reg_read (ssm, FT9361_REG_AGC_VERSION, 1, TRUE);
      return;

    case FTE3600_INIT_CHECK_AGC_VERSION:
      value = fte3600_read_result_byte (self);
      if (value != FT9361_AGC_VERSION)
        {
          fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (
                                        FP_DEVICE_ERROR_PROTO,
                                        "Unexpected FT9361 AGC version %02x; "
                                        "firmware upload is disabled",
                                        value));
          return;
        }
      fpi_ssm_next_state (ssm);
      return;

    case FTE3600_INIT_READ_CONFIG_MARKER:
      fte3600_submit_reg_read (ssm, FT9361_REG_CONFIG_MARKER, 1, TRUE);
      return;

    case FTE3600_INIT_CHECK_CONFIG_MARKER:
      if (fte3600_read_result_byte (self) == 0xbb)
        {
          fpi_ssm_jump_to_state (
              ssm, FTE3600_INIT_FINAL_READ_MCU_STATUS);
          return;
        }
      fpi_ssm_next_state (ssm);
      return;

    case FTE3600_INIT_WRITE_CONFIG_01:
      self->idle_verified = FALSE;
      fte3600_submit_reg_write (ssm, 0x01, 0x01, TRUE);
      return;

    case FTE3600_INIT_CONFIG_01_DELAY:
    case FTE3600_INIT_CONFIG_41_DELAY:
    case FTE3600_INIT_CONFIG_MARKER_DELAY:
    case FTE3600_INIT_CONFIG_22_DELAY:
    case FTE3600_INIT_CONFIG_23_DELAY:
      fpi_ssm_next_state_delayed (ssm, FT9361_CONFIG_DELAY_MS);
      return;

    case FTE3600_INIT_WRITE_CONFIG_41:
      fte3600_submit_reg_write (ssm, FT9361_REG_CONFIG_41, 0x0f, TRUE);
      return;

    case FTE3600_INIT_WRITE_CONFIG_MARKER:
      fte3600_submit_reg_write (ssm, FT9361_REG_CONFIG_MARKER, 0xbb, TRUE);
      return;

    case FTE3600_INIT_VERIFY_CONFIG_MARKER:
      fte3600_submit_reg_read (ssm, FT9361_REG_CONFIG_MARKER, 1, TRUE);
      return;

    case FTE3600_INIT_CHECK_CONFIG_VERIFY:
      value = fte3600_read_result_byte (self);
      if (value != 0xbb)
        {
          fpi_ssm_mark_failed (
              ssm, fpi_device_error_new_msg (
                       FP_DEVICE_ERROR_PROTO,
                       "FT9361 MCU configuration verification failed (%02x)",
                       value));
          return;
        }
      fpi_ssm_next_state (ssm);
      return;

    case FTE3600_INIT_WRITE_CONFIG_22:
      fte3600_submit_reg_write (ssm, FT9361_REG_CONFIG_22, 0x00, TRUE);
      return;

    case FTE3600_INIT_WRITE_CONFIG_23:
      fte3600_submit_reg_write (ssm, FT9361_REG_CONFIG_23, 0x0e, TRUE);
      return;

    case FTE3600_INIT_FINAL_READ_MCU_STATUS:
      fte3600_submit_reg_read (ssm, FT9361_REG_MCU_STATUS, 2, TRUE);
      return;

    case FTE3600_INIT_FINAL_CHECK_MCU_STATUS:
      if (!fte3600_mcu_is_idle (self))
        {
          fpi_ssm_mark_failed (
              ssm, fpi_device_error_new_msg (
                       FP_DEVICE_ERROR_PROTO,
                       "FT9361 MCU left idle during initialization "
                       "(%02x %02x)",
                       self->small_rx[4], self->small_rx[5]));
          return;
        }
      self->idle_verified = TRUE;
      fpi_ssm_next_state (ssm);
      return;

    case FTE3600_INIT_DONE:
      fpi_ssm_mark_completed (ssm);
      return;

    case FTE3600_INIT_NSTATES:
      g_assert_not_reached ();
    }
}

static void
fte3600_init_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);

  if (error)
    {
      self->idle_verified = FALSE;
      fte3600_deassert_hardware_reset_best_effort (
          self, "recovering from initialization failure");
      fte3600_start_reset (self, FTE3600_RESET_FOR_OPEN_ERROR, error);
      return;
    }

  fpi_device_open_complete (dev, NULL);
}

static void
fte3600_arm_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  guint8 mode;

  if (fte3600_fail_if_cancelled (ssm, dev))
    return;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FTE3600_ARM_READ_MCU_STATUS:
      fte3600_submit_reg_read (ssm, FT9361_REG_MCU_STATUS, 2, TRUE);
      return;

    case FTE3600_ARM_CHECK_MCU_STATUS:
      if (fte3600_mcu_is_idle (self))
        fpi_ssm_jump_to_state (ssm, FTE3600_ARM_READ_MODE);
      else
        {
          fp_dbg ("FT9361 unexpectedly busy before mode-1 rearm; recovering");
          fpi_ssm_next_state (ssm);
        }
      return;

    case FTE3600_ARM_RECOVERY_RESET_1:
    case FTE3600_ARM_RECOVERY_RESET_2:
      fte3600_submit_command (ssm, 0x70, TRUE);
      return;

    case FTE3600_ARM_RECOVERY_RESET_DELAY:
      fpi_ssm_next_state_delayed (ssm, FT9361_RESET_DELAY_MS);
      return;

    case FTE3600_ARM_READ_MODE:
      fte3600_submit_reg_read (ssm, FT9361_REG_CAPTURE_MODE, 1, TRUE);
      return;

    case FTE3600_ARM_CHECK_MODE:
      mode = fte3600_read_result_byte (self);
      if (mode == 0x02 || mode == 0x03 || mode == 0x04)
        {
          fpi_ssm_jump_to_state (ssm, FTE3600_ARM_WRITE_MODE);
          return;
        }

      /* Vendor mode-1 rearm stops both capture controls first.  Unknown
       * values deliberately take the same conservative path. */
      if (mode != 0x01)
        fp_dbg ("Stopping FT9361 capture controls from unknown mode %02x",
                mode);
      fpi_ssm_next_state (ssm);
      return;

    case FTE3600_ARM_STOP_START:
      fte3600_submit_reg_write (ssm, FT9361_REG_CAPTURE_START, 0x00, TRUE);
      return;

    case FTE3600_ARM_STOP_ENABLE:
      fte3600_submit_reg_write (ssm, FT9361_REG_CAPTURE_ENABLE, 0x00, TRUE);
      return;

    case FTE3600_ARM_STOP_DELAY:
      fpi_ssm_next_state_delayed (ssm, FT9361_ARM_DELAY_MS);
      return;

    case FTE3600_ARM_WRITE_MODE:
      {
        g_autoptr (GError) error = NULL;

        if (!fte3600_drain_irq_events (self, &error))
          {
            fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
            return;
          }
      }
      self->arm_attempts++;
      fte3600_submit_reg_write (ssm, FT9361_REG_CAPTURE_MODE, 0x01, TRUE);
      return;

    case FTE3600_ARM_WRITE_ENABLE:
      fte3600_submit_reg_write (ssm, FT9361_REG_CAPTURE_ENABLE, 0x01, TRUE);
      return;

    case FTE3600_ARM_WRITE_START:
      fte3600_submit_reg_write (ssm, FT9361_REG_CAPTURE_START, 0x01, TRUE);
      return;

    case FTE3600_ARM_DELAY:
      fpi_ssm_next_state_delayed (ssm, FT9361_ARM_DELAY_MS);
      return;

    case FTE3600_ARM_DRAIN_FINGER_STATUS:
      fte3600_submit_reg_read (ssm, FT9361_REG_FINGER_STATUS, 1, TRUE);
      return;

    case FTE3600_ARM_READ_ARMED_MCU_STATUS:
      fte3600_submit_reg_read (ssm, FT9361_REG_MCU_STATUS, 2, TRUE);
      return;

    case FTE3600_ARM_CHECK_ARMED_MCU_STATUS:
      if (fte3600_mcu_is_idle (self))
        {
          if (self->arm_attempts >= FT9361_ARM_MAX_ATTEMPTS
              || g_get_monotonic_time () >= self->arm_deadline)
            {
              fpi_ssm_mark_failed (
                  ssm, g_error_new_literal (
                           G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                           "FT9361 failed to enter armed mode after bounded "
                           "mode-1 retries"));
              return;
            }
          fp_dbg ("Discarded an FT9361 event which raced with arming");
          fpi_ssm_jump_to_state_delayed (
              ssm, FTE3600_ARM_READ_MCU_STATUS, FT9361_POLL_DELAY_MS);
        }
      else
        {
          fpi_ssm_next_state (ssm);
        }
      return;

    case FTE3600_ARM_DONE:
      fpi_ssm_mark_completed (ssm);
      return;

    case FTE3600_ARM_NSTATES:
      g_assert_not_reached ();
    }
}

static FpiSsm *
fte3600_new_arm_ssm (FpiDeviceFte3600 *self)
{
  self->arm_attempts = 0;
  self->arm_deadline =
      g_get_monotonic_time () + FT9361_ARM_TIMEOUT_MS * 1000;
  return fpi_ssm_new (FP_DEVICE (self), fte3600_arm_handler,
                      FTE3600_ARM_NSTATES);
}

static void
fte3600_capture_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  gint state = fpi_ssm_get_cur_state (ssm);
  guint8 finger_status;

  if (state < FTE3600_CAPTURE_CLEANUP_DISPATCH
      && fte3600_fail_if_cancelled (ssm, dev))
    return;

  switch (state)
    {
    case FTE3600_CAPTURE_PREPARE_ARM:
      if (self->armed)
        fpi_ssm_next_state (ssm);
      else
        fpi_ssm_start_subsm (ssm, fte3600_new_arm_ssm (self));
      return;

    case FTE3600_CAPTURE_WAIT_FINGER_IRQ:
      self->armed = TRUE;
      fte3600_wait_for_irq (ssm);
      return;

    case FTE3600_CAPTURE_POLL_MCU_STATUS:
      fte3600_submit_reg_read (ssm, FT9361_REG_MCU_STATUS, 2, TRUE);
      return;

    case FTE3600_CAPTURE_CHECK_MCU_STATUS:
      if (fte3600_mcu_is_idle (self))
        fpi_ssm_next_state (ssm);
      else if (g_get_monotonic_time () >= self->capture_ready_deadline)
        fpi_ssm_mark_failed (
            ssm, g_error_new_literal (
                     G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                     "FT9361 did not become ready after its finger IRQ"));
      else
        fpi_ssm_jump_to_state_delayed (ssm, FTE3600_CAPTURE_POLL_MCU_STATUS,
                                       FT9361_POLL_DELAY_MS);
      return;

    case FTE3600_CAPTURE_READ_FINGER_STATUS:
      fte3600_submit_reg_read (ssm, FT9361_REG_FINGER_STATUS, 1, TRUE);
      return;

    case FTE3600_CAPTURE_CHECK_FINGER_STATUS:
      finger_status = fte3600_read_result_byte (self);
      if (finger_status == 0x01 || finger_status == 0xa0)
        {
          self->false_irq_count = 0;
          fpi_device_report_finger_status (
              dev, FP_FINGER_STATUS_NEEDED | FP_FINGER_STATUS_PRESENT);
          fpi_ssm_jump_to_state (ssm, FTE3600_CAPTURE_READ_IMAGE);
        }
      else
        {
          self->false_irq_count++;
          if (self->false_irq_count >= FT9361_MAX_FALSE_IRQS)
            {
              fpi_ssm_mark_failed (
                  ssm, g_error_new_literal (
                           G_IO_ERROR, G_IO_ERROR_FAILED,
                           "FT9361 produced too many consecutive non-finger "
                           "interrupts"));
              return;
            }
          fp_dbg ("Ignoring non-finger status %02x and entering quick mode",
                  finger_status);
          fpi_ssm_next_state (ssm);
        }
      return;

    case FTE3600_CAPTURE_QUICK_READ_MCU_STATUS:
      fte3600_submit_reg_read (ssm, FT9361_REG_MCU_STATUS, 2, TRUE);
      return;

    case FTE3600_CAPTURE_QUICK_CHECK_MCU_STATUS:
      if (fte3600_mcu_is_idle (self))
        fpi_ssm_next_state (ssm);
      else
        {
          fp_warn ("FT9361 became busy before quick-mode rearm; recovering");
          self->armed = FALSE;
          fpi_ssm_jump_to_state (ssm, FTE3600_CAPTURE_PREPARE_ARM);
        }
      return;

    case FTE3600_CAPTURE_QUICK_WRITE_MODE:
      fte3600_submit_reg_write (ssm, FT9361_REG_CAPTURE_MODE, 0x02, TRUE);
      return;

    case FTE3600_CAPTURE_QUICK_WRITE_TRIGGER:
      fte3600_submit_reg_write (ssm, FT9361_REG_QUICK_TRIGGER, 0x01, TRUE);
      return;

    case FTE3600_CAPTURE_QUICK_READ_ARMED_MCU_STATUS:
      fte3600_submit_reg_read (ssm, FT9361_REG_MCU_STATUS, 2, TRUE);
      return;

    case FTE3600_CAPTURE_QUICK_CHECK_ARMED_MCU_STATUS:
      if (!fte3600_mcu_is_idle (self))
        {
          self->armed = TRUE;
          fpi_ssm_jump_to_state (ssm, FTE3600_CAPTURE_WAIT_FINGER_IRQ);
        }
      else
        {
          fp_warn ("FT9361 quick-mode rearm stayed idle; recovering in "
                   "regular mode");
          self->armed = FALSE;
          fpi_ssm_jump_to_state (ssm, FTE3600_CAPTURE_PREPARE_ARM);
        }
      return;

    case FTE3600_CAPTURE_READ_IMAGE:
      fte3600_submit_capture (ssm);
      return;

    case FTE3600_CAPTURE_PROCESS_IMAGE:
      fte3600_clear_captured_image (self);
      self->captured_image =
          fp_image_new (FT9361_IMAGE_WIDTH, FT9361_IMAGE_HEIGHT);
      self->captured_image->ppmm = FT9361_IMAGE_PPMM;
      self->captured_image->flags |= FPI_IMAGE_PARTIAL;

      for (gsize i = 0; i < FT9361_IMAGE_SIZE; i++)
        self->captured_image->data[i] =
            (guint8)~self->capture_rx[FT9361_CAPTURE_DATA_OFFSET + i];

      fp_dbg ("Captured FT9361 image (turnaround %02x %02x)",
              self->capture_rx[6], self->capture_rx[7]);
      fte3600_secure_clear (self->capture_rx, FT9361_CAPTURE_FRAME_SIZE);
      /* Register 0x1d is a latched event result, not a live contact signal.
       * Cleanup rearms mode 1 before the matcher only when another enrollment
       * stage is expected.  A terminal capture is physically reset before its
       * action completes, so a GPIO edge from between actions can never be
       * replayed into the next request. */
      fpi_ssm_next_state (ssm);
      return;

    case FTE3600_CAPTURE_CLEANUP_DISPATCH:
      if (fpi_ssm_get_error (ssm))
        {
          fte3600_secure_clear (self->capture_rx,
                                FT9361_CAPTURE_FRAME_SIZE);
          self->armed = FALSE;
          fpi_ssm_jump_to_state (ssm, FTE3600_CAPTURE_CLEANUP_RESET_1);
        }
      else if (fpi_device_get_current_action (dev) != FPI_DEVICE_ACTION_ENROLL
               || self->enroll_stages_passed + 1 >=
                    (guint) fp_device_get_nr_enroll_stages (dev))
        {
          fp_dbg ("Resetting FT9361 before terminal action completion");
          self->armed = FALSE;
          fpi_ssm_jump_to_state (ssm, FTE3600_CAPTURE_CLEANUP_RESET_1);
        }
      else
        {
          fp_dbg ("Immediately rearming FT9361 for the next enrollment stage");
          fpi_ssm_next_state (ssm);
        }
      return;

    case FTE3600_CAPTURE_CLEANUP_REARM:
      self->armed = FALSE;
      fpi_ssm_start_subsm (ssm, fte3600_new_arm_ssm (self));
      return;

    case FTE3600_CAPTURE_CLEANUP_REARM_DONE:
      if (fpi_ssm_get_error (ssm))
        {
          self->armed = FALSE;
          fpi_ssm_jump_to_state (ssm, FTE3600_CAPTURE_CLEANUP_RESET_1);
        }
      else
        {
          self->armed = TRUE;
          fpi_ssm_jump_to_state (ssm, FTE3600_CAPTURE_DONE);
        }
      return;

    case FTE3600_CAPTURE_CLEANUP_RESET_1:
      self->armed = FALSE;
      self->idle_verified = FALSE;
      fte3600_clear_irq_source (self);
      G_GNUC_FALLTHROUGH;
    case FTE3600_CAPTURE_CLEANUP_RESET_2:
      fte3600_submit_command (ssm, 0x70, FALSE);
      return;

    case FTE3600_CAPTURE_CLEANUP_RESET_DELAY:
      fpi_ssm_next_state_delayed (ssm, FT9361_RESET_DELAY_MS);
      return;

    case FTE3600_CAPTURE_CLEANUP_RESET_SETTLE:
      fpi_ssm_next_state_delayed (ssm, FT9361_RESET_SETTLE_MS);
      return;

    case FTE3600_CAPTURE_CLEANUP_RESET_READ_MCU_STATUS:
      fte3600_submit_reg_read (ssm, FT9361_REG_MCU_STATUS, 2, FALSE);
      return;

    case FTE3600_CAPTURE_CLEANUP_RESET_CHECK_MCU_STATUS:
      if (!fte3600_mcu_is_idle (self))
        {
          fpi_ssm_mark_failed (
              ssm, fpi_device_error_new_msg (
                       FP_DEVICE_ERROR_PROTO,
                       "FT9361 MCU did not return to idle after reset "
                       "(%02x %02x)",
                       self->small_rx[4], self->small_rx[5]));
          return;
        }
      self->idle_verified = TRUE;
      fpi_ssm_next_state (ssm);
      return;

    case FTE3600_CAPTURE_DONE:
      fpi_ssm_mark_completed (ssm);
      return;

    case FTE3600_CAPTURE_NSTATES:
      g_assert_not_reached ();
    }
}

static void fte3600_start_capture (FpiDeviceFte3600 *self);

static GError *
fte3600_enroll_retry_error (Fte3600TemplateStatus status)
{
  switch (status)
    {
    case FTE3600_TEMPLATE_RETRY_LOW_CONTRAST:
      return fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL);

    case FTE3600_TEMPLATE_RETRY_INSUFFICIENT_FEATURES:
      return fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER);

    case FTE3600_TEMPLATE_RETRY_DUPLICATE:
    case FTE3600_TEMPLATE_RETRY_INCONSISTENT:
      return fpi_device_retry_new (FP_DEVICE_RETRY_REMOVE_FINGER);

    case FTE3600_TEMPLATE_OK:
    case FTE3600_TEMPLATE_NEED_MORE_SAMPLES:
    case FTE3600_TEMPLATE_INVALID_WIRE:
    case FTE3600_TEMPLATE_UNSUPPORTED_SCHEMA:
    case FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR:
    case FTE3600_TEMPLATE_UNSUPPORTED_POLICY:
    case FTE3600_TEMPLATE_NOT_CALIBRATED:
      g_assert_not_reached ();
    }

  g_assert_not_reached ();
}

static GError *
fte3600_enroll_fatal_error (const Fte3600EnrollJob *job)
{
  switch (job->status)
    {
    case FTE3600_TEMPLATE_INVALID_WIRE:
      return fpi_device_error_new_msg (
          FP_DEVICE_ERROR_DATA_INVALID,
          "FTE3600 BRISK enrollment data was invalid (extract %u, template %u)",
          (guint) job->extract_status, (guint) job->status);

    case FTE3600_TEMPLATE_UNSUPPORTED_SCHEMA:
    case FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR:
    case FTE3600_TEMPLATE_UNSUPPORTED_POLICY:
    case FTE3600_TEMPLATE_NOT_CALIBRATED:
      return fpi_device_error_new_msg (
          FP_DEVICE_ERROR_NOT_SUPPORTED,
          "FTE3600 BRISK enrollment format is unsupported (template %u)",
          (guint) job->status);

    case FTE3600_TEMPLATE_OK:
    case FTE3600_TEMPLATE_NEED_MORE_SAMPLES:
    case FTE3600_TEMPLATE_RETRY_LOW_CONTRAST:
    case FTE3600_TEMPLATE_RETRY_INSUFFICIENT_FEATURES:
    case FTE3600_TEMPLATE_RETRY_DUPLICATE:
    case FTE3600_TEMPLATE_RETRY_INCONSISTENT:
      g_assert_not_reached ();
    }

  g_assert_not_reached ();
}

static Fte3600TemplateStatus
fte3600_extract_status_to_template_status (Fte3600BriskStatus status)
{
  switch (status)
    {
    case FTE3600_BRISK_OK:
      g_assert_not_reached ();

    case FTE3600_BRISK_LOW_CONTRAST:
      return FTE3600_TEMPLATE_RETRY_LOW_CONTRAST;

    case FTE3600_BRISK_INSUFFICIENT_FEATURES:
      return FTE3600_TEMPLATE_RETRY_INSUFFICIENT_FEATURES;

    case FTE3600_BRISK_NO_CONSENSUS:
    case FTE3600_BRISK_INVALID_ARGUMENT:
      return FTE3600_TEMPLATE_INVALID_WIRE;
    }

  g_assert_not_reached ();
}

static void
fte3600_enroll_job_free (Fte3600EnrollJob *job)
{
  if (job == NULL)
    return;

  g_clear_pointer (&job->enroll_template, fte3600_template_free);
  g_clear_pointer (&job->encoded_template, g_bytes_unref);
  fte3600_secure_clear (job->image, sizeof (job->image));
  g_free (job);
}

static void
fte3600_enroll_worker (GTask        *task,
                       gpointer      source_object,
                       gpointer      task_data,
                       GCancellable *cancellable)
{
  Fte3600EnrollJob *job = task_data;
  Fte3600BriskFeatureSet features = { 0 };

  (void) source_object;
  (void) cancellable;

  if (g_task_return_error_if_cancelled (task))
    goto out;

  job->extract_status =
      fte3600_brisk_extract (job->image, sizeof (job->image), &features);
  fte3600_secure_clear (job->image, sizeof (job->image));
  if (g_task_return_error_if_cancelled (task))
    goto out;

  if (job->extract_status != FTE3600_BRISK_OK)
    {
      job->status =
          fte3600_extract_status_to_template_status (job->extract_status);
    }
  else
    {
      job->status = fte3600_template_add_features (job->enroll_template,
                                                   &features, NULL);
      if (g_task_return_error_if_cancelled (task))
        goto out;

      if (job->status == FTE3600_TEMPLATE_OK)
        {
          job->encode_status =
              fte3600_template_encode (job->enroll_template,
                                       &job->encoded_template);
        }
    }

  if (!g_task_return_error_if_cancelled (task))
    g_task_return_boolean (task, TRUE);

out:
  fte3600_secure_clear (&features, sizeof (features));
}

static void
fte3600_enroll_process (FpiDeviceFte3600 *self,
                        Fte3600EnrollJob  *job)
{
  FpDevice *dev = FP_DEVICE (self);
  const guint completed_stages = self->enroll_stages_passed + 1;

  if (job->status == FTE3600_TEMPLATE_OK &&
      job->encode_status != FTE3600_TEMPLATE_OK)
    {
      fte3600_complete_action_error (
          self, fpi_device_error_new_msg (
                    FP_DEVICE_ERROR_DATA_INVALID,
                    "FTE3600 encoder rejected a completed template (%u)",
                    (guint) job->encode_status));
      return;
    }

  switch (job->status)
    {
    case FTE3600_TEMPLATE_RETRY_LOW_CONTRAST:
    case FTE3600_TEMPLATE_RETRY_INSUFFICIENT_FEATURES:
    case FTE3600_TEMPLATE_RETRY_DUPLICATE:
    case FTE3600_TEMPLATE_RETRY_INCONSISTENT:
      fp_info ("Enrollment sample rejected by BRISK/template (%u/%u)",
               (guint) job->extract_status, (guint) job->status);
      fpi_device_enroll_progress (dev, self->enroll_stages_passed, NULL,
                                  fte3600_enroll_retry_error (job->status));
      fte3600_start_capture (self);
      return;

    case FTE3600_TEMPLATE_INVALID_WIRE:
    case FTE3600_TEMPLATE_UNSUPPORTED_SCHEMA:
    case FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR:
    case FTE3600_TEMPLATE_UNSUPPORTED_POLICY:
    case FTE3600_TEMPLATE_NOT_CALIBRATED:
      fte3600_complete_action_error (self,
                                     fte3600_enroll_fatal_error (job));
      return;

    case FTE3600_TEMPLATE_NEED_MORE_SAMPLES:
      if (completed_stages >= FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES ||
          job->encoded_template != NULL)
        {
          fte3600_complete_action_error (
              self, fpi_device_error_new_msg (
                        FP_DEVICE_ERROR_DATA_INVALID,
                        "FTE3600 template did not finish at its declared stage"));
          return;
        }
      break;

    case FTE3600_TEMPLATE_OK:
      if (completed_stages != FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES ||
          job->encoded_template == NULL)
        {
          fte3600_complete_action_error (
              self, fpi_device_error_new_msg (
                        FP_DEVICE_ERROR_DATA_INVALID,
                        "FTE3600 template finished at an unexpected stage"));
          return;
        }
      break;
    }

  g_clear_pointer (&self->enroll_template, fte3600_template_free);
  self->enroll_template = g_steal_pointer (&job->enroll_template);
  self->enroll_stages_passed = completed_stages;
  fpi_device_enroll_progress (dev, self->enroll_stages_passed, NULL, NULL);

  if (job->status == FTE3600_TEMPLATE_NEED_MORE_SAMPLES)
    {
      fte3600_start_capture (self);
      return;
    }

  {
    g_autoptr (GVariant) data = NULL;
    FpPrint *print = NULL;
    const guint8 *wire_data;
    gsize wire_size;

    wire_data = g_bytes_get_data (job->encoded_template, &wire_size);
    if (wire_data == NULL || wire_size < FTE3600_TEMPLATE_WIRE_HEADER_SIZE ||
        wire_size > FTE3600_TEMPLATE_CURRENT_MAX_WIRE_SIZE)
      {
        fte3600_complete_action_error (
            self, fpi_device_error_new_msg (
                      FP_DEVICE_ERROR_DATA_INVALID,
                      "FTE3600 encoder produced an invalid template length"));
        return;
      }

    data = g_variant_ref_sink (
        g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, wire_data, wire_size,
                                   sizeof (*wire_data)));
    g_assert (g_variant_is_of_type (data, G_VARIANT_TYPE ("ay")));
    fpi_device_get_enroll_data (dev, &print);
    fpi_print_set_type (print, FPI_PRINT_RAW);
    g_object_set (print, "fpi-data", data, NULL);

    g_clear_pointer (&self->enroll_template, fte3600_template_free);
    self->enroll_stages_passed = 0;
    self->armed = FALSE;
    fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
  }
}

static void
fte3600_enroll_complete (GObject      *source_object,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (source_object);
  GTask *task = G_TASK (result);
  Fte3600EnrollJob *job = g_task_get_task_data (task);
  g_autoptr (GError) error = NULL;

  (void) user_data;

  if (!g_task_propagate_boolean (task, &error))
    {
      fte3600_complete_action_error (self, g_steal_pointer (&error));
      return;
    }

  fte3600_enroll_process (self, job);
}

static void
fte3600_enroll_capture_async (FpiDeviceFte3600 *self)
{
  FpDevice *dev = FP_DEVICE (self);
  Fte3600EnrollJob *job;
  g_autoptr (GTask) task = NULL;

  g_assert (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_ENROLL);
  g_assert (self->captured_image != NULL);

  job = g_new0 (Fte3600EnrollJob, 1);
  memcpy (job->image, self->captured_image->data, sizeof (job->image));
  fte3600_clear_captured_image (self);
  job->enroll_template = fte3600_template_copy (self->enroll_template);
  if (job->enroll_template == NULL)
    {
      fte3600_enroll_job_free (job);
      fte3600_complete_action_error (
          self, fpi_device_error_new_msg (
                    FP_DEVICE_ERROR_DATA_INVALID,
                    "FTE3600 enrollment template state was missing"));
      return;
    }

  task = g_task_new (self, fpi_device_get_cancellable (dev),
                     fte3600_enroll_complete, NULL);
  g_task_set_task_data (task, job, (GDestroyNotify) fte3600_enroll_job_free);
  g_task_set_return_on_cancel (task, FALSE);
  g_task_run_in_thread (task, fte3600_enroll_worker);
}

#if FTE3600_ENABLE_PERSONAL_AUTH
static GError *
fte3600_verify_template_error (Fte3600TemplateStatus status)
{
  switch (status)
    {
    case FTE3600_TEMPLATE_INVALID_WIRE:
    case FTE3600_TEMPLATE_NEED_MORE_SAMPLES:
    case FTE3600_TEMPLATE_RETRY_LOW_CONTRAST:
    case FTE3600_TEMPLATE_RETRY_INSUFFICIENT_FEATURES:
    case FTE3600_TEMPLATE_RETRY_DUPLICATE:
    case FTE3600_TEMPLATE_RETRY_INCONSISTENT:
      return fpi_device_error_new_msg (
          FP_DEVICE_ERROR_DATA_INVALID,
          "FTE3600 verification template or query was invalid (%u)",
          (guint) status);

    case FTE3600_TEMPLATE_UNSUPPORTED_SCHEMA:
    case FTE3600_TEMPLATE_UNSUPPORTED_EXTRACTOR:
    case FTE3600_TEMPLATE_UNSUPPORTED_POLICY:
    case FTE3600_TEMPLATE_NOT_CALIBRATED:
      return fpi_device_error_new_msg (
          FP_DEVICE_ERROR_NOT_SUPPORTED,
          "FTE3600 verification template policy is unsupported (%u)",
          (guint) status);

    case FTE3600_TEMPLATE_OK:
      g_assert_not_reached ();
    }

  g_assert_not_reached ();
}

static GError *
fte3600_verify_load_template (FpiDeviceFte3600 *self)
{
  FpDevice *dev = FP_DEVICE (self);
  FpPrint *print = NULL;
  g_autoptr(GVariant) data = NULL;
  g_autoptr(GBytes) wire = NULL;
  const guint8 *wire_data;
  gsize wire_size = 0;
  Fte3600TemplateStatus status;

  fpi_device_get_verify_data (dev, &print);
  if (print == NULL || !fp_print_compatible (print, dev) ||
      fpi_print_get_type (print) != FPI_PRINT_RAW)
    return fpi_device_error_new_msg (
        FP_DEVICE_ERROR_DATA_INVALID,
        "FTE3600 verification requires a compatible raw template");

  g_object_get (print, "fpi-data", &data, NULL);
  if (data == NULL ||
      !g_variant_is_of_type (data, G_VARIANT_TYPE ("ay")) ||
      !g_variant_is_normal_form (data))
    return fpi_device_error_new_msg (
        FP_DEVICE_ERROR_DATA_INVALID,
        "FTE3600 verification template has an invalid container");

  wire_data = g_variant_get_fixed_array (data, &wire_size,
                                         sizeof (*wire_data));
  if (wire_data == NULL || wire_size < FTE3600_TEMPLATE_WIRE_HEADER_SIZE ||
      wire_size > FTE3600_TEMPLATE_CURRENT_MAX_WIRE_SIZE)
    return fpi_device_error_new_msg (
        FP_DEVICE_ERROR_DATA_INVALID,
        "FTE3600 verification template has an invalid length");

  wire = g_bytes_new (wire_data, wire_size);
  g_clear_pointer (&self->verify_template, fte3600_template_free);
  status = fte3600_template_decode (
      wire, FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &self->verify_template);
  if (status != FTE3600_TEMPLATE_OK)
    return fte3600_verify_template_error (status);
  if (!fte3600_template_is_ready (self->verify_template))
    {
      g_clear_pointer (&self->verify_template, fte3600_template_free);
      return fpi_device_error_new_msg (
          FP_DEVICE_ERROR_DATA_INVALID,
          "FTE3600 verification template was incomplete");
    }

  return NULL;
}

static void
fte3600_verify_job_free (Fte3600VerifyJob *job)
{
  if (job == NULL)
    return;

  g_clear_pointer (&job->verify_template, fte3600_template_free);
  fte3600_secure_clear (job->image, sizeof (job->image));
  fte3600_secure_clear (&job->comparison, sizeof (job->comparison));
  g_free (job);
}

static void
fte3600_verify_worker (GTask        *task,
                       gpointer      source_object,
                       gpointer      task_data,
                       GCancellable *cancellable)
{
  Fte3600VerifyJob *job = task_data;
  Fte3600BriskFeatureSet features = { 0 };

  (void) source_object;
  (void) cancellable;

  if (g_task_return_error_if_cancelled (task))
    goto out;

  job->extract_status =
      fte3600_brisk_extract (job->image, sizeof (job->image), &features);
  fte3600_secure_clear (job->image, sizeof (job->image));
  if (g_task_return_error_if_cancelled (task))
    goto out;

  if (job->extract_status == FTE3600_BRISK_OK)
    job->compare_status = fte3600_template_compare_features (
        job->verify_template, &features,
        FTE3600_TEMPLATE_LOAD_AUTHENTICATION, &job->comparison);

  if (!g_task_return_error_if_cancelled (task))
    g_task_return_boolean (task, TRUE);

out:
  fte3600_secure_clear (&features, sizeof (features));
}

static void
fte3600_verify_report_retry (FpiDeviceFte3600 *self,
                             FpDeviceRetry      retry)
{
  FpDevice *dev = FP_DEVICE (self);

  fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL,
                            fpi_device_retry_new (retry));
  fpi_device_verify_complete (dev, NULL);
}

static void
fte3600_verify_complete (GObject      *source_object,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (source_object);
  FpDevice *dev = FP_DEVICE (self);
  GTask *task = G_TASK (result);
  Fte3600VerifyJob *job = g_task_get_task_data (task);
  g_autoptr(GError) error = NULL;

  (void) user_data;

  if (!g_task_propagate_boolean (task, &error))
    {
      fte3600_complete_action_error (self, g_steal_pointer (&error));
      return;
    }

  switch (job->extract_status)
    {
    case FTE3600_BRISK_LOW_CONTRAST:
    case FTE3600_BRISK_NO_CONSENSUS:
      fte3600_verify_report_retry (self, FP_DEVICE_RETRY_GENERAL);
      return;

    case FTE3600_BRISK_INSUFFICIENT_FEATURES:
      fte3600_verify_report_retry (self, FP_DEVICE_RETRY_CENTER_FINGER);
      return;

    case FTE3600_BRISK_INVALID_ARGUMENT:
      fte3600_complete_action_error (
          self, fpi_device_error_new_msg (
                    FP_DEVICE_ERROR_DATA_INVALID,
                    "FTE3600 extractor rejected a verification image"));
      return;

    case FTE3600_BRISK_OK:
      break;
    }

  if (job->compare_status ==
      FTE3600_TEMPLATE_RETRY_INSUFFICIENT_FEATURES)
    {
      fte3600_verify_report_retry (self, FP_DEVICE_RETRY_CENTER_FINGER);
      return;
    }
  if (job->compare_status != FTE3600_TEMPLATE_OK)
    {
      fte3600_complete_action_error (
          self, fte3600_verify_template_error (job->compare_status));
      return;
    }

  fp_dbg ("Personal verification compared %u subtemplates; strict passes %u",
          job->comparison.n_compared, job->comparison.diagnostic_passes);
  fpi_device_verify_report (
      dev,
      job->comparison.authentication_accepted ? FPI_MATCH_SUCCESS :
                                                FPI_MATCH_FAIL,
      NULL, NULL);
  fpi_device_verify_complete (dev, NULL);
}

static void
fte3600_verify_capture_async (FpiDeviceFte3600 *self)
{
  FpDevice *dev = FP_DEVICE (self);
  Fte3600VerifyJob *job;
  g_autoptr(GTask) task = NULL;

  g_assert (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY);
  g_assert (self->captured_image != NULL);

  job = g_new0 (Fte3600VerifyJob, 1);
  job->extract_status = FTE3600_BRISK_INVALID_ARGUMENT;
  job->compare_status = FTE3600_TEMPLATE_INVALID_WIRE;
  memcpy (job->image, self->captured_image->data, sizeof (job->image));
  fte3600_clear_captured_image (self);
  job->verify_template = g_steal_pointer (&self->verify_template);
  if (job->verify_template == NULL)
    {
      fte3600_verify_job_free (job);
      fte3600_complete_action_error (
          self, fpi_device_error_new_msg (
                    FP_DEVICE_ERROR_DATA_INVALID,
                    "FTE3600 verification template state was missing"));
      return;
    }

  task = g_task_new (self, fpi_device_get_cancellable (dev),
                     fte3600_verify_complete, NULL);
  g_task_set_task_data (task, job, (GDestroyNotify) fte3600_verify_job_free);
  g_task_set_return_on_cancel (task, FALSE);
  g_task_run_in_thread (task, fte3600_verify_worker);
}
#endif

static void
fte3600_complete_action_error (FpiDeviceFte3600 *self, GError *error)
{
  FpDevice *dev = FP_DEVICE (self);

  if (self->armed && self->spi_fd >= 0 && !self->capturing)
    {
      fte3600_start_reset (self, FTE3600_RESET_FOR_ACTION_ERROR, error);
      return;
    }

  self->armed = FALSE;
  fte3600_clear_captured_image (self);
  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);

  switch (fpi_device_get_current_action (dev))
    {
    case FPI_DEVICE_ACTION_ENROLL:
      self->enroll_stages_passed = 0;
      g_clear_pointer (&self->enroll_template, fte3600_template_free);
      fpi_device_enroll_complete (dev, NULL, error);
      return;

    case FPI_DEVICE_ACTION_CAPTURE:
      fpi_device_capture_complete (dev, NULL, error);
      return;

    case FPI_DEVICE_ACTION_VERIFY:
      g_clear_pointer (&self->verify_template, fte3600_template_free);
      fpi_device_verify_complete (dev, error);
      return;

    case FPI_DEVICE_ACTION_NONE:
    case FPI_DEVICE_ACTION_PROBE:
    case FPI_DEVICE_ACTION_OPEN:
    case FPI_DEVICE_ACTION_CLOSE:
    case FPI_DEVICE_ACTION_IDENTIFY:
    case FPI_DEVICE_ACTION_LIST:
    case FPI_DEVICE_ACTION_DELETE:
    case FPI_DEVICE_ACTION_CLEAR_STORAGE:
      fpi_device_action_error (dev, error);
      return;
    }
}

static void
fte3600_capture_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);

  fte3600_clear_irq_source (self);
  self->capturing = FALSE;
  if (error)
    {
      fte3600_secure_clear (self->capture_rx, FT9361_CAPTURE_FRAME_SIZE);
      fte3600_complete_action_error (self, error);
      return;
    }

  g_assert (self->captured_image != NULL);
  switch (fpi_device_get_current_action (dev))
    {
    case FPI_DEVICE_ACTION_ENROLL:
      fte3600_enroll_capture_async (self);
      return;

    case FPI_DEVICE_ACTION_CAPTURE:
      self->armed = FALSE;
      fpi_device_capture_complete (
          dev, g_steal_pointer (&self->captured_image), NULL);
      return;

    case FPI_DEVICE_ACTION_VERIFY:
#if FTE3600_ENABLE_PERSONAL_AUTH
      fte3600_verify_capture_async (self);
      return;
#else
      g_assert_not_reached ();
#endif

    case FPI_DEVICE_ACTION_NONE:
    case FPI_DEVICE_ACTION_PROBE:
    case FPI_DEVICE_ACTION_OPEN:
    case FPI_DEVICE_ACTION_CLOSE:
    case FPI_DEVICE_ACTION_IDENTIFY:
    case FPI_DEVICE_ACTION_LIST:
    case FPI_DEVICE_ACTION_DELETE:
    case FPI_DEVICE_ACTION_CLEAR_STORAGE:
      fte3600_clear_captured_image (self);
      fpi_device_action_error (
          dev, fpi_device_error_new_msg (
                   FP_DEVICE_ERROR_GENERAL,
                   "Unexpected action completed an FTE3600 capture"));
      return;
    }
}

static void
fte3600_reset_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FTE3600_RESET_1:
      self->idle_verified = FALSE;
      G_GNUC_FALLTHROUGH;
    case FTE3600_RESET_2:
      fte3600_submit_command (ssm, 0x70, FALSE);
      return;

    case FTE3600_RESET_DELAY:
      fpi_ssm_next_state_delayed (ssm, FT9361_RESET_DELAY_MS);
      return;

    case FTE3600_RESET_SETTLE:
      fpi_ssm_next_state_delayed (ssm, FT9361_RESET_SETTLE_MS);
      return;

    case FTE3600_RESET_READ_MCU_STATUS:
      fte3600_submit_reg_read (ssm, FT9361_REG_MCU_STATUS, 2, FALSE);
      return;

    case FTE3600_RESET_CHECK_MCU_STATUS:
      if (!fte3600_mcu_is_idle (self))
        {
          fpi_ssm_mark_failed (
              ssm, fpi_device_error_new_msg (
                       FP_DEVICE_ERROR_PROTO,
                       "FT9361 MCU did not return to idle after reset "
                       "(%02x %02x)",
                       self->small_rx[4], self->small_rx[5]));
          return;
        }
      self->idle_verified = TRUE;
      fpi_ssm_mark_completed (ssm);
      return;

    case FTE3600_RESET_NSTATES:
      g_assert_not_reached ();
    }
}

static void
fte3600_reset_data_free (Fte3600ResetData *data)
{
  g_clear_error (&data->operation_error);
  g_free (data);
}

static void
fte3600_reset_complete (FpiSsm *ssm, FpDevice *dev, GError *reset_error)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  Fte3600ResetData *data = fpi_ssm_get_data (ssm);
  GError *operation_error = g_steal_pointer (&data->operation_error);

  switch (data->purpose)
    {
    case FTE3600_RESET_FOR_OPEN_ERROR:
      if (reset_error)
        {
          fp_warn ("Sensor reset after open failure also failed: %s",
                   reset_error->message);
          g_clear_error (&reset_error);
        }
      if (self->spi_fd >= 0)
        {
          if (close (self->spi_fd) < 0)
            fp_warn (
                "Failed to close FTE3600 SPI device after open failure: %s",
                g_strerror (errno));
          self->spi_fd = -1;
        }
      fte3600_release_gpio (self);
      fpi_device_open_complete (dev, operation_error);
      return;

    case FTE3600_RESET_FOR_CLOSE:
      g_clear_error (&operation_error);
      if (self->spi_fd >= 0)
        {
          if (close (self->spi_fd) < 0 && !reset_error)
            g_set_error (
                &reset_error, G_IO_ERROR, g_io_error_from_errno (errno),
                "Failed to close FTE3600 SPI device: %s", g_strerror (errno));
          self->spi_fd = -1;
        }
      fte3600_release_gpio (self);
      fpi_device_close_complete (dev, reset_error);
      return;

    case FTE3600_RESET_FOR_ACTION_ERROR:
      if (reset_error)
        {
          fp_warn ("Sensor reset after action failure also failed: %s",
                   reset_error->message);
          g_clear_error (&reset_error);
        }
      fte3600_complete_action_error (self, operation_error);
      return;
    }

  g_assert_not_reached ();
}

static void
fte3600_start_reset (FpiDeviceFte3600 *self, Fte3600ResetPurpose purpose,
                     GError *operation_error)
{
  Fte3600ResetData *data;
  FpiSsm *ssm;

  self->armed = FALSE;
  fte3600_clear_irq_source (self);

  data = g_new0 (Fte3600ResetData, 1);
  data->purpose = purpose;
  data->operation_error = operation_error;

  /* RESET_DELAY is deliberately the first cleanup state.  An error from the
   * first 0x70 jumps there, and each later cleanup error advances exactly one
   * state while retaining the first error.  Consequently the second 0x70 and
   * the bounded status read are still attempted, with one final callback. */
  ssm = fpi_ssm_new_full (FP_DEVICE (self), fte3600_reset_handler,
                          FTE3600_RESET_NSTATES, FTE3600_RESET_DELAY,
                          "FT9361 safe reset");
  fpi_ssm_set_data (ssm, data, (GDestroyNotify)fte3600_reset_data_free);
  fpi_ssm_start (ssm, fte3600_reset_complete);
}

static gboolean
fte3600_configure_spi (FpiDeviceFte3600 *self, GError **error)
{
  guint8 mode = SPI_MODE_0;
  guint8 bits = 8;
  guint32 speed = FTE3600_SPI_SPEED_HZ;

  if (ioctl (self->spi_fd, SPI_IOC_WR_MODE, &mode) < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Failed to set FTE3600 SPI mode: %s", g_strerror (errno));
      return FALSE;
    }

  if (ioctl (self->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Failed to set FTE3600 SPI word size: %s",
                   g_strerror (errno));
      return FALSE;
    }

  if (ioctl (self->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Failed to set FTE3600 SPI speed: %s", g_strerror (errno));
      return FALSE;
    }

  return TRUE;
}

static void
fte3600_open (FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  const gchar *path;
  GError *error = NULL;
  FpiSsm *ssm;

  path = fpi_device_get_udev_data (dev, FPI_DEVICE_UDEV_SUBTYPE_SPIDEV);
  if (!path)
    {
      fpi_device_open_complete (
          dev, fpi_device_error_new_msg (
                   FP_DEVICE_ERROR_GENERAL,
                   "No spidev node was provided for FTE3600"));
      return;
    }

  /* spidev exposes no GPIO descriptor.  Never claim a host GPIO until the
   * machine is matched to a routing profile verified on that exact DMI model. */
  if (!fte3600_select_gpio_profile (self, &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }

  self->spi_fd = open (path, O_RDWR | O_CLOEXEC);
  if (self->spi_fd < 0)
    {
      g_set_error (&error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Failed to open FTE3600 SPI device %s: %s", path,
                   g_strerror (errno));
      fpi_device_open_complete (dev, error);
      return;
    }

  if (!fte3600_configure_spi (self, &error))
    {
      close (self->spi_fd);
      self->spi_fd = -1;
      fpi_device_open_complete (dev, error);
      return;
    }

  if (!fte3600_request_gpio (self, &error))
    {
      fte3600_release_gpio (self);
      close (self->spi_fd);
      self->spi_fd = -1;
      fpi_device_open_complete (dev, error);
      return;
    }

  self->idle_verified = FALSE;
  self->init_hardware_reset_attempted = FALSE;
  ssm = fpi_ssm_new (dev, fte3600_init_handler, FTE3600_INIT_NSTATES);
  fpi_ssm_start (ssm, fte3600_init_complete);
}

static void
fte3600_close (FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);

  if (self->spi_fd < 0)
    {
      fpi_device_close_complete (dev, NULL);
      return;
    }

  /* Terminal capture cleanup already ran the complete reset sequence and
   * verified a5 5a.  Repeating it immediately on Release can destabilize this
   * firmware, so only reset here when no terminal cleanup established idle. */
  if (self->idle_verified)
    {
      GError *error = NULL;

      if (close (self->spi_fd) < 0)
        g_set_error (&error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "Failed to close FTE3600 SPI device: %s",
                     g_strerror (errno));
      self->spi_fd = -1;
      self->idle_verified = FALSE;
      fte3600_release_gpio (self);
      fpi_device_close_complete (dev, error);
      return;
    }

  fte3600_start_reset (self, FTE3600_RESET_FOR_CLOSE, NULL);
}

static void
fte3600_start_capture (FpiDeviceFte3600 *self)
{
  FpiSsm *ssm;

  if (self->capturing)
    return;

  self->capturing = TRUE;
  self->idle_verified = FALSE;
  self->false_irq_count = 0;
  fpi_device_report_finger_status (FP_DEVICE (self), FP_FINGER_STATUS_NEEDED);
  ssm = fpi_ssm_new_full (FP_DEVICE (self), fte3600_capture_handler,
                          FTE3600_CAPTURE_NSTATES,
                          FTE3600_CAPTURE_CLEANUP_DISPATCH, "FT9361 capture");
  fpi_ssm_start (ssm, fte3600_capture_complete);
}

static void
fte3600_cancel (FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  FpiSsm *ssm = self->irq_wait_ssm;
  g_autoptr (GError) error = NULL;
  GCancellable *cancellable;

  /* Cancellable SPI transfers and BRISK jobs finish through their normal
   * callbacks.  The GPIO wait has no transfer callback, so wake that state
   * explicitly. */
  if (!ssm)
    return;

  fte3600_clear_irq_source (self);
  cancellable = fpi_device_get_cancellable (dev);
  if (!cancellable
      || !g_cancellable_set_error_if_cancelled (cancellable, &error))
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                 "Fingerprint operation was cancelled");
  fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
}

#if FTE3600_ENABLE_PERSONAL_AUTH
static void
fte3600_enroll (FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);

  g_clear_pointer (&self->verify_template, fte3600_template_free);
  self->enroll_stages_passed = 0;
  g_clear_pointer (&self->enroll_template, fte3600_template_free);
  self->enroll_template = fte3600_template_new ();
  fte3600_start_capture (self);
}

static void
fte3600_verify (FpDevice *dev)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (dev);
  GError *error;

  g_clear_pointer (&self->verify_template, fte3600_template_free);
  error = fte3600_verify_load_template (self);
  if (error != NULL)
    {
      fpi_device_verify_complete (dev, error);
      return;
    }

  fte3600_start_capture (self);
}
#endif

static void
fte3600_capture (FpDevice *dev)
{
  gboolean wait_for_finger;

  fpi_device_get_capture_data (dev, &wait_for_finger);
  if (!wait_for_finger)
    {
      fpi_device_capture_complete (
        dev,
        NULL,
        fpi_device_error_new_msg (
          FP_DEVICE_ERROR_NOT_SUPPORTED,
          "FTE3600 only supports finger-triggered image capture"));
      return;
    }
  fte3600_start_capture (FPI_DEVICE_FTE3600 (dev));
}

static void
fpi_device_fte3600_init (FpiDeviceFte3600 *self)
{
  self->spi_fd = -1;
  self->capture_tx = g_malloc0 (FT9361_CAPTURE_FRAME_SIZE);
  self->capture_rx = g_malloc0 (FT9361_CAPTURE_FRAME_SIZE);

  self->capture_tx[0] = 0x04;
  self->capture_tx[1] = 0xfb;
  self->capture_tx[2] = 0x34;
  self->capture_tx[3] = 0x00;
  self->capture_tx[4] = 0x14;
  self->capture_tx[5] = 0x08;
}

static void
fpi_device_fte3600_finalize (GObject *object)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (object);

  if (self->spi_fd >= 0)
    close (self->spi_fd);
  self->spi_fd = -1;
  fte3600_release_gpio (self);
  g_clear_pointer (&self->enroll_template, fte3600_template_free);
  g_clear_pointer (&self->verify_template, fte3600_template_free);
  fte3600_secure_clear (self->capture_tx, FT9361_CAPTURE_FRAME_SIZE);
  fte3600_secure_clear (self->capture_rx, FT9361_CAPTURE_FRAME_SIZE);
  g_clear_pointer (&self->capture_tx, g_free);
  g_clear_pointer (&self->capture_rx, g_free);
  fte3600_clear_captured_image (self);

  G_OBJECT_CLASS (fpi_device_fte3600_parent_class)->finalize (object);
}

static void
fpi_device_fte3600_class_init (FpiDeviceFte3600Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = "fte3600";
  dev_class->full_name = "FocalTech FT9361 Embedded Fingerprint Sensor";
  dev_class->type = FP_DEVICE_TYPE_UDEV;
  dev_class->id_table = fte3600_id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  /* libfprint's generic model treats the entire user-facing action as
   * full-power heating.  FT9361 instead spends that time armed and idle,
   * with no SPI traffic until its GPIO IRQ; only the millisecond-scale image
   * transaction is active.  No sensor-specific thermal limit is exposed, so
   * the generic three-minute cancellation is not a valid safety model here. */
  dev_class->temp_hot_seconds = -1;
  dev_class->open = fte3600_open;
  dev_class->close = fte3600_close;
#if FTE3600_ENABLE_PERSONAL_AUTH
  /* Enrollment and one-template verification are published together only in
   * an explicit personal-auth build.  A policy-zero build remains useful for
   * controlled image capture, but must not advertise enrollment of templates
   * which it can never authenticate.  This is a personal usability policy,
   * not population FAR calibration. */
  dev_class->enroll = fte3600_enroll;
  dev_class->verify = fte3600_verify;
  dev_class->nr_enroll_stages = FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES;
#endif
  /* Identify remains unavailable: gallery ambiguity and population policy
   * have not been designed or calibrated. */
  dev_class->capture = fte3600_capture;
  dev_class->cancel = fte3600_cancel;

  G_OBJECT_CLASS (klass)->finalize = fpi_device_fte3600_finalize;
  fpi_device_class_auto_initialize_features (dev_class);
}
