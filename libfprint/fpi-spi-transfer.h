/*
 * FPrint spidev transfer handling
 * Copyright (C) 2019-2020 Benjamin Berg <bberg@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include "fpi-compat.h"
#include "fpi-device.h"

G_BEGIN_DECLS

#define FPI_TYPE_SPI_TRANSFER (fpi_spi_transfer_get_type ())

typedef struct _FpiSpiTransfer FpiSpiTransfer;
typedef struct _FpiSsm         FpiSsm;

/**
 * FpiSpiTransferGuard:
 * @device: The device whose transport session is checked
 * @error: Location for the failure reason
 *
 * Runs in the transfer worker before and after each successful SPI message.
 * The driver must keep its session alive until all transfers complete. Guards
 * may read immutable session metadata, but must not call main-context device
 * methods. Return %FALSE and set @error if the session is no longer valid.
 *
 * Returns: Whether the transport session is valid
 */
typedef gboolean (*FpiSpiTransferGuard)(FpDevice *device,
                                        GError  **error);

/* Runs in the worker after validation and the pre-transfer guard, immediately
 * before a single full-duplex message. Useful for a GPIO release that must
 * precede the first SPI clock. Do not call main-context device methods here.
 * A failed preparation suppresses the message; normal completion and the
 * post-transfer guard still apply. The device must outlive the transfer. */
typedef gboolean (*FpiSpiTransferPrepare)(FpDevice *device,
                                          GError  **error);

typedef void (*FpiSpiTransferCallback)(FpiSpiTransfer *transfer,
                                       FpDevice       *dev,
                                       gpointer        user_data,
                                       GError         *error);

/**
 * FpiSpiTransfer:
 * @device: The #FpDevice that the transfer belongs to.
 * @ssm: Storage slot to associate the transfer with a state machine.
 *   Used by fpi_ssm_spi_transfer_cb() to modify the given state machine.
 * @length_wr: The length of the write buffer
 * @length_rd: The length of the read buffer
 * @buffer_wr: The write buffer.
 * @buffer_rd: The read buffer.
 * @sensitive: Whether buffer contents must be redacted from transfer logs.
 *
 * Helper for handling SPI transfers. Transfers can either be pure write/read
 * transfers, a write followed by a read, or a simultaneous full-duplex
 * transfer.
 */
struct _FpiSpiTransfer
{
  /*< public >*/
  FpDevice *device;

  FpiSsm   *ssm;

  gssize    length_wr;
  gssize    length_rd;

  guchar   *buffer_wr;
  guchar   *buffer_rd;

  /*< private >*/
  guint ref_count;

  int   spidev_fd;
  gsize buffer_size;

  /* Callbacks */
  gpointer               user_data;
  FpiSpiTransferCallback callback;
  FpiSpiTransferGuard    guard;
  FpiSpiTransferPrepare  prepare;

  /* Data free function */
  GDestroyNotify free_buffer_wr;
  GDestroyNotify free_buffer_rd;

  /* Transfer options */
  gboolean full_duplex;
  gboolean sensitive;
};

GType              fpi_spi_transfer_get_type (void) G_GNUC_CONST;
void               fpi_spi_transfer_set_device_guard (FpDevice           *device,
                                                      FpiSpiTransferGuard guard);
FpiSpiTransfer     *fpi_spi_transfer_new (FpDevice *device,
                                          int       spidev_fd);
FpiSpiTransfer     *fpi_spi_transfer_new_with_buffer_size (FpDevice *device,
                                                           int       spidev_fd,
                                                           gsize     buffer_size);
FpiSpiTransfer     *fpi_spi_transfer_ref (FpiSpiTransfer *self);
void               fpi_spi_transfer_unref (FpiSpiTransfer *self);

void               fpi_spi_transfer_write (FpiSpiTransfer *transfer,
                                           gsize           length);

FP_GNUC_ACCESS (read_only, 2, 3)
void               fpi_spi_transfer_write_full (FpiSpiTransfer *transfer,
                                                guint8         *buffer,
                                                gsize           length,
                                                GDestroyNotify  free_func);

void               fpi_spi_transfer_read (FpiSpiTransfer *transfer,
                                          gsize           length);

FP_GNUC_ACCESS (write_only, 2, 3)
void               fpi_spi_transfer_read_full (FpiSpiTransfer *transfer,
                                               guint8         *buffer,
                                               gsize           length,
                                               GDestroyNotify  free_func);

void               fpi_spi_transfer_set_full_duplex (FpiSpiTransfer *transfer,
                                                     gboolean        full_duplex);

void               fpi_spi_transfer_set_sensitive (FpiSpiTransfer *transfer,
                                                   gboolean        sensitive);

void               fpi_spi_transfer_set_prepare (FpiSpiTransfer       *transfer,
                                                 FpiSpiTransferPrepare prepare);

void               fpi_spi_transfer_submit (FpiSpiTransfer        *transfer,
                                            GCancellable          *cancellable,
                                            FpiSpiTransferCallback callback,
                                            gpointer               user_data);

gboolean           fpi_spi_transfer_submit_sync (FpiSpiTransfer *transfer,
                                                 GError        **error);


G_DEFINE_AUTOPTR_CLEANUP_FUNC (FpiSpiTransfer, fpi_spi_transfer_unref)

G_END_DECLS
