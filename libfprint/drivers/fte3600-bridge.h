/* SPDX-License-Identifier: MIT
 * FTE3600 resource bridge ABI. All fields have identical 32/64-bit layouts.
 */
#ifndef FTE3600_BRIDGE_H
#define FTE3600_BRIDGE_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define FTE3600_BRIDGE_ABI 1
#define FTE3600_BRIDGE_MAX_TRANSFER 32768U
#define FTE3600_BRIDGE_CAP_CS_POLARITY (1U << 0)

struct fte3600_bridge_info
{
  __u32 abi_version;
  __u32 max_transfer;
  __u32 speed_hz;
  __u32 mode;
  __u32 bits_per_word;
  /* Additive ABI 1 capability; older bridges return zero in this slot. */
  __u32 capabilities;
  __u32 reserved[2];
};

/* SPI_IOC_MESSAGE(1) is supported, with one unsplit transfer and no overrides.
 * poll(POLLIN) reports a pending interrupt; GET_EVENTS atomically consumes it.
 * Events coalesce: this is a wakeup, not a count of fingers or image frames.
 * SET_RESET uses logical assertion, independent of the electrical polarity.
 * One opener only. Removal/suspend invalidates the session; close and reopen.
 */
#define FTE3600_IOC_GET_INFO _IOR ('F', 0x80, struct fte3600_bridge_info)
#define FTE3600_IOC_SET_RESET _IOW ('F', 0x81, __u32)
#define FTE3600_IOC_GET_EVENTS _IOR ('F', 0x82, __u32)
/* Optional discovery control: physical active-low (0) or active-high (1).
 * Does not change CPOL/CPHA, speed or word size. Requires CAP_CS_POLARITY. */
#define FTE3600_IOC_SET_CS_POLARITY _IOW ('F', 0x83, __u32)

#endif
