/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_LEGACY_H
#define FTE3600_LEGACY_H

#include "fte3600_identity.h"
#include "fte3600_recovery.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FTE3600_LEGACY_MAX_IMAGE_BYTES ((size_t) 9216)
#define FTE3600_LEGACY_MAX_TRANSFER_BYTES ((size_t) 9224)

/* Project-local categories, independent of platform error numbers. */
enum fte3600_legacy_status {
  FTE3600_LEGACY_OK = 0,
  FTE3600_LEGACY_IO_ERROR = 1,
  FTE3600_LEGACY_INVALID_INPUT = 2
};

struct fte3600_legacy_geometry
{
  size_t  width, height, image_bytes, transfer_bytes;
  uint8_t rearm_mode;
};

struct fte3600_legacy_frame_result
{
  enum fte3600_legacy_status status;
  enum fte3600_legacy_status rearm_status;
  size_t image_length;
};

/* Returns all zero fields for unsupported sensors. No callbacks. */
struct fte3600_legacy_geometry fte3600_legacy_geometry (enum fte3600_sensor sensor);

/* These bounded operations accept the four legacy sensors in identity.h.
 * Only spi and wait_ms are required from recovery_io; context may be NULL.
 * spi uses distinct initialized TX/RX buffers, returns zero on success and
 * any nonzero int on failure. Both buffers must remain valid until return;
 * callbacks must not retain pointers. Serialize access to a shared device.
 * Invalid arguments invoke no callbacks. No allocation/global state is used.
 * Mode is widened to validate 0..255 without silent unsigned-byte truncation.
 * Config readback mismatches are diagnostic only. Arm success can include a
 * fallback write resetting mode to zero and is not proof of scan readiness.
 */
enum fte3600_legacy_status fte3600_legacy_config (enum fte3600_sensor               sensor,
                                                  const struct fte3600_recovery_io *io);
enum fte3600_legacy_status fte3600_legacy_idle (enum fte3600_sensor               sensor,
                                                const struct fte3600_recovery_io *io);
enum fte3600_legacy_status fte3600_legacy_arm (enum fte3600_sensor               sensor,
                                               uint32_t                          mode,
                                               const struct fte3600_recovery_io *io);

struct fte3600_legacy_arm_observer
{
  void *context;
  /* Passive nested-idle entry/exit only; entry status is OK, ignored.
   * Must not mutate/reenter. Invalid observers produce no effects. */
  void (*idle)(void *,
               int entering,
               enum fte3600_legacy_status);
};
enum fte3600_legacy_status fte3600_legacy_arm_observed (enum fte3600_sensor,
                                                        uint32_t mode,
                                                        const struct fte3600_recovery_io *,
                                                        const struct fte3600_legacy_arm_observer *);

/* Requires a frame already available: no IRQ/finger wait or first-scan arm.
 * Output must have geometry.image_bytes capacity; excess bytes are untouched.
 * All caller objects must remain valid, unchanged and nonoverlapping for the
 * operation. Invalid input leaves output unchanged and returns INVALID_INPUT
 * for both statuses, image_length=0, without I/O. A valid invocation clears
 * the image region first, preventing stale image exposure on failed reads.
 * Frame read is always followed by arm, even on failure. status and
 * image_length depend ONLY on frame read; rearm_status reports the separate
 * attempt. Successful read returns inverted pixels in original stream order.
 * Unspecified clock bytes are transmitted as zero. Stack use is about 19 KiB.
 * For Linux, capacity must cover geometry.transfer_bytes; inspect the Linux
 * error accessor too, as wait_ms cannot report errors through this interface.
 */
struct fte3600_legacy_frame_result fte3600_legacy_frame_fetch (enum fte3600_sensor               sensor,
                                                               uint8_t                          *image,
                                                               size_t                            capacity,
                                                               const struct fte3600_recovery_io *io);

#ifdef __cplusplus
}
#endif
#endif
