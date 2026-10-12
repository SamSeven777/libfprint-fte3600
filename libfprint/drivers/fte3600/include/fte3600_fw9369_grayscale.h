/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_FW9369_GRAYSCALE_H
#define FTE3600_FW9369_GRAYSCALE_H
#include "fte3600_fw9369_startup.h"

#ifdef __cplusplus
extern "C" {
#endif

enum fte3600_fw9369_grayscale_action {
  FTE3600_FW9369_GRAYSCALE_CAPTURE,
  FTE3600_FW9369_GRAYSCALE_WHITE_POINTS,
  FTE3600_FW9369_GRAYSCALE_NORMALIZE
};
enum fte3600_fw9369_grayscale_status {
  FTE3600_FW9369_GRAYSCALE_OK = 0,
  FTE3600_FW9369_GRAYSCALE_INVALID_ARGUMENT = 1,
  FTE3600_FW9369_GRAYSCALE_RESOURCE_ERROR = 2,
  FTE3600_FW9369_GRAYSCALE_WORD_READ_ERROR = 3,
  FTE3600_FW9369_GRAYSCALE_FLOAT_ENVIRONMENT_ERROR = 4
};
enum fte3600_fw9369_grayscale_stage {
  FTE3600_FW9369_GRAYSCALE_BEFORE_WHITE,
  FTE3600_FW9369_GRAYSCALE_BEFORE_NORMALIZE,
  FTE3600_FW9369_GRAYSCALE_FINAL_WORDS
};

struct fte3600_fw9369_grayscale_buffers
{
  const uint16_t *input;
  size_t          input_samples;
  uint16_t       *processed;
  size_t          processed_samples;
  uint8_t        *output;
  size_t          output_bytes;
};
struct fte3600_fw9369_grayscale_workspace
{
  uint8_t raw[FTE3600_CAPTURE_FRAME_BYTES];
  float   histogram[256], scratch[256];
};
struct fte3600_fw9369_grayscale_diagnostics
{
  struct fte3600_capture_diagnostics transport;
  uint8_t                            completed;
};
struct fte3600_fw9369_grayscale_observer
{
  void *context;
  /* Synchronous, read-only checkpoint; cannot retain or modify objects. */
  void  (*stage)(void                               *context,
                 enum fte3600_fw9369_grayscale_stage stage,
                 const uint16_t                     *pixels,
                 size_t                              samples);
};

/* Implements specs/fw9369-grayscale-v1.md for width=64, height=80 only.
 * CAPTURE requires state and physical spi/wait callbacks. It always acquires
 * one frame using state.image_dac, independently of retained image_count.
 * It preserves baseline, work_image, CRC, ready flags and unrelated state;
 * acquisition's mode/configured/check_registers/dac_cache changes are retained.
 * Scalar actions ignore state/io, require exactly 5120 immutable input samples,
 * and produce a separate 5120-word processed image. CAPTURE ignores input.
 * All actions need processed; CAPTURE/NORMALIZE also need 5120 output bytes.
 * WHITE_POINTS ignores output. Extra capacity is untouched.
 *
 * CAPTURE/NORMALIZE require a workspace of at least sizeof(*workspace) bytes
 * with its declared alignment; WHITE_POINTS ignores workspace. There is no
 * allocation or mutable global state. Insufficient output/workspace capacity
 * is RESOURCE_ERROR; invalid dimensions/lengths/pointers or buffer overlaps
 * are INVALID_ARGUMENT. Every preflight failure preserves all objects and
 * invokes no I/O or observer. Normalization requires nearest-even IEEE
 * binary32/binary64; other rounding modes return FLOAT_ENVIRONMENT_ERROR.
 * Compile with the supplied floating-point flags, without reassociation/FMA.
 *
 * After valid entry, optional diagnostics clear once. A WORD_READ_ERROR from
 * acquisition retains transport diagnostics and completed acquisition state
 * changes, while preserving processed/output; no observer runs. Other exhausted
 * retries retain the existing capture compatibility semantics and diagnostics.
 * CRC here means retained baseline CRC; physical FW9369 reads keep their
 * existing protocol behavior. OK is not a calibration or hardware guarantee.
 *
 * All used objects must be valid, correctly aligned and nonoverlapping. Active
 * pixel/workspace/state/diagnostic ranges are checked for overlap; caller also
 * ensures buffer/observer/io descriptors and callback context do not alias them.
 * Callbacks may not mutate or retain any objects. Caller serializes access.
 */
enum fte3600_fw9369_grayscale_status fte3600_fw9369_grayscale_run (enum fte3600_fw9369_grayscale_action            action,
                                                                   size_t                                          width,
                                                                   size_t                                          height,
                                                                   struct fte3600_fw9369_startup_state            *state,
                                                                   const struct fte3600_fw9369_grayscale_buffers  *buffers,
                                                                   struct fte3600_fw9369_grayscale_workspace      *workspace,
                                                                   size_t                                          workspace_bytes,
                                                                   const struct fte3600_capture_io                *io,
                                                                   struct fte3600_fw9369_grayscale_diagnostics    *diagnostics,
                                                                   const struct fte3600_fw9369_grayscale_observer *observer);

#ifdef __cplusplus
}
#endif
#endif
