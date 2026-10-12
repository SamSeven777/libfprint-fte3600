/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_FT93XX_IMAGE_PIPELINE_H
#define FTE3600_FT93XX_IMAGE_PIPELINE_H

#include "fte3600_ft93xx.h"

#ifdef __cplusplus
extern "C" {
#endif

enum fte3600_ft93xx_pipeline_action {
  FTE3600_FT93XX_PIPELINE_CAPTURE,
  FTE3600_FT93XX_PIPELINE_DIFFERENCE,
  FTE3600_FT93XX_PIPELINE_EDGE_BLEND,
  FTE3600_FT93XX_PIPELINE_WHITE_POINTS,
  FTE3600_FT93XX_PIPELINE_NORMALIZE,
  FTE3600_FT93XX_PIPELINE_NOISE
};

enum fte3600_ft93xx_pipeline_status {
  FTE3600_FT93XX_PIPELINE_OK = 0,
  FTE3600_FT93XX_PIPELINE_INVALID_ARGUMENT = 1,
  FTE3600_FT93XX_PIPELINE_TRANSPORT_ERROR = 2,
  FTE3600_FT93XX_PIPELINE_BAD_CRC = 4,
  FTE3600_FT93XX_PIPELINE_UNSUPPORTED_DEVICE = 5,
  FTE3600_FT93XX_PIPELINE_RESOURCE_ERROR = 9,
  FTE3600_FT93XX_PIPELINE_FLOAT_ENVIRONMENT_ERROR = 10
};

struct fte3600_ft93xx_processing_state
{
  uint16_t integration_default;
  uint8_t  integration_limited;
};

struct fte3600_ft93xx_pipeline_buffers
{
  uint8_t  *raw;
  size_t    raw_bytes;
  uint16_t *processed;
  size_t    processed_samples;
  uint8_t  *image;
  size_t    image_bytes;
};

struct fte3600_ft93xx_pipeline_diagnostics
{
  struct fte3600_ft93xx_diagnostics transport;
  uint32_t                          captures;
  uint8_t                           overflow, noise_applied, completed;
};

/* Defaults preserve output on invalid/unknown input. No I/O. */
enum fte3600_ft93xx_pipeline_status fte3600_ft93xx_processing_defaults (uint16_t                                chip_id,
                                                                        uint8_t                                 version,
                                                                        struct fte3600_ft93xx_processing_state *processing);

/* Size in bytes for caller-owned workspace. Returns zero for edge, white,
 * noise and difference; unknown IDs/actions also return zero. CAPTURE and
 * NORMALIZE need the returned size with alignment suitable for uint32_t and
 * float (malloc alignment suffices). There is no hidden allocation. */
size_t fte3600_ft93xx_pipeline_workspace_size (uint16_t                            chip_id,
                                               enum fte3600_ft93xx_pipeline_action action);

/* Run the reviewed image pipeline for the admitted model width/height.
 * CAPTURE and DIFFERENCE use count=1 physical acquisition. raw_bytes must
 * cover width*(height+extra_rows)*2, including extra rows. The visible raw
 * prefix is host uint16_t on success, and extra rows remain wire-order bytes.
 * Read raw samples with memcpy; raw has no alignment requirement.
 *
 * All operations require geometry, state, processing and buffers. CAPTURE,
 * DIFFERENCE, EDGE_BLEND, WHITE_POINTS and NORMALIZE need N processed samples;
 * CAPTURE, NORMALIZE and NOISE need N output bytes. NOISE operates directly on
 * image; processed is ignored. Scalar operations never touch raw or state.
 * Scalar 16-bit inputs must be in 0..4092. Extra buffer capacity is untouched.
 * Physical operations require spi/wait callbacks and the existing valid raw
 * acquisition state/extra-row domains, with integration admitting the entire
 * u16 domain per the retained integration addendum. Scalar operations ignore io.
 *
 * Caller serializes access and supplies valid, nonoverlapping objects.
 * Invalid input, insufficient capacity/workspace or non-nearest FP rounding
 * leave ALL outputs/state/diagnostics unchanged, without I/O. CAPTURE and
 * NORMALIZE require IEEE binary32 round-to-nearest. Compile this translation
 * unit without fast-math/reassociation or FP contraction (supplied makefile).
 *
 * After valid entry diagnostics clear once. Transport/CRC failure stops with
 * raw prefix and state changes from acquisition retained, but processed,
 * image and processing state unchanged. A successful DIFFERENCE updates raw,
 * processed and integration but leaves image untouched. Integration arithmetic
 * uses u16 wrap and preserves noncanonical integration_limited unless set to 1.
 * A later pipeline acquisition uses wrapped retained integration unchanged.
 *
 * OK is compatibility zero for CAPTURE, not a quality/calibration guarantee.
 * Other actions have no compatibility integer return; this API always reports
 * a typed local status. completed records actual processing completion. */
enum fte3600_ft93xx_pipeline_status fte3600_ft93xx_pipeline_run (uint16_t                                      chip_id,
                                                                 uint16_t                                      raw_id,
                                                                 const struct fte3600_ft93xx_geometry         *geometry,
                                                                 struct fte3600_ft93xx_state                  *state,
                                                                 struct fte3600_ft93xx_processing_state       *processing,
                                                                 enum fte3600_ft93xx_pipeline_action           action,
                                                                 const struct fte3600_ft93xx_pipeline_buffers *buffers,
                                                                 void                                         *workspace,
                                                                 size_t                                        workspace_bytes,
                                                                 const struct fte3600_ft93xx_io               *io,
                                                                 struct fte3600_ft93xx_pipeline_diagnostics   *diagnostics);

#ifdef __cplusplus
}
#endif
#endif
