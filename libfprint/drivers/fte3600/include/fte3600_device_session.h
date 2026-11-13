/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_DEVICE_SESSION_H
#define FTE3600_DEVICE_SESSION_H
#include "fte3600_factory_integration.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Single serialized owner; no worker threads, OS fd ownership or IRQ wait.
 * All callbacks run synchronously. The platform supplies physical reset levels
 * (0 assert, 1 release), in-place SPI (high bit failure, buffer unchanged on
 * failure), monotonic clock and explicit factory boundary policy. A boundary
 * returns success only after doing/validating its documented platform job;
 * Linux-inapplicable registry/framework jobs may explicitly be no-ops.
 * error returns a latched nonzero platform error, including void wait failures;
 * clear_error begins a diagnostic interval, not a physical recovery. The
 * platform must keep fatal/disconnected conditions latched until repaired.
 * SPI/wait/gpio/boundary/clock/error/clear_error are all mandatory. */
struct fte3600_device_io
{
  void    *context;
  uint32_t (*spi)(void *,
                  uint8_t *,
                  size_t);
  void     (*wait_ms)(void *,
                      uint32_t);
  void     (*wait_us)(void *,
                      uint32_t);
  uint32_t (*gpio)(void *,
                   int);
  uint32_t (*boundary)(void *,
                       enum fte3600_factory_boundary);
  uint32_t (*clock_ms)(void *);
  int      (*error)(void *);
  void     (*clear_error)(void *);
  /* Optional final physical H + SPI 55 aa after H/10ms/L/20ms. Writes the
   * GPIO status, returns overall high-bit failure status. A successful GPIO
   * means SPI was attempted; no callbacks may interrupt the pair.
   * NULL uses ordinary callbacks. Private source ABI: rebuild consumers. */
  uint32_t (*release_and_sync)(void *,
                               uint32_t *gpio_status);
};
struct fte3600_device_config
{
  struct fte3600_device_io io;
  /* Explicit validated, immutable payloads. No discovery/substitution.
   * Payload memory must remain alive until close. scene must be zero for
   * cold startup; initial_model UNKNOWN and resume_cleanup zero are required.
   * This API does not claim scene-2 contact-only readiness. */
  struct fte3600_factory_integration_input startup;
  /* Required identity binding for startup.firmware; UNKNOWN if absent. */
  enum fte3600_factory_model legacy_firmware_model;
};
enum fte3600_device_error {
  FTE3600_DEVICE_OK,
  FTE3600_DEVICE_INVALID_ARGUMENT,
  FTE3600_DEVICE_NO_MEMORY,
  FTE3600_DEVICE_NOT_READY,
  FTE3600_DEVICE_BUSY,
  FTE3600_DEVICE_PLATFORM_ERROR,
  FTE3600_DEVICE_STARTUP_ERROR,
  FTE3600_DEVICE_CALIBRATION_ERROR,
  FTE3600_DEVICE_MISSING_FIRMWARE,
  FTE3600_DEVICE_UNSUPPORTED,
  FTE3600_DEVICE_ACQUISITION_ERROR,
  FTE3600_DEVICE_NO_FRAME,
  FTE3600_DEVICE_CANCELLED,
  FTE3600_DEVICE_FIRMWARE_MISMATCH,
  FTE3600_DEVICE_RECOVERY_REQUIRED
};
enum fte3600_device_state {
  FTE3600_DEVICE_CREATED, FTE3600_DEVICE_IDLE,
  FTE3600_DEVICE_ARMED, FTE3600_DEVICE_FAULT
};
struct fte3600_device_status
{
  enum fte3600_device_state state;
  enum fte3600_device_error error;
  enum fte3600_factory_model model;
  uint16_t raw_id, chip_id, width, height, extra_rows;
  /* calibration_complete means all family-required initialization stages
  * passed, including acknowledgment checks where diagnostics expose them.
  * Legacy/FT9368 have no separate numeric baseline calibration stage. */
  uint8_t  startup_complete, calibration_complete, image_available;
  uint64_t image_sequence, spi_calls;
  uint32_t failed_spi_calls, last_spi_status, last_gpio_status;
  int      platform_error;
  unsigned component_error;
  uint32_t compatibility_status;
  uint32_t baseline_crc;
  uint16_t calibrated_image_dac;
  /* Raw-array flow (FT9769): median of the last scan, and its shift against
   * the no-finger reference for the last touch or release decision. */
  int      raw_level, raw_shift;
  /* Percentage of image rows whose mean sat at least RAW_TOUCH_SHIFT below
   * the reference in the last accepted raw frame: how much of the strip the
   * finger actually covered. */
  int      raw_contact;
};
struct fte3600_device_session;
/* create copies configuration/callback tables, owns all mutable family state
 * and buffers, and performs no hardware I/O. start performs actual factory
 * detection/startup, verifies family readiness and retains calibrated state.
 * start after a fault is explicit hardware recovery and invalidates the old
 * image. Selection alone never establishes startup_complete. */
enum fte3600_device_error fte3600_device_create (const struct fte3600_device_config *,
                                                 struct fte3600_device_session **);
enum fte3600_device_error fte3600_device_start (struct fte3600_device_session *);
/* arm prepares one request. IRQ is supplied by the serialized caller after
 * its platform wait; capture is an immediate attempt for bring-up. Both call
 * the actual family acquisition/processing path. NO_FRAME leaves the request
 * armed. A completed image is retained, with increasing sequence, until the
 * next successful image or start. Errors never publish partially written
 * pixels or advance sequence. Copy requires explicit sequence match, avoiding
 * stale images being mistaken for a new request result. */
enum fte3600_device_error fte3600_device_arm (struct fte3600_device_session *);
enum fte3600_device_error fte3600_device_irq (struct fte3600_device_session *);
/* While ARMED: whether the part already latched a touch, so the caller
 * should proceed to irq without waiting for an interrupt edge. Families
 * without such a latch report 0. */
enum fte3600_device_error fte3600_device_pending (struct fte3600_device_session *, int *pending);
enum fte3600_device_error fte3600_device_capture (struct fte3600_device_session *);
/* FW9369 only: poll one physical status and, on UP, confirm/maintain/rearm.
 * released is written only on success; false means still held/unknown. The
 * caller supplies bounded waits, deadline and cancellation through callbacks.
 * Capture arms Up before publishing pixels and requires this operation to
 * complete before the next arm. Completed release is idempotent. State is
 * staged until all maintenance/rearm I/O succeeds; errors fault the session.
 * A queued Down after release is preserved by the next arm. Empty FW9369
 * status checks during capture/release are passive: no ACK or reconfiguration.
 * All automatic arms use the separate Linux event-register policy. */
enum fte3600_device_error fte3600_device_release_step (struct fte3600_device_session *,
                                                       int *released);
/* Reviewed FW9369 SPI recovery after RECOVERY_REQUIRED: physical reset pulse,
 * 5 ms wait and checked chip startup, without rediscovery/upload. Preserve the
 * last published image and sequence. waiting_release keeps release pending and
 * rearms Up; otherwise retain the pending capture and startup's Down arm.
 * Caller bounds attempts per action. Failure requires explicit cold restart. */
enum fte3600_device_error fte3600_device_recover_fw9369 (struct fte3600_device_session *,
                                                         int waiting_release);
enum fte3600_device_error fte3600_device_copy_image (const struct fte3600_device_session *,
                                                     uint64_t sequence,
                                                     uint8_t *pixels,
                                                     size_t   capacity);
/* Cancels the serialized pending request, not an in-flight callback. An idle
 * cancellation is idempotent. No OS/concurrent cancellation claim. */
enum fte3600_device_error fte3600_device_cancel (struct fte3600_device_session *);
enum fte3600_device_error fte3600_device_get_status (const struct fte3600_device_session *,
                                                     struct fte3600_device_status *);
const char *fte3600_device_error_string (enum fte3600_device_error);
/* Releases session allocations; caller closes/disables platform resources.
 * No callback uses the session after this returns. NULL is permitted. */
void fte3600_device_close (struct fte3600_device_session *);
#ifdef __cplusplus
}
#endif
#endif
