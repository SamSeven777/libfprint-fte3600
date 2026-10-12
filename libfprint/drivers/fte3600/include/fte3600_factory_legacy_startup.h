/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_FACTORY_LEGACY_STARTUP_H
#define FTE3600_FACTORY_LEGACY_STARTUP_H
#include "fte3600_factory_startup.h"
#include "fte3600_legacy_boot.h"
#ifdef __cplusplus
extern "C" {
#endif
struct fte3600_factory_legacy_identity { size_t      length;
                                         const char *sha256;
};
struct fte3600_factory_legacy_selected
{
  enum fte3600_factory_model model;
  uint16_t                                      width, height;
  uint8_t                                       firmware_version, agc_version, mode_register, bound;
  uint32_t                                      startup_wait_ms;
  const struct fte3600_factory_legacy_identity *original_application;
  const uint8_t                                *application;
  size_t                                        application_length;
  const struct fte3600_factory_startup_io      *board;
};
struct fte3600_factory_legacy_state
{
  struct fte3600_factory_legacy_selected selected;
  uint32_t                               boot_edition;
  uint8_t                                irq_enabled;
};
struct fte3600_factory_legacy_input
{
  uint32_t       scene, resume_cleanup;
  enum fte3600_factory_model initial_model;
  const uint8_t *firmware;
  size_t         firmware_length;
};
enum fte3600_factory_legacy_phase {
  FTE3600_FL_SHORT_DETECT, FTE3600_FL_BYTE_PROBE, FTE3600_FL_WORD_WAKE,
  FTE3600_FL_WORD_PROBE, FTE3600_FL_RESET, FTE3600_FL_PING, FTE3600_FL_PING_ONCE,
  FTE3600_FL_APPLICATION, FTE3600_FL_FACTORY, FTE3600_FL_FACTORY_RETRY,
  FTE3600_FL_IDENTITY, FTE3600_FL_SELECT, FTE3600_FL_INITIALIZE,
  FTE3600_FL_IDLE, FTE3600_FL_VERSION, FTE3600_FL_LOAD, FTE3600_FL_BOOT_A,
  FTE3600_FL_CONFIG, FTE3600_FL_ARM, FTE3600_FL_DISCARD,
  FTE3600_FL_BOOT_B, FTE3600_FL_UPLOAD_A8
};
struct fte3600_factory_legacy_result
{
  uint32_t status;
  enum fte3600_factory_model model;
  uint8_t  contact_pending, sensor_constructed;
  uint64_t physical_calls, event_count, clock_us, byte_protocol_calls;
  uint64_t information_replies_consumed;
  uint32_t transport_failures, last_transport_failure, gpio_failures, last_gpio_failure;
  uint32_t reset_calls, last_reset_status, constructions, destructions;
  uint32_t initializer_calls, initializer_failures, last_initializer_status;
  uint32_t version_failures, load_attempts, load_failures, arm_failures;
  uint32_t discovery_exhausted, wake_unacknowledged, negotiation_exhausted;
};
struct fte3600_factory_legacy_observer
{
  void *context;
  void  (*phase)(void *,
                 enum fte3600_factory_legacy_phase,
                 int      entering,
                 int      has_status,
                 uint32_t status,
                 const struct fte3600_factory_legacy_result *);
};
enum fte3600_factory_legacy_error {
  FTE3600_FACTORY_LEGACY_OK, FTE3600_FACTORY_LEGACY_INVALID_ARGUMENT,
  FTE3600_FACTORY_LEGACY_UNSUPPORTED, FTE3600_FACTORY_LEGACY_WORD_READ_ERROR,
  FTE3600_FACTORY_LEGACY_NO_MEMORY, FTE3600_FACTORY_LEGACY_RESPONSE_UNAVAILABLE
};
/* A transport can reject a locally unavailable/undefined synthetic response
 * distinctly from a modeled device error. This value is never compatibility
 * success; the outer API returns RESPONSE_UNAVAILABLE after the current child. */
#define FTE3600_FACTORY_LEGACY_UNAVAILABLE UINT32_C(0xe0000001)
enum fte3600_factory_legacy_error fte3600_factory_legacy_state_defaults (struct fte3600_factory_legacy_state *);
enum fte3600_factory_legacy_error fte3600_factory_legacy_defaults (enum fte3600_factory_model,
                                                                   struct fte3600_factory_legacy_selected *,
                                                                   const struct fte3600_factory_startup_io *);
/* SPI-only rounds 0..2, actual legacy construction and startup. Later forced
 * FT9368 recovery returns UNSUPPORTED. Caller provides immutable synthetic or
 * independently licensed payload bytes, length 1..14184; original identities
 * are metadata only and never substitute bytes. Only FT9338's factory route
 * initializes. Application geometry arms all four except initial scene contact.
 *
 * All objects and payload extents must be aligned/valid/disjoint; callbacks and
 * their contexts must remain valid, serialized and not alias mutable arguments.
 * Optional observers are passive, synchronous, and must not mutate/reenter.
 * Preflight errors leave state/result unchanged and invoke nothing. A valid
 * invocation clears result and the previous borrowed selection once before
 * callbacks; no physical destruction is implied. Boot/IRQ retain state until
 * actual operations change them. initial_model is a scalar, never a live
 * allocation. Selected bindings are borrowed references. No heap allocation
 * is needed for logical selection; failed initializers clear the selected
 * object. Firmware loaders retain their own bounded temporary allocation and
 * NO_MEMORY distinction.
 * Physical response cursors remain in the caller's shared transport context.
 * Local errors preserve completed effects; compatibility status is meaningful
 * only on local OK. This boundary does not establish working sensor firmware.
 */
enum fte3600_factory_legacy_error fte3600_factory_legacy_startup_run (const struct fte3600_factory_legacy_input *,
                                                                      struct fte3600_factory_legacy_state *,
                                                                      const struct fte3600_factory_startup_io *,
                                                                      const struct fte3600_factory_legacy_observer *,
                                                                      struct fte3600_factory_legacy_result *);
enum fte3600_factory_legacy_child {
  FTE3600_FL_CHILD_PING, FTE3600_FL_CHILD_APPLICATION,
  FTE3600_FL_CHILD_FACTORY, FTE3600_FL_CHILD_ARM, FTE3600_FL_CHILD_INITIALIZE
};
/* Additive physical child entry: no outer discovery, construction or tail.
 * Input/model/selection and cumulative result come from the owner. It retains
 * prior diagnostics and clock, replacing status with this child's result.
 * ARM/INITIALIZE require a selected legacy model. Existing ownership and
 * preflight rules apply; negative initialization does not discard selection. */
enum fte3600_factory_legacy_error fte3600_factory_legacy_child_run (enum fte3600_factory_legacy_child,
                                                                    const struct fte3600_factory_legacy_input *,
                                                                    struct fte3600_factory_legacy_state *,
                                                                    const struct fte3600_factory_startup_io *,
                                                                    const struct fte3600_factory_legacy_observer *,
                                                                    struct fte3600_factory_legacy_result *);
/* Additive entry permits NULL/zero firmware on non-initializing children.
* INITIALIZE and the original entry retain strict payload requirements. */
enum fte3600_factory_legacy_error fte3600_factory_legacy_child_run_optional_firmware (enum fte3600_factory_legacy_child,
                                                                                      const struct fte3600_factory_legacy_input *,
                                                                                      struct fte3600_factory_legacy_state *,
                                                                                      const struct fte3600_factory_startup_io *,
                                                                                      const struct fte3600_factory_legacy_observer *,
                                                                                      struct fte3600_factory_legacy_result *);
#ifdef __cplusplus
}
#endif
#endif
