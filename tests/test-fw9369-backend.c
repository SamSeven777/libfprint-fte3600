/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Real state machines, packet builders and asynchronous transfer ownership.
 * This fixture models public wire behavior using synthetic samples only. */
#include <string.h>
#include <linux/spi/spidev.h>
#include "drivers/fte3600-fw9369.h"
#include "drivers/fte3600-protocol.h"
#include "drivers/fte3600-legacy-recovery.h"

G_DEFINE_TYPE (FpiDeviceFte3600, fpi_device_fte3600, FP_TYPE_DEVICE)

static struct
{
  guint         mode_reads, mode_failures;
  gboolean      mode_io_error;
  guint         recovery_fault;
  guint         recovery_setups;
  gint64        recovery_ack_time;
  gint64        recovery_start_time;
  GCancellable *cancellable;
  guint8        sfr[256];
  guint16       words[0x8000];
  guint         transactions;
  guint         images;
  guint         irqs;
  guint16       irq_events[8];
  guint         irq_count;
  guint         irq_index;
  gboolean      uncovered_first_irq;
  gboolean      cancel_after_fault_ack;
  guint         sleep_irq;
  guint16       event_after_ack;
  gboolean      fdt_running;
  guint         fdt_commands;
  gboolean      fail_fdt_restart;
  gboolean      cancel_fdt_restart;
  guint16       release_events[4];
  guint         release_count;
  guint         release_index;
  guint         cancel_release_after;
  guint         idle_commands;
  guint         status_reads;
  guint         fail_transaction;
  guint         fdt_samples;
  guint         fdt_writes;
  guint         drains;
  guint         wake_commands;
  guint         sleep_commands;
  guint         id_reads;
  guint         event_reads;
  guint         unavailable_ids;
  guint         wakes_at_irq;
  guint         events_at_irq;
  guint         wrong_id_transaction;
  guint16       irq_identity;
  gint64        sleep_time;
  gboolean      sleeping;
  gboolean      sleep_on_irq;
  gboolean      fail_sleep;
  gboolean      short_sleep;
  gboolean      fail_mask;
  gboolean      fail_ack;
  gboolean      reject_mask;
  gboolean      cancel_communication_wake;
  gboolean      discovering;
  gboolean      physical_cs_high;
  gboolean      fail_discovery_wake;
  gboolean      change_discovery_id;
  guint         discovery_ids;
  guint         blank_discovery_ids;
  guint         discovery_app_reads;
  guint         discovery_legacy_commands;
  guint         discovery_rom_commands;
  guint         discovery_firmware_commands;
  guint         discovery_9368_wakes[2];
  guint         discovery_info_reads[2];
  guint         discovery_special_wakes[2];
  guint         discovery_mode_writes[2];
  guint         discovery_mode_reads[2];
  guint         discovery_mode_writes_since_wake[2];
  guint         discovery_mode_reads_since_wake[2];
  guint         discovery_id_queries[2];
  gboolean      discovery_wake_finished[2];
  guint         cs_changes;
  guint         reset_edges;
  gboolean      reset_asserted;
  gboolean      fail_fast_reset;
  gboolean      cancel_fast_reset;
  gint64        reset_times[3];
  guint         failure_transaction;
  gboolean      completed;
  gboolean      finger;
  gboolean      smic;
  gboolean      cancel_irq;
  gboolean      cancel_image;
  gboolean      cancel_bank;
  gboolean      fail_image;
  gboolean      unstable;
  gboolean      low_fdt;
  gboolean      low_image;
  gboolean      reject_idle;
  gboolean      fail_release_cleanup;
  gboolean      enrolling;
  gboolean      fail_release_arm;
  gboolean      cancel_release_arm;
  GError       *error;
} mock;

static void
fpi_device_fte3600_init (FpiDeviceFte3600 *self)
{
}

static void
fpi_device_fte3600_class_init (FpiDeviceFte3600Class *klass)
{
  FpDeviceClass *device = FP_DEVICE_CLASS (klass);

  device->id = "fw9369-backend-test";
  device->full_name = "FW9369 simulated backend";
  device->type = FP_DEVICE_TYPE_VIRTUAL;
  device->features = FP_DEVICE_FEATURE_CAPTURE;
}

static guint16
be16 (const guint8 *p)
{
  return ((guint16) p[0] << 8) | p[1];
}

static void
put16 (guint8 *p, guint16 value)
{
  p[0] = value >> 8;
  p[1] = value & 0xff;
}

void
fpi_fte3600_secure_clear (gpointer data, gsize size)
{
  if (data)
    memset (data, 0, size);
}

void
fpi_fte3600_clear_captured_image (FpiDeviceFte3600 *self)
{
  g_clear_object (&self->captured_image);
}

void
fpi_fte3600_clear_irq_source (FpiDeviceFte3600 *self)
{
  self->irq_wait_ssm = NULL;
}

gboolean
fpi_fte3600_drain_irq_events (FpiDeviceFte3600 *self, GError **error)
{
  mock.drains++;
  return TRUE;
}

gboolean
fpi_fte3600_fail_if_cancelled (FpiSsm *ssm, FpDevice *dev)
{
  GError *error = NULL;

  if (!g_cancellable_set_error_if_cancelled (mock.cancellable, &error))
    return FALSE;
  fpi_ssm_mark_failed (ssm, error);
  return TRUE;
}

void
fpi_fte3600_wait_for_irq (FpiSsm *ssm)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));

  /* The sensor cannot produce a new detection event after C0 until C2
   * restarts FDT. Do not let the mock conceal an unarmed second wait. */
  g_assert_true (mock.fdt_running);
  mock.irqs++;
  mock.wakes_at_irq = mock.wake_commands;
  mock.events_at_irq = mock.event_reads;
  if (mock.sleep_on_irq || mock.sleep_irq == mock.irqs)
    mock.sleeping = TRUE;
  if (mock.irq_identity)
    mock.words[0x1a8b] = mock.irq_identity;
  self->armed = FALSE;
  if (mock.cancel_irq)
    {
      g_cancellable_cancel (mock.cancellable);
      fpi_fte3600_fail_if_cancelled (ssm, FP_DEVICE (self));
    }
  else
    {
      if (mock.irq_count)
        {
          gboolean release = (mock.words[0x1881] & 0x83) == 0;

          g_assert_cmpuint (mock.words[0x1a83] & (release ? 4 : 2), ==, release ? 4 : 2);
          if (!mock.words[0x1a82])
            {
              g_assert_cmpuint (mock.irq_index, <, mock.irq_count);
              mock.words[0x1a82] = mock.irq_events[mock.irq_index++];
            }
          mock.finger = !(mock.uncovered_first_irq && mock.irq_index == 1) &&
                        !!(mock.words[0x1a82] & 2);
          fpi_ssm_next_state (ssm);
          return;
        }
      if ((mock.words[0x1881] & 0x83) == 0)
        {
          g_assert_cmpuint (mock.words[0x1a83] & 4, ==, 4);
          if (!mock.words[0x1a82])
            {
              g_assert_cmpuint (mock.release_index, <, mock.release_count);
              mock.words[0x1a82] |= mock.release_events[mock.release_index++];
            }
          if (mock.cancel_release_after && mock.cancel_release_after == mock.release_index)
            g_cancellable_cancel (mock.cancellable);
          if (mock.fail_release_cleanup)
            {
              mock.sfr[0x80] = 0x52;
              mock.reject_idle = TRUE;
            }
          mock.finger = mock.words[0x1a82] != 4;
        }
      else
        {
          g_assert_cmpuint (mock.words[0x1881] & 0x83, ==, 0x81);
          g_assert_cmpuint (mock.words[0x1a83] & 2, ==, 2);
          mock.finger = TRUE;
          mock.words[0x1a82] |= 2;
        }
      fpi_ssm_next_state (ssm);
    }
}

GCancellable *__wrap_fpi_device_get_cancellable (FpDevice *device);
FpiDeviceAction __wrap_fpi_device_get_current_action (FpDevice *device);

FpiDeviceAction
__wrap_fpi_device_get_current_action (FpDevice *device)
{
  return mock.enrolling ? FPI_DEVICE_ACTION_ENROLL : FPI_DEVICE_ACTION_CAPTURE;
}

GCancellable *
__wrap_fpi_device_get_cancellable (FpDevice *device)
{
  return mock.cancellable;
}

static void
emulate_transfer (FpiSpiTransfer *transfer)
{
  const guint8 *tx = transfer->buffer_wr;
  guint8 *rx = transfer->buffer_rd;
  gsize length = transfer->length_wr;
  guint16 address;

  g_assert_cmpint (transfer->length_rd, ==, transfer->length_wr);
  g_assert_true (transfer->full_duplex);
  if (!mock.discovering)
    g_assert_cmpint (!!(FPI_DEVICE_FTE3600 (transfer->device)->spi_mode & SPI_CS_HIGH),
                     ==, mock.physical_cs_high);
  if (mock.discovering)
    {
      FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (transfer->device);

      memset (rx, 0, length);
      /* Other discovery dialects do not wake this sleeping FW9369. */
      if (!!(self->spi_mode & SPI_CS_HIGH) != mock.physical_cs_high)
        return;
      if (tx[0] == 0x10 || (tx[0] == 0x70 && length == 1) ||
          (tx[0] == 0xff && length == 4) ||
          (tx[0] == 0x91 && length == 39) ||
          (tx[0] == 0x04 && length == 10))
        return;
      if (tx[0] == 0x04 && length == 12 && be16 (tx + 2) == 0x9a8b)
        {
          if (mock.sleeping)
            {
              mock.blank_discovery_ids++;
            }
          else if (++mock.discovery_ids == 2 && mock.change_discovery_id)
            {
              put16 (rx + 6, 0x9365);
              return;
            }
        }
      /* ROM/firmware fallback after observing this family is not allowed. */
      g_assert_true (tx[0] == 0x04 || tx[0] == 0x08 || tx[0] == 0x09 ||
                     tx[0] == 0x5a || tx[0] == 0xa5 || tx[0] == 0xc0);
    }
  g_assert_cmpuint (length, >=, 3);
  g_assert_cmpuint (tx[0] ^ tx[1], ==, 0xff);
  memset (rx, 0, length);
  /* Sleeping hardware can expose a blank ID before wake. Configuration
   * writes cannot make it awake; normal register/image access is forbidden. */
  if (mock.sleeping && tx[0] != 0x5a)
    {
      if ((tx[0] == 0x04 && be16 (tx + 2) == 0x9a8b) ||
          (tx[0] == 0x09 && tx[2] == 0xc6))
        {
          if (tx[0] == 0x04)
            mock.id_reads++;
          return;
        }
      g_error ("Access %02x before waking sleeping FW9369", tx[0]);
    }
  switch (tx[0])
    {
    case 0x08:
      g_assert_cmpuint (length, ==, 5);
      if (tx[2] == 0x80)
        mock.status_reads++;
      rx[4] = mock.sfr[tx[2]];
      if (tx[2] == 0xc6)
        {
          mock.mode_reads++;
          if (mock.mode_reads <= mock.mode_failures)
            rx[4] = 0;
        }
      return;

    case 0x09:
      g_assert_cmpuint (length, ==, 4);
      mock.sfr[tx[2]] = tx[3];
      if (mock.cancel_bank && tx[2] == 0x9a && tx[3] == 0x5a)
        g_cancellable_cancel (mock.cancellable);
      return;

    case 0xc0:
      g_assert_cmpuint (length, ==, 3);
      mock.fdt_running = FALSE;
      mock.idle_commands++;
      if (!mock.reject_idle)
        mock.sfr[0x80] = 0x50;
      return;

    case 0x5a:
      mock.wake_commands++;
      mock.sleeping = FALSE;
      if (mock.irqs && mock.cancel_communication_wake)
        g_cancellable_cancel (mock.cancellable);
      g_assert_cmpuint (length, ==, 3);
      return;

    case 0xc1:
      g_assert_cmpuint (length, ==, 3);
      mock.fdt_running = FALSE;
      if (!mock.fail_mask && !mock.reject_mask)
        g_assert_cmpuint (mock.words[0x1a83] & 0x7ff, ==, 0);
      mock.sleeping = TRUE;
      return;

    case 0xa5:
      g_assert_cmpuint (length, ==, 3);
      return;

    case 0xc2:
      g_assert_cmpuint (length, ==, 3);
      mock.fdt_running = TRUE;
      mock.fdt_commands++;
      if (mock.irqs && mock.cancel_fdt_restart)
        g_cancellable_cancel (mock.cancellable);
      return;

    case 0xc4:
      g_assert_cmpuint (length, ==, 3);
      mock.fdt_running = FALSE;
      mock.sfr[0x80] = 0x54;
      return;

    case 0x06:
      g_assert_cmpuint (length, ==, 10246);
      g_assert_cmpuint (be16 (tx + 2), ==, 0x9a05);
      g_assert_cmpuint (be16 (tx + 4), ==, 5120);
      g_assert_cmpuint (mock.words[0x1800] & 1, ==, 1);
      g_assert_cmpuint (mock.words[0x1807], ==, mock.smic ? 0x12a1 : 0x18e1);
      g_assert_true (transfer->sensitive);
      for (guint i = 0; i < 5120; i++)
        put16 (rx + 6 + i * 2, mock.low_image ? 0 :
               mock.finger ? (i % 2 ? 1848 : 2148) : 2048);
      mock.images++;
      if (mock.cancel_image)
        g_cancellable_cancel (mock.cancellable);
      return;

    case 0x04:
    case 0x05:
      break;

    default:
      g_error ("Unestablished FW9369 command %02x", tx[0]);
    }
  g_assert_cmpuint (length, >=, 8);
  g_assert_cmpuint (tx[2] & 0x80, ==, 0x80);
  address = be16 (tx + 2) & 0x7fff;
  if (tx[0] == 0x04)
    {
      if (be16 (tx + 4) == 4)
        {
          g_assert_cmpuint (length, ==, 14);
          g_assert_cmpuint (address, ==, mock.smic ? 0xe8 : 0xb8);
          g_assert_true (transfer->sensitive);
          for (guint i = 0; i < 4; i++)
            put16 (rx + 6 + i * 2, mock.low_fdt ? 0 :
                   512 + (mock.unstable && mock.fdt_samples % 2 ? 40 : 0));
          mock.fdt_samples++;
        }
      else
        {
          g_assert_cmpuint (length, ==, 12);
          g_assert_cmpuint (be16 (tx + 4), ==, 1);
          if (address == 0x1a8b)
            {
              mock.id_reads++;
              if (mock.words[address] != 0x9362 && mock.words[address] != 0 && mock.words[address] != 0xffff)
                mock.wrong_id_transaction = mock.transactions;
              if (mock.unavailable_ids)
                {
                  mock.unavailable_ids--;
                  return;
                }
            }
          if (address == 0x1a82)
            mock.event_reads++;
          put16 (rx + 6, mock.words[address]);
        }
      return;
    }
  if (be16 (tx + 4) == 8)
    {
      g_assert_cmpuint (length, ==, 22);
      g_assert_cmpuint (address, ==, mock.smic ? 0xe0 : 0xb0);
      g_assert_cmpuint (mock.sfr[0x9a], ==, 0x5a);
      for (guint i = 0; i < 4; i++)
        g_assert_cmpuint (be16 (tx + 6 + 2 * i), ==, 482);
      for (guint i = 14; i < 22; i++)
        g_assert_cmpuint (tx[i], ==, 0);
      mock.fdt_writes++;
      return;
    }
  g_assert_cmpuint (length, ==, 8);
  g_assert_cmpuint (be16 (tx + 4), ==, 1);
  if (address == 0x1a84)
    {
      mock.words[0x1a82] &= ~be16 (tx + 6);
      if (mock.irqs == 1 && mock.event_after_ack)
        {
          mock.words[0x1a82] |= mock.event_after_ack;
          mock.event_after_ack = 0;
        }
    }
  else if (address == 0x1a83 && mock.reject_mask)
    {
      return;
    }
  else
    {
      mock.words[address] = be16 (tx + 6);
    }
  if (address == 0x1885)
    {
      g_assert_cmpuint (be16 (tx + 6), ==, 1);
      mock.words[0x1a82] |= 8;
    }
  if (address == 0x1881 && (be16 (tx + 6) & 0x83) == 0 && mock.cancel_release_arm)
    g_cancellable_cancel (mock.cancellable);
}

typedef struct
{
  FpiSpiTransfer        *transfer;
  GCancellable          *cancellable;
  FpiSpiTransferCallback callback;
  gpointer               user_data;
} Pending;

static gboolean
cancel_recovery_wait (gpointer user_data)
{
  g_cancellable_cancel (mock.cancellable);
  return G_SOURCE_REMOVE;
}

static void
record_discovery_transfer (FpiSpiTransfer *transfer)
{
  static const guint8 wake_9368[] = { 0xff, 0x00, 0x00, 0x00 };
  static const guint8 info_9368[] = { 0x91, 0x80, 0x00, 0x20, 0x00, 0x00, 0x00 };
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (transfer->device);
  guint cs = !!(self->spi_mode & SPI_CS_HIGH);
  const guint8 *tx = transfer->buffer_wr;
  gsize length = transfer->length_wr;

  /* Record every submitted transaction, even when the selected CS cannot
   * reach the chip. Zero-filled replies must not conceal extra protocols. */
  if (tx[0] == 0xff)
    {
      g_assert_cmpmem (tx, length, wake_9368, sizeof wake_9368);
      g_assert_cmpuint (mock.discovery_9368_wakes[cs], ==, mock.discovery_info_reads[cs]);
      mock.discovery_9368_wakes[cs]++;
    }
  else if (tx[0] == 0x91)
    {
      g_assert_cmpuint (length, ==, 39);
      g_assert_cmpmem (tx, sizeof info_9368, info_9368, sizeof info_9368);
      g_assert_cmpuint (mock.discovery_9368_wakes[cs], ==, mock.discovery_info_reads[cs] + 1);
      mock.discovery_info_reads[cs]++;
    }
  else if (tx[0] == 0x5a)
    {
      g_assert_cmpuint (mock.discovery_info_reads[cs], ==, 2);
      mock.discovery_special_wakes[cs]++;
      mock.discovery_mode_writes_since_wake[cs] = 0;
      mock.discovery_mode_reads_since_wake[cs] = 0;
      mock.discovery_wake_finished[cs] = FALSE;
    }
  else if (tx[0] == 0xa5)
    {
      g_assert_cmpuint (mock.discovery_special_wakes[cs], >, 0);
      mock.discovery_wake_finished[cs] = TRUE;
    }
  else if (tx[0] == 0x09 && length == 4 && tx[2] == 0xc6)
    {
      g_assert_true (mock.discovery_wake_finished[cs]);
      g_assert_cmpuint (tx[3], ==, 1);
      mock.discovery_mode_writes[cs]++;
      mock.discovery_mode_writes_since_wake[cs]++;
    }
  else if (tx[0] == 0x08 && length == 5 && tx[2] == 0xc6)
    {
      mock.discovery_mode_reads[cs]++;
      mock.discovery_mode_reads_since_wake[cs]++;
      g_assert_cmpuint (mock.discovery_mode_reads_since_wake[cs], ==,
                        mock.discovery_mode_writes_since_wake[cs]);
    }
  else if (tx[0] == 0x04 && length == 12 && be16 (tx + 2) == 0x9a8b)
    {
      /* Reset is not identity evidence. Both C6 helpers must have completed
       * on this CS after this attempt's explicit wake, even after reset. */
      g_assert_true (mock.discovery_wake_finished[cs]);
      g_assert_cmpuint (mock.discovery_mode_reads_since_wake[cs], ==,
                        cs == mock.physical_cs_high ? 2 : 62);
      g_assert_cmpuint (mock.discovery_mode_writes_since_wake[cs], ==,
                        mock.discovery_mode_reads_since_wake[cs]);
      mock.discovery_id_queries[cs]++;
    }
  else if (tx[0] == 0x10)
    {
      mock.discovery_app_reads++;
    }
  else if (length == 1 && tx[0] == 0x70)
    {
      mock.discovery_legacy_commands++;
    }
  else if (tx[0] == 0x90 || tx[0] == 0x55)
    {
      mock.discovery_rom_commands++;
    }
  else if (tx[0] == 0x05)
    {
      mock.discovery_firmware_commands++;
    }
}

static gboolean
complete_transfer (gpointer user_data)
{
  Pending *pending = user_data;
  FpiSpiTransfer *transfer = pending->transfer;
  GError *error = NULL;

  mock.transactions++;
  if (mock.irqs && transfer->buffer_wr[0] == 0x08 && transfer->buffer_wr[2] == 0xc6)
    {
      mock.recovery_setups++;
      if (!mock.recovery_start_time)
        mock.recovery_start_time = g_get_monotonic_time ();
      switch (mock.recovery_fault)
        {
        case 1:
          error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Recovery transport failure");
          break;

        case 2:
          g_cancellable_cancel (mock.cancellable);
          break;

        case 3:
          mock.words[0x1a8b] = 0x9365;
          break;

        case 4:
          mock.unstable = TRUE;
          break;

        case 5:
          error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT, "Short recovery transfer");
          break;

        case 6:
          error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CLOSED, "Resource generation changed");
          break;
        }
    }
  if (mock.irqs && transfer->buffer_wr[0] == 0x05 &&
      be16 (transfer->buffer_wr + 2) == 0x9a84 &&
      (be16 (transfer->buffer_wr + 6) & 0x610) && !mock.recovery_ack_time)
    {
      mock.recovery_ack_time = g_get_monotonic_time ();
      if (mock.cancel_after_fault_ack)
        g_timeout_add (1, cancel_recovery_wait, NULL);
    }
  if (mock.discovering)
    record_discovery_transfer (transfer);
  if (!error && pending->cancellable)
    g_cancellable_set_error_if_cancelled (pending->cancellable, &error);
  if (!error && mock.mode_io_error && transfer->buffer_wr[0] == 0x08 &&
      transfer->buffer_wr[2] == 0xc6)
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "C6 transport failure");
  if (!error && mock.discovering && mock.fail_discovery_wake && transfer->buffer_wr[0] == 0x5a)
    {
      mock.failure_transaction = mock.transactions;
      error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Injected discovery wake failure");
    }
  if (transfer->buffer_wr[0] == 0xc1)
    {
      mock.sleep_commands++;
      mock.sleep_time = g_get_monotonic_time ();
      if (!error && (mock.fail_sleep || mock.short_sleep))
        error = g_error_new_literal (G_IO_ERROR,
                                     mock.short_sleep ? G_IO_ERROR_PARTIAL_INPUT : G_IO_ERROR_FAILED,
                                     "Injected sleep transfer failure");
    }
  if (!error && mock.fail_mask && transfer->buffer_wr[0] == 0x04 &&
      be16 (transfer->buffer_wr + 2) == 0x9a83)
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Injected mask read failure");
  if (!error && mock.fail_ack && transfer->buffer_wr[0] == 0x05 &&
      be16 (transfer->buffer_wr + 2) == 0x9a84)
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Injected event acknowledge failure");
  if (!error && (mock.transactions == mock.fail_transaction ||
                 (mock.fail_image && transfer->buffer_wr[0] == 0x06)))
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Injected SPI failure");
  if (!error && mock.fail_release_arm && transfer->buffer_wr[0] == 0x05 &&
      (be16 (transfer->buffer_wr + 2) & 0x7fff) == 0x1881 &&
      (be16 (transfer->buffer_wr + 6) & 0x83) == 0)
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Injected release-arm failure");
  if (!error && mock.irqs && mock.fail_fdt_restart && transfer->buffer_wr[0] == 0xc2)
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Injected FDT restart failure");
  if (!error)
    emulate_transfer (transfer);
  pending->callback (transfer, transfer->device, pending->user_data, error);
  fpi_spi_transfer_unref (transfer);
  g_clear_object (&pending->cancellable);
  g_free (pending);
  return G_SOURCE_REMOVE;
}

void __wrap_fpi_spi_transfer_submit (FpiSpiTransfer        *transfer,
                                     GCancellable          *cancellable,
                                     FpiSpiTransferCallback callback,
                                     gpointer               user_data);
void
__wrap_fpi_spi_transfer_submit (FpiSpiTransfer        *transfer,
                                GCancellable          *cancellable,
                                FpiSpiTransferCallback callback,
                                gpointer               user_data)
{
  Pending *pending = g_new0 (Pending, 1);

  pending->transfer = transfer;
  pending->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  pending->callback = callback;
  pending->user_data = user_data;
  g_idle_add (complete_transfer, pending);
}

/* Thin transport adapters only: all protocol selection lives in production. */
void
fpi_fte3600_submit_transfer (FpiSsm *ssm, FpiSpiTransfer *transfer, gboolean cancellable)
{
  transfer->ssm = ssm;
  __wrap_fpi_spi_transfer_submit (transfer, cancellable ? mock.cancellable : NULL,
                                  fpi_ssm_spi_transfer_cb, NULL);
}

void
fpi_fte3600_try_reg_read (FpiSsm *ssm, guint8 reg, gsize result_len, gboolean cancellable)
{
  fpi_fte3600_submit_reg_read (ssm, reg, result_len, cancellable);
}

void
fpi_fte3600_submit_reg_read (FpiSsm *ssm, guint8 reg, gsize result_len, gboolean cancellable)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  gsize length = FTE3600_REG_READ_HEADER_SIZE + result_len;

  g_autoptr(GError) error = NULL;
  FpiSpiTransfer *transfer = fpi_spi_transfer_new_with_buffer_size (
    FP_DEVICE (self), self->spi_fd, self->max_transfer);

  g_assert_cmpuint (length, <=, sizeof self->small_rx);
  fpi_spi_transfer_write (transfer, length);
  g_assert_cmpuint (fpi_fte3600_build_app_read (transfer->buffer_wr, length, reg,
                                                result_len, &error), ==, length);
  g_assert_no_error (error);
  fpi_spi_transfer_read_full (transfer, self->small_rx, length, NULL);
  fpi_spi_transfer_set_full_duplex (transfer, TRUE);
  self->small_rx_valid = TRUE;
  fpi_fte3600_submit_transfer (ssm, transfer, cancellable);
}

guint8
fpi_fte3600_read_result_byte (FpiDeviceFte3600 *self)
{
  return self->small_rx[FTE3600_REG_RESULT_OFFSET];
}

gboolean
fpi_fte3600_mcu_is_idle (FpiDeviceFte3600 *self)
{
  return self->small_rx_valid && self->small_rx[4] == 0xa5 && self->small_rx[5] == 0x5a;
}

gboolean
fpi_fte3600_set_cs_polarity (FpiDeviceFte3600 *self, gboolean active_high, GError **error)
{
  gboolean current_high = !!(self->spi_mode & SPI_CS_HIGH);

  g_assert_cmpuint (self->spi_mode & (SPI_CPOL | SPI_CPHA), ==, SPI_MODE_3);
  if (!(self->transport_capabilities & FTE3600_TRANSPORT_CAP_CS_POLARITY))
    {
      /* A fixed-CS transport allows a no-op restore, never a MODE change. */
      g_assert_cmpint (active_high, ==, current_high);
      return TRUE;
    }
  if (active_high != current_high)
    mock.cs_changes++;
  self->spi_mode = (self->spi_mode & ~SPI_CS_HIGH) | (active_high ? SPI_CS_HIGH : 0);
  return TRUE;
}

void
fpi_fte3600_set_hardware_reset (FpiSsm *ssm, FpiDeviceFte3600 *self, gboolean asserted)
{
  if (mock.discovering)
    {
      g_assert_cmpuint (self->identity.sensor, ==, FTE3600_SENSOR_UNKNOWN);
      g_assert_null (self->sensor);
    }
  mock.reset_edges++;
  mock.reset_asserted = asserted;
  if (self->fast_open)
    {
      static const gboolean expected[] = { FALSE, TRUE, FALSE };
      guint edge = mock.reset_edges - 1;

      g_assert_cmpuint (edge, <, G_N_ELEMENTS (expected));
      g_assert_cmpint (asserted, ==, expected[edge]);
      mock.reset_times[edge] = g_get_monotonic_time ();
      if (asserted && mock.cancel_fast_reset)
        g_cancellable_cancel (mock.cancellable);
      if (asserted && mock.fail_fast_reset)
        {
          fpi_ssm_mark_failed (ssm, g_error_new_literal (
                                 G_IO_ERROR, G_IO_ERROR_FAILED, "Injected reset failure"));
          return;
        }
    }
  if (asserted)
    mock.sleeping = FALSE;
  self->idle_verified = FALSE;
  fpi_ssm_next_state (ssm);
}

G_GNUC_NORETURN FpiSsm *
fpi_fte3600_legacy38_identify_new (FpiDeviceFte3600 *self, gboolean boot_a)
{
  g_error ("FW9369 reopen must never enter legacy ROM discovery");
}

static void
complete_ssm (FpiSsm *ssm, FpDevice *device, GError *error)
{
  mock.error = error;
  mock.completed = TRUE;
}

static void
run_ssm (FpiSsm *ssm)
{
  mock.completed = FALSE;
  g_clear_error (&mock.error);
  fpi_ssm_start (ssm, complete_ssm);
  while (!mock.completed)
    g_main_context_iteration (NULL, TRUE);
}

static FpiDeviceFte3600 *
new_device (void)
{
  FpiDeviceFte3600 *self;
  GError *error = NULL;

  self = g_object_new (fpi_device_fte3600_get_type (), NULL);
  self->backend = fpi_fte3600_fw9369_backend ();
  self->max_transfer = 16384;
  self->image_size = 5120;
  self->capture_frame_size = 10246;
  self->capture_tx = g_malloc0 (self->capture_frame_size);
  self->capture_rx = g_malloc0 (self->capture_frame_size);
  g_assert_true (self->backend->prepare_capture (self, &error));
  g_assert_no_error (error);
  return self;
}

static FpiDeviceFte3600 *
setup (gboolean smic)
{
  memset (&mock, 0, sizeof mock);
  mock.cancellable = g_cancellable_new ();
  mock.smic = smic;
  mock.sfr[0x9b] = smic ? 0x4c : 0;
  mock.sfr[0x80] = 0x50;
  mock.words[0x1a8b] = 0x9362;
  return new_device ();
}

static void
destroy_device (FpiDeviceFte3600 *self)
{
  g_assert_false (self->armed);
  g_assert_null (self->irq_wait_ssm);
  self->backend->destroy (self);
  self->backend->destroy (self);
  g_free (self->capture_tx);
  g_free (self->capture_rx);
  g_clear_object (&self->captured_image);
  g_object_unref (self);
}

static void
teardown (FpiDeviceFte3600 *self)
{
  destroy_device (self);
  g_clear_object (&mock.cancellable);
  g_clear_error (&mock.error);
}

static void
test_capture (gconstpointer process)
{
  FpiDeviceFte3600 *self = setup (GPOINTER_TO_UINT (process));

  run_ssm (self->backend->create_init (self));
  g_assert_no_error (mock.error);
  g_assert_true (self->idle_verified);
  g_assert_cmpuint (mock.fdt_samples, >=, 4);
  g_assert_cmpuint (mock.images, >=, 4);
  for (guint i = 0; i < 2; i++)
    {
      run_ssm (self->backend->create_capture (self));
      g_assert_no_error (mock.error);
      g_assert_true (self->idle_verified);
      g_assert_nonnull (self->captured_image);
      g_assert_cmpuint (self->captured_image->data[66], ==, 255);
      g_assert_cmpuint (self->captured_image->data[67], ==, 0);
      for (guint j = 0; j < 10246; j++)
        g_assert_cmpuint (self->capture_rx[j], ==, 0);
    }
  g_assert_cmpuint (mock.fdt_writes, ==, 2);
  g_assert_cmpuint (mock.sleep_commands, ==, 0);
  g_assert_cmpuint (mock.sfr[0x9a], ==, 0);
  teardown (self);
}

static void
test_capture_error (gconstpointer scenario)
{
  FpiDeviceFte3600 *self = setup (FALSE);

  run_ssm (self->backend->create_init (self));
  g_assert_no_error (mock.error);
  switch (GPOINTER_TO_UINT (scenario))
    {
    case 0: mock.cancel_irq = TRUE;
      break;

    case 1: mock.cancel_image = TRUE;
      break;

    case 2: mock.fail_image = TRUE;
      break;

    case 3: mock.fail_transaction = mock.transactions + 10;
      break;

    case 4: mock.cancel_bank = TRUE;
      break;

    default: g_assert_not_reached ();
    }
  run_ssm (self->backend->create_capture (self));
  g_assert_nonnull (mock.error);
  g_assert_true (self->idle_verified);
  g_assert_null (self->captured_image);
  g_assert_cmpuint (mock.sfr[0x9a], ==, 0);
  g_cancellable_reset (mock.cancellable);
  mock.cancel_irq = mock.cancel_image = mock.fail_image = FALSE;
  mock.cancel_bank = FALSE;
  mock.fail_transaction = 0;
  run_ssm (self->backend->create_capture (self));
  g_assert_no_error (mock.error);
  g_assert_nonnull (self->captured_image);
  teardown (self);
}

static void
test_init_error (gconstpointer scenario)
{
  FpiDeviceFte3600 *self = setup (FALSE);

  switch (GPOINTER_TO_UINT (scenario))
    {
    case 0: mock.sfr[0x9b] = 0xff;
      break;

    case 1: mock.words[0x1a8b] = 0;
      break;

    case 2: mock.unstable = TRUE;
      break;

    case 3: mock.fail_transaction = 1;
      break;

    case 4: mock.low_fdt = TRUE;
      break;

    case 5: mock.low_image = TRUE;
      break;

    case 6: mock.words[0x1a8b] = 0x9365;
      break;

    default: g_assert_not_reached ();
    }
  run_ssm (self->backend->create_init (self));
  g_assert_nonnull (mock.error);
  g_assert_cmpint (self->idle_verified, ==, GPOINTER_TO_UINT (scenario) != 6);
  g_assert_cmpint (self->session_failed, ==, GPOINTER_TO_UINT (scenario) == 6);
  if (GPOINTER_TO_UINT (scenario) == 6)
    {
      g_assert_error (mock.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
      g_assert_cmpuint (mock.wrong_id_transaction, >, 0);
      g_assert_cmpuint (mock.transactions, ==, mock.wrong_id_transaction);
    }
  g_assert_cmpuint (mock.transactions, <, 3000);
  run_ssm (self->backend->create_capture (self));
  g_assert_nonnull (mock.error);
  g_assert_null (self->captured_image);
  if (GPOINTER_TO_UINT (scenario) == 6)
    g_assert_cmpuint (mock.transactions, ==, mock.wrong_id_transaction);
  teardown (self);
}

static void
test_cleanup_error (void)
{
  FpiDeviceFte3600 *self = setup (FALSE);

  mock.fail_transaction = 1; /* Relock fails; still attempt C0 and status. */
  run_ssm (self->backend->create_reset (self));
  g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpuint (mock.idle_commands, ==, 1);
  g_assert_cmpuint (mock.status_reads, ==, 1);
  g_assert_false (self->idle_verified);
  mock.sfr[0x80] = 0x54;
  mock.reject_idle = TRUE;
  run_ssm (self->backend->create_reset (self));
  g_assert_error (mock.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_assert_false (self->idle_verified);
  g_assert_cmpuint (mock.status_reads, ==, 11);
  teardown (self);
}

static void
test_shutdown (gconstpointer scenario_ptr)
{
  guint scenario = GPOINTER_TO_UINT (scenario_ptr);
  FpiDeviceFte3600 *self = setup (FALSE);

  run_ssm (self->backend->create_init (self));
  g_assert_no_error (mock.error);
  mock.words[0x1a83] = 0xffff;
  mock.words[0x1a82] = 0xffff;
  mock.fail_sleep = scenario == 1 || scenario == 3;
  mock.short_sleep = scenario == 2;
  mock.fail_transaction = scenario == 3 ? mock.transactions + 1 : 0;
  mock.fail_mask = scenario == 4;
  mock.reject_idle = scenario == 5;
  if (mock.reject_idle)
    mock.sfr[0x80] = 0x54;
  mock.fail_ack = scenario == 6;
  mock.reject_mask = scenario == 8;
  if (scenario == 7)
    g_cancellable_cancel (mock.cancellable);
  run_ssm (self->backend->create_shutdown (self));
  g_assert_false (self->idle_verified);
  g_assert_cmpuint (mock.sleep_commands, ==, 1);
  g_assert_cmpint (g_get_monotonic_time () - mock.sleep_time, >=, 1000);
  g_assert_cmpuint (mock.words[0x1a82], ==, scenario == 6 ? 0xffff : 0xf800);
  if (!mock.fail_mask && !mock.reject_mask)
    g_assert_cmpuint (mock.words[0x1a83], ==, 0xf800);
  if (scenario == 0 || scenario == 7)
    {
      g_assert_no_error (mock.error);
      g_assert_true (mock.sleeping);
      g_cancellable_reset (mock.cancellable);
      /* A subsequent arm must wake before any FDT/image configuration. */
      run_ssm (self->backend->create_capture (self));
      g_assert_no_error (mock.error);
      g_assert_false (mock.sleeping);
      g_assert_nonnull (self->captured_image);
      g_assert_cmpuint (mock.sleep_commands, ==, 1);
    }
  else if (scenario == 5 || scenario == 8)
    {
      g_assert_error (mock.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
    }
  else
    {
      g_assert_error (mock.error, G_IO_ERROR,
                      (scenario == 2 ? G_IO_ERROR_PARTIAL_INPUT : G_IO_ERROR_FAILED));
    }
  if (scenario == 3)
    g_assert_cmpstr (mock.error->message, ==, "Injected SPI failure");
  teardown (self);
}

static void
test_irq_communication (gconstpointer scenario_ptr)
{
  guint scenario = GPOINTER_TO_UINT (scenario_ptr);
  FpiDeviceFte3600 *self = setup (FALSE);
  guint images;

  run_ssm (self->backend->create_init (self));
  g_assert_no_error (mock.error);
  images = mock.images;
  mock.id_reads = 0;
  mock.sleep_on_irq = scenario == 0 || scenario == 3;
  mock.cancel_communication_wake = scenario == 3;
  mock.unavailable_ids = scenario == 1 ? 100 : 0;
  mock.irq_identity = scenario == 2 ? 0x9365 : 0;
  run_ssm (self->backend->create_capture (self));
  if (scenario == 0)
    {
      g_assert_no_error (mock.error);
      g_assert_cmpuint (mock.id_reads, ==, 4);
      g_assert_cmpuint (mock.event_reads - mock.events_at_irq, ==, 1);
      g_assert_nonnull (self->captured_image);
    }
  else
    {
      if (scenario == 3)
        g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      else
        g_assert_error (mock.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
      g_assert_cmpuint (mock.images, ==, images);
      g_assert_cmpuint (mock.event_reads, ==, mock.events_at_irq);
      g_assert_cmpuint (mock.wake_commands - mock.wakes_at_irq, ==,
                        scenario == 1 ? 10 : scenario == 3 ? 1 : 0);
      if (scenario == 2)
        {
          g_assert_true (self->session_failed);
          g_assert_false (self->idle_verified);
          g_assert_cmpuint (mock.transactions, ==, mock.wrong_id_transaction);
        }
    }
  teardown (self);
}

static void
test_fdt_recovery (gconstpointer scenario_ptr)
{
  const guint scenario = GPOINTER_TO_UINT (scenario_ptr);
  const gboolean release = scenario & 1;
  const guint kind = scenario / 2;
  FpiDeviceFte3600 *self = setup (FALSE);
  guint images, fdt_commands, fdt_writes, drains;

  run_ssm (self->backend->create_init (self));
  g_assert_no_error (mock.error);
  if (release)
    {
      run_ssm (self->backend->create_capture (self));
      g_assert_no_error (mock.error);
      g_clear_object (&self->captured_image);
    }
  mock.irqs = 0;
  images = mock.images;
  fdt_commands = mock.fdt_commands;
  fdt_writes = mock.fdt_writes;
  drains = mock.drains;
  /* First deliver an unrelated event (or an empty IRQ); then a real event
   * for the configured detector. Only the first IRQ needs a wake recovery. */
  mock.sleep_irq = kind == 2 ? 0 : 1;
  mock.irq_events[0] = kind == 1 ? 0 : release ? 2 : 4;
  mock.irq_events[1] = release ? 4 : 2;
  mock.irq_count = kind == 3 ? 1 : 2;
  if (kind == 3)
    mock.event_after_ack = mock.irq_events[1];
  mock.fail_fdt_restart = kind == 4;
  mock.cancel_fdt_restart = kind == 5;
  run_ssm (release ? self->backend->create_wait_release (self) :
           self->backend->create_capture (self));

  /* Restarting the detector must preserve its mode, baseline and new
   * latches, rather than executing the arm script or draining IRQs again. */
  g_assert_cmpuint (mock.fdt_writes, ==, fdt_writes + 1);
  g_assert_cmpuint (mock.drains, ==, drains + 1);
  g_assert_cmpuint (mock.fdt_commands, ==,
                    fdt_commands + (kind == 2 || kind == 4 ? 1 : 2));
  g_assert_true (self->idle_verified);
  g_assert_false (self->armed);
  g_assert_false (mock.fdt_running);
  if (kind >= 4)
    {
      g_assert_error (mock.error, G_IO_ERROR,
                      (kind == 4 ? G_IO_ERROR_FAILED : G_IO_ERROR_CANCELLED));
      g_assert_cmpuint (mock.irqs, ==, 1);
      g_assert_cmpuint (mock.images, ==, images);
      g_assert_null (self->captured_image);
    }
  else
    {
      g_assert_no_error (mock.error);
      g_assert_cmpuint (mock.irqs, ==, 2);
      g_assert_cmpuint (mock.irq_index, ==, mock.irq_count);
      g_assert_cmpuint (mock.images, ==, images + !release);
      if (release)
        g_assert_null (self->captured_image);
      else
        g_assert_nonnull (self->captured_image);
    }
  teardown (self);
}

static void
test_shutdown_fast_reopen (gconstpointer scenario_ptr)
{
  guint scenario = GPOINTER_TO_UINT (scenario_ptr);
  FpiDeviceFte3600 *self = setup (scenario == 1);
  guint transactions, images;

  g_autoptr(GError) error = NULL;

  run_ssm (self->backend->create_init (self));
  g_assert_no_error (mock.error);
  g_assert_cmpuint (mock.reset_edges, ==, 0);
  run_ssm (self->backend->create_shutdown (self));
  g_assert_no_error (mock.error);
  g_assert_true (mock.sleeping);
  transactions = mock.transactions;
  images = mock.images;

  /* Close destroys backend data but keeps the validated identity. Unlike the
  * discovery-reopen test, reuse it with the silicon still asleep from C1. */
  self->backend->destroy (self);
  g_assert_true (self->backend->prepare_capture (self, &error));
  g_assert_no_error (error);
  self->fast_open = TRUE;
  mock.fail_fast_reset = scenario == 2;
  mock.cancel_fast_reset = scenario == 3;
  if (scenario == 4)
    mock.words[0x1a8b] = 0x9391;
  run_ssm (self->backend->create_init (self));

  g_assert_cmpuint (mock.reset_edges, ==, 3);
  g_assert_false (mock.reset_asserted);
  g_assert_cmpint (mock.reset_times[1] - mock.reset_times[0], >=, 10000);
  g_assert_cmpint (mock.reset_times[2] - mock.reset_times[1], >=, 20000);
  g_assert_cmpint (g_get_monotonic_time () - mock.reset_times[2], >=, 10000);
  g_assert_cmpuint (mock.discovery_ids, ==, 0);
  if (scenario == 2 || scenario == 3)
    {
      g_assert_error (mock.error, G_IO_ERROR,
                      (scenario == 2 ? G_IO_ERROR_FAILED : G_IO_ERROR_CANCELLED));
      g_assert_cmpuint (mock.transactions, ==, transactions);
      g_assert_true (self->session_failed);
    }
  else if (scenario == 4)
    {
      g_assert_error (mock.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
      g_assert_cmpuint (mock.images, ==, images);
    }
  else
    {
      g_assert_no_error (mock.error);
      g_assert_false (mock.sleeping);
      g_assert_true (self->idle_verified);
      g_assert_cmpuint (mock.images, >, images);
      run_ssm (self->backend->create_capture (self));
      g_assert_no_error (mock.error);
      g_assert_nonnull (self->captured_image);
    }
  teardown (self);
}

static void
test_shutdown_discovery_reopen (gconstpointer scenario_ptr)
{
  guint scenario = GPOINTER_TO_UINT (scenario_ptr);
  gboolean fixed_cs = scenario == 4;
  gboolean wrong_original_cs = scenario == 3 || scenario == 5;
  FpiDeviceFte3600 *self = setup (FALSE);
  guint images, fdt_samples, transactions, wake_commands;

  mock.physical_cs_high = wrong_original_cs;
  self->spi_mode = SPI_MODE_3 | (mock.physical_cs_high ? SPI_CS_HIGH : 0);
  self->transport_capabilities = fixed_cs ? 0 : FTE3600_TRANSPORT_CAP_CS_POLARITY;
  run_ssm (self->backend->create_init (self));
  g_assert_no_error (mock.error);
  run_ssm (self->backend->create_capture (self));
  g_assert_no_error (mock.error);
  g_assert_nonnull (self->captured_image);
  /* The user lifts the finger before closing and leaves it off during the
  * next open's calibration. Sleep is entered by the real shutdown SSM. */
  mock.finger = FALSE;
  run_ssm (self->backend->create_shutdown (self));
  g_assert_no_error (mock.error);
  g_assert_true (mock.sleeping);
  g_assert_false (self->idle_verified);
  images = mock.images;
  fdt_samples = mock.fdt_samples;
  transactions = mock.transactions;
  wake_commands = mock.wake_commands;

  /* Replace the entire host object, not merely its backend data. Do not call
   * setup(): the same silicon, registers and C1 sleep state must survive. */
  destroy_device (self);
  self = new_device ();
  g_assert_cmpuint (self->identity.sensor, ==, FTE3600_SENSOR_UNKNOWN);
  g_assert_false (self->idle_verified);
  g_assert_true (mock.sleeping);
  g_assert_cmpuint (mock.transactions, ==, transactions);
  self->spi_mode = SPI_MODE_3; /* New transport session starts at its baseline. */
  self->transport_capabilities = fixed_cs ? 0 : FTE3600_TRANSPORT_CAP_CS_POLARITY;
  mock.discovering = TRUE;
  mock.fail_discovery_wake = scenario == 1;
  mock.change_discovery_id = scenario == 2 || scenario == 5;
  run_ssm (fpi_fte3600_discovery_new (self, FALSE));
  mock.discovering = FALSE;
  /* These assertions cover all submitted CS variants, including commands
   * for which the simulated chip returned only zeroes. */
  g_assert_cmpuint (mock.discovery_app_reads, ==, 0);
  g_assert_cmpuint (mock.discovery_legacy_commands, ==, 0);
  g_assert_cmpuint (mock.discovery_rom_commands, ==, 0);
  g_assert_cmpuint (mock.discovery_firmware_commands, ==, 0);
  g_assert_cmpuint (mock.blank_discovery_ids, ==, 0);
  g_assert_cmpuint (mock.sleep_commands, ==, 1);
  g_assert_cmpuint (mock.discovery_9368_wakes[wrong_original_cs], ==, 2);
  g_assert_cmpuint (mock.discovery_info_reads[wrong_original_cs], ==, 2);
  g_assert_cmpuint (mock.discovery_special_wakes[wrong_original_cs], ==, 1);
  if (scenario == 1)
    {
      g_assert_cmpuint (mock.discovery_mode_writes[0], ==, 0);
      g_assert_cmpuint (mock.discovery_id_queries[0], ==, 0);
    }
  else
    {
      g_assert_cmpuint (mock.discovery_mode_writes[wrong_original_cs], ==, 2);
      g_assert_cmpuint (mock.discovery_mode_reads[wrong_original_cs], ==, 2);
      g_assert_cmpuint (mock.discovery_id_queries[wrong_original_cs], ==, 2);
    }
  if (wrong_original_cs)
    {
      /* On the unresponsive connection both C6 helpers exhaust 31 attempts
       * but still read ID. One reset separates the two complete attempts;
       * neither reset nor a C6 write can grant a sensor identity. */
      g_assert_cmpuint (mock.discovery_special_wakes[0], ==, 2);
      g_assert_cmpuint (mock.discovery_mode_writes[0], ==, 124);
      g_assert_cmpuint (mock.discovery_mode_reads[0], ==, 124);
      g_assert_cmpuint (mock.discovery_id_queries[0], ==, 4);
      g_assert_cmpuint (mock.reset_edges, >=, 3);
    }
  if (fixed_cs)
    {
      g_assert_cmpuint (mock.cs_changes, ==, 0);
      g_assert_cmpuint (mock.discovery_9368_wakes[1], ==, 0);
      g_assert_cmpuint (mock.discovery_special_wakes[1], ==, 0);
    }
  if (scenario == 0 || scenario == 3 || fixed_cs)
    {
      g_assert_no_error (mock.error);
      g_assert_false (mock.sleeping);
      g_assert_cmpuint (self->identity.sensor, ==, FTE3600_SENSOR_FT9369);
      g_assert_cmpuint (self->identity.response, ==, 0x9362);
      g_assert_cmpuint (mock.discovery_ids, ==, 2);
      g_assert_cmpuint (mock.reset_edges, ==, scenario == 3 ? 3 : 0);
      g_assert_cmpuint (mock.wake_commands - wake_commands, ==, 1);
      g_assert_cmpuint (self->spi_mode, ==, SPI_MODE_3 | (scenario == 3 ? SPI_CS_HIGH : 0));
      self->sensor = fpi_fte3600_sensor_get (self->identity.sensor);
      run_ssm (self->backend->create_init (self));
      g_assert_no_error (mock.error);
      g_assert_cmpuint (mock.fdt_samples, >, fdt_samples);
      run_ssm (self->backend->create_capture (self));
      g_assert_no_error (mock.error);
      g_assert_nonnull (self->captured_image);
      g_assert_cmpuint (mock.images, >, images);
      g_assert_true (self->idle_verified);
    }
  else
    {
      if (scenario == 1)
        {
          g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_FAILED);
          g_assert_cmpuint (mock.transactions, ==, mock.failure_transaction);
        }
      else
        {
          g_assert_error (mock.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
          g_assert_cmpuint (mock.discovery_ids, ==, 2);
        }
      g_assert_cmpuint (self->identity.sensor, ==, FTE3600_SENSOR_UNKNOWN);
      g_assert_cmpuint (mock.images, ==, images);
      g_assert_cmpuint (self->spi_mode, ==, SPI_MODE_3);
    }
  teardown (self);
}

static void
test_wait_release (gconstpointer scenario_ptr)
{
  guint scenario = GPOINTER_TO_UINT (scenario_ptr);
  FpiDeviceFte3600 *self = setup (scenario & 1);
  guint images;

  run_ssm (self->backend->create_init (self));
  g_assert_no_error (mock.error);
  run_ssm (self->backend->create_capture (self));
  g_assert_no_error (mock.error);
  g_assert_nonnull (self->captured_image);
  g_clear_object (&self->captured_image);
  images = mock.images;
  g_assert_nonnull (self->backend->create_wait_release);
  mock.release_events[0] = 2; /* DOWN cannot complete release. */
  mock.release_events[1] = 6; /* UP|DOWN has ambiguous ordering. */
  mock.release_events[2] = 4;
  mock.release_count = 3;
  switch (scenario)
    {
    case 0: break;

    case 1: mock.cancel_irq = TRUE;
      break;

    case 2:
      mock.release_events[1] = 2;
      mock.cancel_release_after = 2;
      break;

    case 3:
      /* ESD now recovers and waits for a fresh UP. The sensor is uncovered
       * while recalibrating; an ESD event itself is not release evidence. */
      mock.irq_events[0] = 0x401;
      mock.irq_events[1] = 4;
      mock.irq_count = 2;
      break;

    case 4: mock.fail_transaction = mock.transactions + 1;
      break;

    case 5: mock.fail_release_cleanup = TRUE;
      break;

    default: g_assert_not_reached ();
    }
  run_ssm (self->backend->create_wait_release (self));
  if (scenario == 3)
    g_assert_cmpuint (mock.images, >, images);
  else
    g_assert_cmpuint (mock.images, ==, images);
  g_assert_null (self->captured_image);
  g_assert_false (self->armed);
  g_assert_cmpuint (mock.sfr[0x9a], ==, 0);
  if (scenario == 0 || scenario == 3)
    {
      g_assert_no_error (mock.error);
      g_assert_cmpuint (mock.release_index, ==, scenario == 3 ? 0 : 3);
      mock.irq_count = 0;
      run_ssm (self->backend->create_capture (self));
      g_assert_no_error (mock.error);
      g_assert_nonnull (self->captured_image);
    }
  else if (scenario == 1 || scenario == 2)
    {
      g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    }
  else if (scenario == 4)
    {
      g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_FAILED);
    }
  else
    {
      g_assert_error (mock.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
    }
  g_assert_cmpint (self->idle_verified, ==, scenario != 5);
  teardown (self);
}

static void
test_enroll_prearm (gconstpointer scenario_ptr)
{
  guint scenario = GPOINTER_TO_UINT (scenario_ptr);
  FpiDeviceFte3600 *self = setup (FALSE);
  guint fdt_writes, drains;

  run_ssm (self->backend->create_init (self));
  g_assert_no_error (mock.error);
  mock.enrolling = TRUE;
  fpi_device_set_nr_enroll_stages (FP_DEVICE (self), 8);
  self->enroll_stages_passed = scenario == 3 ? 7 : 0;
  mock.cancel_release_arm = scenario == 1;
  mock.fail_release_arm = scenario == 2;
  run_ssm (self->backend->create_capture (self));
  if (scenario == 0 || scenario == 4)
    {
      g_assert_no_error (mock.error);
      g_assert_nonnull (self->captured_image);
      g_assert_true (self->armed);
      g_assert_false (self->idle_verified);
      g_assert_cmpuint (mock.words[0x1881] & 0x83, ==, 0);
      g_clear_object (&self->captured_image);
      /* Finger lifts while the host is matching: its already-pending UP
       * must survive until wait_release, without requiring a second edge. */
      mock.words[0x1a82] = 4;
      mock.sleep_on_irq = scenario == 4;
      fdt_writes = mock.fdt_writes;
      drains = mock.drains;
      run_ssm (self->backend->create_wait_release (self));
      g_assert_no_error (mock.error);
      g_assert_cmpuint (mock.release_index, ==, 0);
      g_assert_cmpuint (mock.fdt_writes, ==, fdt_writes);
      g_assert_cmpuint (mock.drains, ==, drains);
      g_assert_cmpuint (mock.words[0x1a82], ==, 0);
      g_assert_true (self->idle_verified);
      g_assert_false (self->armed);
      mock.sleep_on_irq = FALSE;
      /* Final acquisition must be idle when the core completes enrollment. */
      self->enroll_stages_passed = 7;
      run_ssm (self->backend->create_capture (self));
      g_assert_no_error (mock.error);
      g_assert_true (self->idle_verified);
      g_assert_false (self->armed);
    }
  else
    {
      if (scenario == 1)
        g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      else if (scenario == 2)
        g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_FAILED);
      else
        g_assert_no_error (mock.error);
      g_assert_true (self->idle_verified);
      g_assert_false (self->armed);
      if (scenario != 3)
        g_assert_null (self->captured_image);
    }
  teardown (self);
}


static void
test_mode_negotiation (gconstpointer data)
{
  guint scenario = GPOINTER_TO_UINT (data);
  FpiDeviceFte3600 *self = setup (scenario / 4);
  guint variant = scenario % 4;

  mock.mode_failures = variant == 0 ? 0 : variant == 1 ? 1 : G_MAXUINT;
  mock.mode_io_error = variant == 3;
  run_ssm (self->backend->create_init (self));
  if (variant == 3)
    {
      g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_FAILED);
    }
  else
    {
      g_assert_no_error (mock.error);
      g_assert_true (self->idle_verified);
      g_assert_cmpuint (mock.mode_reads, ==, variant == 0 ? 2 : variant == 1 ? 3 : 62);
      run_ssm (self->backend->create_capture (self));
      g_assert_no_error (mock.error);
      g_assert_nonnull (self->captured_image);
    }
  teardown (self);
}

static void
test_event_recovery (gconstpointer scenario_ptr)
{
  guint scenario = GPOINTER_TO_UINT (scenario_ptr);
  gboolean release = scenario & 1;
  static const guint16 events[] = { 0x11, 0x201, 0x401, 0x617 };
  FpiDeviceFte3600 *self = setup ((scenario >> 1) & 1);
  guint images, samples, modes;

  run_ssm (self->backend->create_init (self));
  g_assert_no_error (mock.error);
  images = mock.images;
  samples = mock.fdt_samples;
  modes = mock.mode_reads;
  /* The open was cached, but live recovery must not pulse GPIO again. */
  self->fast_open = TRUE;
  self->enroll_stages_passed = 2;
  mock.irq_events[0] = events[scenario >> 2];
  mock.irq_events[1] = release ? 4 : 2;
  mock.irq_count = 2;
  /* Event bits are deliberately separate from physical occupancy here:
   * stale simultaneous DOWN/UP must not supply a sample or finish release. */
  mock.uncovered_first_irq = TRUE;
  run_ssm (release ? self->backend->create_wait_release (self) :
           self->backend->create_capture (self));
  g_assert_no_error (mock.error);
  g_assert_cmpuint (self->enroll_stages_passed, ==, 2);
  g_assert_cmpuint (mock.irq_index, ==, 2);
  g_assert_cmpuint (mock.reset_edges, ==, 0);
  g_assert_cmpuint (mock.mode_reads, ==, modes + 2);
  g_assert_cmpuint (mock.fdt_samples, ==, samples * 2);
  g_assert_cmpuint (mock.images, ==, images * 2 + !release);
  g_assert_cmpint (mock.recovery_start_time - mock.recovery_ack_time, >=, 5000);
  g_assert_true (self->idle_verified);
  if (release)
    {
      g_assert_null (self->captured_image);
    }
  else
    {
      g_assert_nonnull (self->captured_image);
      g_assert_cmpuint (self->captured_image->data[66], ==, 255);
    }
  /* The next acquisition still works in the same open device. */
  mock.irq_count = 0;
  run_ssm (self->backend->create_capture (self));
  g_assert_no_error (mock.error);
  g_assert_nonnull (self->captured_image);
  teardown (self);
}

static void
test_event_recovery_error (gconstpointer scenario_ptr)
{
  guint scenario = GPOINTER_TO_UINT (scenario_ptr);
  gboolean release = scenario & 1;
  guint fault = scenario >> 1;
  FpiDeviceFte3600 *self = setup (FALSE);
  guint images;

  run_ssm (self->backend->create_init (self));
  g_assert_no_error (mock.error);
  images = mock.images;
  mock.recovery_fault = fault;
  mock.cancel_after_fault_ack = fault == 7;
  mock.irq_count = 4;
  for (guint i = 0; i < mock.irq_count; i++)
    mock.irq_events[i] = 0x11;
  run_ssm (release ? self->backend->create_wait_release (self) :
           self->backend->create_capture (self));
  g_assert_nonnull (mock.error);
  g_assert_null (self->captured_image);
  g_assert_false (self->armed);
  g_assert_cmpuint (mock.sfr[0x9a], ==, 0);
  if (fault == 0)
    {
      g_assert_error (mock.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
      g_assert_nonnull (strstr (mock.error->message, "persist after 3 reinitializations"));
      g_assert_cmpuint (mock.irq_index, ==, 4);
      g_assert_cmpuint (mock.recovery_setups, ==, 6);
      g_assert_cmpuint (mock.images, ==, images * 4);
    }
  else if (fault == 3 || fault == 4)
    {
      g_assert_error (mock.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
      if (fault == 3)
        {
          g_assert_true (self->session_failed);
          g_assert_cmpuint (mock.transactions, ==, mock.wrong_id_transaction);
        }
    }
  else
    {
      GIOErrorEnum code = fault == 1 ? G_IO_ERROR_FAILED : (fault == 2 || fault == 7) ? G_IO_ERROR_CANCELLED :
                          fault == 5 ? G_IO_ERROR_PARTIAL_INPUT : G_IO_ERROR_CLOSED;

      g_assert_error (mock.error, G_IO_ERROR, code);
      if (fault == 7)
        g_assert_cmpuint (mock.recovery_setups, ==, 0);
    }
  /* A failed recovery cannot leave the old calibration usable. */
  g_cancellable_reset (mock.cancellable);
  mock.recovery_fault = 0;
  mock.irq_count = 0;
  run_ssm (self->backend->create_capture (self));
  g_assert_error (mock.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_assert_nonnull (strstr (mock.error->message, "no validated baseline"));
  teardown (self);
}

static void
test_enroll_event_recovery (void)
{
  FpiDeviceFte3600 *self = setup (FALSE);

  run_ssm (self->backend->create_init (self));
  g_assert_no_error (mock.error);
  mock.enrolling = TRUE;
  fpi_device_set_nr_enroll_stages (FP_DEVICE (self), 8);
  for (guint stage = 2; stage < 4; stage++)
    {
      self->enroll_stages_passed = stage;
      mock.irq_index = 0;
      mock.irq_count = 4;
      mock.irq_events[0] = mock.irq_events[1] = mock.irq_events[2] = 0x11;
      mock.irq_events[3] = 2;
      run_ssm (self->backend->create_capture (self));
      g_assert_no_error (mock.error);
      g_assert_cmpuint (self->enroll_stages_passed, ==, stage);
      g_assert_cmpuint (mock.irq_index, ==, 4);
      g_assert_nonnull (self->captured_image);
      g_assert_true (self->armed);
      g_clear_object (&self->captured_image);

      /* INVALID arrives on the detector prearmed before matcher work.
       * Recovery must invalidate that latch and rearm for a fresh UP. */
      mock.irq_index = 0;
      mock.irq_count = 2;
      mock.irq_events[0] = 0x11;
      mock.irq_events[1] = 4;
      run_ssm (self->backend->create_wait_release (self));
      g_assert_no_error (mock.error);
      g_assert_cmpuint (self->enroll_stages_passed, ==, stage);
      g_assert_cmpuint (mock.irq_index, ==, 2);
      g_assert_null (self->captured_image);
      g_assert_true (self->idle_verified);
      g_assert_false (self->armed);
    }
  teardown (self);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_data_func ("/fw9369-backend/db-capture", GUINT_TO_POINTER (0), test_capture);
  g_test_add_data_func ("/fw9369-backend/smic-capture", GUINT_TO_POINTER (1), test_capture);
  g_test_add_func ("/fw9369-backend/cleanup-error", test_cleanup_error);
  g_test_add_func ("/fw9369-backend/enroll-event-recovery", test_enroll_event_recovery);
  for (guint i = 0; i < 16; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/fw9369-backend/event-recovery/%u", i);

      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_event_recovery);
    }
  for (guint i = 0; i < 16; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/fw9369-backend/event-recovery-error/%u", i);

      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_event_recovery_error);
    }
  for (guint i = 0; i < 5; i++)
    {
      static const gchar *cases[] = { "db", "smic", "reset-error", "cancel", "changed-id" };
      g_autofree gchar *name = g_strdup_printf ("/fw9369-backend/fast-reopen/%s", cases[i]);

      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_shutdown_fast_reopen);
    }
  for (guint i = 0; i < 6; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/fw9369-backend/shutdown-reopen/%u", i);
      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_shutdown_discovery_reopen);
    }
  for (guint i = 0; i < 9; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/fw9369-backend/shutdown/%u", i);
      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_shutdown);
    }
  for (guint i = 0; i < 4; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/fw9369-backend/communication/%u", i);
      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_irq_communication);
    }
  for (guint i = 0; i < 5; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/fw9369-backend/enroll-prearm/%u", i);

      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_enroll_prearm);
    }
  for (guint i = 0; i < 12; i++)
    {
      static const gchar *cases[] = { "unrelated", "empty", "awake-control",
                                      "latched", "restart-error", "restart-cancel" };
      g_autofree gchar *name = g_strdup_printf ("/fw9369-backend/fdt-recovery/%s/%s",
                                                i & 1 ? "release" : "capture", cases[i / 2]);

      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_fdt_recovery);
    }
  for (guint i = 0; i < 6; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/fw9369-backend/release/%u", i);

      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_wait_release);
    }
  for (guint i = 0; i < 5; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/fw9369-backend/capture-error/%u", i);
      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_capture_error);
    }
  for (guint i = 0; i < 7; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/fw9369-backend/init-error/%u", i);
      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_init_error);
    }
  for (guint i = 0; i < 8; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/fw9369-backend/mode-negotiation/%u", i);
      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_mode_negotiation);
    }
  return g_test_run ();
}
