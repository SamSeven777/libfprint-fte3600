/*
 * Sensor catalog and identity-policy tests
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>
#include <string.h>

#include "drivers/fte3600-sensor.h"

static void
test_catalog_consistency (void)
{
  g_autoptr(GHashTable) names = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr(GHashTable) filenames = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr(GHashTable) digests = g_hash_table_new (g_str_hash, g_str_equal);
  gboolean protocols[FTE3600_PROTOCOL_COUNT] = { FALSE };
  guint protocol_count = 0;
  guint capture_count = 0;
  guint geometry_count = 0;
  guint firmware_count = 0;

  g_assert_null (fpi_fte3600_sensor_get (FTE3600_SENSOR_UNKNOWN));
  g_assert_null (fpi_fte3600_sensor_get (FTE3600_SENSOR_COUNT));
  g_assert_null (fpi_fte3600_sensor_get ((Fte3600Sensor) - 1));
  g_assert_null (fpi_fte3600_sensor_get ((Fte3600Sensor) G_MAXINT));

  for (Fte3600Sensor sensor = FTE3600_SENSOR_UNKNOWN + 1;
       sensor < FTE3600_SENSOR_COUNT; sensor++)
    {
      const Fte3600SensorDescriptor *descriptor = fpi_fte3600_sensor_get (sensor);

      g_assert_nonnull (descriptor);
      g_assert_cmpint (descriptor->sensor, ==, sensor);
      g_assert_true (g_hash_table_add (names, (gpointer) descriptor->name));
      g_assert_cmpint (descriptor->protocol, >, FTE3600_PROTOCOL_UNKNOWN);
      g_assert_cmpint (descriptor->protocol, <, FTE3600_PROTOCOL_COUNT);
      protocols[descriptor->protocol] = TRUE;
      g_assert_cmpint (descriptor->width == 0, ==, descriptor->height == 0);
      geometry_count += descriptor->width != 0;
      g_assert_cmpint (descriptor->firmware == NULL, ==, descriptor->firmware_count == 0);

      if (descriptor->capabilities & FTE3600_SENSOR_CAP_CAPTURE)
        {
          capture_count++;
          g_assert_cmpuint (descriptor->width, >, 0);
          g_assert_cmpuint (descriptor->height, >, 0);
        }

      if (descriptor->capabilities & FTE3600_SENSOR_CAP_FIRMWARE_LOAD)
        {
          g_assert_true (descriptor->protocol == FTE3600_PROTOCOL_FT95A8 ||
                         descriptor->protocol == FTE3600_PROTOCOL_FT9338);
          g_assert_cmpuint (descriptor->firmware_count, ==, 1);
          g_assert_cmpint (descriptor->firmware[0].role, ==, FTE3600_FIRMWARE_APPLICATION);
        }

      for (gsize i = 0; i < descriptor->firmware_count; i++)
        {
          const Fte3600Firmware *firmware = &descriptor->firmware[i];

          firmware_count++;
          g_assert_cmpuint (firmware->size, >, 0);
          g_assert_true (g_str_has_prefix (firmware->filename, "fte3600/"));
          g_assert_true (g_str_has_suffix (firmware->filename, ".bin"));
          g_assert_null (strstr (firmware->filename, ".."));
          g_assert_false (g_path_is_absolute (firmware->filename));
          g_assert_cmpuint (strlen (firmware->sha256), ==, 64);
          for (gsize digit = 0; digit < 64; digit++)
            g_assert_true (g_ascii_isxdigit (firmware->sha256[digit]));
          g_assert_true (g_hash_table_add (filenames, (gpointer) firmware->filename));
          g_assert_true (g_hash_table_add (digests, (gpointer) firmware->sha256));
        }
    }

  for (gsize i = 0; i < G_N_ELEMENTS (protocols); i++)
    protocol_count += protocols[i];

  g_assert_cmpuint (g_hash_table_size (names), ==, 8);
  g_assert_cmpuint (protocol_count, ==, 6);
  g_assert_cmpuint (geometry_count, ==, 8);
  g_assert_cmpuint (capture_count, ==, 8);
  g_assert_cmpuint (firmware_count, ==, 6);
}

static void
test_shared_protocol_separate_firmware (void)
{
  const Fte3600SensorDescriptor *ft9338 = fpi_fte3600_sensor_get (FTE3600_SENSOR_FT9338);
  const Fte3600SensorDescriptor *ft9348 = fpi_fte3600_sensor_get (FTE3600_SENSOR_FT9348);
  const Fte3600SensorDescriptor *ft9361 = fpi_fte3600_sensor_get (FTE3600_SENSOR_FT9361);
  const Fte3600SensorDescriptor *ft9536 = fpi_fte3600_sensor_get (FTE3600_SENSOR_FT9536);
  const Fte3600SensorDescriptor *ft9368 = fpi_fte3600_sensor_get (FTE3600_SENSOR_FT9368);
  const Fte3600SensorDescriptor *ft9365 = fpi_fte3600_sensor_get (FTE3600_SENSOR_FT9365);
  const Fte3600SensorDescriptor *ft9769 = fpi_fte3600_sensor_get (FTE3600_SENSOR_FT9769);

  g_assert_cmpint (ft9338->protocol, ==, ft9536->protocol);
  g_assert_cmpstr (ft9338->firmware[0].sha256, !=, ft9536->firmware[0].sha256);
  g_assert_cmpint (ft9348->protocol, ==, ft9361->protocol);
  g_assert_cmpstr (ft9348->firmware[0].sha256, !=, ft9361->firmware[0].sha256);
  g_assert_cmpuint (ft9348->firmware[0].size, ==, 10312);
  g_assert_cmpuint (ft9361->firmware[0].size, ==, 10396);
  g_assert_cmpuint (ft9348->width * ft9348->height, ==, 9216);
  g_assert_cmpuint (ft9361->width * ft9361->height, ==, 5120);
  g_assert_cmpuint (ft9536->width, ==, 64);
  g_assert_cmpuint (ft9536->height, ==, 128);
  g_assert_cmpuint (ft9368->firmware_count, ==, 2);
  g_assert_cmpint (ft9368->firmware[0].role, ==, FTE3600_FIRMWARE_APPLICATION);
  g_assert_cmpint (ft9368->firmware[1].role, ==, FTE3600_FIRMWARE_PRAMBOOT);
  g_assert_cmpint (ft9365->protocol, !=, ft9769->protocol);
}

static void
test_runtime_exhaustive (void)
{
  guint recognized = 0;

  for (guint pair = 0; pair <= G_MAXUINT16; pair++)
    {
      Fte3600Identity identity = fpi_fte3600_identify_runtime (pair >> 8, pair & 0xff);

      g_assert_cmpint (identity.evidence, ==, FTE3600_IDENTITY_RUNTIME_GEOMETRY);
      g_assert_cmpuint (identity.response, ==, pair);
      g_assert_cmpuint (identity.otp, ==, 0);
      g_assert_cmpint (fpi_fte3600_runtime_sensor (pair >> 8, pair & 0xff), ==,
                       identity.sensor);
      g_assert_false (fpi_fte3600_identity_allows_firmware (&identity));

      if (identity.sensor != FTE3600_SENSOR_UNKNOWN)
        {
          const Fte3600SensorDescriptor *descriptor = fpi_fte3600_sensor_get (identity.sensor);

          recognized++;
          g_assert_nonnull (descriptor);
          g_assert_cmpuint (descriptor->width, ==, pair >> 8);
          g_assert_cmpuint (descriptor->height, ==, pair & 0xff);
        }
    }

  g_assert_cmpuint (recognized, ==, 4);
  g_assert_cmpint (fpi_fte3600_identify_runtime (0x40, 0x50).sensor, ==, FTE3600_SENSOR_FT9361);
  g_assert_cmpint (fpi_fte3600_identify_runtime (0x60, 0x60).sensor, ==, FTE3600_SENSOR_FT9348);
  g_assert_cmpint (fpi_fte3600_identify_runtime (0x58, 0x58).sensor, ==, FTE3600_SENSOR_FT9338);
  g_assert_cmpint (fpi_fte3600_identify_runtime (0x40, 0x80).sensor, ==, FTE3600_SENSOR_FT9536);
}

static void
test_a8_spi_otp_exhaustive (void)
{
  const guint16 families[] = { 0x2b50, 0x95a8, 0x23dd };

  for (gsize i = 0; i < G_N_ELEMENTS (families); i++)
    {
      guint ft9348_count = 0;
      guint ft9361_count = 0;

      for (guint otp = 0; otp <= G_MAXUINT8; otp++)
        {
          Fte3600Identity identity = fpi_fte3600_identify_a8_spi (families[i], otp);

          g_assert_cmpint (identity.evidence, ==, FTE3600_IDENTITY_ROM_A8_SPI_OTP);
          g_assert_cmpuint (identity.response, ==, families[i]);
          g_assert_cmpuint (identity.otp, ==, otp);
          g_assert_cmpint (identity.sensor, ==, fpi_fte3600_boot_sensor (families[i], otp));

          if (identity.sensor == FTE3600_SENSOR_FT9348)
            ft9348_count++;
          else if (identity.sensor == FTE3600_SENSOR_FT9361)
            ft9361_count++;
          else
            g_assert_cmpint (identity.sensor, ==, FTE3600_SENSOR_UNKNOWN);

          g_assert_cmpint (fpi_fte3600_identity_allows_firmware (&identity), ==,
                           identity.sensor != FTE3600_SENSOR_UNKNOWN);
        }

      g_assert_cmpuint (ft9348_count, ==, 48);
      g_assert_cmpuint (ft9361_count, ==, 48);
      /* SPI uses the low nibble directly; a USB adjustment here would change
      * the mapping of these values or lose their positive identification. */
      g_assert_cmpint (fpi_fte3600_identify_a8_spi (families[i], 0x01).sensor, ==,
                       FTE3600_SENSOR_FT9348);
      g_assert_cmpint (fpi_fte3600_identify_a8_spi (families[i], 0x04).sensor, ==,
                       FTE3600_SENSOR_FT9361);
      g_assert_cmpint (fpi_fte3600_identify_a8_spi (families[i], 0xff).sensor, ==,
                       FTE3600_SENSOR_FT9361);
    }
}

static void
test_a8_family_exhaustive (void)
{
  guint recognized = 0;

  for (guint family = 0; family <= G_MAXUINT16; family++)
    {
      Fte3600Identity identity = fpi_fte3600_identify_a8_spi (family, 4);

      if (identity.sensor != FTE3600_SENSOR_UNKNOWN)
        {
          recognized++;
          g_assert_cmpint (identity.sensor, ==, FTE3600_SENSOR_FT9361);
        }
      else
        {
          g_assert_false (fpi_fte3600_identity_allows_firmware (&identity));
        }
    }

  g_assert_cmpuint (recognized, ==, 3);
}

static void
test_special_id_exhaustive (void)
{
  guint recognized = 0;
  guint unmapped = 0;

  for (guint chip_id = 0; chip_id <= G_MAXUINT16; chip_id++)
    {
      Fte3600Identity identity = fpi_fte3600_identify_special (chip_id);

      g_assert_cmpuint (identity.response, ==, chip_id);
      g_assert_cmpuint (identity.otp, ==, 0);
      g_assert_false (fpi_fte3600_identity_allows_firmware (&identity));
      if (identity.evidence == FTE3600_IDENTITY_KNOWN_UNMAPPED_ID)
        {
          unmapped++;
          g_assert_cmpint (identity.sensor, ==, FTE3600_SENSOR_UNKNOWN);
        }
      else
        {
          g_assert_cmpint (identity.evidence, ==, FTE3600_IDENTITY_SPECIAL_CHIP_ID);
        }

      recognized += identity.sensor != FTE3600_SENSOR_UNKNOWN;
    }

  g_assert_cmpuint (recognized, ==, 5);
  g_assert_cmpuint (unmapped, ==, 7);
  g_assert_cmpint (fpi_fte3600_identify_special (0x9362).sensor, ==, FTE3600_SENSOR_FT9369);
  g_assert_cmpint (fpi_fte3600_identify_special (0x9365).sensor, ==, FTE3600_SENSOR_FT9365);
  g_assert_cmpint (fpi_fte3600_identify_special (0x9368).sensor, ==, FTE3600_SENSOR_FT9368);
  g_assert_cmpint (fpi_fte3600_identify_special (0x9391).sensor, ==, FTE3600_SENSOR_FT9769);
  g_assert_cmpint (fpi_fte3600_identify_special (0x9392).sensor, ==, FTE3600_SENSOR_FT9769);
  /* A backend's name is never a substitute for its observed response. */
  g_assert_cmpint (fpi_fte3600_identify_special (0x9369).sensor, ==, FTE3600_SENSOR_UNKNOWN);
  g_assert_cmpint (fpi_fte3600_identify_special (0x9769).sensor, ==, FTE3600_SENSOR_UNKNOWN);
  g_assert_cmpint (fpi_fte3600_identify_special (0x9361).sensor, ==, FTE3600_SENSOR_UNKNOWN);
  g_assert_cmpint (fpi_fte3600_identify_special (0x4050).sensor, ==, FTE3600_SENSOR_UNKNOWN);
}

static void
test_boot_protocol_boundaries (void)
{
  guint boot_a_count = 0;
  guint boot_b_count = 0;

  for (guint value = 0; value <= G_MAXUINT8; value++)
    {
      Fte3600Identity a = fpi_fte3600_identify_boot_a (value);
      Fte3600Identity b = fpi_fte3600_identify_boot_b38_spi (value);

      g_assert_cmpint (a.evidence, ==, FTE3600_IDENTITY_ROM_BOOT_A);
      g_assert_cmpuint (a.response, ==, value);
      g_assert_cmpint (b.evidence, ==, FTE3600_IDENTITY_ROM_BOOT_B38_SPI_OTP);
      g_assert_cmpuint (b.otp, ==, value);
      g_assert_cmpint (fpi_fte3600_identity_allows_firmware (&a), ==, value == 2);
      g_assert_false (fpi_fte3600_identity_allows_firmware (&b));
      boot_a_count += a.sensor != FTE3600_SENSOR_UNKNOWN;
      boot_b_count += b.sensor != FTE3600_SENSOR_UNKNOWN;
    }

  g_assert_cmpuint (boot_a_count, ==, 1);
  g_assert_cmpuint (boot_b_count, ==, 32);
  g_assert_cmpint (fpi_fte3600_identify_boot_a (2).sensor, ==, FTE3600_SENSOR_FT9536);
  g_assert_cmpint (fpi_fte3600_identify_boot_a (0).sensor, ==, FTE3600_SENSOR_UNKNOWN);
  g_assert_cmpint (fpi_fte3600_identify_boot_a (0xff).sensor, ==, FTE3600_SENSOR_UNKNOWN);
  g_assert_cmpint (fpi_fte3600_identify_boot_b38_spi (0x10).sensor, ==, FTE3600_SENSOR_FT9338);
  g_assert_cmpint (fpi_fte3600_identify_boot_b38_spi (0x20).sensor, ==, FTE3600_SENSOR_FT9536);
  g_assert_cmpint (fpi_fte3600_identify_boot_b38_spi (0xff).sensor, ==, FTE3600_SENSOR_UNKNOWN);
}

static void
test_firmware_evidence_cannot_be_substituted (void)
{
  Fte3600Identity identity = { 0 };

  g_assert_false (fpi_fte3600_identity_allows_firmware (NULL));
  g_assert_false (fpi_fte3600_identity_allows_firmware (&identity));
  identity = fpi_fte3600_identify_a8_spi (0x95a8, 4);
  g_assert_true (fpi_fte3600_identity_allows_firmware (&identity));

  /* Changing only the selected backend cannot make a sibling payload valid. */
  identity.sensor = FTE3600_SENSOR_FT9348;
  g_assert_false (fpi_fte3600_identity_allows_firmware (&identity));
  identity = fpi_fte3600_identify_a8_spi (0x95a8, 1);
  identity.sensor = FTE3600_SENSOR_FT9361;
  g_assert_false (fpi_fte3600_identity_allows_firmware (&identity));

  identity = fpi_fte3600_identify_a8_spi (0x95a8, 4);
  identity.response = 0xffff;
  g_assert_false (fpi_fte3600_identity_allows_firmware (&identity));
  identity = fpi_fte3600_identify_a8_spi (0x95a8, 4);
  identity.otp = 0;
  g_assert_false (fpi_fte3600_identity_allows_firmware (&identity));
  identity = fpi_fte3600_identify_runtime (0x40, 0x50);
  identity.evidence = FTE3600_IDENTITY_ROM_A8_SPI_OTP;
  identity.otp = 4;
  g_assert_false (fpi_fte3600_identity_allows_firmware (&identity));

  /* The same response bytes mean different things in the named protocols. */
  identity = fpi_fte3600_identify_a8_spi (0x95a8, 4);
  identity.evidence = FTE3600_IDENTITY_SPECIAL_CHIP_ID;
  g_assert_false (fpi_fte3600_identity_allows_firmware (&identity));
  identity.evidence = (Fte3600IdentityEvidence) G_MAXINT;
  g_assert_false (fpi_fte3600_identity_allows_firmware (&identity));

  identity = fpi_fte3600_identify_boot_a (2);
  g_assert_true (fpi_fte3600_identity_allows_firmware (&identity));
  identity.response = 0x102;
  g_assert_false (fpi_fte3600_identity_allows_firmware (&identity));
  identity = fpi_fte3600_identify_boot_b38_spi (0x10);
  g_assert_false (fpi_fte3600_identity_allows_firmware (&identity));
  identity.response = 0x5858; /* Positive runtime context followed by matching ROM OTP. */
  g_assert_true (fpi_fte3600_identity_allows_firmware (&identity));
  identity.response = 0x4080;
  g_assert_false (fpi_fte3600_identity_allows_firmware (&identity));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/fte3600/sensor/catalog", test_catalog_consistency);
  g_test_add_func ("/fte3600/sensor/shared-protocol", test_shared_protocol_separate_firmware);
  g_test_add_func ("/fte3600/sensor/runtime-exhaustive", test_runtime_exhaustive);
  g_test_add_func ("/fte3600/sensor/a8-spi-otp-exhaustive", test_a8_spi_otp_exhaustive);
  g_test_add_func ("/fte3600/sensor/a8-family-exhaustive", test_a8_family_exhaustive);
  g_test_add_func ("/fte3600/sensor/special-id-exhaustive", test_special_id_exhaustive);
  g_test_add_func ("/fte3600/sensor/boot-boundaries", test_boot_protocol_boundaries);
  g_test_add_func ("/fte3600/sensor/firmware-evidence", test_firmware_evidence_cannot_be_substituted);
  return g_test_run ();
}
