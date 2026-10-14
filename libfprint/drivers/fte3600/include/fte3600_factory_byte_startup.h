/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_FACTORY_BYTE_STARTUP_H
#define FTE3600_FACTORY_BYTE_STARTUP_H
#include "fte3600_factory_startup.h"
#include "fte3600_ft9368_boot.h"
#ifdef __cplusplus
extern "C" {
#endif
struct fte3600_factory_byte_io
{
  void    *context;
  /* High bit means failure; failed exchanges leave the frame unchanged. */
  uint32_t (*spi)(void *,
                  uint8_t *,
                  size_t);
  void     (*wait_ms)(void *,
                      uint32_t);
  void     (*wait_us)(void *,
                      uint32_t);
  uint32_t (*gpio)(void *,
                   int level);
  uint32_t (*boundary)(void *,
                       enum fte3600_factory_boundary);
  /* Optional final physical H + SPI 55 aa after H/10ms/L/20ms. Writes the
   * GPIO status, returns overall high-bit failure status. A successful GPIO
   * means SPI was attempted; no callbacks may interrupt the pair.
   * NULL uses ordinary callbacks. Private source ABI: rebuild consumers. */
  uint32_t (*release_and_sync)(void *,
                               uint32_t *gpio_status);
};
struct fte3600_factory_byte_identity { size_t      length;
                                       const char *sha256;
};
/* Identities only: these objects do not contain firmware bytes. */
extern const struct fte3600_factory_byte_identity fte3600_factory_byte_application_identity;
extern const struct fte3600_factory_byte_identity fte3600_factory_byte_pram_identity;
struct fte3600_factory_byte_selected
{
  enum fte3600_factory_model model;
  uint16_t                                    width, height;
  uint8_t                                     expected_version, agc_version;
  /* Legacy settle/mode fields are deliberately unrepresented: unspecified. */
  const struct fte3600_factory_byte_io       *board;
  const struct fte3600_factory_byte_identity *application_identity, *pram_identity;
  struct fte3600_ft9368_boot_image            application, pram;
  uint8_t                                     bound;
};
struct fte3600_factory_byte_state
{
  struct fte3600_factory_byte_selected selected;
  uint32_t                             attempt_counter;
};
struct fte3600_factory_byte_input
{
  uint32_t                         scene, resume_cleanup;
  enum fte3600_factory_model initial_model;
  struct fte3600_ft9368_boot_image pram, application;
};
enum fte3600_factory_byte_phase {
  FTE3600_BYTE_SHORT_DETECT, FTE3600_BYTE_PROBE, FTE3600_BYTE_DISCARD,
  FTE3600_BYTE_SELECT, FTE3600_BYTE_BIND, FTE3600_BYTE_LOAD,
  FTE3600_BYTE_VERSION, FTE3600_BYTE_PRAM, FTE3600_BYTE_APPLICATION,
  FTE3600_BYTE_RESET
};
struct fte3600_factory_byte_result
{
  uint32_t status;
  enum fte3600_factory_model model;
  uint8_t  contact_pending, sensor_constructed;
  uint64_t physical_calls, event_count, clock_us, byte_protocol_calls;
  uint64_t information_replies_consumed;
  uint32_t transport_failures, last_transport_failure, gpio_failures, last_gpio_failure;
  uint32_t reset_calls, last_reset_status, version_status, version_failures;
  uint32_t version_identity_mismatches, pram_failures, application_failures;
  uint32_t boot_identity_failures, erase_failures, program_failures, checksum_failures;
  uint32_t erase_poll_mismatches, program_poll_mismatches, postload_identity_failures;
  uint32_t pram_attempts, application_attempts, last_pram_status, last_application_status;
};
struct fte3600_factory_byte_observer
{
  void *context;
  /* Passive, synchronous entry/exit observation; no mutation or reentry.
  * has_status is false for void stages and exits after a local error. */
  void (*phase)(void *,
                enum fte3600_factory_byte_phase,
                int      entering,
                int      has_status,
                uint32_t status,
                const struct fte3600_factory_byte_result *);
};
enum fte3600_factory_byte_error {
  FTE3600_FACTORY_BYTE_OK, FTE3600_FACTORY_BYTE_INVALID_ARGUMENT,
  FTE3600_FACTORY_BYTE_UNSUPPORTED, FTE3600_FACTORY_BYTE_PRAM_READ_ERROR,
  FTE3600_FACTORY_BYTE_APPLICATION_READ_ERROR
};
enum fte3600_factory_byte_error fte3600_factory_byte_state_defaults (struct fte3600_factory_byte_state *);
enum fte3600_factory_byte_error fte3600_factory_ft9368_defaults (struct fte3600_factory_byte_selected *,
                                                                 const struct fte3600_factory_byte_io *);
/* Allocation-free SPI direct-route composition. Caller owns explicit synthetic
 * payloads (positive lengths); PRAM accessible extent includes full 128-byte
 * padding. No firmware discovery, generation, or fallback success callbacks.
 * All pointed-to objects must be valid, aligned, serialized and disjoint,
 * including payload extents. Preflight errors leave state/result unchanged
 * with no callbacks. Context storage is caller-owned and must not alias mutable
 * arguments. State retains its PRAM counter across calls. After successful
 * preflight, each call clears the old selected binding and result before any
 * callback. Early boundary/unsupported exits therefore leave no old borrowed
 * board/payload references. Local errors retain completed effects and stop
 * dependent work.
 * Compatibility status is meaningful only on local OK. Observer is optional.
 * Callbacks, pointers and immutable inputs must remain valid until return;
 * selected payload/board bindings remain borrowed references afterward. */
enum fte3600_factory_byte_error fte3600_factory_byte_startup_run (const struct fte3600_factory_byte_input *,
                                                                  struct fte3600_factory_byte_state *,
                                                                  const struct fte3600_factory_byte_io *,
                                                                  const struct fte3600_factory_byte_observer *,
                                                                  struct fte3600_factory_byte_result *);
/* Additive child entry for a previously constructed/bound selection. No outer
 * boundaries, discovery, selection or tail. Keeps cumulative diagnostics and
 * counter; resets only this loader's compatibility status. check_version is
 * 0 or 1. The same payload, alias and observer requirements apply. */
enum fte3600_factory_byte_error fte3600_factory_byte_load_selected (struct fte3600_factory_byte_state *,
                                                                    int check_version,
                                                                    const struct fte3600_factory_byte_io *,
                                                                    const struct fte3600_factory_byte_observer *,
                                                                    struct fte3600_factory_byte_result *);
#ifdef __cplusplus
}
#endif
#endif
