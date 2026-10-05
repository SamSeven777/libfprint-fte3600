/*
 * FocalTech FTE3600 sensor family driver
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "fte3600.h"
#include "fte3600-build-config.h"
#include "fte3600-protocol.h"
#include "fte3600-sensor.h"
#include "fte3600-firmware.h"
#include "fte3600-resources.h"
#include "drivers_api.h"

typedef struct _Fte3600Backend Fte3600Backend;
#define FTE3600_TRANSPORT_CAP_CS_POLARITY (1U << 0)
G_DECLARE_FINAL_TYPE (FpiDeviceFte3600, fpi_device_fte3600, FPI,
                      DEVICE_FTE3600, FpDevice);

struct _FpiDeviceFte3600
{
  FpDevice                       parent;

  gint                           spi_fd;
  gint                           reset_fd;
  gint                           irq_fd;
  gboolean                       spi_configured;
  gboolean                       spi_configuration_invalid;
  guint32                        original_speed;
  guint8                         original_bits;
  Fte3600Resources               resources;
  gboolean                       capturing;
  gboolean                       armed;
  gboolean                       idle_verified;
  gboolean                       session_failed;
  gboolean                       init_hardware_reset_attempted;
  gboolean                       init_firmware_upload_attempted;
  guint                          init_mcu_status_attempts;
  GBytes                        *firmware_bytes;
  guint                          enroll_stages_passed;
  gboolean                       enroll_needs_release;
  gboolean                       waiting_for_release;

  guint32                        max_transfer;
  guint32                        spi_mode;
  guint32                        transport_capabilities;
  const Fte3600SensorDescriptor *sensor;
  const Fte3600Backend          *backend;
  gpointer                       backend_data;
  Fte3600Identity                identity;
  Fte3600Identity                rom_identity;
  Fte3600Sensor                  probed_sensor;
  Fte3600Identity                cached_identity;
  guint64                        cached_generation;
  gchar                         *cached_glue_path;
  gboolean                       cached_identity_valid;
  gboolean                       cached_cs_high;
  gboolean                       fast_open;
  gsize                          image_size;
  gsize                          capture_frame_size;
  gboolean                       discovery_boot_only;
  gboolean                       discovery_touched;
  guint8                         identity_high;
  guint8                         otp;
  guint16                        family;
  guint8                         discovery_rx[64];
  GSource                       *irq_source;
  GSource                       *irq_guard_source;
  FpiSsm                        *irq_wait_ssm;
  gint64                         arm_deadline;
  guint                          arm_attempts;
  gint64                         capture_ready_deadline;
  guint                          false_irq_count;

  struct _Fte3600Template       *enroll_template;
  struct _Fte3600Template       *verify_template;
  FpImage                       *captured_image;

  guint8                         small_rx[FTE3600_SMALL_FRAME_SIZE];
  gboolean                       small_rx_valid;
  guint8                        *capture_tx;
  guint8                        *capture_rx;
};

/* A backend owns chip protocol state machines. The core owns the device fd,
 * action completion and host matching; the kernel owns electrical resources. */
struct _Fte3600Backend
{
  gconstpointer configuration;
  /* Raw wire sample width and framing, independent of the final 8-bit image.
   * prepare_capture may only encode buffers/allocate host state, never do I/O. */
  guint bytes_per_pixel;
  guint frame_overhead;
  /* Zero requires a single complete wire frame. Chunked backends specify the
   * largest individual transaction, including its command and trailer. */
  guint    required_transfer_size;
  gboolean (*prepare_capture) (FpiDeviceFte3600 *self,
                               GError          **error);
  void     (*destroy) (FpiDeviceFte3600 *self);
  FpiSsm  *(*create_init) (FpiDeviceFte3600 *self);
  FpiSsm  *(*create_capture) (FpiDeviceFte3600 *self);
  /* Optional, protocol-backed release observation between enrollment frames.
   * Success must leave armed=FALSE and idle_verified=TRUE. Clearing a latched
   * interrupt is not evidence of release. NULL preserves capture behavior for
   * protocols whose release indication has not yet been established. */
  FpiSsm *(*create_wait_release) (FpiDeviceFte3600 *self);
  /* Reusable awake idle after an action, including cancellation. */
  FpiSsm *(*create_reset) (FpiDeviceFte3600 *self);
  /* Optional final-close quiesce. Runs even after verified awake idle, with
   * transport/IRQ/reset ownership still held. Completion only establishes
   * that the documented shutdown sequence finished, not an invented sleep
   * status. Clear idle_verified; the core always releases resources, including
   * on failure. Backends without this hook retain their verified idle cleanup.
   */
  FpiSsm *(*create_shutdown) (FpiDeviceFte3600 *self);
};

const Fte3600Backend *fpi_fte3600_backend_for_sensor (Fte3600Sensor sensor);
const Fte3600Backend *fpi_fte3600_legacy_backend (Fte3600Sensor sensor);
FpiSsm *fpi_fte3600_discovery_new (FpiDeviceFte3600 *self,
                                   gboolean          boot_only);

typedef enum {
  FTE3600_RESET_FOR_OPEN_ERROR,
  FTE3600_RESET_FOR_CLOSE,
  FTE3600_RESET_FOR_ACTION_ERROR,
} Fte3600ResetPurpose;

typedef struct
{
  Fte3600ResetPurpose purpose;
  GError             *operation_error;
} Fte3600ResetData;

void fpi_fte3600_secure_clear (gpointer data,
                               gsize    size);
void fpi_fte3600_clear_irq_source (FpiDeviceFte3600 *self);
void fpi_fte3600_deassert_hardware_reset_best_effort (FpiDeviceFte3600 *self,
                                                      const gchar      *context);
void fpi_fte3600_release_transport (FpiDeviceFte3600 *self);
gboolean fpi_fte3600_drain_irq_events (FpiDeviceFte3600 *self,
                                       GError          **error);
void fpi_fte3600_wait_for_irq (FpiSsm *ssm);
void fpi_fte3600_submit_transfer (FpiSsm         *ssm,
                                  FpiSpiTransfer *transfer,
                                  gboolean        cancellable);
void fpi_fte3600_submit_reg_write (FpiSsm  *ssm,
                                   guint8   reg,
                                   guint8   value,
                                   gboolean cancellable);
void fpi_fte3600_submit_reg_read (FpiSsm  *ssm,
                                  guint8   reg,
                                  gsize    result_len,
                                  gboolean cancellable);
guint8 fpi_fte3600_read_result_byte (FpiDeviceFte3600 *self);
gboolean fpi_fte3600_mcu_is_idle (FpiDeviceFte3600 *self);
void fpi_fte3600_set_hardware_reset (FpiSsm           *ssm,
                                     FpiDeviceFte3600 *self,
                                     gboolean          asserted);
gboolean fpi_fte3600_fail_if_cancelled (FpiSsm   *ssm,
                                        FpDevice *dev);
gboolean fpi_fte3600_transport_open (FpiDeviceFte3600 *self,
                                     GError          **error);
gboolean fpi_fte3600_transport_close (FpiDeviceFte3600 *self,
                                      GError          **error);
gboolean fpi_fte3600_transport_check (FpDevice *device,
                                      GError  **error);
gboolean fpi_fte3600_set_cs_polarity (FpiDeviceFte3600 *self,
                                      gboolean          active_high,
                                      GError          **error);
void fpi_fte3600_clear_captured_image (FpiDeviceFte3600 *self);
void fpi_fte3600_start_boot_discovery (FpiSsm *parent);
