/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_DISCOVERY_H
#define FTE3600_DISCOVERY_H

#include "fte3600_identity.h"
#include "fte3600_recovery.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Perform one bounded legacy SPI identity transaction on a selected device.
 * The caller must establish that this legacy protocol applies and serialize
 * access to the device. No allocation, retries, firmware or device setup occur.
 *
 * Reuses the recovery callback layout: gpio/spi return zero on success, any
 * nonzero value on failure. gpio, spi and wait_ms are required; clock_ms is
 * unused and may be null. context may be null. SPI tx/rx buffers are distinct,
 * synchronous, and may not be retained; successful SPI must fill all rx bytes.
 * GPIO uses recovery_io's physical-level convention: 0 means low, 1 high.
 * The observed callback numbers do not establish the device's physical reset
 * polarity. No automatic polarity conversion occurs. Direct Linux composition
 * requires the caller to establish the board-level reset mapping and supply
 * the selected output GPIO without active-low inversion.
 *
 * Invalid arguments return INVALID_INPUT before any callbacks. A failed I/O
 * callback immediately returns TRANSPORT_ERROR, with no cleanup or waits.
 * Every non-OK return contains UNKNOWN sensor and boot, including failure of
 * the final OTP cleanup write. Callbacks and their context must remain valid
 * until return, and callbacks must return for bounded completion.
 */
struct fte3600_identity_result fte3600_discover_legacy (const struct fte3600_recovery_io *io);

/* Same operation, with optional passive reset entry/exit observation. */
struct fte3600_identity_result fte3600_discover_legacy_observed (const struct fte3600_recovery_io    *io,
                                                                 const struct fte3600_reset_observer *observer);

/* Additive factory state observation. After valid preflight, boot_edition is
 * set to 1 before the marker exchange and to 0 after its success. A completed
 * recognized legacy Boot B changes it to 1; failures and unrecognized replies
 * retain the partial value. Identity return remains normalized exactly as in
 * the older entries. The caller replaces its model only on OK. boot_edition,
 * io and observer must be aligned, valid, disjoint and not mutated/reentered
 * by callbacks. Invalid input changes nothing and invokes no callbacks. */
struct fte3600_identity_result fte3600_discover_legacy_retained (const struct fte3600_recovery_io *,
                                                                 const struct fte3600_reset_observer *,
                                                                 uint32_t *boot_edition);

#ifdef __cplusplus
}
#endif

#endif
