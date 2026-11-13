/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_FT93XX_H
#define FTE3600_FT93XX_H

#include "fte3600_recovery.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FTE3600_FT93XX_MAX_TRANSFER ((size_t) 1798)

enum fte3600_ft93xx_status {
  FTE3600_FT93XX_OK = 0,
  FTE3600_FT93XX_INVALID_ARGUMENT = 1,
  FTE3600_FT93XX_TRANSPORT_ERROR = 2,
  FTE3600_FT93XX_EFUSE_ERROR = 3,
  FTE3600_FT93XX_BAD_CRC = 4,
  FTE3600_FT93XX_UNSUPPORTED_DEVICE = 5,
  FTE3600_FT93XX_FDT_SEARCH_FAILED = 6,
  FTE3600_FT93XX_FDT_INVALID_BASE = 7,
  FTE3600_FT93XX_IMAGE_SEARCH_FAILED = 8
};

struct fte3600_ft93xx_geometry
{
  uint16_t width, height, extra_rows;
};

struct fte3600_ft93xx_state
{
  uint8_t  dac, gain, dac_polarity;
  uint16_t integration;
  uint8_t  sample_time, sample_mode, subtract_baseline;
  uint8_t  mode, configured, check_registers;
};

struct fte3600_ft93xx_io
{
  void *context;
  /* Synchronous shared TX/RX buffer; success fills requested reply bytes.
   * High status bit denotes failure (signed 32-bit semantics). Both
   * callbacks are required and must return; neither may retain buffers.
   * The library restores deterministic TX bytes before each retry. */
  uint32_t (*spi)(void    *context,
                  uint8_t *buffer,
                  size_t   length);
  void (*wait_ms)(void    *context,
                  uint32_t milliseconds);
};

struct fte3600_ft93xx_diagnostics
{
  size_t   raw_bytes_completed;
  uint32_t failed_attempts, exhausted_transactions, last_failure;
  uint8_t  idle_unacknowledged, mode_unacknowledged, ready_unacknowledged;
  uint8_t  voltage_unacknowledged, spi_mode_unacknowledged;
};

struct fte3600_ft93xx_identity
{
  uint16_t chip_id, raw_id;
};

#define FTE3600_FT93XX_SERIAL_SIZE ((size_t) 17)

struct fte3600_ft93xx_preparation
{
  struct fte3600_ft93xx_identity identity;
  struct fte3600_ft93xx_geometry geometry;
  uint8_t                        foundry, version, features;
  uint8_t                        serial[17];
  struct fte3600_ft93xx_state    state;
  uint8_t                        calibration_pending;
};

struct fte3600_ft93xx_fdt_state
{
  uint8_t fdt_dac, fdt_configured, threshold_mode, threshold_cached;
};

/* Independent controller storage; the existing analog state ABI is retained. */
struct fte3600_ft93xx_fdt_controller
{
  struct fte3600_ft93xx_fdt_state analog;
  uint8_t                         default_dac, dac_calibrated;
  uint16_t                        replacement;
  uint8_t                         damaged, max_dac, step;
  uint8_t                         margin;
  uint8_t                         armed_direction;
  uint16_t                        base_up[8], base_down[8];
  uint16_t                        base_up_checksum, base_down_checksum;
};

enum fte3600_ft93xx_fdt_action {
  FTE3600_FT93XX_FDT_MEASURE,
  FTE3600_FT93XX_FDT_READ,
  FTE3600_FT93XX_FDT_SAMPLE,
  FTE3600_FT93XX_FDT_SEARCH,
  FTE3600_FT93XX_FDT_VALIDATE_BASE,
  FTE3600_FT93XX_FDT_ADJUST_BASE,
  FTE3600_FT93XX_FDT_BASELINE,
  FTE3600_FT93XX_FDT_WRITE_BASE,
  FTE3600_FT93XX_FDT_ARM
};

struct fte3600_ft93xx_fdt_diagnostics
{
  struct fte3600_ft93xx_diagnostics transport;
  uint32_t                          measurements, readiness_timeouts, search_attempts, search_failures;
  uint32_t                          baseline_rounds;
  uint8_t                           baseline_valid, adjustment_fallback;
};

enum fte3600_ft93xx_status fte3600_ft93xx_fdt_controller_defaults (uint16_t                              chip_id,
                                                                   uint8_t                               foundry,
                                                                   uint8_t                               version,
                                                                   struct fte3600_ft93xx_fdt_controller *controller);

/* Execute a physical FDT operation, without allocation or global state.
 * argument is count 0..255 for BASELINE, direction 0..1 for ARM, zero otherwise.
 * values holds >=4 uint16_t for READ/SAMPLE/VALIDATE_BASE/ADJUST_BASE, >=8 for
 * WRITE_BASE, and is ignored otherwise. Only READ/SAMPLE/ADJUST_BASE modify it.
 * state/controller and io are required (including VALIDATE_BASE, which has no
 * I/O). Unknown IDs, invalid state, zero step, max_dac<2 or bad arguments leave
 * all state, output and diagnostics unchanged. All objects must not overlap.
 *
 * A completed failed SEARCH returns FDT_SEARCH_FAILED; VALIDATE_BASE outside
 * 500..900 returns FDT_INVALID_BASE (both correspond to compatibility ffffffff).
 * Other completed operations correspond to compatibility zero where defined.
 * Readiness exhaustion continues to data read; inspect readiness_timeouts.
 * BASELINE can return OK despite failed searches/minima: inspect diagnostics.
 * Local transport/CRC errors stop immediately, preserving completed changes;
 * no undefined bytes are consumed and no recovery I/O is invented. SEARCH
 * clears threshold_mode on every non-invalid exit, including local errors.
 * CRC validation accepts zero or correct CRC; bad nonzero CRC returns BAD_CRC
 * without the out-of-scope CRC recovery path. Diagnostics are optional and
 * cleared once per valid call, then accumulated across nested operations. */
enum fte3600_ft93xx_status fte3600_ft93xx_fdt_run (uint16_t                               chip_id,
                                                   uint8_t                                foundry,
                                                   uint32_t                               features,
                                                   struct fte3600_ft93xx_state           *state,
                                                   struct fte3600_ft93xx_fdt_controller  *controller,
                                                   enum fte3600_ft93xx_fdt_action         action,
                                                   unsigned int                           argument,
                                                   uint16_t                              *values,
                                                   size_t                                 value_count,
                                                   const struct fte3600_ft93xx_io        *io,
                                                   struct fte3600_ft93xx_fdt_diagnostics *diagnostics);

struct fte3600_ft93xx_image_controller
{
  uint8_t  nav_ready, default_dac, initialized;
  uint16_t statistic;
  uint8_t  max_dac, step;
};

enum fte3600_ft93xx_image_action {
  FTE3600_FT93XX_IMAGE_CAPTURE,
  FTE3600_FT93XX_IMAGE_STATISTIC,
  FTE3600_FT93XX_IMAGE_TAILS,
  FTE3600_FT93XX_IMAGE_REFINE,
  FTE3600_FT93XX_IMAGE_SEARCH,
  FTE3600_FT93XX_IMAGE_BASELINE
};

struct fte3600_ft93xx_image_result
{
  uint16_t statistic, left, right;
};

struct fte3600_ft93xx_image_diagnostics
{
  struct fte3600_ft93xx_diagnostics transport;
  uint32_t                          captures, mode_timeouts, readiness_timeouts;
  uint32_t                          search_attempts, search_failures, refinement_captures;
};

/* Largest reviewed full profile, in uint16_t samples. Identity is discovered
 * by I/O, so capacity is checked against this bound before any transaction. */
#define FTE3600_FT93XX_STARTUP_SAMPLES ((size_t) 10240)

struct fte3600_ft93xx_final_state
{
  uint8_t  startup_fdt_dac;
  uint16_t saved_chip_id, saved_width, saved_height;
};

struct fte3600_ft93xx_device
{
  struct fte3600_ft93xx_preparation      preparation;
  struct fte3600_ft93xx_fdt_controller   fdt;
  struct fte3600_ft93xx_image_controller image;
  struct fte3600_ft93xx_final_state final;
};

enum fte3600_ft93xx_startup_phase {
  FTE3600_FT93XX_STARTUP_FDT_BASELINE,
  FTE3600_FT93XX_STARTUP_IMAGE_BASELINE,
  FTE3600_FT93XX_STARTUP_AUTOMATIC_ARM,
  FTE3600_FT93XX_STARTUP_FINALIZE
};

struct fte3600_ft93xx_startup_observer
{
  void *context;
  /* Passive synchronous checkpoint; must not mutate device, I/O or pixels.
   * The first three precede their stage; FINALIZE follows saved-state updates. */
  void (*phase)(void                               *context,
                enum fte3600_ft93xx_startup_phase   phase,
                const struct fte3600_ft93xx_device *device);
};

struct fte3600_ft93xx_startup_diagnostics
{
  struct fte3600_ft93xx_diagnostics       transport; /* Sum/OR across all stages. */
  struct fte3600_ft93xx_diagnostics       preparation;
  struct fte3600_ft93xx_fdt_diagnostics   fdt, arm;
  struct fte3600_ft93xx_image_diagnostics image;
  uint8_t                                 completed;
};

/* Cold initialization, without hardware or allocation. Retained invocations
 * reuse the resulting device or override its fields before full_startup. */
enum fte3600_ft93xx_status fte3600_ft93xx_device_defaults (struct fte3600_ft93xx_device *device);

/* Full reviewed preparation -> baseline(10) -> image baseline -> arm(1).
 * Caller owns device and >= STARTUP_SAMPLES host uint16_t pixels. No allocation.
 * Caller serializes access and supplies valid, nonoverlapping objects. Invalid
 * arguments leave state, pixels and diagnostics untouched and invoke no I/O.
 * Common check_registers 0..2 and retained FDT threshold_mode/dac_calibrated/
 * damaged and image initialized 0..1 are validated; overwritten scalar fields
 * may contain any values of their declared types. Identity always comes from
 * physical probe replies. Each stage shares common state and the same I/O.
 * OK is compatibility zero, not proof of calibration success: inspect each
 * stage's search_failures, baseline_valid and timeout/fault diagnostics.
 * Later stages never clear earlier diagnostics. Transport raw_bytes_completed
 * is the last navigation capture's copied prefix. Local transport/CRC errors
 * stop immediately with partial state and completed=0; unknown ID stops before
 * calibration. Successful return records identity/DAC and clears both
 * calibration flags. Pixels outside the navigation prefix remain unchanged. */
enum fte3600_ft93xx_status fte3600_ft93xx_full_startup (struct fte3600_ft93xx_device                 *device,
                                                        uint16_t                                     *pixels,
                                                        size_t                                        capacity,
                                                        const struct fte3600_ft93xx_io               *io,
                                                        const struct fte3600_ft93xx_startup_observer *observer,
                                                        struct fte3600_ft93xx_startup_diagnostics    *diagnostics);

/* These helpers leave output untouched on invalid/unknown input. Navigation
 * geometry has the normal width, the reviewed navigation height and no extra
 * rows. Defaults do not change common acquisition state. */
enum fte3600_ft93xx_status fte3600_ft93xx_navigation_geometry (uint16_t                        chip_id,
                                                               struct fte3600_ft93xx_geometry *geometry);
enum fte3600_ft93xx_status fte3600_ft93xx_image_controller_defaults (uint16_t                                chip_id,
                                                                     uint8_t                                 version,
                                                                     struct fte3600_ft93xx_image_controller *controller);

/* capacity is in uint16_t samples; navigation capacity suffices except mode0
 * STATISTIC, which requires the full visible image. mode is 0/1 for STATISTIC,
 * and must be 1 otherwise. pixels are host-order on entry and completed
 * capture; extra capacity is untouched. All state, controller, pixels, io and
 * result pointers are required, even for scalar actions (which perform no I/O).
 * Successful STATISTIC/TAILS set only the corresponding result fields. Other
 * actions leave result unchanged; controller.statistic is a distinct cache.
 *
 * Unknown IDs/invalid arguments leave everything unchanged before I/O. Common
 * acquisition domains match capture. nav_ready/initialized are 0/1, max_dac is
 * 5..255, step is nonzero. Objects must be valid, nonoverlapping and serialized.
 * No allocation, successful capture stub, reset or OS interrupt is provided.
 *
 * IMAGE_SEARCH_FAILED corresponds to compatibility ffffffff. Completed outer
 * BASELINE returns OK even after five failed searches; inspect diagnostics.
 * Timeout counters accumulate and capture still reads FIFO. Local transport
 * and bad ordinary-read CRC stop immediately, preserving prior changes. FIFO
 * trailers are discarded as in raw capture. Failed FIFO leaves completed raw
 * chunks in pixels; a failed final acknowledgment leaves a decoded image.
 * Diagnostics clear once per valid call. transport.raw_bytes_completed denotes
 * the most recent capture's successfully copied FIFO prefix. */
enum fte3600_ft93xx_status fte3600_ft93xx_image_cal_run (uint16_t                                 chip_id,
                                                         uint16_t                                 raw_id,
                                                         struct fte3600_ft93xx_state             *state,
                                                         struct fte3600_ft93xx_image_controller  *controller,
                                                         enum fte3600_ft93xx_image_action         action,
                                                         unsigned int                             mode,
                                                         uint16_t                                *pixels,
                                                         size_t                                   capacity,
                                                         struct fte3600_ft93xx_image_result      *result,
                                                         const struct fte3600_ft93xx_io          *io,
                                                         struct fte3600_ft93xx_image_diagnostics *diagnostics);

/* Physical efuse helpers accept any scalar chip ID and foundry byte. Foundry
 * outside 1..3 returns zero bytes without physical I/O. Serial capacity must
 * be >= SERIAL_SIZE; additional capacity is untouched. Invalid arguments
 * change nothing and invoke no callbacks. Successful byte reads commit one
 * byte at a time; an error preserves the remaining serial output. */
enum fte3600_ft93xx_status fte3600_ft93xx_efuse_read (uint16_t                           chip_id,
                                                      uint8_t                            foundry,
                                                      uint8_t                            address,
                                                      uint8_t                           *value,
                                                      const struct fte3600_ft93xx_io    *io,
                                                      struct fte3600_ft93xx_diagnostics *diagnostics);
enum fte3600_ft93xx_status fte3600_ft93xx_foundry (uint16_t                           chip_id,
                                                   uint16_t                           raw_id,
                                                   uint8_t                           *foundry,
                                                   const struct fte3600_ft93xx_io    *io,
                                                   struct fte3600_ft93xx_diagnostics *diagnostics);
enum fte3600_ft93xx_status fte3600_ft93xx_serial_read (uint16_t                           chip_id,
                                                       uint8_t                            foundry,
                                                       uint8_t                           *serial,
                                                       size_t                             capacity,
                                                       const struct fte3600_ft93xx_io    *io,
                                                       struct fte3600_ft93xx_diagnostics *diagnostics);

/* After board reset/settle, independently discover identity and prepare cold
 * defaults. Valid calls clear output/diagnostics; errors leave partial output
 * with calibration_pending=0. Unknown discovered IDs stop locally before
 * foundry/serial work. OK sets calibration_pending=1, never calibration success
 * or authentication readiness. No reset, baseline, measurement or FDT setup. */
enum fte3600_ft93xx_status fte3600_ft93xx_startup (struct fte3600_ft93xx_preparation *output,
                                                   const struct fte3600_ft93xx_io    *io,
                                                   struct fte3600_ft93xx_diagnostics *diagnostics);

/* FDT defaults replace all four fields; known logical ID and foundry 0..3
 * required. No I/O. Unknown ID returns UNSUPPORTED_DEVICE without mutation. */
enum fte3600_ft93xx_status fte3600_ft93xx_fdt_defaults (uint16_t                         chip_id,
                                                        uint8_t                          foundry,
                                                        uint8_t                          version,
                                                        struct fte3600_ft93xx_fdt_state *fdt);

/* Separate analog/clock/FDT mode configuration; no measurement or calibration.
 * Requires polarity 0..3, subtraction 0..1, register check 0..2, foundry 0..3,
 * fdt_configured and threshold_mode 0..1. Cache accepts any byte (aa initially).
 * Other common fields are preserved, including image configured and DAC/gain.
 * Unknown ID fails before I/O. Invalid arguments change nothing. Valid calls
 * clear diagnostics; transport/CRC errors stop immediately, preserving prior
 * successful writes and state updates. A failure while the protected window
 * is open does not issue unrequested recovery I/O. Caller must recover before
 * reuse. The common mode flag a1 is not an acknowledgment from hardware. */
enum fte3600_ft93xx_status fte3600_ft93xx_fdt_configure (uint16_t                           chip_id,
                                                         uint8_t                            foundry,
                                                         struct fte3600_ft93xx_state       *state,
                                                         struct fte3600_ft93xx_fdt_state   *fdt,
                                                         const struct fte3600_ft93xx_io    *io,
                                                         struct fte3600_ft93xx_diagnostics *diagnostics);

/* A zero result supplies one byte; nonzero leaves geometry/version unchanged. */
typedef int (*fte3600_ft93xx_efuse_fn)(void    *context,
                                       uint8_t  foundry,
                                       uint8_t  offset,
                                       uint8_t *value);

/* Unknown logical IDs retain prior geometry; version is still assigned.
 * 9391/9392 assign four extra rows; other IDs retain prior extra rows. */
enum fte3600_ft93xx_status fte3600_ft93xx_area (uint16_t                        chip_id,
                                                uint16_t                        raw_id,
                                                uint8_t                         foundry,
                                                struct fte3600_ft93xx_geometry *geometry,
                                                uint8_t                        *version,
                                                fte3600_ft93xx_efuse_fn         efuse,
                                                void                           *context);

uint8_t fte3600_ft93xx_features (uint16_t chip_id,
                                 uint16_t raw_id,
                                 uint8_t  version,
                                 uint8_t  foundry);

/* Assign acquisition defaults for the ten reviewed IDs; preserve mode,
 * configured and check_registers. Unknown IDs are invalid and change nothing. */
enum fte3600_ft93xx_status fte3600_ft93xx_configuration (uint16_t                     chip_id,
                                                         uint8_t                      version,
                                                         struct fte3600_ft93xx_state *state);

/* FT9769-class raw-array image regime (see ft93xx.c). */
enum fte3600_ft93xx_status fte3600_ft93xx_raw_image_state (uint16_t raw_id, struct fte3600_ft93xx_state *state);
/* Reads whether the part has latched a finger-detect event since the last
 * arm (a touch that landed while the host was busy and whose interrupt edge
 * may already have been consumed). */
enum fte3600_ft93xx_status fte3600_ft93xx_fdt_pending (const struct fte3600_ft93xx_io *io, int *pending);
size_t fte3600_ft93xx_frame_count (uint8_t count);

/* Positive width/height; extra rows may be zero. Arithmetic is checked even
 * on 32-bit hosts. On invalid input, *length remains unchanged. */
enum fte3600_ft93xx_status fte3600_ft93xx_payload_size (const struct fte3600_ft93xx_geometry *geometry,
                                                        uint8_t                               count,
                                                        size_t                               *length);

/* For both acquisition operations: capacity must cover payload_size;
 * extra capacity is untouched. Invalid inputs invoke no callbacks and change
 * nothing, including diagnostics. All pointed-to objects must be valid and
 * nonoverlapping. Caller serializes access. No allocation or global state.
 *
 * An exhausted transaction stops immediately with TRANSPORT_ERROR after its
 * third 10 ms wait. Earlier state changes and successfully copied raw chunks
 * remain; later output bytes are untouched. No uninitialized bytes are read.
 * This defined local failure is outside the reference parity domain. */
enum fte3600_ft93xx_status fte3600_ft93xx_image_read (const struct fte3600_ft93xx_geometry *geometry,
                                                      uint8_t                               count,
                                                      uint8_t                              *payload,
                                                      size_t                                capacity,
                                                      const struct fte3600_ft93xx_io       *io,
                                                      struct fte3600_ft93xx_diagnostics    *diagnostics);

/* Requires valid post-startup/calibration state and listed chip width/height.
 * 9391/9392 require four extra rows; others admit retained extra-row state.
 * DAC 0..255, gain 0..15, polarity 0..3, integration 1..512, timing 0..15,
 * sample mode 0..3, subtraction/configured 0..1, check state 0..2.
 *
 * On success the first width*height samples in payload are host uint16_t
 * values, copied with memcpy (no alignment requirement). Remaining bytes stay
 * raw. Only this first visible prefix is decoded, including for count > 1.
 * Copy each sample with memcpy into a uint16_t for portable access. Raw ID
 * 9392 reverses consecutive groups of four samples. On transport error no
 * decoding occurs. OK does not imply idle/mode/ready were acknowledged. */
/* Word reads accept zero CRC or correct CRC-16/CCITT (init ffff, poly 1021).
 * Incorrect nonzero CRC stops with local BAD_CRC, without recovery or retry.
 * That defined failure is outside the reviewed parity domain. */
enum fte3600_ft93xx_status fte3600_ft93xx_capture (uint16_t                              chip_id,
                                                   uint16_t                              raw_id,
                                                   const struct fte3600_ft93xx_geometry *geometry,
                                                   struct fte3600_ft93xx_state          *state,
                                                   uint8_t                               count,
                                                   uint8_t                              *payload,
                                                   size_t                                capacity,
                                                   const struct fte3600_ft93xx_io       *io,
                                                   struct fte3600_ft93xx_diagnostics    *diagnostics);

/* Additive retained-state acquisition for the reviewed full image pipeline.
 * Same contract as capture, except integration admits the full u16 domain
 * (including zero). Register 1807 uses the low16 bits of unsigned
 * ((integration-1) modulo 2^32)*32 | sample_time. The older capture entry
 * retains its 1..512 validation. All other domains and failure rules remain
 * unchanged; this does not initialize/calibrate the board. */
enum fte3600_ft93xx_status fte3600_ft93xx_capture_retained (uint16_t                              chip_id,
                                                            uint16_t                              raw_id,
                                                            const struct fte3600_ft93xx_geometry *geometry,
                                                            struct fte3600_ft93xx_state          *state,
                                                            uint8_t                               count,
                                                            uint8_t                              *payload,
                                                            size_t                                capacity,
                                                            const struct fte3600_ft93xx_io       *io,
                                                            struct fte3600_ft93xx_diagnostics    *diagnostics);

/* Bounded inner identification: voltage/SPI-mode preparation, then at most
 * three ID attempts. No GPIO reset, board selection, firmware or startup.
 * On OK, chip_id is a recognized logical ID or zero after three rejections;
 * raw_id retains the last observed ID. Unacknowledged preparation continues.
 * Invalid input changes nothing and invokes no callbacks. Valid calls clear
 * identity/diagnostics first; transport or CRC errors leave partial identity.
 * The callback, retry, CRC and nonoverlap contracts above also apply. */
enum fte3600_ft93xx_status fte3600_ft93xx_probe (struct fte3600_ft93xx_identity    *identity,
                                                 const struct fte3600_ft93xx_io    *io,
                                                 struct fte3600_ft93xx_diagnostics *diagnostics);

/* Explicit bridge to recovery and Linux callbacks. Requires only spi/wait_ms.
 * Maps any nonzero recovery result to raw failure, uses separate TX/RX,
 * and requires transfer capacity >= MAX_TRANSFER. recovery must outlive io.
 * Invalid inputs leave output unchanged. For Linux use
 * io_from_recovery(fte3600_linux_callbacks(transport), &io), and inspect
 * fte3600_linux_error afterwards (wait cannot return timing errors). */
enum fte3600_ft93xx_status fte3600_ft93xx_io_from_recovery (const struct fte3600_recovery_io *recovery,
                                                            struct fte3600_ft93xx_io         *output);

#ifdef __cplusplus
}
#endif
#endif
