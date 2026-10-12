/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_CAPTURE_H
#define FTE3600_CAPTURE_H

#include "fte3600_image.h"
#include "fte3600_recovery.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FTE3600_CAPTURE_FRAME_BYTES ((size_t) 10240)
#define FTE3600_CAPTURE_TRANSFER_BYTES ((size_t) 10246)
#define FTE3600_CAPTURE_MAX_FRAMES 5
#define FTE3600_CAPTURE_STATE_INITIALIZER {54, 2, 1, 0, 0, 0, 170, 64}

struct fte3600_capture_state
{
  uint8_t  dac, shift, channel_mode, mode, configured, check_registers;
  uint8_t  dac_cache;
  uint16_t period;
};

struct fte3600_capture_io
{
  void *context;
  /* Synchronous shared TX/RX storage; callbacks must not retain it.
   * The high status bit alone denotes failure. Preserve buffer bytes when
   * no response was received. Both callbacks are required; context may be
   * NULL. Callbacks must return. Protocol retries belong to this library. */
  uint32_t (*spi)(void    *context,
                  uint8_t *buffer,
                  size_t   length);
  void (*wait_ms)(void    *context,
                  uint32_t milliseconds);
};

/* Optional per-operation diagnostics, reset only after argument validation.
 * Compatibility success does not imply a ready sensor or valid frame data. */
struct fte3600_capture_diagnostics
{
  uint32_t failed_attempts, exhausted_transactions, last_failure;
  uint8_t  ready_timeout;
};

enum fte3600_capture_status {
  FTE3600_CAPTURE_OK = 0,
  FTE3600_CAPTURE_INVALID_ARGUMENT = 1,
  FTE3600_CAPTURE_WORD_READ_ERROR = 2,
  FTE3600_CAPTURE_FIFO_READ_ERROR = 3
};

/* Counts 0/1 mean one frame; 6..255 mean five. */
size_t fte3600_capture_frame_count (uint8_t requested);

/* Initialized FW9369 only: no reset, setup, finger wait, or baseline handling.
 * payload needs frame_count(count)*FRAME_BYTES writable bytes; extra capacity
 * is left unchanged. Invalid arguments invoke no callbacks and change nothing.
 * All pointed-to objects must be valid and nonoverlapping for the call. Caller
 * serializes access to a device. No allocation or global state is used.
 *
 * Three failed word-read attempts stop immediately with WORD_READ_ERROR,
 * without using the response, obtaining frames, or acknowledging the image.
 * Earlier state/I/O changes remain. This is a local safety extension outside
 * the reviewed compatibility domain. Other exhausted retries continue. */
enum fte3600_capture_status fte3600_capture_fifo (uint8_t                             count,
                                                  uint8_t                            *payload,
                                                  size_t                              capacity,
                                                  const struct fte3600_capture_io    *io,
                                                  struct fte3600_capture_diagnostics *diagnostics);

enum fte3600_capture_status fte3600_capture_frames (struct fte3600_capture_state       *state,
                                                    uint8_t                             count,
                                                    uint8_t                            *payload,
                                                    size_t                              capacity,
                                                    const struct fte3600_capture_io    *io,
                                                    struct fte3600_capture_diagnostics *diagnostics);

/* Additive bounded variant: after three failed FIFO attempts, stop before
 * copying missing response bytes or sending final acknowledgment. Successful
 * earlier frames and acquisition state remain; there is no rollback. This
 * FIFO_READ_ERROR is local, with no claimed compatibility return. All other
 * behavior and validation match capture_frames; the old entry is unchanged. */
enum fte3600_capture_status fte3600_capture_frames_checked (struct fte3600_capture_state       *state,
                                                            uint8_t                             count,
                                                            uint8_t                            *payload,
                                                            size_t                              capacity,
                                                            const struct fte3600_capture_io    *io,
                                                            struct fte3600_capture_diagnostics *diagnostics);

/* Run only the reviewed idle/image-configuration prefix, without triggering,
 * reading FIFO or acknowledging an image. Same argument/error/state and
 * diagnostic rules as capture_frames; no output buffer is needed. */
enum fte3600_capture_status fte3600_capture_configure (struct fte3600_capture_state       *state,
                                                       const struct fte3600_capture_io    *io,
                                                       struct fte3600_capture_diagnostics *diagnostics);

/* Adapt existing recovery/Linux callbacks. Only spi/wait_ms are required.
 * recovery must remain valid and unchanged while output is in use. The
 * adapter copies TX to separate storage and maps recovery zero/nonzero to
 * raw success/high-bit failure; it does not reinterpret positive recovery
 * statuses as successful raw statuses. Invalid input leaves output unchanged.
 * Linux transport capacity must allow 10246 bytes. Inspect its error accessor
 * after use, particularly because wait_ms cannot return timing failures. */
enum fte3600_capture_status fte3600_capture_io_from_recovery (const struct fte3600_recovery_io *recovery,
                                                              struct fte3600_capture_io        *output);

struct fte3600_capture_bridge
{
  struct fte3600_capture_state       state;
  struct fte3600_capture_io          io;
  /* Most recent valid acquisition only, including WORD_READ_ERROR. */
  struct fte3600_capture_diagnostics diagnostics;
  /* Since initialization or explicit reset; initialize to zero before use.
   * Counts saturate at UINT32_MAX; last_failure retains the latest nonzero
   * failure; ready_timeout retains any timeout across acquisitions. */
  struct fte3600_capture_diagnostics cumulative_diagnostics;
};

/* Start a diagnostic interval, for example before image_search or refine.
 * Clears only cumulative_diagnostics, preserving state, io and diagnostics.
 * No callbacks; NULL returns INVALID_ARGUMENT without changes. */
enum fte3600_capture_status fte3600_capture_bridge_reset_cumulative (struct fte3600_capture_bridge *bridge);

/* Direct fte3600_image_capture_fn implementation. The context is a bridge.
 * Writes requested_dac on every call and retains acquisition state. Successful
 * compatibility capture decodes one frame high-byte first. Local errors leave
 * pixels unchanged; valid calls retain state changes made before an error.
 * Every valid acquisition, including WORD_READ_ERROR, updates both diagnostics
 * fields. Invalid arguments change neither field. Inspect cumulative_diagnostics
 * after an operation that invokes this callback more than once. */
int fte3600_capture_image (void    *context,
                           uint8_t  requested_dac,
                           uint16_t pixels[FTE3600_IMAGE_PIXELS]);

#ifdef __cplusplus
}
#endif
#endif
