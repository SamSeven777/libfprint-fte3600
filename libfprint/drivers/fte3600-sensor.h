/*
 * FocalTech sensor identity and protocol metadata
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

/* These are backend names, not values read from a silicon ID register. */
typedef enum {
  FTE3600_SENSOR_UNKNOWN,
  FTE3600_SENSOR_FT9338,
  FTE3600_SENSOR_FT9348,
  FTE3600_SENSOR_FT9361,
  FTE3600_SENSOR_FT9536,
  FTE3600_SENSOR_FT9365,
  FTE3600_SENSOR_FT9368,
  FTE3600_SENSOR_FT9369,
  FTE3600_SENSOR_FT9769,
  FTE3600_SENSOR_COUNT,
} Fte3600Sensor;

/* An operation family is a sharing boundary, not a promise of wire or firmware
 * compatibility between every member. In particular, FT9348 != FT9361. */
typedef enum {
  FTE3600_PROTOCOL_UNKNOWN,
  FTE3600_PROTOCOL_FT9338,
  FTE3600_PROTOCOL_FT95A8,
  FTE3600_PROTOCOL_FT9368,
  FTE3600_PROTOCOL_FT9369,
  FTE3600_PROTOCOL_FT9365,
  FTE3600_PROTOCOL_FT9769,
  FTE3600_PROTOCOL_COUNT,
} Fte3600Protocol;

typedef enum {
  FTE3600_SENSOR_CAP_NONE                   = 0,
  FTE3600_SENSOR_CAP_CAPTURE                = 1 << 0,
  FTE3600_SENSOR_CAP_FIRMWARE_LOAD          = 1 << 1,
} Fte3600SensorCapability;

typedef enum {
  FTE3600_FIRMWARE_APPLICATION,
  FTE3600_FIRMWARE_PRAMBOOT,
} Fte3600FirmwareRole;

typedef struct
{
  Fte3600FirmwareRole role;
  /* Relative to /usr/lib/firmware; a Linux naming convention, not a vendor
   * filename. Records describe audited payloads, not redistributed files. */
  const gchar *filename;
  gsize        size;
  const gchar *sha256;
} Fte3600Firmware;

typedef struct
{
  Fte3600Sensor   sensor;
  const gchar    *name;
  Fte3600Protocol protocol;
  /* Zero means not established. Geometry does not specify a frame format. */
  guint16         width;
  guint16         height;
  /* Image density in pixels/mm; zero leaves the unknown default.
   * Per-profile provenance and limits: docs/fte3600/image-resolution.md.
   * Never infer pixel pitch from a chip's dimensions or a related model. */
  gdouble                 image_ppmm;
  Fte3600SensorCapability capabilities;
  const Fte3600Firmware  *firmware;
  gsize                   firmware_count;
  guint                   restart_wait_ms;
} Fte3600SensorDescriptor;

typedef enum {
  FTE3600_IDENTITY_NONE,
  FTE3600_IDENTITY_RUNTIME_GEOMETRY,
  FTE3600_IDENTITY_ROM_A8_SPI_OTP,
  FTE3600_IDENTITY_ROM_BOOT_A,
  FTE3600_IDENTITY_ROM_BOOT_B38_SPI_OTP,
  FTE3600_IDENTITY_SPECIAL_CHIP_ID,
  FTE3600_IDENTITY_KNOWN_UNMAPPED_ID,
} Fte3600IdentityEvidence;

typedef struct
{
  Fte3600Sensor           sensor;
  Fte3600IdentityEvidence evidence;
  /* Original runtime pair, ROM family, special ID, or boot-A register value.
   * Evidence remains set even when the observation has no known mapping. */
  guint16 response;
  guint8  otp;
} Fte3600Identity;

/* Returns NULL for UNKNOWN or an out-of-range enum. Storage is immutable. */
const Fte3600SensorDescriptor *fpi_fte3600_sensor_get (Fte3600Sensor sensor);

/* Classifiers perform no I/O. Callers must obtain responses through the named
 * protocol. They must not try different interpretations of the same bytes. */
Fte3600Identity fpi_fte3600_identify_runtime (guint8 high,
                                              guint8 low);
Fte3600Identity fpi_fte3600_identify_a8_spi (guint16 family,
                                             guint8  otp);
Fte3600Identity fpi_fte3600_identify_special (guint16 chip_id);

/* Boot-A requires the positive ef boot marker and the documented register
 * preparation before reading fe. Only fe == 2 has a positive mapping; the
 * vendor's unconditional FT9338 fallback is intentionally not reproduced. */
Fte3600Identity fpi_fte3600_identify_boot_a (guint8 reg_fe);

/* Requires independently established boot-B38 context. An unrecognized A8
 * family is not such evidence. OTP ff requests a boot-A check, so it remains
 * UNKNOWN here instead of being misreported as a confirmed FT9338. */
Fte3600Identity fpi_fte3600_identify_boot_b38_spi (guint8 otp);

/* Capability plus positive, revalidated ROM evidence. A runtime geometry
 * match alone never authorizes an upload, even for an implemented backend. */
gboolean fpi_fte3600_identity_allows_firmware (const Fte3600Identity *identity);

/* Compatibility helpers for existing users of the pure classifiers. */
Fte3600Sensor fpi_fte3600_runtime_sensor (guint8 high,
                                          guint8 low);
Fte3600Sensor fpi_fte3600_boot_sensor (guint16 family,
                                       guint8  otp);
