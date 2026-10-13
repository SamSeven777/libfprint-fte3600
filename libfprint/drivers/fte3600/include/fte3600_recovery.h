/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_RECOVERY_H
#define FTE3600_RECOVERY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FTE3600_RECOVERY_MAX_FIRMWARE ((size_t) 65527)

enum fte3600_recovery_boot {
  FTE3600_RECOVERY_BOOT_A = 1,
  FTE3600_RECOVERY_BOOT_B = 2
};

/* Project-local categories, not errno or Windows status values. */
enum fte3600_recovery_status {
  FTE3600_RECOVERY_OK = 0,
  FTE3600_RECOVERY_IO_ERROR = 1,
  FTE3600_RECOVERY_FAILED = 2,
  FTE3600_RECOVERY_INVALID_INPUT = 3,
  FTE3600_RECOVERY_NO_MEMORY = 4
};

struct fte3600_recovery_io
{
  void *context;
  /* Physical requested reset level: 1 releases reset; 0 asserts it. */
  int   (*gpio)(void *context,
                int   level);
  /*
   * Synchronous full-duplex transfer. tx and rx are distinct nonnull buffers
   * of length bytes. Success must fill rx; neither pointer may be retained.
   * gpio/spi return zero on success and any nonzero value on I/O failure.
   */
  int (*spi)(void          *context,
             const uint8_t *tx,
             uint8_t       *rx,
             size_t         length);
  /* Wait/clock callbacks cannot report errors in this revision. */
  void (*wait_ms)(void    *context,
                  uint32_t duration_ms);
  /* Monotonic milliseconds modulo 2^32, including I/O and waits. */
  uint32_t (*clock_ms)(void *context);
  /* Optional explicit final reset release + SPI 55 aa. The caller already
  * requested H/10ms/L/20ms. Set *gpio_status to the release result and
  * return the overall raw result (same categories as gpio/spi). Zero GPIO
  * status means sync was attempted; a failed release must skip sync.
  * Synchronous, no retained pointers. NULL preserves portable callbacks.
  * Private source ABI extension: rebuild all callback-table consumers. */
  int (*release_and_sync)(void *context,
                          int  *gpio_status);
};

/* Optional synchronous, passive reset observation. raw_status is the reset
 * GPIO callback result; it is zero on entry. Compound reset/sync defers exit
 * until after sync, retaining the GPIO result. No mutation or reentry. */
struct fte3600_reset_observer
{
  void *context;
  void  (*reset)(void *context,
                 int   entering,
                 int   raw_status);
};
enum fte3600_recovery_status fte3600_recover_observed (enum fte3600_recovery_boot           boot,
                                                       const uint8_t                       *firmware,
                                                       size_t                               firmware_length,
                                                       uint32_t                             startup_wait_ms,
                                                       const struct fte3600_recovery_io    *io,
                                                       const struct fte3600_reset_observer *observer);

/*
 * Recover using caller-owned immutable firmware and synchronous callbacks.
 * All callback pointers are required, even when a path does not use the clock.
 * context may be null. firmware must reference firmware_length readable bytes.
 * Valid firmware lengths are 1..FTE3600_RECOVERY_MAX_FIRMWARE. The readback
 * transfer length (firmware_length + 8) must fit its 16-bit wire field.
 * Invalid arguments and allocation failure return without invoking callbacks.
 * Caller inputs and callbacks must remain valid and unchanged until return.
 * Callers serialize operations on a shared device; this library has no global
 * mutable state. Callbacks must return, and time must advance during polling.
 * No cancellation, firmware selection, or cleanup GPIO request is supplied.
 */
enum fte3600_recovery_status fte3600_recover (enum fte3600_recovery_boot        boot,
                                              const uint8_t                    *firmware,
                                              size_t                            firmware_length,
                                              uint32_t                          startup_wait_ms,
                                              const struct fte3600_recovery_io *io);

#ifdef __cplusplus
}
#endif

#endif
