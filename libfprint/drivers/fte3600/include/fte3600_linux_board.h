/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_LINUX_BOARD_H
#define FTE3600_LINUX_BOARD_H
#include "fte3600_device_session.h"
#include <signal.h>
#ifdef __cplusplus
extern "C" {
#endif
struct fte3600_linux_board;
struct fte3600_linux_board_config
{
  const char                  *sys_root, *dev_root; /* Defaults /sys, /dev. */
  const char                  *spi, *gpio, *uio; /* Basenames: spidevB.C, gpiochipN, uioN. */
  size_t                       transfer_capacity; /* 1..INT_MAX. One transaction, never split. */
  const volatile sig_atomic_t *cancelled;
  /* Optional thread-safe cancellation query. The caller owns context and
   * keeps both alive through close; query is invoked on the owner thread.
   * Use an atomic/GCancellable inside query, never share sig_atomic_t across
   * threads. The older cancelled pointer remains for signal handlers. */
  int   (*is_cancelled)(void *context);
  void *cancel_context;
};
struct fte3600_linux_board_info
{
  uint64_t generation;
  uint32_t acpi_mode, acpi_speed_hz, spi_bufsiz;
  unsigned cs_control, irq_active_low;
};
/* Validate only: reads sysfs metadata and stats nodes; NEVER opens a device.
* Requires exact FTE3600 firmware identity, ancestry and character identities.
* Synthetic roots retain the same validation; regular files never qualify. */
int fte3600_linux_board_validate (const struct fte3600_linux_board_config *,
                                  struct fte3600_linux_board_info *);
/* euid zero required. Claim active-low reset OUTPUT logical 0, then UIO
 * O_RDONLY|O_NONBLOCK. Own all opened fds. Check generation/identity around
 * every hardware operation. Sticky failures prevent further hardware I/O.
 * Configure native CS from ACPI; fixed CS NEVER writes mode. Preserve initial
 * bits/speed; native CS returns to ACPI mode on close (not stale trial mode).
 * Effective speed is the lesser of ACPI maximum and saved nonzero spidev
 * maximum; saved zero falls back to ACPI. Use it for every transfer.
 * No old fte3600_linux_create composition: reset uses active-low conversion. */
int fte3600_linux_board_open (const struct fte3600_linux_board_config *,
                              struct fte3600_linux_board **);
const struct fte3600_device_io *fte3600_linux_board_callbacks (struct fte3600_linux_board *);
int fte3600_linux_board_get_info (const struct fte3600_linux_board *,
                                  struct fte3600_linux_board_info *);
/* Cached last configured/readback-verified mode; no I/O. After a failed mode
 * change this is the previous mode, not a new electrical-state measurement. */
int fte3600_linux_board_get_mode (const struct fte3600_linux_board *,
                                  uint32_t *);
/* Native CS only; fixed CS returns ENOTSUP without issuing a mode write. */
int fte3600_linux_board_set_cs (struct fte3600_linux_board *,
                                int active_high);
/* 1 event, 0 timeout, -1 error; full 4-byte native UIO counter; bounded
 * monotonic wait, periodic lease checks and optional signal cancellation. */
int fte3600_linux_board_wait_irq (struct fte3600_linux_board *,
                                  uint32_t  timeout_ms,
                                  uint32_t *counter);
/* Close UIO before reset. Restore settings only while identity/generation
 * remains valid; verify restored ACPI mode. Report restoration/lease errors;
 * never retry failed close. Frees even after failure. */
int fte3600_linux_board_close (struct fte3600_linux_board *);
#ifdef __cplusplus
}
#endif
#endif
