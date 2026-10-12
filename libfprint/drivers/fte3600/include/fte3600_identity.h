/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_IDENTITY_H
#define FTE3600_IDENTITY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Project-local categories, not Linux errno or Windows status values. */
enum fte3600_status {
  FTE3600_OK,
  FTE3600_UNRECOGNIZED,
  FTE3600_TRANSPORT_ERROR,
  FTE3600_INVALID_INPUT
};

enum fte3600_family {
  FTE3600_FAMILY_UNKNOWN,
  FTE3600_FAMILY_LEGACY,
  FTE3600_FAMILY_A8
};

enum fte3600_sensor {
  FTE3600_SENSOR_UNKNOWN,
  FTE3600_FT9338,
  FTE3600_FT9348,
  FTE3600_FT9361,
  FTE3600_FT9536
};

enum fte3600_boot {
  FTE3600_BOOT_UNKNOWN,
  FTE3600_BOOT_A,
  FTE3600_BOOT_B
};

struct fte3600_family_result
{
  enum fte3600_status status;
  enum fte3600_family family;
};

struct fte3600_identity_result
{
  enum fte3600_status status;
  enum fte3600_sensor sensor;
  enum fte3600_boot boot;
};

/*
 * Inputs are scalar responses; these functions perform no device I/O.
 * read_ok must be exactly 0 or 1. Response words must fit in 16 bits,
 * response bytes in 8 bits. Domain errors precede transport errors.
 * All results are returned by value. Every non-OK result has UNKNOWN
 * family or UNKNOWN sensor and boot, including after earlier successes.
 */
struct fte3600_family_result fte3600_classify_family (uint32_t response_word,
                                                      int      read_ok);

/*
 * The caller must have obtained boot marker 0xef before using this path.
 * read_ok reports whether the prepared FE read succeeded.
 */
struct fte3600_identity_result fte3600_classify_boot_a (uint32_t response_byte,
                                                        int      read_ok);

/* read_ok is 1 only when both the family and prepared OTP reads succeeded. */
struct fte3600_identity_result fte3600_classify_otp (uint32_t family_word,
                                                     uint32_t otp_byte,
                                                     int      read_ok);

#ifdef __cplusplus
}
#endif

#endif
