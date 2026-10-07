/*
 * FocalTech sensor identity and protocol metadata
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Independently expressed technical facts and provenance:
 * docs/fte3600/windows-hardware-inventory.{md,json}
 * docs/fte3600/windows-runtime-adaptation.md
 * docs/fte3600/image-resolution.md
 *
 * A catalog entry establishes neither a working Linux backend nor permission
 * to use another sensor's firmware. No vendor code or payload is included.
 */

#include "fte3600-sensor.h"

static const Fte3600Firmware firmware_ft9338[] = {
  {
    FTE3600_FIRMWARE_APPLICATION, "fte3600/ft9338.bin", 14184,
    "ca4490163a1754639e945da3bd6ecbb4a498138962d611fc825dc129819efc46",
  },
};

static const Fte3600Firmware firmware_ft9348[] = {
  {
    FTE3600_FIRMWARE_APPLICATION, "fte3600/ft9348.bin", 10312,
    "48d658d588c297a5d749c1f4bd6a0f5bd3d6ede59040e1674f95fe9db08eede2",
  },
};

static const Fte3600Firmware firmware_ft9361[] = {
  {
    FTE3600_FIRMWARE_APPLICATION, "fte3600/ft9361.bin", 10396,
    "027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f",
  },
};

static const Fte3600Firmware firmware_ft9368[] = {
  {
    FTE3600_FIRMWARE_APPLICATION, "fte3600/ft9368-app.bin", 27120,
    "9997aafac8eb9a1aecc7ba5b41212004fe1b325e7bdd1ea9ca08ae54b803e2e0",
  },
  {
    FTE3600_FIRMWARE_PRAMBOOT, "fte3600/ft9368-pramboot.bin", 6096,
    "c93a807eaaa34d9e79fbab77b86e910a419fd633ea98b628b72c64b285ffd191",
  },
};

static const Fte3600Firmware firmware_ft9536[] = {
  {
    FTE3600_FIRMWARE_APPLICATION, "fte3600/ft9536.bin", 11934,
    "4a62b5d9a8a7b4620bec7843763b030632a5a37a54c4cb7ed2cd96d5c7451e79",
  },
};

static const Fte3600SensorDescriptor sensors[] = {
  {
    .sensor = FTE3600_SENSOR_FT9338,
    .name = "FT9338",
    .protocol = FTE3600_PROTOCOL_FT9338,
    .width = 88, .height = 88,
    .image_ppmm = 20.0,
    .capabilities = FTE3600_SENSOR_CAP_CAPTURE | FTE3600_SENSOR_CAP_FIRMWARE_LOAD,
    .firmware = firmware_ft9338,
    .firmware_count = G_N_ELEMENTS (firmware_ft9338),
    .restart_wait_ms = 80,
  },
  {
    .sensor = FTE3600_SENSOR_FT9348,
    .name = "FT9348",
    .protocol = FTE3600_PROTOCOL_FT95A8,
    .width = 96, .height = 96,
    .image_ppmm = 20.0,
    .capabilities = FTE3600_SENSOR_CAP_CAPTURE | FTE3600_SENSOR_CAP_FIRMWARE_LOAD,
    .firmware = firmware_ft9348,
    .firmware_count = G_N_ELEMENTS (firmware_ft9348),
    .restart_wait_ms = 80,
  },
  {
    .sensor = FTE3600_SENSOR_FT9361,
    .name = "FT9361",
    .protocol = FTE3600_PROTOCOL_FT95A8,
    .width = 64, .height = 80,
    .image_ppmm = 20.0, /* Retained A1 profile value; see image-resolution.md. */
    .capabilities = FTE3600_SENSOR_CAP_CAPTURE | FTE3600_SENSOR_CAP_FIRMWARE_LOAD,
    .firmware = firmware_ft9361,
    .firmware_count = G_N_ELEMENTS (firmware_ft9361),
    .restart_wait_ms = 80,
  },
  {
    .sensor = FTE3600_SENSOR_FT9536,
    .name = "FT9536",
    .protocol = FTE3600_PROTOCOL_FT9338,
    .width = 64, .height = 128,
    .image_ppmm = 20.0,
    .capabilities = FTE3600_SENSOR_CAP_CAPTURE | FTE3600_SENSOR_CAP_FIRMWARE_LOAD,
    .firmware = firmware_ft9536,
    .firmware_count = G_N_ELEMENTS (firmware_ft9536),
    .restart_wait_ms = 180,
  },
  {
    .sensor = FTE3600_SENSOR_FT9365,
    .name = "FT9365",
    .protocol = FTE3600_PROTOCOL_FT9365,
    .width = 64, .height = 80,
    .image_ppmm = 552.0 / 25.4, /* Manufacturer's Simplified Chinese nominal DPI. */
    .capabilities = FTE3600_SENSOR_CAP_CAPTURE,
    .restart_wait_ms = 0,
  },
  {
    .sensor = FTE3600_SENSOR_FT9368,
    .name = "FT9368",
    .protocol = FTE3600_PROTOCOL_FT9368,
    .width = 64, .height = 80,
    .image_ppmm = 0.0, /* Physical pixel pitch has not been established. */
    .capabilities = FTE3600_SENSOR_CAP_CAPTURE,
    .firmware = firmware_ft9368,
    .firmware_count = G_N_ELEMENTS (firmware_ft9368),
    .restart_wait_ms = 80,
  },
  {
    .sensor = FTE3600_SENSOR_FT9369,
    .name = "FT9369",
    .protocol = FTE3600_PROTOCOL_FT9369,
    .width = 64, .height = 80,
    .image_ppmm = 0.0, /* Physical pixel pitch has not been established. */
    .capabilities = FTE3600_SENSOR_CAP_CAPTURE,
    .restart_wait_ms = 80,
  },
  {
    .sensor = FTE3600_SENSOR_FT9769,
    .name = "FT9769",
    .protocol = FTE3600_PROTOCOL_FT9769,
    .width = 40, .height = 196,
    .image_ppmm = 564.0 / 25.4, /* Manufacturer's Simplified Chinese nominal DPI. */
    .capabilities = FTE3600_SENSOR_CAP_CAPTURE,
    .restart_wait_ms = 0,
  },
};

G_STATIC_ASSERT (G_N_ELEMENTS (sensors) == FTE3600_SENSOR_COUNT - 1);

const Fte3600SensorDescriptor *
fpi_fte3600_sensor_get (Fte3600Sensor sensor)
{
  for (gsize i = 0; i < G_N_ELEMENTS (sensors); i++)
    if (sensors[i].sensor == sensor)
      return &sensors[i];

  return NULL;
}

Fte3600Identity
fpi_fte3600_identify_runtime (guint8 high, guint8 low)
{
  Fte3600Identity identity = {
    .evidence = FTE3600_IDENTITY_RUNTIME_GEOMETRY,
    .response = ((guint16) high << 8) | low,
  };

  switch (identity.response)
    {
    case 0x5858: identity.sensor = FTE3600_SENSOR_FT9338;
      break;

    case 0x6060: identity.sensor = FTE3600_SENSOR_FT9348;
      break;

    case 0x4050: identity.sensor = FTE3600_SENSOR_FT9361;
      break;

    case 0x4080: identity.sensor = FTE3600_SENSOR_FT9536;
      break;
    }

  return identity;
}

Fte3600Identity
fpi_fte3600_identify_a8_spi (guint16 family, guint8 otp)
{
  Fte3600Identity identity = {
    .evidence = FTE3600_IDENTITY_ROM_A8_SPI_OTP,
    .response = family,
    .otp = otp,
  };

  if (family != 0x2b50 && family != 0x95a8 && family != 0x23dd)
    return identity;

  switch (otp & 0x0f)
    {
    case 1:
    case 2:
    case 3:
      identity.sensor = FTE3600_SENSOR_FT9348;
      break;

    case 4:
    case 14:
    case 15:
      identity.sensor = FTE3600_SENSOR_FT9361;
      break;
    }

  return identity;
}

Fte3600Identity
fpi_fte3600_identify_special (guint16 chip_id)
{
  Fte3600Identity identity = {
    .evidence = FTE3600_IDENTITY_SPECIAL_CHIP_ID,
    .response = chip_id,
  };

  switch (chip_id)
    {
    case 0x9362: identity.sensor = FTE3600_SENSOR_FT9369;
      break;

    case 0x9365: identity.sensor = FTE3600_SENSOR_FT9365;
      break;

    case 0x9368: identity.sensor = FTE3600_SENSOR_FT9368;
      break;

    case 0x9391:
    case 0x9392:
      identity.sensor = FTE3600_SENSOR_FT9769;
      break;

    case 0x9349:
    case 0x9363:
    case 0x9372:
    case 0x9395:
    case 0x9396:
    case 0x9397:
    case 0x9398:
      identity.evidence = FTE3600_IDENTITY_KNOWN_UNMAPPED_ID;
      break;
    }

  return identity;
}

Fte3600Identity
fpi_fte3600_identify_boot_a (guint8 reg_fe)
{
  Fte3600Identity identity = {
    .evidence = FTE3600_IDENTITY_ROM_BOOT_A,
    .response = reg_fe,
  };

  if (reg_fe == 2)
    identity.sensor = FTE3600_SENSOR_FT9536;

  return identity;
}

Fte3600Identity
fpi_fte3600_identify_boot_b38_spi (guint8 otp)
{
  Fte3600Identity identity = {
    .evidence = FTE3600_IDENTITY_ROM_BOOT_B38_SPI_OTP,
    .otp = otp,
  };

  switch (otp >> 4)
    {
    case 1: identity.sensor = FTE3600_SENSOR_FT9338;
      break;

    case 2: identity.sensor = FTE3600_SENSOR_FT9536;
      break;
    }

  return identity;
}

gboolean
fpi_fte3600_identity_allows_firmware (const Fte3600Identity *identity)
{
  const Fte3600SensorDescriptor *descriptor;
  Fte3600Identity validated;

  if (!identity)
    return FALSE;

  descriptor = fpi_fte3600_sensor_get (identity->sensor);
  if (!descriptor || !(descriptor->capabilities & FTE3600_SENSOR_CAP_FIRMWARE_LOAD))
    return FALSE;

  if (identity->evidence == FTE3600_IDENTITY_ROM_A8_SPI_OTP)
    {
      validated = fpi_fte3600_identify_a8_spi (identity->response, identity->otp);
    }
  else if (identity->evidence == FTE3600_IDENTITY_ROM_BOOT_A)
    {
      if (identity->response != 2 || identity->otp != 0)
        return FALSE;
      validated = fpi_fte3600_identify_boot_a (identity->response);
    }
  else if (identity->evidence == FTE3600_IDENTITY_ROM_BOOT_B38_SPI_OTP)
    {
      Fte3600Identity runtime = fpi_fte3600_identify_runtime (identity->response >> 8, identity->response);
      validated = fpi_fte3600_identify_boot_b38_spi (identity->otp);
      if (runtime.sensor != identity->sensor)
        return FALSE;
    }
  else
    {
      return FALSE;
    }
  return validated.sensor != FTE3600_SENSOR_UNKNOWN && validated.sensor == identity->sensor;
}

Fte3600Sensor
fpi_fte3600_runtime_sensor (guint8 high, guint8 low)
{
  return fpi_fte3600_identify_runtime (high, low).sensor;
}

Fte3600Sensor
fpi_fte3600_boot_sensor (guint16 family, guint8 otp)
{
  return fpi_fte3600_identify_a8_spi (family, otp).sensor;
}
