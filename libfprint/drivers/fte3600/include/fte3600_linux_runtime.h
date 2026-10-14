/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_LINUX_RUNTIME_H
#define FTE3600_LINUX_RUNTIME_H
#include "fte3600_linux_board.h"
#include <gio/gio.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifndef FTE3600_FIRMWARE_DIR
#define FTE3600_FIRMWARE_DIR "/usr/lib/firmware/fte3600"
#endif
struct fte3600_linux_runtime;
enum fte3600_linux_runtime_error {
  FTE3600_RUNTIME_OK,
  FTE3600_RUNTIME_INVALID_ARGUMENT,
  FTE3600_RUNTIME_NO_MEMORY,
  FTE3600_RUNTIME_PAIRING,
  FTE3600_RUNTIME_IO,
  FTE3600_RUNTIME_FIRMWARE,
  FTE3600_RUNTIME_SESSION,
  FTE3600_RUNTIME_CANCELLED,
  FTE3600_RUNTIME_TIMEOUT,
  FTE3600_RUNTIME_RELEASE_UNSUPPORTED,
  FTE3600_RUNTIME_NOT_READY
};
struct fte3600_linux_runtime_diagnostic
{
  enum fte3600_linux_runtime_error code;
  int                              system_error;
  enum fte3600_device_error        device_error;
  char                             firmware_file[64];
  /* Error-time scalar snapshot. No frame, template or firmware bytes.
   * last_configured_mode is cached, not an electrical measurement after a
   * failed mode change; cs_retry_attempted also records unsuccessful trials. */
  unsigned has_session_status, component_error;
  enum fte3600_factory_model model;
  uint16_t raw_id, chip_id;
  uint32_t acpi_mode, last_configured_mode;
  unsigned cs_control, cs_retry_attempted;
};
struct fte3600_linux_runtime_config
{
  /* Exact absolute spidev path supplied by libfprint; no hidraw/USB route.
   * Test roots default to /sys and /dev; all normal board checks still run. */
  const char *spidev_path, *sys_root, *dev_root, *firmware_dir;
};
struct fte3600_linux_runtime_frame
{
  uint8_t                     *pixels;
  size_t                       length, stride;
  struct fte3600_device_status status;
};
struct fte3600_linux_runtime_pair
{
  char                            spi[64], gpio[64], uio[64];
  struct fte3600_linux_board_info board;
};
/* Read-only exact ancestry/ABI/cdev matching. Ambiguous valid pairs fail.
 * Never spawns a scanner or opens a hardware character device. */
enum fte3600_linux_runtime_error fte3600_linux_runtime_discover (const struct fte3600_linux_runtime_config *,
                                                                 struct fte3600_linux_runtime_pair *,
                                                                 struct fte3600_linux_runtime_diagnostic *);
/* Synchronous, single serialized owner (e.g. a GTask worker). open claims and
 * performs actual cold startup, returning measured identity/geometry. It
 * tries the opposite CS polarity once only when native CS control is advertised
 * and startup reports unconfirmed identity without a transport failure. The
 * retry uses a fresh session; close restores the original ACPI mode.
 * Calls retain a cancellable reference only for their duration; cancellation may be
 * signalled safely from any thread. Failed open always releases resources.
 * Cancelled/failed I/O requires close+open before reuse. */
enum fte3600_linux_runtime_error fte3600_linux_runtime_open (const struct fte3600_linux_runtime_config *,
                                                             GCancellable *,
                                                             struct fte3600_linux_runtime **,
                                                             struct fte3600_linux_runtime_diagnostic *);
enum fte3600_linux_runtime_error fte3600_linux_runtime_start (struct fte3600_linux_runtime *,
                                                              GCancellable *,
                                                              struct fte3600_linux_runtime_diagnostic *);
/* Capture arms and waits for a UIO event, with physical status qualification
 * and bounded polling for FW9369. FW9369 automatically completes pending
 * release/maintenance first under the same deadline, preserving queued touches.
 * Caller supplies an
 * empty frame; successful output owns malloc pixels and exact sequence status.
 * Timeout covers arm/wait/acquire and callbacks observe its monotonic deadline. */
enum fte3600_linux_runtime_error fte3600_linux_runtime_capture (struct fte3600_linux_runtime *,
                                                                GCancellable *,
                                                                uint32_t timeout_ms,
                                                                struct fte3600_linux_runtime_frame *,
                                                                struct fte3600_linux_runtime_diagnostic *);
/* FT9368 uses healthy information observations. FW9369 uses physical UP flags,
 * three reviewed FDT confirmations, retained maintenance and Down rearm;
 * repeated calls after completion do not repeat maintenance. Partial failure,
 * cancellation or timeout requires close+open before another capture.
 * Unknown family release semantics fail explicitly; no synthetic finger-up. */
enum fte3600_linux_runtime_error fte3600_linux_runtime_wait_release (struct fte3600_linux_runtime *,
                                                                     GCancellable *,
                                                                     uint32_t timeout_ms,
                                                                     struct fte3600_linux_runtime_diagnostic *);
enum fte3600_linux_runtime_error fte3600_linux_runtime_get_status (const struct fte3600_linux_runtime *,
                                                                   struct fte3600_device_status *);
void fte3600_linux_runtime_frame_clear (struct fte3600_linux_runtime_frame *);
/* Always frees, including on close failure. NULL is allowed. */
enum fte3600_linux_runtime_error fte3600_linux_runtime_close (struct fte3600_linux_runtime *,
                                                              struct fte3600_linux_runtime_diagnostic *);
const char *fte3600_linux_runtime_error_string (enum fte3600_linux_runtime_error);
/* Shared verified firmware loader. Requires empty owned[2] and firmware-free
 * config, binds legacy model explicitly, and leaves outputs unchanged on error.
 * Successful owned data must outlive its session; caller frees both pointers. */
enum fte3600_linux_runtime_error fte3600_linux_firmware_load (const char *directory,
                                                              enum fte3600_factory_model,
                                                              struct fte3600_device_config *,
                                                              uint8_t *owned[2],
                                                              struct fte3600_linux_runtime_diagnostic *);
#ifdef __cplusplus
}
#endif
#endif
