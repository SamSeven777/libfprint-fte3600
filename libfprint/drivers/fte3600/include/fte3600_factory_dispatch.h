/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_FACTORY_DISPATCH_H
#define FTE3600_FACTORY_DISPATCH_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum fte3600_factory_model {
  FTE3600_MODEL_UNKNOWN, FTE3600_MODEL_FT9338, FTE3600_MODEL_FT9348,
  FTE3600_MODEL_FT9361, FTE3600_MODEL_FT9368, FTE3600_MODEL_FT9536,
  FTE3600_MODEL_FW9369, FTE3600_MODEL_FT9769, FTE3600_MODEL_FT9365
};
struct fte3600_factory_classification
{
  uint32_t status;
  int      replace_model;
  enum fte3600_factory_model model;
};
struct fte3600_factory_io
{
  void    *context;
  uint32_t (*initial_contact)(void *);
  uint8_t  (*short_detect)(void *);
  uint16_t (*probe_byte_id)(void *);
  uint16_t (*probe_word_id)(void *);
  void     (*discard_sensor)(void *);
  void     (*configure_byte_sensor)(void *);
  void     (*wake_word_sensor)(void *);
  uint32_t (*select_sensor)(void *,
                            enum fte3600_factory_model);
  uint32_t (*load_byte_firmware)(void *,
                                 int check_version);
  uint32_t (*reset_sensor)(void *);
  uint32_t (*start_fw9369)(void *);
  uint32_t (*start_modern_registers)(void *);
  uint32_t (*start_modern_calibration)(void *);
  uint32_t (*legacy_ping)(void *);
  /* Write the result synchronously; replacement applies even on failure. */
  void     (*legacy_application)(void *,
                                 struct fte3600_factory_classification *);
  void     (*legacy_factory)(void *,
                             struct fte3600_factory_classification *);
  uint32_t (*arm_legacy)(void *,
                         uint8_t mode);
  uint32_t (*load_9338_firmware)(void *);
  uint32_t (*command)(void *,
                      uint16_t command,
                      size_t   length);
  void (*wait_ms)(void *,
                  uint32_t);
};
enum fte3600_factory_ignored {
  FTE3600_FACTORY_IGNORED_SELECT, FTE3600_FACTORY_IGNORED_RESET,
  FTE3600_FACTORY_IGNORED_FW9369, FTE3600_FACTORY_IGNORED_REGISTERS,
  FTE3600_FACTORY_IGNORED_CALIBRATION, FTE3600_FACTORY_IGNORED_COMMAND,
  FTE3600_FACTORY_IGNORED_COUNT
};
struct fte3600_factory_input
{
  uint32_t bus, scene;
  enum fte3600_factory_model model;
  int      cleanup;
};
struct fte3600_factory_result
{
  uint32_t                   status;
  enum fte3600_factory_model model;
  int                        contact_pending;
  uint32_t                   ignored_failures[FTE3600_FACTORY_IGNORED_COUNT];
};
enum fte3600_factory_error {
  FTE3600_FACTORY_OK, FTE3600_FACTORY_INVALID_ARGUMENT,
  FTE3600_FACTORY_INVALID_CLASSIFICATION
};
/* Portable orchestration only. All callbacks are required; no allocation or
 * default hardware I/O. Callers serialize access and keep arguments disjoint
 * and valid. Invalid arguments leave result unchanged before any callback.
 * Invalid callback classification stops locally, retaining partial output.
 * Compatibility statuses live in result->status, separate from local errors.
 * Ignored high-bit statuses are counted by category; select's exact fatal
 * 0xc0000001 is acted on and is therefore not counted as ignored.
 * Callback completion is not proof of firmware, startup, or hardware support.
 */
enum fte3600_factory_error fte3600_factory_dispatch (const struct fte3600_factory_input *,
                                                     const struct fte3600_factory_io *,
                                                     struct fte3600_factory_result *);
#ifdef __cplusplus
}
#endif
#endif
