/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_FT9368_LIFECYCLE_H
#define FTE3600_FT9368_LIFECYCLE_H

#include "fte3600_ft9368.h"

#ifdef __cplusplus
extern "C" {
#endif

struct fte3600_ft9368_lifecycle_state
{
  uint8_t  poa_mode;
  uint32_t scene;
};

struct fte3600_ft9368_lifecycle_io
{
  struct fte3600_ft9368_io protocol;
  void                    *gpio_context;
  /* Physical level 0 or 1. High status bit means failure; exact raw status
   * is retained. Must not add unrecorded waits or exchanges. */
  uint32_t (*gpio)(void *context,
                   int   level);
  void    *notification_context;
  /* Request scheduling only: the library implements no worker or thread.
   * This opaque result is ignored, including zero. */
  uint64_t (*schedule_notification)(void *context);
};

/* Four bounded SPI lifecycle operations; no complete power-on, IRQ, driver,
 * firmware update, or authentication support. State is always unchanged.
 * All four callbacks and nonnull state/io are required for every operation,
 * even when the selected path would not use them. Contexts may be null.
 * Invalid arguments return the project-local FT9368_INVALID_ARGUMENT before
 * any callback. Detection otherwise returns 0/1; reset/enter_poa return raw
 * statuses as specified. Callback errors ignored by the protocol stay ignored.
 * No allocation or global mutable state. Callers serialize operations and
 * keep objects valid, unchanged, and nonoverlapping until return. */
uint32_t fte3600_ft9368_lifecycle_reset (const struct fte3600_ft9368_lifecycle_state *state,
                                         const struct fte3600_ft9368_lifecycle_io    *io);
uint32_t fte3600_ft9368_lifecycle_detect (const struct fte3600_ft9368_lifecycle_state *state,
                                          const struct fte3600_ft9368_lifecycle_io    *io);
uint32_t fte3600_ft9368_lifecycle_detect_poa (const struct fte3600_ft9368_lifecycle_state *state,
                                              const struct fte3600_ft9368_lifecycle_io    *io);
uint32_t fte3600_ft9368_lifecycle_enter_poa (const struct fte3600_ft9368_lifecycle_state *state,
                                             const struct fte3600_ft9368_lifecycle_io    *io);

/* Adapt existing recovery/Linux callbacks plus an explicit notification
 * scheduling callback. Requires recovery spi/gpio/wait_ms, not clock_ms.
 * Source table remains caller-owned and must stay valid and unchanged.
 * SPI uses the existing FT9368 adapter. GPIO maps recovery zero to zero and
 * every nonzero value to FT9368_ADAPTER_IO_ERROR, regardless of sign.
 * This adapter adds no error latch, retry, notification execution, or reset
 * polarity inversion. Detection continues after reset failure as specified.
 * Invalid input leaves *output unchanged. Output/source must not overlap.
 * These operations need Linux transfer capacity >=39 bytes. Inspect
 * fte3600_linux_error() after use: protocol success can conceal transport
 * errors; fatal Linux timing errors retain their existing transport behavior.
 */
uint32_t fte3600_ft9368_lifecycle_io_from_recovery (const struct fte3600_recovery_io   *recovery,
                                                    void                               *notification_context,
                                                    uint64_t (                         *schedule_notification )(void *),
                                                    struct fte3600_ft9368_lifecycle_io *output);

#ifdef __cplusplus
}
#endif
#endif
