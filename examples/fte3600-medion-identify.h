/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Explicit Medion legacy-sensor diagnostic, outside automatic discovery.
 */
#pragma once

#include <gio/gio.h>
#include "drivers/fte3600-sensor.h"

typedef struct
{
  gpointer user_data;
  gsize    max_transfer;
  /* One full-duplex transaction. TRUE means success. */
  gboolean (*exchange) (gpointer      user_data,
                        const guint8 *tx,
                        guint8       *rx,
                        gsize         length,
                        GError      **error);
  /* TRUE asserts controller-level low; FALSE releases high. Ignore cancellation
   * here: an already-started reset must always be released. */
  gboolean (*set_reset) (gpointer user_data,
                         gboolean asserted,
                         GError **error);
  /* Complete H10/L20/H followed by 55 AA in one synchronous operation.
   * Prepare/check the transport before the pulse and check again after SPI.
   * Do not log, dispatch events, allocate or inspect configuration between
   * the final release and sync. Finish the operation before cancellation.
   * Even a failed assertion must be held low and followed by a release attempt. */
  gboolean (*reset_and_sync) (gpointer user_data,
                              GError **error);
  /* Wait at least milliseconds, without failing merely due to cancellation. */
  gboolean (*wait) (gpointer user_data,
                    guint    milliseconds,
                    GError **error);
  /* TRUE means cancelled, like g_cancellable_set_error_if_cancelled(). */
  gboolean (*check_cancelled) (gpointer user_data,
                               GError **error);
  /* Optional, synchronous; only stage names and non-biometric register values. */
  void (*report) (gpointer     user_data,
                  const gchar *message);
} Fte3600MedionIdentifyIo;

/* This explicitly requested Medion candidate experiment may enter ROM and
 * configure OTP access after an empty application response. It performs no
 * firmware upload or flash operation. It is NOT automatic detection policy.
 *
 * TRUE requires a repeated, stable known result and successful reset cleanup.
 * FALSE leaves result zeroed, with raw observations available through report.
 * For ROM_BOOT_B38_SPI_OTP, response is the observed family mailbox value,
 * NOT a fabricated runtime geometry. The result is only for diagnostic output:
 * never copy it to a device's identity/rom_identity or authorize an upload.
 */
gboolean fte3600_medion_identify_legacy (const Fte3600MedionIdentifyIo *io,
                                         Fte3600Identity               *result,
                                         GError                       **error);
