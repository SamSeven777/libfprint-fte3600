/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_RESET_SYNC_H
#define FTE3600_RESET_SYNC_H
#include "fte3600_recovery.h"

/* Shared protocol composition. The optional transport owns only the final
 * release/sync pair; ordinary pulse steps and portable observer order remain
 * unchanged. This helper is private to the independent protocol components. */
static inline int
fte3600_reset_and_sync (
  const struct fte3600_recovery_io *io, const struct fte3600_reset_observer *o)
{
  const uint8_t tx[2] = {0x55, 0xaa};
  uint8_t rx[2];
  int status, gpio_status;

  if (o)
    o->reset (o->context, 1, 0);
  status = io->gpio (io->context, 1);
  if (!status)
    {
      io->wait_ms (io->context, 10);
      status = io->gpio (io->context, 0);
    }
  if (!status)
    {
      io->wait_ms (io->context, 20);
      if (io->release_and_sync)
        {
          status = io->release_and_sync (io->context, &gpio_status);
          if (o)
            o->reset (o->context, 0, gpio_status);
          return status;
        }
      status = io->gpio (io->context, 1);
    }
  if (o)
    o->reset (o->context, 0, status);
  return status ? status : io->spi (io->context, tx, rx, sizeof (tx));
}
#endif
