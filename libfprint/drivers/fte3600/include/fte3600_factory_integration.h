/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_FACTORY_INTEGRATION_H
#define FTE3600_FACTORY_INTEGRATION_H
#include "fte3600_factory_byte_startup.h"
#include "fte3600_factory_legacy_startup.h"
#ifdef __cplusplus
extern "C" {
#endif
/* One prepared SPI owner. All six callbacks are required. */
typedef struct fte3600_factory_byte_io fte3600_factory_integration_io;
struct fte3600_factory_integration_input
{
  uint32_t                         scene, resume_cleanup;
  enum fte3600_factory_model initial_model;
  struct fte3600_ft9368_boot_image pram, application;
  const uint8_t                   *firmware;
  size_t                           firmware_length;
};
struct fte3600_factory_integration_state
{
  struct fte3600_factory_startup_state  word;
  struct fte3600_factory_byte_state     byte;
  struct fte3600_factory_legacy_state   legacy;
  enum fte3600_factory_model selected_model;
  const fte3600_factory_integration_io *board;
};
enum fte3600_factory_integration_phase {
  FTE3600_FI_SHORT, FTE3600_FI_BYTE_PROBE, FTE3600_FI_WORD_WAKE,
  FTE3600_FI_WORD_PROBE, FTE3600_FI_SELECT, FTE3600_FI_RESET,
  FTE3600_FI_MODERN_BIND, FTE3600_FI_MODERN_STARTUP, FTE3600_FI_MODERN_FDT,
  FTE3600_FI_MODERN_IMAGE, FTE3600_FI_MODERN_ARM, FTE3600_FI_MODERN_FINALIZE,
  FTE3600_FI_FW_STARTUP, FTE3600_FI_FW_FDT, FTE3600_FI_FW_IMAGE, FTE3600_FI_FW_ARM,
  FTE3600_FI_DISCARD, FTE3600_FI_BYTE_BIND, FTE3600_FI_BYTE_LOAD,
  FTE3600_FI_BYTE_VERSION, FTE3600_FI_PRAM, FTE3600_FI_APPLICATION,
  FTE3600_FI_PING, FTE3600_FI_PING_ONCE, FTE3600_FI_LEGACY_APPLICATION,
  FTE3600_FI_LEGACY_FACTORY, FTE3600_FI_LEGACY_RETRY, FTE3600_FI_LEGACY_IDENTITY,
  FTE3600_FI_LEGACY_INITIALIZE, FTE3600_FI_LEGACY_IDLE, FTE3600_FI_LEGACY_VERSION,
  FTE3600_FI_LEGACY_LOAD, FTE3600_FI_LEGACY_BOOT_A, FTE3600_FI_LEGACY_CONFIG,
  FTE3600_FI_LEGACY_ARM, FTE3600_FI_LEGACY_BOOT_B, FTE3600_FI_LEGACY_UPLOAD_A8
};
struct fte3600_factory_integration_result
{
  uint32_t                              status;
  enum fte3600_factory_model model;
  uint8_t                               contact_pending, sensor_constructed, modern_ran, fw_ran;
  uint8_t                               check_version;
  uint64_t                              physical_calls, event_count, clock_us, byte_protocol_calls;
  uint64_t                              information_replies_consumed;
  uint32_t                              transport_failures, last_transport_failure, gpio_failures, last_gpio_failure;
  uint32_t                              constructions, destructions, child_status;
  struct fte3600_factory_startup_result word;
  struct fte3600_factory_byte_result    byte;
  struct fte3600_factory_legacy_result  legacy;
};
struct fte3600_factory_integration_observer
{
  void *context;
  /* Passive synchronous nested entry/exit. All spans/counters are global.
   * SELECT exit follows completed profile/payload construction. A defined
   * status has its reviewed width; no status is invented for local errors. */
  void (*phase)(void *,
                enum fte3600_factory_integration_phase,
                int      entering,
                int      has_status,
                uint32_t status,
                const struct fte3600_factory_integration_result *);
};
enum fte3600_factory_integration_error {
  FTE3600_FACTORY_INTEGRATION_OK, FTE3600_FACTORY_INTEGRATION_INVALID_ARGUMENT,
  FTE3600_FACTORY_INTEGRATION_WORD_READ_ERROR, FTE3600_FACTORY_INTEGRATION_BYTE_ERROR,
  FTE3600_FACTORY_INTEGRATION_LEGACY_ERROR, FTE3600_FACTORY_INTEGRATION_MODERN_ERROR,
  FTE3600_FACTORY_INTEGRATION_FW_ERROR, FTE3600_FACTORY_INTEGRATION_UNDEFINED_ARM,
  FTE3600_FACTORY_INTEGRATION_RESPONSE_UNAVAILABLE,
  FTE3600_FACTORY_INTEGRATION_MISSING_FIRMWARE,
  FTE3600_FACTORY_INTEGRATION_FIRMWARE_MODEL_MISMATCH,
  FTE3600_FACTORY_INTEGRATION_UNCONFIRMED_IDENTITY
};
#define FTE3600_FACTORY_INTEGRATION_UNAVAILABLE UINT32_C(0xe0000001)
enum fte3600_factory_integration_error fte3600_factory_integration_state_defaults (struct fte3600_factory_integration_state *);
/* One real five-round operation, no allocation for logical bindings. Storage
 * capacity is bytes. Firmware is immutable caller storage; no discovery or
 * substitution of manufacturer payloads. PRAM has explicit block padding.
 * All objects and payload extents must be aligned, valid and disjoint.
 * Callbacks/contexts remain valid, serialized, and must not alias mutable
 * arguments or mutate/reenter the operation. Invalid preflight changes nothing
 * and calls nothing. Each valid invocation clears only old logical selection
 * and result before callbacks, retaining PRAM/boot/IRQ/modern/FW state.
 * Completed effects survive local failure; status is meaningful only on OK.
 * selected_model/current board describe binding, result.model is the retained
 * classification scalar, and modern_ran/fw_ran plus child diagnostics describe
 * invoked startup. Selection never implies readiness. Only board and byte's
 * selected.board retain the caller I/O pointer; word/legacy profile board
 * fields are NULL, preventing borrowed internal adapters from escaping.
 * All physical cursors and synthetic device memory stay in caller context.
 */
enum fte3600_factory_integration_error fte3600_factory_integration_run (const struct fte3600_factory_integration_input *,
                                                                        struct fte3600_factory_integration_state *,
                                                                        struct fte3600_factory_startup_storage *,
                                                                        size_t capacity,
                                                                        const fte3600_factory_integration_io *,
                                                                        const struct fte3600_factory_integration_observer *,
                                                                        struct fte3600_factory_integration_result *);
/* Additive hardware-owner entry: absent payloads are exactly NULL/zero
 * extents. Present payloads retain all strict validation. Discovery and word
 * startup do not need firmware. A selected byte loader or legacy initializer
 * requiring absent payloads stops with MISSING_FIRMWARE before loader I/O.
 * Present legacy payloads must be bound to firmware_model; a different
 * selected legacy model returns FIRMWARE_MODEL_MISMATCH before initialization.
 * Unlike the strict compatibility entry, this hardware policy refuses the
 * round-3/4 forced FT9368 loader when no positive device identity was obtained:
 * UNCONFIRMED_IDENTITY is returned before that selection/binding or any load.
 * FW9369 startup uses the local Linux automatic detection policy (1A83 low
 * seven bits 0016 before first automatic ACK/C2). The original entry retains
 * its strict all-payload preflight and original component behavior unchanged. */
enum fte3600_factory_integration_error fte3600_factory_integration_run_optional_firmware (const struct fte3600_factory_integration_input *,
                                                                                          enum fte3600_factory_model firmware_model,
                                                                                          struct fte3600_factory_integration_state *,
                                                                                          struct fte3600_factory_startup_storage *,
                                                                                          size_t capacity,
                                                                                          const fte3600_factory_integration_io *,
                                                                                          const struct fte3600_factory_integration_observer *,
                                                                                          struct fte3600_factory_integration_result *);
#ifdef __cplusplus
}
#endif
#endif
