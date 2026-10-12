/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_FACTORY_STARTUP_H
#define FTE3600_FACTORY_STARTUP_H
#include "fte3600_factory_dispatch.h"
#include "fte3600_ft93xx.h"
#include "fte3600_fw9369_startup.h"
#ifdef __cplusplus
extern "C" {
#endif

enum fte3600_factory_boundary {
  FTE3600_BOUNDARY_PLATFORM, FTE3600_BOUNDARY_INTERFACE,
  FTE3600_BOUNDARY_SETTINGS, FTE3600_BOUNDARY_PREPARE,
  FTE3600_BOUNDARY_RESOURCES, FTE3600_BOUNDARY_INITIAL_CONTACT,
  FTE3600_BOUNDARY_FRAMEWORK_INIT, FTE3600_BOUNDARY_REGISTRY_READ,
  FTE3600_BOUNDARY_REGISTRY_CLEAR, FTE3600_BOUNDARY_FRAMEWORK_FINISH
};
enum fte3600_factory_startup_phase {
  FTE3600_PHASE_SHORT_DETECT, FTE3600_PHASE_BYTE_PROBE,
  FTE3600_PHASE_WORD_WAKE, FTE3600_PHASE_WORD_PROBE,
  FTE3600_PHASE_SELECT, FTE3600_PHASE_RESET,
  FTE3600_PHASE_MODERN_BIND, FTE3600_PHASE_MODERN_STARTUP,
  FTE3600_PHASE_MODERN_FDT, FTE3600_PHASE_MODERN_IMAGE,
  FTE3600_PHASE_MODERN_ARM, FTE3600_PHASE_MODERN_FINALIZE,
  FTE3600_PHASE_FW_STARTUP, FTE3600_PHASE_FW_FDT,
  FTE3600_PHASE_FW_IMAGE, FTE3600_PHASE_FW_ARM
};
struct fte3600_factory_startup_io
{
  void *context;
  /* One shared physical SPI object, high-bit failure semantics. Synchronous;
   * on failure leave bytes unchanged, on success fill all response bytes. */
  uint32_t (*spi)(void *,
                  uint8_t *,
                  size_t);
  void     (*wait_ms)(void *,
                      uint32_t);
  uint32_t (*gpio)(void *,
                   unsigned int level);
  /* Explicit platform operations; return exact caller status. Registry READ
   * returns status, not registry contents: those are input.resume_cleanup. */
  uint32_t (*boundary)(void *,
                       enum fte3600_factory_boundary);
};
struct fte3600_factory_selected
{
  enum fte3600_factory_model model;
  uint16_t                                 width, height;
  const uint8_t                           *application, *pram;
  size_t                                   application_size, pram_size;
  const struct fte3600_factory_startup_io *board;
  /* Unspecified firmware/AGC/legacy fields deliberately have no defaults or
   * representation here. This is an independent logical object. */
};
struct fte3600_factory_startup_state
{
  struct fte3600_factory_selected     selected;
  struct fte3600_ft93xx_device        modern;
  struct fte3600_fw9369_startup_state fw;
};
struct fte3600_factory_startup_storage
{
  uint16_t                                pixels[FTE3600_FT93XX_STARTUP_SAMPLES];
  struct fte3600_fw9369_startup_workspace fw;
};
struct fte3600_factory_startup_input
{
  uint32_t scene, resume_cleanup;
  enum fte3600_factory_model initial_model;
};
enum fte3600_factory_startup_error {
  FTE3600_FACTORY_STARTUP_OK, FTE3600_FACTORY_STARTUP_INVALID_ARGUMENT,
  FTE3600_FACTORY_STARTUP_UNSUPPORTED, FTE3600_FACTORY_STARTUP_WORD_READ_ERROR,
  FTE3600_FACTORY_STARTUP_MODERN_ERROR, FTE3600_FACTORY_STARTUP_FW_ERROR
};
struct fte3600_factory_startup_result
{
  /* Compatibility status is meaningful only when the API returns local OK. */
  uint32_t                                  status;
  enum fte3600_factory_model model;
  uint8_t                                   contact_pending, sensor_constructed, modern_ran, fw_ran;
  uint16_t                                  outer_raw_id;
  uint64_t                                  physical_calls, event_count, clock_ms, byte_protocol_calls;
  uint32_t                                  transport_failures, last_transport_failure, gpio_failures;
  uint32_t                                  last_gpio_failure, reset_calls, last_reset_status;
  uint32_t                                  discovery_exhausted, wake_unacknowledged, negotiation_exhausted;
  uint32_t                                  child_status;
  struct fte3600_ft93xx_startup_diagnostics modern;
  struct fte3600_fw9369_startup_diagnostics fw;
};
struct fte3600_factory_startup_observer
{
  void *context;
  /* Entry then exit, nested in execution order. Snapshots count physical
   * SPI/GPIO/wait events and requested milliseconds, never wall clock.
   * On exit has_status distinguishes defined compatibility returns from
   * void stages and local errors. Passive callbacks must not mutate/reenter
   * this operation, its state/storage/I/O or diagnostics. */
  void (*phase)(void *,
                enum fte3600_factory_startup_phase,
                int                                          entering,
                int                                          has_status,
                uint32_t                                     status,
                const struct fte3600_factory_startup_result *snapshot);
};
/* Reusable word-discovery primitives. The caller supplies a cumulative result;
 * these never clear it. Optional observer is passive as above. RESET includes
 * the requested post-reset wait, outside its reset phase (zero omits it).
 * Invalid input makes no callbacks or writes. PROBE rejects exhausted reads
 * locally and leaves *identity untouched. Objects must be valid/disjoint. */
enum fte3600_factory_word_operation {
  FTE3600_FACTORY_WORD_WAKE, FTE3600_FACTORY_WORD_PROBE,
  FTE3600_FACTORY_WORD_RESET
};
enum fte3600_factory_startup_error fte3600_factory_word_operation (enum fte3600_factory_word_operation,
                                                                   uint32_t reset_wait_ms,
                                                                   const struct fte3600_factory_startup_io *,
                                                                   const struct fte3600_factory_startup_observer *,
                                                                   struct fte3600_factory_startup_result *,
                                                                   uint16_t *identity);
/* Named independent constructors: no allocation or I/O. Invalid arguments
 * leave destination unchanged. Geometry is constructor geometry, distinct
 * from later physical identity/profile geometry. */
enum fte3600_factory_startup_error fte3600_factory_fw9369_defaults (struct fte3600_factory_selected *,
                                                                    const struct fte3600_factory_startup_io *);
enum fte3600_factory_startup_error fte3600_factory_ft9365_defaults (struct fte3600_factory_selected *,
                                                                    const struct fte3600_factory_startup_io *);
enum fte3600_factory_startup_error fte3600_factory_ft9769_defaults (struct fte3600_factory_selected *,
                                                                    const struct fte3600_factory_startup_io *);
enum fte3600_factory_startup_error fte3600_factory_startup_state_defaults (struct fte3600_factory_startup_state *);
/* SPI-only prepared board. Apply retained family state once before call.
 * Required objects must be aligned, valid, serialized and disjoint; capacity
 * is bytes and must cover storage. Runtime storage is caller-owned, allocation
 * free, with no firmware payload. Invalid arguments cause no effects. Valid
 * calls clear the old selected binding and result before any callback, and
 * preserve family state until its actual child changes it. Early exits leave
 * no old borrowed board/payload references. Local errors retain completed
 * effects and stop dependent work;
 * result.status is separate compatibility status. No legacy/byte recovery.
 * Observers are optional and must remain valid until return. Callers retain
 * reply/register cursors and actual clock in their shared transport context. */
enum fte3600_factory_startup_error fte3600_factory_startup_run (const struct fte3600_factory_startup_input *,
                                                                struct fte3600_factory_startup_state *,
                                                                struct fte3600_factory_startup_storage *,
                                                                size_t capacity,
                                                                const struct fte3600_factory_startup_io *,
                                                                const struct fte3600_factory_startup_observer *,
                                                                struct fte3600_factory_startup_result *);
#ifdef __cplusplus
}
#endif
#endif
