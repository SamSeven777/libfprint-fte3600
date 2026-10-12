/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_LEGACY_LIFECYCLE_H
#define FTE3600_LEGACY_LIFECYCLE_H

#include "fte3600_legacy.h"

#ifdef __cplusplus
extern "C" {
#endif

enum fte3600_legacy_lifecycle_operation {
  FTE3600_LEGACY_START, FTE3600_LEGACY_IRQ, FTE3600_LEGACY_RESUME,
  FTE3600_LEGACY_SUSPEND, FTE3600_LEGACY_CANCEL
};

enum fte3600_legacy_lifecycle_status {
  FTE3600_LEGACY_LIFECYCLE_OK,
  FTE3600_LEGACY_LIFECYCLE_HANDLED,
  FTE3600_LEGACY_LIFECYCLE_INVALID_INPUT
};

enum fte3600_legacy_completion {
  FTE3600_LEGACY_COMPLETE_OK, FTE3600_LEGACY_COMPLETE_CANCELLED
};

struct fte3600_legacy_lifecycle_state
{
  uint8_t request_pending;   /* Exactly zero or one; other fields are bytes. */
  uint8_t request_active, request_kind, irq_enabled, suppress_irq, irq_count;
  uint8_t cached_frame, power_flag, health_running;
};

struct fte3600_legacy_lifecycle_options
{
  /* Boolean inputs must be exactly zero or one. Token identity and
   * cancellation ownership are supplied by the serialized caller. */
  int      completion_cancelled, cancel_matches, cancel_context_guard;
  uint32_t power_policy, power_action, previous_power_state;
};

struct fte3600_legacy_lifecycle_io
{
  struct fte3600_recovery_io protocol;
  void                      *request_context;
  /* Complete device/firmware initialization remains caller-owned.
   * Zero means success; any other value is failure. Result is ignored. */
  int (*initialize)(void *context);
  /* matched_pending compares this token with the token pending at entry.
   * image is NULL/length zero unless this operation obtained fresh pixels.
   * A completion with OK may have no image; it does not establish capture
   * success. Neither the image nor other callback buffers may be retained. */
  void (*complete)(void                          *context,
                   int                            matched_pending,
                   enum fte3600_legacy_completion result,
                   const uint8_t                 *image,
                   size_t                         image_length);
};

struct fte3600_legacy_lifecycle_result
{
  enum fte3600_legacy_lifecycle_status status;
  size_t image_length;
};

/* Bounded non-GUI profile for the four legacy models, using an already
 * constructed transport. This function does not register requests, set up
 * physical IRQs, run firmware initialization, or implement asynchronous races.
 * Preserve state between calls. Caller serializes all operations, supplies
 * token identity/ownership, and keeps all objects valid and nonoverlapping.
 * Callbacks may inspect state but must not mutate it or reenter this library.
 * spi/wait_ms/initialize/complete, state/options/io, and image are required
 * for every operation; other recovery callbacks and contexts may be NULL.
 * Capacity must cover geometry.image_bytes even for non-frame operations.
 * Invalid input makes no calls and leaves state and image unchanged.
 * Every valid invocation clears the image region first; excess capacity is
 * untouched. Only a successful frame transfer returns a nonzero image_length.
 * The outer OK/HANDLED result swallows nested transport/initializer failures;
 * callers must observe callbacks/transport diagnostics separately. Completion
 * ownership is independent of frame validity. No allocation/global state.
 * Stack use includes the existing roughly 19 KiB frame-fetch buffers.
 * Linux transport capacity must cover geometry.transfer_bytes and callers
 * must inspect its error accessor, including wait errors hidden by void I/O.
 */
struct fte3600_legacy_lifecycle_result fte3600_legacy_lifecycle_run (enum fte3600_sensor                            sensor,
                                                                     enum fte3600_legacy_lifecycle_operation        operation,
                                                                     struct fte3600_legacy_lifecycle_state         *state,
                                                                     const struct fte3600_legacy_lifecycle_options *options,
                                                                     uint8_t                                       *image,
                                                                     size_t                                         capacity,
                                                                     const struct fte3600_legacy_lifecycle_io      *io);

#ifdef __cplusplus
}
#endif
#endif
