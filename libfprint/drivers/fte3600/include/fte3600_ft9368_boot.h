/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_FT9368_BOOT_H
#define FTE3600_FT9368_BOOT_H
#include "fte3600_ft9368.h"
#ifdef __cplusplus
extern "C" {
#endif
#define FTE3600_FT9368_PRAM_MAX ((size_t) 6096)
#define FTE3600_FT9368_APPLICATION_MAX ((size_t) 27120)
struct fte3600_ft9368_boot_image
{
  const uint8_t *data;
  size_t         length;
  size_t         capacity;
};
struct fte3600_ft9368_boot_io
{
  struct fte3600_ft9368_io protocol;
  void                    *gpio_context;
  uint32_t                 (*gpio)(void *,
                                   int);
  void                    *microsecond_context;
  void                     (*wait_us)(void *,
                                      uint32_t);
};
enum fte3600_ft9368_boot_error {
  FTE3600_FT9368_BOOT_OK,
  FTE3600_FT9368_BOOT_INVALID_ARGUMENT,
  FTE3600_FT9368_BOOT_TRANSPORT_FAILURE
};
/* Caller supplies firmware and explicit PRAM padding: nonempty PRAM capacity
 * must reach 128*ceil(length/128), length <=6096 and divisible by four.
 * Application capacity must reach its even length <=27120. NULL data is valid
 * only at length zero. No source allocation, padding inference or firmware
 * discovery occurs. Buffers stay immutable; caller serializes operations and
 * keeps pointed-to objects valid, unchanged and nonoverlapping until return.
 * All I/O callbacks are mandatory. Frames are synchronous; successful reads
 * must fill the complete payload, failed exchanges must leave frames unchanged.
 *
 * Local errors are separate from compatibility results (pram 0/1,
 * application 0/1/2/3/5, load 0/0xc0000001). *result changes only on local OK.
 * Argument errors occur before side effects or counter changes. Failed payload
 * reads stop immediately with TRANSPORT_FAILURE, including inside load/version
 * probes; they are never retried as compatibility failures. Ignored write/GPIO
 * statuses retain the reviewed behavior. Counter increments modulo 2^32.
 * No allocation/global state. Real writes may be persistent; this module does
 * not choose a device, firmware, board mapping, or establish startup success.
 */
enum fte3600_ft9368_boot_error fte3600_ft9368_boot_pram (const struct fte3600_ft9368_boot_image *,
                                                         uint32_t *attempt_counter,
                                                         const struct fte3600_ft9368_boot_io *,
                                                         uint32_t *result);
/* Optional passive reset observation for composition. Entry has status zero;
 * exit has the actual reset compatibility status. The observer must not
 * mutate/reenter the operation or any objects it uses. A supplied observer
 * requires a callback. Existing PRAM behavior is otherwise unchanged. */
struct fte3600_ft9368_boot_observer
{
  void *context;
  void  (*reset)(void *,
                 int      entering,
                 uint32_t status);
};
enum fte3600_ft9368_boot_error fte3600_ft9368_boot_pram_observed (const struct fte3600_ft9368_boot_image *,
                                                                  uint32_t *attempt_counter,
                                                                  const struct fte3600_ft9368_boot_io *,
                                                                  const struct fte3600_ft9368_boot_observer *,
                                                                  uint32_t *result);
enum fte3600_ft9368_boot_error fte3600_ft9368_boot_application (const struct fte3600_ft9368_boot_image *,
                                                                const struct fte3600_ft9368_boot_io *,
                                                                uint32_t *result);
enum fte3600_ft9368_boot_error fte3600_ft9368_boot_load (const struct fte3600_ft9368_boot_image *pram,
                                                         const struct fte3600_ft9368_boot_image *application,
                                                         uint32_t                               *attempt_counter,
                                                         int                                     check_version,
                                                         uint8_t                                 expected_version,
                                                         const struct fte3600_ft9368_boot_io *,
                                                         uint32_t                               *result);
/* Recovery callbacks remain caller-owned. Requires spi, gpio, wait_ms plus
 * explicit microsecond callback; its context can be the Linux callback context
 * with fte3600_linux_wait_us. No unit conversion or board I/O is inserted.
 * Recovery zero/nonzero maps to zero/FT9368_ADAPTER_IO_ERROR. Invalid input
 * leaves output unchanged. Inspect Linux error after every operation because
 * compatibility paths intentionally ignore write/GPIO/wait failures. Linux
 * transfer_capacity >=263 is sufficient for these three operations.
 */
enum fte3600_ft9368_boot_error fte3600_ft9368_boot_io_from_recovery (const struct fte3600_recovery_io *,
                                                                     void *microsecond_context,
                                                                     void ( *wait_us )(void *, uint32_t),
                                                                     struct fte3600_ft9368_boot_io *output);
#ifdef __cplusplus
}
#endif
#endif
