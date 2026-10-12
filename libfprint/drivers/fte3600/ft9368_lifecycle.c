/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_ft9368_lifecycle.h"

static int
valid (const struct fte3600_ft9368_lifecycle_state *state,
       const struct fte3600_ft9368_lifecycle_io    *io)
{
  return state && io && io->protocol.exchange && io->protocol.wait_ms &&
         io->gpio && io->schedule_notification;
}

static int
negative (uint32_t status)
{
  return (status & UINT32_C (0x80000000)) != 0;
}

static void
wait_ms (const struct fte3600_ft9368_lifecycle_io *io, uint32_t ms)
{
  io->protocol.wait_ms (io->protocol.context, ms);
}

static int
identity_matches (const uint8_t information[32])
{
  return information[19] == 0x93 && information[20] == 0x68;
}

static int
finger_present (uint8_t poa_mode, const uint8_t information[32])
{
  return information[2] == 0x11 && (poa_mode || information[1] == 0x11);
}

uint32_t
fte3600_ft9368_lifecycle_reset (
  const struct fte3600_ft9368_lifecycle_state *state,
  const struct fte3600_ft9368_lifecycle_io    *io)
{
  static const int levels[] = {1, 0, 1};
  static const uint32_t delays[] = {10, 20};
  unsigned int i;
  uint32_t status;

  if (!valid (state, io))
    return FTE3600_FT9368_INVALID_ARGUMENT;
  for (i = 0; i < 3; ++i)
    {
      status = io->gpio (io->gpio_context, levels[i]);
      if (negative (status))
        return status;
      if (i < 2)
        wait_ms (io, delays[i]);
    }
  return 0;
}

uint32_t
fte3600_ft9368_lifecycle_detect (
  const struct fte3600_ft9368_lifecycle_state *state,
  const struct fte3600_ft9368_lifecycle_io    *io)
{
  uint8_t information[32];
  uint32_t status = 0;
  unsigned int attempt;

  if (!valid (state, io))
    return FTE3600_FT9368_INVALID_ARGUMENT;
  for (attempt = 0; attempt < 2; ++attempt)
    {
      status = fte3600_ft9368_info (sizeof information, information,
                                    sizeof information, &io->protocol);
      if (finger_present (state->poa_mode, information))
        {
          (void) io->schedule_notification (io->notification_context);
          return 1;
        }
    }
  if (!negative (status) && information[2] != 0x22 &&
      !(information[0] == 0xff && information[1] == 0xff) &&
      identity_matches (information))
    return 0;

  (void) fte3600_ft9368_lifecycle_reset (state, io);
  for (attempt = 0; attempt < 5; ++attempt)
    {
      wait_ms (io, 200);
      (void) fte3600_ft9368_info (sizeof information, information,
                                  sizeof information, &io->protocol);
      (void) fte3600_ft9368_info (sizeof information, information,
                                  sizeof information, &io->protocol);
      if (identity_matches (information))
        break;
    }
  (void) fte3600_ft9368_info (sizeof information, information,
                              sizeof information, &io->protocol);
  return 0;
}

uint32_t
fte3600_ft9368_lifecycle_detect_poa (
  const struct fte3600_ft9368_lifecycle_state *state,
  const struct fte3600_ft9368_lifecycle_io    *io)
{
  uint8_t information[6];
  uint32_t status;

  if (!valid (state, io))
    return FTE3600_FT9368_INVALID_ARGUMENT;
  (void) fte3600_ft9368_wake (&io->protocol);
  status = fte3600_ft9368_info (sizeof information, information,
                                sizeof information, &io->protocol);
  if (!negative (status) && information[2] == 0x11)
    {
      (void) io->schedule_notification (io->notification_context);
      return 1;
    }
  return 0;
}

uint32_t
fte3600_ft9368_lifecycle_enter_poa (
  const struct fte3600_ft9368_lifecycle_state *state,
  const struct fte3600_ft9368_lifecycle_io    *io)
{
  uint8_t discarded[4];

  if (!valid (state, io))
    return FTE3600_FT9368_INVALID_ARGUMENT;
  if (state->scene != 2)
    return 0;
  return fte3600_ft9368_command (0xf080, sizeof discarded, discarded,
                                 sizeof discarded, &io->protocol);
}

static uint32_t
recovery_gpio (void *context, int level)
{
  const struct fte3600_recovery_io *recovery = context;

  return recovery->gpio (recovery->context, level) == 0 ? 0 :
         FTE3600_FT9368_ADAPTER_IO_ERROR;
}

uint32_t
fte3600_ft9368_lifecycle_io_from_recovery (
  const struct fte3600_recovery_io *recovery,
  void *notification_context, uint64_t (*schedule_notification)(void *),
  struct fte3600_ft9368_lifecycle_io *output)
{
  struct fte3600_ft9368_lifecycle_io adapted;
  uint32_t status;

  if (!recovery || !recovery->gpio || !schedule_notification || !output)
    return FTE3600_FT9368_INVALID_ARGUMENT;
  status = fte3600_ft9368_io_from_recovery (recovery, &adapted.protocol);
  if (status != 0)
    return status;
  adapted.gpio_context = (void *) recovery;
  adapted.gpio = recovery_gpio;
  adapted.notification_context = notification_context;
  adapted.schedule_notification = schedule_notification;
  *output = adapted;
  return 0;
}
