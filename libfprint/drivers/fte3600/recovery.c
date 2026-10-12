/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_recovery.h"

#include <stdlib.h>
#include <string.h>

static int
reset_raw (const struct fte3600_recovery_io *io)
{
  int error = io->gpio (io->context, 1);

  if (error)
    return error;
  io->wait_ms (io->context, 10);
  error = io->gpio (io->context, 0);
  if (error)
    return error;
  io->wait_ms (io->context, 20);
  return io->gpio (io->context, 1);
}
static int
reset_pulse (const struct fte3600_recovery_io    *io,
             const struct fte3600_reset_observer *o)
{
  if (o)
    o->reset (o->context, 1, 0);
  int status = reset_raw (io);
  if (o)
    o->reset (o->context, 0, status);
  return status;
}

static int
read_register (const struct fte3600_recovery_io *io,
               uint8_t address, uint8_t reply[2])
{
  const uint8_t tx[6] = {0x10, 0xef, address, 0, 0, 0};
  uint8_t rx[6] = {0};

  if (io->spi (io->context, tx, rx, sizeof (tx)))
    return 1;
  reply[0] = rx[4];
  reply[1] = rx[5];
  return 0;
}

static int
write_register (const struct fte3600_recovery_io *io,
                enum fte3600_recovery_boot boot,
                uint8_t address, uint8_t value)
{
  const int is_a = boot == FTE3600_RECOVERY_BOOT_A;
  const uint8_t tx[5] = {is_a ? 0x09 : 0x11, is_a ? 0xf6 : 0xee,
                         address, value, 0};
  uint8_t rx[5] = {0};

  return io->spi (io->context, tx, rx, is_a ? 4 : 5) != 0;
}

/* Each call has its own elapsed-time window, checked before every attempt. */
static enum fte3600_recovery_status
poll_boot_b (const struct fte3600_recovery_io *io, uint8_t address,
             uint8_t expected, const struct fte3600_reset_observer *o)
{
  const uint32_t start = io->clock_ms (io->context);

  while ((uint32_t) (io->clock_ms (io->context) - start) < UINT32_C (2000))
    {
      uint8_t reply[2];
      if (address == 0x02)
        {
          if (reset_pulse (io, o))
            return FTE3600_RECOVERY_IO_ERROR;
          io->wait_ms (io->context, 20);
        }
      if (address == 0x0b &&
          write_register (io, FTE3600_RECOVERY_BOOT_B, 0xa5, 0))
        return FTE3600_RECOVERY_IO_ERROR;
      if (read_register (io, address, reply))
        return FTE3600_RECOVERY_IO_ERROR;
      if (reply[0] == expected && (address != 0x02 || reply[1] == 0x38))
        return FTE3600_RECOVERY_OK;
      io->wait_ms (io->context, 10);
    }
  return FTE3600_RECOVERY_FAILED;
}

static enum fte3600_recovery_status
prepare_boot_a (const struct fte3600_recovery_io *io, const struct fte3600_reset_observer *o)
{
  static const uint8_t writes[][2] = {
    {0xc8, 0xff}, {0xca, 0xff}, {0xcb, 0xff}, {0xb9, 0xbf}, {0xb9, 0xff}
  };
  const uint8_t sync_tx[2] = {0x55, 0xaa};
  uint8_t sync_rx[2] = {0};
  size_t i;

  if (reset_pulse (io, o) ||
      io->spi (io->context, sync_tx, sync_rx, sizeof (sync_tx)))
    return FTE3600_RECOVERY_IO_ERROR;
  for (i = 0; i < sizeof (writes) / sizeof (writes[0]); ++i)
    if (write_register (io, FTE3600_RECOVERY_BOOT_A,
                        writes[i][0], writes[i][1]))
      return FTE3600_RECOVERY_IO_ERROR;
  io->wait_ms (io->context, 20);
  return FTE3600_RECOVERY_OK;
}

static enum fte3600_recovery_status
prepare_boot_b (const struct fte3600_recovery_io *io, const struct fte3600_reset_observer *o)
{
  uint8_t ignored_reply[2];
  enum fte3600_recovery_status status = poll_boot_b (io, 0x02, 0x93, o);

  if (status != FTE3600_RECOVERY_OK)
    return status;
  if (write_register (io, FTE3600_RECOVERY_BOOT_B, 0x08, 1) ||
      read_register (io, 0x1a, ignored_reply))
    return FTE3600_RECOVERY_IO_ERROR;
  status = poll_boot_b (io, 0x09, 1, o);
  if (status != FTE3600_RECOVERY_OK)
    return status;
  return write_register (io, FTE3600_RECOVERY_BOOT_B, 0xa9, 1) ?
         FTE3600_RECOVERY_IO_ERROR : FTE3600_RECOVERY_OK;
}

static enum fte3600_recovery_status
finish_boot_b (const struct fte3600_recovery_io *io, const struct fte3600_reset_observer *o)
{
  enum fte3600_recovery_status status = poll_boot_b (io, 0x0b, 0x6a, o);

  if (status != FTE3600_RECOVERY_OK)
    return status;
  status = poll_boot_b (io, 0x07, 0x92, o);
  if (status != FTE3600_RECOVERY_OK)
    return status;
  return write_register (io, FTE3600_RECOVERY_BOOT_B, 0xa2, 0) ?
         FTE3600_RECOVERY_IO_ERROR : FTE3600_RECOVERY_OK;
}

static enum fte3600_recovery_status
startup (const struct fte3600_recovery_io *io, uint32_t startup_wait_ms,
         const struct fte3600_reset_observer *o)
{
  unsigned attempt;

  if (reset_pulse (io, o))
    return FTE3600_RECOVERY_IO_ERROR;
  io->wait_ms (io->context, 10);
  if (reset_pulse (io, o))
    return FTE3600_RECOVERY_IO_ERROR;
  io->wait_ms (io->context, startup_wait_ms);
  for (attempt = 0; attempt < 20; ++attempt)
    {
      uint8_t reply[2];
      if (!read_register (io, 0x20, reply) &&
          reply[0] == 0xa5 && reply[1] == 0x5a)
        return FTE3600_RECOVERY_OK;
      io->wait_ms (io->context, 2);
    }
  return FTE3600_RECOVERY_FAILED;
}

static enum fte3600_recovery_status
transfer_firmware (enum fte3600_recovery_boot boot,
                   const struct fte3600_recovery_io *io,
                   const uint8_t *firmware, size_t length,
                   uint8_t *tx, uint8_t *rx)
{
  const size_t readback_length = length + 8;
  int readback_error;

  tx[0] = 0x05;
  tx[1] = 0xfa;
  tx[4] = (uint8_t) (length >> 8);
  tx[5] = (uint8_t) length;
  memcpy (tx + 6, firmware, length);
  if (io->spi (io->context, tx, rx, length + 7))
    return FTE3600_RECOVERY_IO_ERROR;
  io->wait_ms (io->context, boot == FTE3600_RECOVERY_BOOT_A ? 2 : 10);

  memset (tx, 0, readback_length);
  memset (rx, 0, readback_length);
  tx[0] = 0x04;
  tx[1] = 0xfb;
  tx[4] = (uint8_t) (readback_length >> 8);
  tx[5] = (uint8_t) readback_length;
  readback_error = io->spi (io->context, tx, rx, readback_length);
  if (readback_error)
    return FTE3600_RECOVERY_FAILED;
  if (boot == FTE3600_RECOVERY_BOOT_B)
    io->wait_ms (io->context, 5);
  if (memcmp (rx + (boot == FTE3600_RECOVERY_BOOT_A ? 6 : 8),
              firmware, length))
    return FTE3600_RECOVERY_FAILED;
  return FTE3600_RECOVERY_OK;
}

enum fte3600_recovery_status
fte3600_recover_observed (enum fte3600_recovery_boot boot, const uint8_t *firmware,
                          size_t firmware_length, uint32_t startup_wait_ms,
                          const struct fte3600_recovery_io *io,
                          const struct fte3600_reset_observer *o)
{
  enum fte3600_recovery_status status;

  uint8_t *tx;
  uint8_t *rx;
  if ((boot != FTE3600_RECOVERY_BOOT_A && boot != FTE3600_RECOVERY_BOOT_B) ||
      !firmware || firmware_length == 0 ||
      firmware_length > FTE3600_RECOVERY_MAX_FIRMWARE ||
      !io || !io->gpio || !io->spi || !io->wait_ms || !io->clock_ms ||
      (o && !o->reset))
    return FTE3600_RECOVERY_INVALID_INPUT;

  tx = calloc (firmware_length + 8, 1);
  rx = calloc (firmware_length + 8, 1);
  if (!tx || !rx)
    {
      free (rx);
      free (tx);
      return FTE3600_RECOVERY_NO_MEMORY;
    }

  status = boot == FTE3600_RECOVERY_BOOT_A ? prepare_boot_a (io, o) :
           prepare_boot_b (io, o);
  if (status == FTE3600_RECOVERY_OK)
    status = transfer_firmware (boot, io, firmware, firmware_length, tx, rx);
  if (status == FTE3600_RECOVERY_OK && boot == FTE3600_RECOVERY_BOOT_B)
    status = finish_boot_b (io, o);
  if (status == FTE3600_RECOVERY_OK)
    status = startup (io, startup_wait_ms, o);
  free (rx);
  free (tx);
  return status;
}

enum fte3600_recovery_status
fte3600_recover (enum fte3600_recovery_boot boot, const uint8_t *firmware,
                 size_t length, uint32_t settle, const struct fte3600_recovery_io *io)
{
  return fte3600_recover_observed (boot, firmware, length, settle, io, NULL);
}
