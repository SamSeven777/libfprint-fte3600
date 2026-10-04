/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Real state machines, packet builders and asynchronous transfer ownership.
 * This fixture models public wire behavior using synthetic samples only. */
#include <string.h>
#include "drivers/fte3600-fw9369.h"

G_DEFINE_TYPE (FpiDeviceFte3600, fpi_device_fte3600, FP_TYPE_DEVICE)

static struct
{
  GCancellable *cancellable;
  guint8 sfr[256];
  guint16 words[0x8000];
  guint transactions;
  guint images;
  guint irqs;
  guint16 release_events[4];
  guint release_count;
  guint release_index;
  guint cancel_release_after;
  guint idle_commands;
  guint status_reads;
  guint fail_transaction;
  guint fdt_samples;
  guint fdt_writes;
  guint drains;
  gboolean completed;
  gboolean finger;
  gboolean smic;
  gboolean cancel_irq;
  gboolean cancel_image;
  gboolean cancel_bank;
  gboolean fail_image;
  gboolean unstable;
  gboolean low_fdt;
  gboolean low_image;
  gboolean reject_idle;
  gboolean fail_release_cleanup;
  gboolean enrolling;
  gboolean fail_release_arm;
  gboolean cancel_release_arm;
  GError *error;
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

  mock.irqs++;
  self->armed = FALSE;
  if (mock.cancel_irq)
    {
      g_cancellable_cancel (mock.cancellable);
      fpi_fte3600_fail_if_cancelled (ssm, FP_DEVICE (self));
    }
  else
    {
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
  g_assert_cmpuint (length, >=, 3);
  g_assert_cmpuint (tx[0] ^ tx[1], ==, 0xff);
  memset (rx, 0, length);
  switch (tx[0])
    {
    case 0x08:
      g_assert_cmpuint (length, ==, 5);
      if (tx[2] == 0x80)
        mock.status_reads++;
      rx[4] = mock.sfr[tx[2]];
      return;
    case 0x09:
      g_assert_cmpuint (length, ==, 4);
      mock.sfr[tx[2]] = tx[3];
      if (mock.cancel_bank && tx[2] == 0x9a && tx[3] == 0x5a)
        g_cancellable_cancel (mock.cancellable);
      return;
    case 0xc0:
      g_assert_cmpuint (length, ==, 3);
      mock.idle_commands++;
      if (!mock.reject_idle)
        mock.sfr[0x80] = 0x50;
      return;
    case 0x5a:
    case 0xa5:
    case 0xc2:
      g_assert_cmpuint (length, ==, 3);
      return;
    case 0xc4:
      g_assert_cmpuint (length, ==, 3);
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
    mock.words[0x1a82] &= ~be16 (tx + 6);
  else
    mock.words[address] = be16 (tx + 6);
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
  FpiSpiTransfer *transfer;
  GCancellable *cancellable;
  FpiSpiTransferCallback callback;
  gpointer user_data;
} Pending;

static gboolean
complete_transfer (gpointer user_data)
{
  Pending *pending = user_data;
  FpiSpiTransfer *transfer = pending->transfer;
  GError *error = NULL;

  mock.transactions++;
  if (pending->cancellable)
    g_cancellable_set_error_if_cancelled (pending->cancellable, &error);
  if (!error && (mock.transactions == mock.fail_transaction ||
                 (mock.fail_image && transfer->buffer_wr[0] == 0x06)))
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Injected SPI failure");
  if (!error && mock.fail_release_arm && transfer->buffer_wr[0] == 0x05 &&
      (be16 (transfer->buffer_wr + 2) & 0x7fff) == 0x1881 &&
      (be16 (transfer->buffer_wr + 6) & 0x83) == 0)
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Injected release-arm failure");
  if (!error)
    emulate_transfer (transfer);
  pending->callback (transfer, transfer->device, pending->user_data, error);
  fpi_spi_transfer_unref (transfer);
  g_clear_object (&pending->cancellable);
  g_free (pending);
  return G_SOURCE_REMOVE;
}

void __wrap_fpi_spi_transfer_submit (FpiSpiTransfer *transfer,
                                     GCancellable *cancellable,
                                     FpiSpiTransferCallback callback,
                                     gpointer user_data);
void
__wrap_fpi_spi_transfer_submit (FpiSpiTransfer *transfer,
                                GCancellable *cancellable,
                                FpiSpiTransferCallback callback,
                                gpointer user_data)
{
  Pending *pending = g_new0 (Pending, 1);

  pending->transfer = transfer;
  pending->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  pending->callback = callback;
  pending->user_data = user_data;
  g_idle_add (complete_transfer, pending);
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
setup (gboolean smic)
{
  FpiDeviceFte3600 *self;
  GError *error = NULL;

  memset (&mock, 0, sizeof mock);
  mock.cancellable = g_cancellable_new ();
  mock.smic = smic;
  mock.sfr[0x9b] = smic ? 0x4c : 0;
  mock.sfr[0x80] = 0x50;
  mock.words[0x1a8b] = 0x9362;
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

static void
teardown (FpiDeviceFte3600 *self)
{
  g_assert_false (self->armed);
  g_assert_null (self->irq_wait_ssm);
  self->backend->destroy (self);
  self->backend->destroy (self);
  g_free (self->capture_tx);
  g_free (self->capture_rx);
  g_clear_object (&self->captured_image);
  g_object_unref (self);
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
    case 0: mock.cancel_irq = TRUE; break;
    case 1: mock.cancel_image = TRUE; break;
    case 2: mock.fail_image = TRUE; break;
    case 3: mock.fail_transaction = mock.transactions + 10; break;
    case 4: mock.cancel_bank = TRUE; break;
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
    case 0: mock.sfr[0x9b] = 0xff; break;
    case 1: mock.words[0x1a8b] = 0; break;
    case 2: mock.unstable = TRUE; break;
    case 3: mock.fail_transaction = 1; break;
    case 4: mock.low_fdt = TRUE; break;
    case 5: mock.low_image = TRUE; break;
    default: g_assert_not_reached ();
    }
  run_ssm (self->backend->create_init (self));
  g_assert_nonnull (mock.error);
  g_assert_true (self->idle_verified);
  g_assert_cmpuint (mock.transactions, <, 3000);
  run_ssm (self->backend->create_capture (self));
  g_assert_nonnull (mock.error);
  g_assert_null (self->captured_image);
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
    case 1: mock.cancel_irq = TRUE; break;
    case 2:
      mock.release_events[1] = 2;
      mock.cancel_release_after = 2;
      break;
    case 3: mock.release_events[0] = 0x400; break;
    case 4: mock.fail_transaction = mock.transactions + 1; break;
    case 5: mock.fail_release_cleanup = TRUE; break;
    default: g_assert_not_reached ();
    }
  run_ssm (self->backend->create_wait_release (self));
  g_assert_cmpuint (mock.images, ==, images);
  g_assert_null (self->captured_image);
  g_assert_false (self->armed);
  g_assert_cmpuint (mock.sfr[0x9a], ==, 0);
  if (scenario == 0)
    {
      g_assert_no_error (mock.error);
      g_assert_cmpuint (mock.release_index, ==, 3);
      run_ssm (self->backend->create_capture (self));
      g_assert_no_error (mock.error);
      g_assert_nonnull (self->captured_image);
    }
  else if (scenario == 1 || scenario == 2)
    g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  else if (scenario == 4)
    g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  else
    g_assert_error (mock.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
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
  if (scenario == 0)
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

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_data_func ("/fw9369-backend/db-capture", GUINT_TO_POINTER (0), test_capture);
  g_test_add_data_func ("/fw9369-backend/smic-capture", GUINT_TO_POINTER (1), test_capture);
  g_test_add_func ("/fw9369-backend/cleanup-error", test_cleanup_error);
  for (guint i = 0; i < 4; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/fw9369-backend/enroll-prearm/%u", i);

      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_enroll_prearm);
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
  for (guint i = 0; i < 6; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/fw9369-backend/init-error/%u", i);
      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_init_error);
    }
  return g_test_run ();
}
