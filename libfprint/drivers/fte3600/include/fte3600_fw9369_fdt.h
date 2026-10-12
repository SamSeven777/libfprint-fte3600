/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_FW9369_FDT_H
#define FTE3600_FW9369_FDT_H

#include "fte3600_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FTE3600_FW9369_FDT_STATE_INITIALIZER {27, 0, 0, 0, 0, 0, 0, 0, 0}
#define FTE3600_FW9369_FDT_CONFIG_INITIALIZER \
  {1, 0, 0, 120, 1, 60, 200, 4, 4, 50, 45}

struct fte3600_fw9369_fdt_state
{
  uint8_t dac, valid, calibrating, dac_cache, calibrating_cache;
  uint8_t mode, configured, fdt_configured, check_registers;
};

struct fte3600_fw9369_fdt_config
{
  uint8_t channel_mode, foundry, detect_state, fdt_rows;
  uint8_t integration_multiplier, rate, fast_rate, repeat, average;
  uint8_t threshold_low, threshold_high;
};

enum fte3600_fw9369_fdt_status {
  FTE3600_FW9369_FDT_OK = 0,
  FTE3600_FW9369_FDT_FAILED = 1,
  FTE3600_FW9369_FDT_INVALID_ARGUMENT = 2,
  FTE3600_FW9369_FDT_WORD_READ_ERROR = 3,
  FTE3600_FW9369_FDT_ADC_READ_ERROR = 4
};

/* Shared image settings for composed startup operations. NULL retains the
 * standalone FDT prefix defaults (DAC 0, shift 2, period 200). */
struct fte3600_fw9369_fdt_image_config
{
  uint8_t  dac, shift;
  uint16_t period;
};
enum fte3600_fw9369_fdt_operation {
  FTE3600_FW9369_FDT_CONFIGURE,
  FTE3600_FW9369_FDT_MEASURE,
  FTE3600_FW9369_FDT_CALIBRATE
};
/* CONFIGURE stops before manual trigger/poll/read. MEASURE requires capacity
 * for the selected channels; other operations ignore output and capacity.
 * State, diagnostic and local-read-error rules below also apply here. */
enum fte3600_fw9369_fdt_status fte3600_fw9369_fdt_run (enum fte3600_fw9369_fdt_operation             operation,
                                                       struct fte3600_fw9369_fdt_state              *state,
                                                       const struct fte3600_fw9369_fdt_config       *config,
                                                       const struct fte3600_fw9369_fdt_image_config *image,
                                                       uint16_t                                     *channels,
                                                       size_t                                        capacity,
                                                       const struct fte3600_capture_io              *io,
                                                       struct fte3600_capture_diagnostics           *diagnostics);

/* Initialized FW9369 only; synthetic defaults are not power-on defaults.
 * Config requires channel_mode 0/1 and nonzero rate and fast_rate. mode 1
 * reads four channels, mode 0 eight. All state fields retain their byte values.
 * Read/measure capacity is in uint16_t channels and may exceed the count;
 * extra elements remain unchanged. Read only needs config; it does no scan.
 *
 * Invalid arguments invoke no callbacks and leave all outputs unchanged.
 * Diagnostics are optional, reset after validation, and aggregate the entire
 * call including every calibration measurement and image configuration.
 * ready_timeout retains any five-poll exhaustion; OK alone is not hardware
 * success. Other exhausted commands/writes/byte reads continue by contract.
 * Word/ADC read exhaustion stops immediately with a distinct local error;
 * read/measure output remains unchanged. Earlier state and I/O remain.
 *
 * Calibration clears calibrating even on a local read error; retains valid on
 * all failures and requested DAC on sampling errors. Retained DAC may differ
 * from dac_cache at successful return; no extra scan or analog restoration is
 * made. The image configuration prefix uses DAC 0, shift 2, period 200.
 *
 * No allocations or mutable globals. Objects must be valid/nonoverlapping;
 * caller serializes device access and callbacks must not mutate state/config.
 * The existing recovery/Linux adapter can supply this capture_io interface. */
enum fte3600_fw9369_fdt_status fte3600_fw9369_fdt_scan (struct fte3600_fw9369_fdt_state        *state,
                                                        const struct fte3600_fw9369_fdt_config *config,
                                                        const struct fte3600_capture_io        *io,
                                                        struct fte3600_capture_diagnostics     *diagnostics);

enum fte3600_fw9369_fdt_status fte3600_fw9369_fdt_read (const struct fte3600_fw9369_fdt_config *config,
                                                        uint16_t                               *channels,
                                                        size_t                                  capacity,
                                                        const struct fte3600_capture_io        *io,
                                                        struct fte3600_capture_diagnostics     *diagnostics);

enum fte3600_fw9369_fdt_status fte3600_fw9369_fdt_measure (struct fte3600_fw9369_fdt_state        *state,
                                                           const struct fte3600_fw9369_fdt_config *config,
                                                           uint16_t                               *channels,
                                                           size_t                                  capacity,
                                                           const struct fte3600_capture_io        *io,
                                                           struct fte3600_capture_diagnostics     *diagnostics);

enum fte3600_fw9369_fdt_status fte3600_fw9369_fdt_calibrate (struct fte3600_fw9369_fdt_state        *state,
                                                             const struct fte3600_fw9369_fdt_config *config,
                                                             const struct fte3600_capture_io        *io,
                                                             struct fte3600_capture_diagnostics     *diagnostics);

#ifdef __cplusplus
}
#endif
#endif
