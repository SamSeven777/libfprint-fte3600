/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_FT9368_H
#define FTE3600_FT9368_H

#include "fte3600_recovery.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FTE3600_FT9368_IMAGE_BYTES ((size_t) 5120)
#define FTE3600_FT9368_TRANSFER_BYTES ((size_t) 5127)
#define FTE3600_FT9368_INVALID_OUTPUT UINT32_C(0xc022001c)
#define FTE3600_FT9368_VERSION_MISMATCH UINT32_C(0xc000000d)
/* Project-local validation result, outside the reviewed parity domain. */
#define FTE3600_FT9368_INVALID_ARGUMENT UINT32_C(0xe0000001)
/* The explicit recovery adapter's mapping, not a recovered device status. */
#define FTE3600_FT9368_ADAPTER_IO_ERROR UINT32_C(0xc0000185)

struct fte3600_ft9368_io
{
  void *context;
  /* Synchronous shared TX/RX buffer. On high-bit-set status leave the
   * buffer unchanged; otherwise fill the complete response payload.
   * Do not retain the pointer. Exact status bits are protocol-visible. */
  uint32_t (*exchange)(void    *context,
                       uint8_t *buffer,
                       size_t   length);
  void (*wait_ms)(void    *context,
                  uint32_t milliseconds);
};

/* Running FT9368 application protocol only. No reset, power, IRQ handling,
 * startup orchestration, firmware loading/update, or device selection.
 * Both callbacks are required, context may be NULL. Callers serialize access;
 * all pointed-to objects must be valid and nonoverlapping until return.
 * No allocation or global mutable state is used. Callback calls are bounded.
 *
 * Read output must be nonnull even for length zero. Null output returns
 * INVALID_OUTPUT. Invalid IO, lengths, commands, and capacities return
 * INVALID_ARGUMENT, without callbacks or output changes. Extra capacity is
 * untouched. probe/shortcut return zero without callbacks on invalid IO.
 * Allowed read lengths: 0..57 and 64..5120. A failed exchange still copies
 * the zero payload to output, under the callback contract above.
 */
uint32_t fte3600_ft9368_command (uint16_t                        command,
                                 size_t                          length,
                                 uint8_t                        *output,
                                 size_t                          capacity,
                                 const struct fte3600_ft9368_io *io);
uint32_t fte3600_ft9368_info (size_t                          length,
                              uint8_t                        *output,
                              size_t                          capacity,
                              const struct fte3600_ft9368_io *io);
uint16_t fte3600_ft9368_probe (const struct fte3600_ft9368_io *io);
int fte3600_ft9368_shortcut (const struct fte3600_ft9368_io *io);
uint32_t fte3600_ft9368_wake (const struct fte3600_ft9368_io *io);
uint32_t fte3600_ft9368_capture (uint8_t                        *image,
                                 size_t                          capacity,
                                 const struct fte3600_ft9368_io *io);
/* Serialized notification pulse, not a thread or a request owner.
 * signal wakes, then reads four bytes with F280 for any nonzero enabled
 * value or F180 for zero. It returns that final exchange's exact status,
 * even after failed/exhausted wake attempts (at most seven exchanges).
 * worker receives a pending-request snapshot by value. Zero does no I/O;
 * nonzero runs signal-on, waits 400 ms, then signal-off, ignoring both
 * statuses and returning zero (at most fourteen exchanges, 460 ms waits).
 * Both callbacks are required even for an idle worker. Invalid IO returns
 * INVALID_ARGUMENT before callbacks. Neither API changes caller pending
 * state, schedules work, completes a request, or manages cancellation.
 */
uint32_t fte3600_ft9368_signal (uint8_t                         enabled,
                                const struct fte3600_ft9368_io *io);
uint32_t fte3600_ft9368_worker (int                             request_pending,
                                const struct fte3600_ft9368_io *io);
uint32_t fte3600_ft9368_clear (const struct fte3600_ft9368_io *io);
uint32_t fte3600_ft9368_double_info (const struct fte3600_ft9368_io *io);
/* A zero result can also mean three identity mismatches. It is not proof
 * of sensor identity, correct firmware, or working hardware. */
uint32_t fte3600_ft9368_check_version (uint8_t                         expected_version,
                                       const struct fte3600_ft9368_io *io);

/* Only recovery spi/wait_ms are required. Source must remain unchanged and
 * valid while output is used. Distinct TX/RX storage preserves the FT9368
 * failure boundary even if a failing recovery callback modifies RX.
 * Recovery zero/nonzero maps to zero/ADAPTER_IO_ERROR, independently of sign.
 * Invalid input leaves output unchanged. Linux transfer capacity must cover
 * 5127 bytes; inspect fte3600_linux_error() after every operation because waits
 * cannot report timing errors and several protocol paths ignore I/O statuses.
 * This adapter adds no retries and does not latch errors or stop later calls.
 */
uint32_t fte3600_ft9368_io_from_recovery (const struct fte3600_recovery_io *recovery,
                                          struct fte3600_ft9368_io         *output);

#ifdef __cplusplus
}
#endif
#endif
