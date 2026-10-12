/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_LEGACY_BOOT_H
#define FTE3600_LEGACY_BOOT_H

#include "fte3600_legacy_lifecycle.h"

#ifdef __cplusplus
extern "C" {
#endif

enum fte3600_legacy_boot_operation {
  FTE3600_LEGACY_BOOT_VERSION,
  FTE3600_LEGACY_BOOT_LOAD,
  FTE3600_LEGACY_BOOT_INITIALIZE,
  FTE3600_LEGACY_BOOT_UPLOAD_A8
};

enum fte3600_legacy_boot_status {
  FTE3600_LEGACY_BOOT_OK,
  FTE3600_LEGACY_BOOT_IO_ERROR,
  FTE3600_LEGACY_BOOT_DEVICE_ABSENT,
  FTE3600_LEGACY_BOOT_VERSION_MISMATCH,
  FTE3600_LEGACY_BOOT_FAILED,
  FTE3600_LEGACY_BOOT_INVALID_INPUT,
  FTE3600_LEGACY_BOOT_NO_MEMORY,
  FTE3600_LEGACY_BOOT_NOT_RUN   /* Lifecycle diagnostic and observer entry. */
};

/* Typed callback outcome for THIS entry point only. All other nonzero GPIO
 * and SPI outcomes mean ordinary I/O failure. Existing recovery/runtime APIs
 * continue to interpret every nonzero callback result as ordinary failure. */
#define FTE3600_LEGACY_BOOT_RAW_DEVICE_ABSENT 2

struct fte3600_legacy_boot_profile
{
  enum fte3600_sensor sensor;
  enum fte3600_recovery_boot boot;
  uint8_t  expected_firmware, expected_agc;
  uint32_t startup_wait_ms;
};

enum fte3600_legacy_boot_phase {
  FTE3600_LEGACY_PHASE_IDLE, FTE3600_LEGACY_PHASE_VERSION,
  FTE3600_LEGACY_PHASE_LOAD, FTE3600_LEGACY_PHASE_CONFIG,
  FTE3600_LEGACY_PHASE_ARM, FTE3600_LEGACY_PHASE_BOOT_A,
  FTE3600_LEGACY_PHASE_BOOT_B, FTE3600_LEGACY_PHASE_UPLOAD_A8,
  FTE3600_LEGACY_PHASE_RESET
};
struct fte3600_legacy_boot_observer
{
  void *context;
  /* Passive synchronous observation, no mutation or reentry. Entry status
   * is NOT_RUN. Exit status is the actual child's result, including an arm
   * error which INITIALIZE deliberately ignores. */
  void (*phase)(void *,
                enum fte3600_legacy_boot_phase,
                int entering,
                enum fte3600_legacy_boot_status);
};
enum fte3600_legacy_boot_status fte3600_legacy_boot_run_observed (const struct fte3600_legacy_boot_profile *,
                                                                  enum fte3600_legacy_boot_operation,
                                                                  const uint8_t *,
                                                                  size_t,
                                                                  uint8_t *,
                                                                  const struct fte3600_recovery_io *,
                                                                  const struct fte3600_legacy_boot_observer *);

/* Unsupported sensors produce a zero profile; supported defaults use Boot A.
* The caller may select Boot B for FT9338/FT9536 and override expected bytes
* or startup wait. These are software defaults, not universal board facts. */
struct fte3600_legacy_boot_profile fte3600_legacy_boot_defaults (enum fte3600_sensor sensor);

/* Synchronous composition over actual GPIO/SPI/wait/clock callbacks. All
 * callbacks, profile, firmware and irq_enabled are required for every operation.
 * Firmware length is 1..FTE3600_RECOVERY_MAX_FIRMWARE and is never modified.
 * Valid inputs and callback objects remain alive, unchanged and nonoverlapping
 * until return. The caller serializes access; callbacks may inspect IRQ state
 * but must not mutate it or reenter. Boot B is only supported for 9338/9536;
 * UPLOAD_A8 is only supported for 9348/9361 and leaves the IRQ byte unchanged.
 * Invalid arguments invoke no callbacks and leave IRQ state unchanged.
 * LOAD sets IRQ to 0 then to 1 on every exit, including allocation failure;
 * INITIALIZE changes IRQ only when LOAD runs. Version and arm failures have
 * the exact swallowing behavior described by specs/legacy-boot-v1.md.
 * NO_MEMORY is a local resource failure. No firmware selection, asynchronous
 * request ownership, electrical IRQ enablement or physical validation is done.
 * Wait errors cannot be represented; clocks must advance during polling.
 */
enum fte3600_legacy_boot_status fte3600_legacy_boot_run (const struct fte3600_legacy_boot_profile *profile,
                                                         enum fte3600_legacy_boot_operation        operation,
                                                         const uint8_t                            *firmware,
                                                         size_t                                    firmware_length,
                                                         uint8_t                                  *irq_enabled,
                                                         const struct fte3600_recovery_io         *io);

struct fte3600_legacy_boot_lifecycle_io
{
  struct fte3600_recovery_io protocol;
  void                      *completion_context;
  void                       (*complete)(void                          *context,
                                         int                            matched_pending,
                                         enum fte3600_legacy_completion result,
                                         const uint8_t                 *image,
                                         size_t                         image_length);
};

struct fte3600_legacy_boot_lifecycle_result
{
  struct fte3600_legacy_lifecycle_result lifecycle;
  int                                    initialize_called;
  enum fte3600_legacy_boot_status initialize_status;
};

/* Serialized convenience composition of the two reviewed contracts. Replaces
 * the lifecycle initialize callback with the real synchronous boot operation.
 * All boot and lifecycle arguments are validated before any callback, state or
 * image mutation; all four protocol callbacks and complete are always required.
 * The protocol callbacks/context are preserved, and complete receives exactly
 * completion_context and the lifecycle's completion arguments. The caller owns
 * request tokens/power policy and obeys all lifecycle lifetime/ownership rules.
 * Boot uses a private IRQ byte initialized from state; if initialization runs,
 * its final value is copied to state only after the lifecycle returns. The
 * lifecycle state cannot be mutated/reentered by callbacks. Transport observers
 * must not interpret the outer state's IRQ byte as live boot IRQ state.
 * initialize_status is NOT_RUN if not called (also on invalid input). Outer
 * lifecycle status still swallows initializer failure; inspect this diagnostic.
 * Covered combined behavior is the reviewed serialized non-GUI boundary in
 * specs/legacy-full-lifecycle-v1.md, with synthetic physical replies and caller
 * firmware. Finite conformance does not establish physical support, firmware
 * execution or equivalence under concurrent IRQ/state changes.
 */
struct fte3600_legacy_boot_lifecycle_result fte3600_legacy_boot_lifecycle_run (const struct fte3600_legacy_boot_profile      *profile,
                                                                               enum fte3600_legacy_lifecycle_operation        operation,
                                                                               struct fte3600_legacy_lifecycle_state         *state,
                                                                               const struct fte3600_legacy_lifecycle_options *options,
                                                                               uint8_t                                       *image,
                                                                               size_t                                         capacity,
                                                                               const uint8_t                                 *firmware,
                                                                               size_t                                         firmware_length,
                                                                               const struct fte3600_legacy_boot_lifecycle_io *io);

#ifdef __cplusplus
}
#endif
#endif
