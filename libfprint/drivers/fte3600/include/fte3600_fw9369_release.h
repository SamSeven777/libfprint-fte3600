/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_FW9369_RELEASE_H
#define FTE3600_FW9369_RELEASE_H
#include "fte3600_fw9369_startup.h"
#ifdef __cplusplus
extern "C" {
#endif
enum fte3600_fw9369_detection { FTE3600_FW9369_DOWN = 1, FTE3600_FW9369_UP = 2 };
enum fte3600_fw9369_contact {
  FTE3600_FW9369_CONTACT_NONE, FTE3600_FW9369_CONTACT_DOWN,
  FTE3600_FW9369_CONTACT_UP, FTE3600_FW9369_CONTACT_INVALID
};
enum fte3600_fw9369_release_status {
  FTE3600_FW9369_RELEASE_OK, FTE3600_FW9369_RELEASE_INVALID_ARGUMENT,
  FTE3600_FW9369_RELEASE_WORD_READ_ERROR, FTE3600_FW9369_RELEASE_ADC_READ_ERROR,
  FTE3600_FW9369_RELEASE_TRANSPORT_ERROR, FTE3600_FW9369_RELEASE_NOT_READY,
  FTE3600_FW9369_RELEASE_IMAGE_ERROR, FTE3600_FW9369_RELEASE_UNDEFINED_NEIGHBORHOOD
};
enum fte3600_fw9369_release_outcome {
  FTE3600_FW9369_RELEASE_PENDING, FTE3600_FW9369_RELEASE_CONFIRMED,
  FTE3600_FW9369_RELEASE_RENEWED_TOUCH
};
enum fte3600_fw9369_event {
  FTE3600_FW9369_EVENT_PENDING, FTE3600_FW9369_EVENT_TOUCH,
  FTE3600_FW9369_EVENT_RELEASED, FTE3600_FW9369_EVENT_RECOVERY
};
struct fte3600_fw9369_observation
{
  enum fte3600_fw9369_contact contact;
  uint16_t                    samples[8];
  int32_t                     differences[8];
};
struct fte3600_fw9369_release_diagnostics
{
  struct fte3600_capture_diagnostics transport;
  unsigned                           observations, confirmed, maintained, recalibrated, image_status;
};
/* Reviewed release-v1 operations, serialized with all other sensor I/O.
 * Strict runtime policy rejects exhausted transactions/readiness. Invalid
 * arguments invoke no callbacks and leave outputs/diagnostics unchanged.
 * Valid calls clear diagnostics; errors preserve prior physical/state effects,
 * but never publish an observation/outcome. Caller may stage retained state.
 * No allocation, IRQ waiting, GPIO, clock, implicit recovery or mutable globals. */
enum fte3600_fw9369_release_status fte3600_fw9369_observe (struct fte3600_fw9369_startup_state *,
                                                           enum fte3600_fw9369_detection,
                                                           struct fte3600_fw9369_observation *,
                                                           const struct fte3600_capture_io *,
                                                           struct fte3600_fw9369_release_diagnostics *);
enum fte3600_fw9369_release_status fte3600_fw9369_confirm_release (struct fte3600_fw9369_startup_state *,
                                                                   uint16_t samples[3][8],
                                                                   int     *confirmed,
                                                                   const struct fte3600_capture_io *,
                                                                   struct fte3600_fw9369_release_diagnostics *);
/* Pure retained-state helpers: samples must be the three accepted vectors.
 * CRC covers only active words; inactive channels remain unchanged. */
enum fte3600_fw9369_release_status fte3600_fw9369_update_fdt (struct fte3600_fw9369_startup_state *,
                                                              const uint16_t samples[3][8]);
enum fte3600_fw9369_release_status fte3600_fw9369_baseline_needs_reset (const struct fte3600_fw9369_startup_state *,
                                                                        int *needed);
enum fte3600_fw9369_release_status fte3600_fw9369_arm_detection (struct fte3600_fw9369_startup_state *,
                                                                 enum fte3600_fw9369_detection,
                                                                 const struct fte3600_capture_io *,
                                                                 struct fte3600_fw9369_release_diagnostics *);
/* Read status then acknowledge that exact value. Does not classify combined
 * flags or claim contact/release from an IRQ notification alone. */
enum fte3600_fw9369_release_status fte3600_fw9369_read_irq (uint16_t *status,
                                                            const struct fte3600_capture_io *,
                                                            struct fte3600_fw9369_release_diagnostics *);
/* Three-observation confirmation; when maintenance_pending, update FDT,
 * observe Down, warm-update image, observe Down, and conditionally recalibrate.
 * No rearm: the session must complete Up/Down rearm before publishing outcome.
 * A renewed touch retains reviewed maintenance effects; it is not release. */
enum fte3600_fw9369_release_status fte3600_fw9369_release_run (struct fte3600_fw9369_startup_state *,
                                                               int maintenance_pending,
                                                               struct fte3600_fw9369_startup_workspace *,
                                                               size_t,
                                                               enum fte3600_fw9369_release_outcome *,
                                                               const struct fte3600_capture_io *,
                                                               struct fte3600_fw9369_release_diagnostics *);
/* Complete reviewed ordinary event query: communication check/retries, initial
 * status priority, release work and late status/acknowledgments. Late UP may
 * supersede the initial result and always requests maintenance. Linux applies
 * the renewed-touch postcheck guard to both paths. Rejected normal UP increments
 * retained rejections (eleventh resets it and requests recovery); successful or
 * late confirmation does not reset it. Final Down rearm belongs to caller. */
enum fte3600_fw9369_release_status fte3600_fw9369_event_run (struct fte3600_fw9369_startup_state *,
                                                             int       maintenance_pending,
                                                             unsigned *rejections,
                                                             struct fte3600_fw9369_startup_workspace *,
                                                             size_t,
                                                             enum fte3600_fw9369_event *,
                                                             const struct fte3600_capture_io *,
                                                             struct fte3600_fw9369_release_diagnostics *);
#ifdef __cplusplus
}
#endif
#endif
