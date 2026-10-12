/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_FW9369_STARTUP_H
#define FTE3600_FW9369_STARTUP_H
#include "fte3600_fw9369_fdt.h"
#ifdef __cplusplus
extern "C" {
#endif

struct fte3600_fw9369_startup_state
{
  struct fte3600_fw9369_fdt_state  fdt;
  struct fte3600_fw9369_fdt_config config;
  uint8_t                          image_dac, image_valid;
  uint16_t                         image_median;
  uint8_t                          baseline_flag_1, baseline_flag_2;
  uint16_t                         baseline_crc;
  uint8_t                          arm_mode;
  uint16_t                         fdt_down_crc, fdt_up_crc, period;
  uint8_t                          image_count, shift;
  uint16_t                         fdt_reference[8], fdt_down[8], fdt_up[8];
  uint16_t                         baseline[FTE3600_IMAGE_PIXELS], work_image[FTE3600_IMAGE_PIXELS];
};
#define FTE3600_FW9369_STARTUP_STATE_INITIALIZER \
  { \
    .fdt = FTE3600_FW9369_FDT_STATE_INITIALIZER, \
    .config = FTE3600_FW9369_FDT_CONFIG_INITIALIZER, \
    .image_dac = 54, .image_median = 512, .period = 200, \
    .image_count = 1, .shift = 2 }

struct fte3600_fw9369_startup_workspace
{
  uint8_t  raw[FTE3600_CAPTURE_FRAME_BYTES];
  uint16_t image[FTE3600_IMAGE_PIXELS];
};
enum fte3600_fw9369_startup_operation {
  FTE3600_FW9369_STARTUP,
  FTE3600_FW9369_BASELINES,
  FTE3600_FW9369_FDT_BASELINE,
  FTE3600_FW9369_IMAGE_BASELINE
};
enum fte3600_fw9369_startup_status {
  FTE3600_FW9369_STARTUP_OK,
  FTE3600_FW9369_STARTUP_FAILED,
  FTE3600_FW9369_STARTUP_INVALID_ARGUMENT,
  FTE3600_FW9369_STARTUP_WORD_READ_ERROR,
  FTE3600_FW9369_STARTUP_ADC_READ_ERROR,
  FTE3600_FW9369_STARTUP_UNDEFINED_BASELINE,
  FTE3600_FW9369_STARTUP_UNDEFINED_NEIGHBORHOOD,
  FTE3600_FW9369_STARTUP_FIFO_READ_ERROR
};
enum fte3600_fw9369_startup_warning {
  FTE3600_FW9369_SPI_MODE_EXHAUSTED = 1,
  FTE3600_FW9369_UNRECOGNIZED_ID = 2,
  FTE3600_FW9369_UNRECOGNIZED_FOUNDRY = 4,
  FTE3600_FW9369_FDT_BASELINE_FAILED = 8,
  FTE3600_FW9369_IMAGE_BASELINE_FAILED = 16,
  FTE3600_FW9369_LOCAL_ERROR = 32
};
struct fte3600_fw9369_startup_diagnostics
{
  struct fte3600_capture_diagnostics transport;
  uint32_t                           warnings, calibration_attempts, calibration_failures;
  /* completed: bit 0 FDT stage, bit 1 image stage, bit 2 automatic arm.
   * Status fields meaningful when corresponding stage completed, including
   * a typed local error. A later successful stage never erases a failure. */
  uint32_t completed, fdt_status, image_status;
};

enum fte3600_fw9369_startup_phase {
  FTE3600_FW9369_PHASE_WAKE, FTE3600_FW9369_PHASE_FDT,
  FTE3600_FW9369_PHASE_IMAGE, FTE3600_FW9369_PHASE_ARM
};
struct fte3600_fw9369_startup_observer
{
  void *context;
  /* Passive, synchronous entry/exit notification. Status is meaningful on
   * exit only. The callback must not mutate or reenter this operation's
   * state, workspace, transport or diagnostics; it may inspect them. */
  void (*phase)(void *,
                enum fte3600_fw9369_startup_phase,
                int entering,
                enum fte3600_fw9369_startup_status);
};

/* Additive full-startup entry with passive stage observations. Identical to
 * STARTUP through startup_run when observer is NULL. The observer and context
 * remain caller-owned and valid until return. All existing storage, validation
 * and serialization requirements apply. */
enum fte3600_fw9369_startup_status fte3600_fw9369_startup_observed (struct fte3600_fw9369_startup_state          *state,
                                                                    struct fte3600_fw9369_startup_workspace      *workspace,
                                                                    size_t                                        capacity,
                                                                    const struct fte3600_capture_io              *io,
                                                                    const struct fte3600_fw9369_startup_observer *observer,
                                                                    struct fte3600_fw9369_startup_diagnostics    *diagnostics);

/* Full startup with the same passive observations and an additional local
 * FIFO exhaustion guard. Missing FIFO responses never enter image calibration
 * or baseline storage. Earlier state and diagnostics remain; no rollback or
 * original compatibility result is claimed for FIFO_READ_ERROR. This guard
 * is independent of whether observer is supplied. Older entries are unchanged. */
enum fte3600_fw9369_startup_status fte3600_fw9369_startup_checked (struct fte3600_fw9369_startup_state          *state,
                                                                   struct fte3600_fw9369_startup_workspace      *workspace,
                                                                   size_t                                        capacity,
                                                                   const struct fte3600_capture_io              *io,
                                                                   const struct fte3600_fw9369_startup_observer *observer,
                                                                   struct fte3600_fw9369_startup_diagnostics    *diagnostics);

/* Bounded FW9369 startup after board/SPI initialization; no GPIO or IRQ setup.
 * All stages use physical SPI, with no high-level success provider. STARTUP
 * returns OK after defined calibration failures, which remain in diagnostics.
 * FAILED maps to compatibility FFFFFFFF; local typed errors have no defined
 * compatibility value. No successful FDT calibration retains old thresholds
 * and stops with UNDEFINED_BASELINE. Other failures retain completed effects.
 *
 * Standalone stages admit channel_mode 1 and nonzero rates; STARTUP admits
 * initial channel_mode 0/1 and replaces factory settings before calibration.
 * Only sample_limit 10 and reset 1 are supported for every operation.
 * Workspace capacity is bytes, at least sizeof(*workspace); excess untouched.
 * No allocation/global state. Required pointers must be valid, nonoverlapping,
 * correctly aligned; callbacks must not mutate them. Caller serializes access.
 * Invalid arguments perform no callbacks and change no state/storage/diagnostic.
 * Valid calls reset diagnostics once, then accumulate through all stages. */
enum fte3600_fw9369_startup_status fte3600_fw9369_startup_run (enum fte3600_fw9369_startup_operation      operation,
                                                               struct fte3600_fw9369_startup_state       *state,
                                                               uint8_t                                    sample_limit,
                                                               uint8_t                                    reset,
                                                               struct fte3600_fw9369_startup_workspace   *workspace,
                                                               size_t                                     capacity,
                                                               const struct fte3600_capture_io           *io,
                                                               struct fte3600_fw9369_startup_diagnostics *diagnostics);

/* Retained image baseline: reset == 1 searches the DAC then replaces baseline;
 * every other byte skips search and applies the reviewed in-place warm update.
 * Acquires max(1, image_count) frames and sums raw samples modulo 65536.
 * UNDEFINED_NEIGHBORHOOD is a local safety error, with no compatibility return:
 * it retains baseline, CRC and ready flags, but preserves acquired work and
 * capture state/diagnostics. Reset 1 does not need that neighborhood guard.
 * Same storage, callback, channel/rate and invalid-argument rules as above.
 * Repeated calls retain state; the caller retains physical response cursors.
 * Diagnostics describe this call and reset at its start. */
enum fte3600_fw9369_startup_status fte3600_fw9369_image_baseline_run (struct fte3600_fw9369_startup_state       *state,
                                                                      uint8_t                                    reset,
                                                                      struct fte3600_fw9369_startup_workspace   *workspace,
                                                                      size_t                                     capacity,
                                                                      const struct fte3600_capture_io           *io,
                                                                      struct fte3600_fw9369_startup_diagnostics *diagnostics);
/* Re-arm retained automatic finger detection using calibrated thresholds.
 * No measurement, image baseline update, reset or allocation. channel_mode
 * must be 1 and rate/fast_rate nonzero. Same nonoverlap/serialized ownership
 * rules as startup. Diagnostics clear on valid entry and report transport
 * exhaustion separately from compatibility success; completed bit 2 means
 * arm sequence completed. Baselines and their CRC/flags remain unchanged. */
enum fte3600_fw9369_startup_status fte3600_fw9369_startup_arm (struct fte3600_fw9369_startup_state *,
                                                               const struct fte3600_capture_io *,
                                                               struct fte3600_fw9369_startup_diagnostics *);
#ifdef __cplusplus
}
#endif
#endif
