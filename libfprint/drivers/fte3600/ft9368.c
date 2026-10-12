/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_ft9368.h"

#include <string.h>

static int
valid_io (const struct fte3600_ft9368_io *io)
{
  return io && io->exchange && io->wait_ms;
}

static int
negative (uint32_t status)
{
  return (status & UINT32_C (0x80000000)) != 0;
}

static int
valid_length (size_t length)
{
  return length <= 57 || (length >= 64 && length <= 5120);
}

static int
valid_command (uint16_t command)
{
  return command == 0xff00 || command == 0x9180 || command == 0x9080 ||
         command == 0xf680 || command == 0xf080 || command == 0xf280 ||
         command == 0xf180;
}

/* Internal callers have already validated arguments and use bounded sizes. */
static uint32_t
read_payload (const struct fte3600_ft9368_io *io,
              uint16_t command, size_t length, uint8_t *output)
{
  uint8_t frame[FTE3600_FT9368_TRANSFER_BYTES] = {0};
  size_t header = length ? 7 : 4;
  uint32_t status;

  frame[0] = (uint8_t) (command >> 8);
  frame[1] = (uint8_t) command;
  frame[2] = (uint8_t) (length >> 8);
  frame[3] = (uint8_t) length;
  status = io->exchange (io->context, frame, header + length);
  if (length)
    memcpy (output, frame + header, length);
  return status;
}

uint32_t
fte3600_ft9368_command (uint16_t command, size_t length,
                        uint8_t *output, size_t capacity, const struct fte3600_ft9368_io *io)
{
  if (!output)
    return FTE3600_FT9368_INVALID_OUTPUT;
  if (!valid_io (io) || !valid_length (length) || !valid_command (command) ||
      capacity < length)
    return FTE3600_FT9368_INVALID_ARGUMENT;
  return read_payload (io, command, length, output);
}

uint32_t
fte3600_ft9368_info (size_t length, uint8_t *output, size_t capacity,
                     const struct fte3600_ft9368_io *io)
{
  return fte3600_ft9368_command (0x9180, length, output, capacity, io);
}

static void
wake_request (const struct fte3600_ft9368_io *io, uint32_t wait_ms)
{
  (void) read_payload (io, 0xff00, 0, NULL);
  io->wait_ms (io->context, wait_ms);
}

uint16_t
fte3600_ft9368_probe (const struct fte3600_ft9368_io *io)
{
  uint8_t information[32];

  if (!valid_io (io))
    return 0;
  wake_request (io, 5);
  (void) read_payload (io, 0x9180, sizeof information, information);
  return (uint16_t) (((uint16_t) information[19] << 8) | information[20]);
}

int
fte3600_ft9368_shortcut (const struct fte3600_ft9368_io *io)
{
  uint8_t information[6];

  if (!valid_io (io))
    return 0;
  wake_request (io, 5);
  (void) read_payload (io, 0x9180, sizeof information, information);
  return information[2] == 0x11;
}

uint32_t
fte3600_ft9368_wake (const struct fte3600_ft9368_io *io)
{
  uint8_t information[4];
  uint32_t status = 0;
  unsigned int attempt;

  if (!valid_io (io))
    return FTE3600_FT9368_INVALID_ARGUMENT;
  for (attempt = 0; attempt < 3; ++attempt)
    {
      wake_request (io, 10);
      status = read_payload (io, 0x9180, sizeof information, information);
      if (!information[0] || information[0] != information[1] ||
          information[0] != information[2] || information[0] != information[3])
        break;
    }
  return status;
}

uint32_t
fte3600_ft9368_capture (uint8_t *image, size_t capacity,
                        const struct fte3600_ft9368_io *io)
{
  if (!image)
    return FTE3600_FT9368_INVALID_OUTPUT;
  if (!valid_io (io) || capacity < FTE3600_FT9368_IMAGE_BYTES)
    return FTE3600_FT9368_INVALID_ARGUMENT;
  memset (image, 0, FTE3600_FT9368_IMAGE_BYTES);
  (void) fte3600_ft9368_wake (io);
  return read_payload (io, 0x9080, FTE3600_FT9368_IMAGE_BYTES, image);
}

uint32_t
fte3600_ft9368_signal (uint8_t                         enabled,
                       const struct fte3600_ft9368_io *io)
{
  uint8_t discarded[4];

  if (!valid_io (io))
    return FTE3600_FT9368_INVALID_ARGUMENT;
  (void) fte3600_ft9368_wake (io);
  return read_payload (io, enabled ? 0xf280 : 0xf180,
                       sizeof discarded, discarded);
}

uint32_t
fte3600_ft9368_worker (int                             request_pending,
                       const struct fte3600_ft9368_io *io)
{
  if (!valid_io (io))
    return FTE3600_FT9368_INVALID_ARGUMENT;
  if (!request_pending)
    return 0;
  (void) fte3600_ft9368_signal (1, io);
  io->wait_ms (io->context, 400);
  (void) fte3600_ft9368_signal (0, io);
  return 0;
}

uint32_t
fte3600_ft9368_clear (const struct fte3600_ft9368_io *io)
{
  uint8_t discarded[6];

  if (!valid_io (io))
    return FTE3600_FT9368_INVALID_ARGUMENT;
  return read_payload (io, 0x9080, sizeof discarded, discarded);
}

uint32_t
fte3600_ft9368_double_info (const struct fte3600_ft9368_io *io)
{
  uint8_t discarded[32];
  uint32_t status;
  unsigned int i;

  if (!valid_io (io))
    return FTE3600_FT9368_INVALID_ARGUMENT;
  for (i = 0; i < 2; ++i)
    {
      status = read_payload (io, 0x9180, sizeof discarded, discarded);
      if (negative (status))
        return status;
    }
  return 0;
}

uint32_t
fte3600_ft9368_check_version (uint8_t                         expected_version,
                              const struct fte3600_ft9368_io *io)
{
  uint8_t information[32];
  uint32_t status;
  unsigned int attempt;

  if (!valid_io (io))
    return FTE3600_FT9368_INVALID_ARGUMENT;
  for (attempt = 0; attempt < 3; ++attempt)
    {
      (void) fte3600_ft9368_wake (io);
      status = read_payload (io, 0x9180, sizeof information, information);
      if (negative (status))
        return status;
      if (information[19] == 0x93 && information[20] == 0x68)
        return information[21] == expected_version ? 0 :
               FTE3600_FT9368_VERSION_MISMATCH;
      io->wait_ms (io->context, 20);
    }
  return 0;
}

static uint32_t
recovery_exchange (void *context, uint8_t *buffer, size_t length)
{
  const struct fte3600_recovery_io *recovery = context;
  uint8_t tx[FTE3600_FT9368_TRANSFER_BYTES];
  uint8_t rx[FTE3600_FT9368_TRANSFER_BYTES] = {0};

  if (!buffer || length > sizeof tx)
    return FTE3600_FT9368_INVALID_ARGUMENT;
  memcpy (tx, buffer, length);
  if (recovery->spi (recovery->context, tx, rx, length) != 0)
    return FTE3600_FT9368_ADAPTER_IO_ERROR;
  memcpy (buffer, rx, length);
  return 0;
}

static void
recovery_wait (void *context, uint32_t milliseconds)
{
  const struct fte3600_recovery_io *recovery = context;

  recovery->wait_ms (recovery->context, milliseconds);
}

uint32_t
fte3600_ft9368_io_from_recovery (
  const struct fte3600_recovery_io *recovery, struct fte3600_ft9368_io *output)
{
  struct fte3600_ft9368_io adapted;

  if (!recovery || !recovery->spi || !recovery->wait_ms || !output)
    return FTE3600_FT9368_INVALID_ARGUMENT;
  adapted.context = (void *) recovery;
  adapted.exchange = recovery_exchange;
  adapted.wait_ms = recovery_wait;
  *output = adapted;
  return 0;
}
