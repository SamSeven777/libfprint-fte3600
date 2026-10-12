/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_legacy.h"
#include <string.h>

struct fte3600_legacy_geometry
fte3600_legacy_geometry (enum fte3600_sensor sensor)
{
  struct fte3600_legacy_geometry geometry = {0, 0, 0, 0, 0};

  switch (sensor)
    {
    case FTE3600_FT9338: geometry.width = 88;
      geometry.height = 88;
      break;

    case FTE3600_FT9348: geometry.width = 96;
      geometry.height = 96;
      break;

    case FTE3600_FT9361: geometry.width = 64;
      geometry.height = 80;
      break;

    case FTE3600_FT9536: geometry.width = 64;
      geometry.height = 128;
      break;

    case FTE3600_SENSOR_UNKNOWN:
    default: return geometry;
    }
  geometry.image_bytes = geometry.width * geometry.height;
  geometry.transfer_bytes = geometry.image_bytes + 8;
  geometry.rearm_mode = sensor == FTE3600_FT9338 ? 2 : 1;
  return geometry;
}

static int
valid (enum fte3600_sensor sensor, const struct fte3600_recovery_io *io)
{
  return io != NULL && io->spi != NULL && io->wait_ms != NULL &&
         fte3600_legacy_geometry (sensor).image_bytes != 0;
}

static uint8_t
mode_register (enum fte3600_sensor sensor)
{
  return sensor == FTE3600_FT9536 ? 0x47 : 0x76;
}

static int
write_byte (const struct fte3600_recovery_io *io, uint8_t reg, uint8_t value)
{
  const uint8_t tx[5] = {0x11, 0xee, reg, value, 0};
  uint8_t rx[5] = {0};

  return io->spi (io->context, tx, rx, sizeof tx);
}

static int
read_byte (const struct fte3600_recovery_io *io, uint8_t reg, uint8_t *value)
{
  const uint8_t tx[5] = {0x10, 0xef, reg, 0, 0};
  uint8_t rx[5] = {0};
  int error = io->spi (io->context, tx, rx, sizeof tx);

  if (error == 0)
    *value = rx[4];
  return error;
}

static int
mcu_ready (const struct fte3600_recovery_io *io, int *ready)
{
  const uint8_t tx[6] = {0x10, 0xef, 0x20, 0, 0, 0};
  uint8_t rx[6] = {0};
  int error = io->spi (io->context, tx, rx, sizeof tx);

  if (error == 0)
    *ready = rx[4] == 0xa5 && rx[5] == 0x5a;
  return error;
}

enum fte3600_legacy_status
fte3600_legacy_config (enum fte3600_sensor sensor, const struct fte3600_recovery_io *io)
{
  static const uint8_t registers[] = {0x01, 0x41, 0x30, 0x22, 0x23};
  static const uint8_t values[] = {0x01, 0x0f, 0xbb, 0x00, 0x0e};
  uint8_t readback = 0;
  size_t i, count;
  int extended;
  uint32_t delay;

  if (!valid (sensor, io))
    return FTE3600_LEGACY_INVALID_INPUT;
  extended = sensor == FTE3600_FT9348 || sensor == FTE3600_FT9361;
  delay = extended ? 2 : 1;
  count = extended ? 5 : 3;
  if (extended)
    {
      if (read_byte (io, 0x30, &readback))
        return FTE3600_LEGACY_IO_ERROR;
      if (readback == 0xbb)
        return FTE3600_LEGACY_OK;
    }
  for (i = 0; i < count; ++i)
    {
      if (write_byte (io, registers[i], values[i]))
        return FTE3600_LEGACY_IO_ERROR;
      io->wait_ms (io->context, delay);
      if (i == 2 && read_byte (io, 0x30, &readback))
        return FTE3600_LEGACY_IO_ERROR;
    }
  return FTE3600_LEGACY_OK;
}

enum fte3600_legacy_status
fte3600_legacy_idle (enum fte3600_sensor sensor, const struct fte3600_recovery_io *io)
{
  const uint8_t tx[1] = {0x70};
  uint8_t rx[1] = {0}, mode = 0;

  if (!valid (sensor, io))
    return FTE3600_LEGACY_INVALID_INPUT;
  if (io->spi (io->context, tx, rx, sizeof tx))
    return FTE3600_LEGACY_IO_ERROR;
  io->wait_ms (io->context, 5);
  if (io->spi (io->context, tx, rx, sizeof tx) ||
      read_byte (io, mode_register (sensor), &mode))
    return FTE3600_LEGACY_IO_ERROR;
  if (mode < 2 || mode > 4)
    {
      if (write_byte (io, 0x1e, 0) || write_byte (io, 0x1f, 0))
        return FTE3600_LEGACY_IO_ERROR;
      io->wait_ms (io->context, 10);
    }
  return FTE3600_LEGACY_OK;
}

enum fte3600_legacy_status
fte3600_legacy_arm_observed (enum fte3600_sensor sensor, uint32_t mode,
                             const struct fte3600_recovery_io *io,
                             const struct fte3600_legacy_arm_observer *o)
{
  int ready = 0;
  uint8_t ignored = 0;

  if (!valid (sensor, io) || mode > UINT8_MAX || (o && !o->idle))
    return FTE3600_LEGACY_INVALID_INPUT;
  if (mcu_ready (io, &ready))
    return FTE3600_LEGACY_IO_ERROR;
  if (!ready)
    {
      if (o)
        o->idle (o->context, 1, FTE3600_LEGACY_OK);
      enum fte3600_legacy_status status = fte3600_legacy_idle (sensor, io);
      if (o)
        o->idle (o->context, 0, status);
      if (status != FTE3600_LEGACY_OK)
        return FTE3600_LEGACY_IO_ERROR;
    }
  if (write_byte (io, mode_register (sensor), (uint8_t) mode))
    return FTE3600_LEGACY_IO_ERROR;
  switch (mode)
    {
    case 1:
      if (write_byte (io, 0x1f, 1) || write_byte (io, 0x1e, 1))
        return FTE3600_LEGACY_IO_ERROR;
      io->wait_ms (io->context, 10);
      if (read_byte (io, 0x1d, &ignored))
        return FTE3600_LEGACY_IO_ERROR;
      break;

    case 2:
      if (write_byte (io, 0x54, 1))
        return FTE3600_LEGACY_IO_ERROR;
      break;

    case 3:
      if (write_byte (io, 0, 0x20))
        return FTE3600_LEGACY_IO_ERROR;
      io->wait_ms (io->context, 1);
      break;

    default: break;
    }
  if (mcu_ready (io, &ready))
    return FTE3600_LEGACY_IO_ERROR;
  if (ready && write_byte (io, mode_register (sensor), 0))
    return FTE3600_LEGACY_IO_ERROR;
  return FTE3600_LEGACY_OK;
}

enum fte3600_legacy_status
fte3600_legacy_arm (enum fte3600_sensor sensor, uint32_t mode,
                    const struct fte3600_recovery_io *io)
{
  return fte3600_legacy_arm_observed (sensor, mode, io, NULL);
}

struct fte3600_legacy_frame_result
fte3600_legacy_frame_fetch (enum fte3600_sensor sensor, uint8_t *image,
                            size_t capacity, const struct fte3600_recovery_io *io)
{
  struct fte3600_legacy_frame_result result = {
    FTE3600_LEGACY_INVALID_INPUT, FTE3600_LEGACY_INVALID_INPUT, 0
  };
  struct fte3600_legacy_geometry geometry = fte3600_legacy_geometry (sensor);
  uint8_t tx[FTE3600_LEGACY_MAX_TRANSFER_BYTES] = {0};
  uint8_t rx[FTE3600_LEGACY_MAX_TRANSFER_BYTES] = {0};
  size_t i;

  if (!valid (sensor, io) || image == NULL || capacity < geometry.image_bytes)
    return result;
  memset (image, 0, geometry.image_bytes);
  tx[0] = 0x04;
  tx[1] = 0xfb;
  tx[2] = 0x34;
  tx[4] = (uint8_t) (geometry.transfer_bytes >> 8);
  tx[5] = (uint8_t) geometry.transfer_bytes;
  result.status = io->spi (io->context, tx, rx, geometry.transfer_bytes) == 0 ?
                  FTE3600_LEGACY_OK : FTE3600_LEGACY_IO_ERROR;
  if (result.status == FTE3600_LEGACY_OK)
    {
      for (i = 0; i < geometry.image_bytes; ++i)
        image[i] = (uint8_t) (UINT8_MAX - rx[i + 8]);
      result.image_length = geometry.image_bytes;
    }
  result.rearm_status = fte3600_legacy_arm (sensor, geometry.rearm_mode, io);
  return result;
}
