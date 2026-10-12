/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_IMAGE_H
#define FTE3600_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FTE3600_IMAGE_WIDTH 64
#define FTE3600_IMAGE_HEIGHT 80
#define FTE3600_IMAGE_PIXELS (FTE3600_IMAGE_WIDTH * FTE3600_IMAGE_HEIGHT)
#define FTE3600_IMAGE_REGION_PIXELS 4680
#define FTE3600_IMAGE_MAX_SEARCH_CAPTURES 30
#define FTE3600_IMAGE_MAX_CAPTURES 32

struct fte3600_image_statistics
{
  uint16_t median;
  uint16_t low_count;
  uint16_t high_count;
};

struct fte3600_image_state
{
  uint8_t  dac;
  bool     initialized;
  uint16_t stored_median;
  uint8_t  auxiliary_state;
};

/* Independent project API values, not a device or vendor ABI. */
enum fte3600_image_status {
  FTE3600_IMAGE_OK = 0,
  FTE3600_IMAGE_FAILED = 1,
  FTE3600_IMAGE_INVALID_ARGUMENT = 2,
  FTE3600_IMAGE_CAPTURE_ERROR = 3
};

/*
 * Fill all 5,120 row-major pixels and return zero. A nonzero return reports a
 * local provider failure. The buffer is borrowed only until callback return.
 * No hardware operations, timing, or device transport are provided here.
 */
typedef int (*fte3600_image_capture_fn)(void    *context,
                                        uint8_t  requested_dac,
                                        uint16_t pixels[FTE3600_IMAGE_PIXELS]);

/* Both pointers are required; invalid arguments leave output unchanged. */
enum fte3600_image_status fte3600_image_measure (const uint16_t                   pixels[FTE3600_IMAGE_PIXELS],
                                                 struct fte3600_image_statistics *statistics);

/*
 * Implement the fixed profile in specs/image-v1.md. All pointers other than
 * context are required. The caller supplies a mutable image buffer and keeps
 * it, state, and context valid for the duration of the call. State, image,
 * and callback context must not overlap; the callback must not mutate state.
 * Calls using separate state/buffers/contexts are independent. The functions
 * allocate no memory and use no mutable globals.
 *
 * Search uses at most 30 search captures and 32 total captures, initializes
 * an uninitialized DAC to 54, and retains initialization even on failure.
 * FAILED is a completed search without crossing; stored_median is unchanged.
 * Search leaves the last captured image in pixels (an additional local API
 * exposure, absent from the reviewed JSON search interface).
 *
 * Refinement uses zero or one capture and returns OK for both accepted and
 * rejected trials. A rejected trial restores only DAC: its pixels remain in
 * the buffer, and stored_median remains unchanged. Initialization and opaque
 * auxiliary state are never changed by refinement.
 *
 * INVALID_ARGUMENT and CAPTURE_ERROR are local extensions, not reference-
 * validated behavior. Invalid arguments have no side effects. Capture failure
 * stops immediately, leaves DAC at the failed request, preserves other state
 * at the point of that request, and leaves any callback-written pixels in
 * the buffer. A failed refinement capture does not restore the earlier DAC.
 */
enum fte3600_image_status fte3600_image_search (struct fte3600_image_state *state,
                                                uint16_t                    pixels[FTE3600_IMAGE_PIXELS],
                                                fte3600_image_capture_fn    capture,
                                                void                       *context);

enum fte3600_image_status fte3600_image_refine (struct fte3600_image_state *state,
                                                uint16_t                    pixels[FTE3600_IMAGE_PIXELS],
                                                fte3600_image_capture_fn    capture,
                                                void                       *context);

#ifdef __cplusplus
}
#endif

#endif
