/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Real FT9368 backends, SSMs and transfer ownership; simulated boundaries.
* Generated firmware/image bytes are deliberately unrelated to vendor data.
* The firmware-loader wrapper tests updater sequencing, not hash validation;
* tests/test-fte3600-firmware.c independently exercises the real loader. */
#include <string.h>
#include "drivers/fte3600-ft9368.h"
#include "drivers/fte3600-ft9368-update.h"
#include "drivers/fte3600-ft9368-protocol.h"

G_DEFINE_TYPE (FpiDeviceFte3600, fpi_device_fte3600, FP_TYPE_DEVICE)

static struct
{
  GCancellable *cancellable;
  guint8        pram[6096];
  guint8        app[27120];
  guint8        ram[6096];
  guint8        flash[27120];
  gsize         selected;
  gsize         selected_length;
  gsize         flash_address;
  gsize         program_length;
  gsize         written;
  gsize         verified;
  guint16       status;
  guint         transactions;
  guint         loads;
  guint         resets;
  guint         images;
  guint         clears;
  guint         irqs;
  guint         fail_transaction;
  guint         fail_clear;
  guint         fail_clear_number;
  guint         wake_checks;
  guint         unready_wakes;
  guint         info_reads;
  guint         inject_info_read;
  guint16       injected_id;
  guint         identity_transaction;
  gboolean      injected_unhealthy;
  gboolean      short_info;
  gint64        wake_time;
  gboolean      zero_wake;
  gboolean      wrong_info;
  gboolean      fail_wakecheck;
  gboolean      short_wakecheck;
  gboolean      cancel_wakecheck;
  gboolean      reset_asserted;
  gboolean      updating;
  gboolean      erased;
  gboolean      finger;
  gboolean      completed;
  gboolean      corrupt_pram;
  gboolean      wrong_boot_id;
  gboolean      wrong_checksum;
  gboolean      wrong_version;
  gboolean      reject_erase;
  gboolean      reject_program;
  gboolean      cancel_pram;
  gboolean      cancel_erase;
  gboolean      cancel_irq;
  gboolean      cancel_image;
  gboolean      cancel_reset;
  gint64        assertion_time;
  gboolean      missing_firmware;
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

  device->id = "ft9368-backend-test";
  device->full_name = "FT9368 simulated backend";
  device->type = FP_DEVICE_TYPE_VIRTUAL;
  device->features = FP_DEVICE_FEATURE_CAPTURE;
}

static guint16
be16 (const guint8 *p)
{
  return ((guint16) p[0] << 8) | p[1];
}

static void
put_status (guint8 *p, guint16 value)
{
  p[0] = value >> 8;
  p[1] = value;
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
      mock.finger = TRUE;
      fpi_ssm_next_state (ssm);
    }
}

void
fpi_fte3600_set_hardware_reset (FpiSsm *ssm, FpiDeviceFte3600 *self,
                                gboolean asserted)
{
  if (!asserted && mock.reset_asserted)
    g_assert_cmpint (g_get_monotonic_time () - mock.assertion_time, >=, 19000);
  mock.resets++;
  mock.reset_asserted = asserted;
  if (asserted)
    {
      mock.assertion_time = g_get_monotonic_time ();
      if (mock.cancel_reset)
        g_cancellable_cancel (mock.cancellable);
    }
  self->idle_verified = FALSE;
  fpi_ssm_next_state (ssm);
}

GCancellable *__wrap_fpi_device_get_cancellable (FpDevice *device);
GCancellable *
__wrap_fpi_device_get_cancellable (FpDevice *device)
{
  return mock.cancellable;
}

GBytes *__wrap_fpi_fte3600_firmware_load (const Fte3600Firmware *firmware,
                                          const gchar           *path,
                                          GError               **error);
GBytes *
__wrap_fpi_fte3600_firmware_load (const Fte3600Firmware *firmware,
                                  const gchar *path, GError **error)
{
  mock.loads++;
  if (mock.missing_firmware)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Simulated missing firmware");
      return NULL;
    }
  if (firmware->role == FTE3600_FIRMWARE_APPLICATION)
    {
      g_assert_cmpuint (firmware->size, ==, sizeof mock.app);
      g_assert_true (g_str_has_suffix (path, "/fte3600/ft9368-app.bin"));
      return g_bytes_new_static (mock.app, sizeof mock.app);
    }
  g_assert_cmpuint (firmware->role, ==, FTE3600_FIRMWARE_PRAMBOOT);
  g_assert_cmpuint (firmware->size, ==, sizeof mock.pram);
  g_assert_true (g_str_has_suffix (path, "/fte3600/ft9368-pramboot.bin"));
  return g_bytes_new_static (mock.pram, sizeof mock.pram);
}

static void
emulate_transfer (FpiSpiTransfer *transfer)
{
  const guint8 *tx = transfer->buffer_wr;
  guint8 *rx = transfer->buffer_rd;
  gsize length = transfer->length_wr;
  guint16 command;
  gsize payload;

  g_assert_cmpint (transfer->length_rd, ==, transfer->length_wr);
  g_assert_true (transfer->full_duplex);
  memset (rx, 0, length);
  if (tx[0] == 0x70)
    {
      g_assert_true (mock.updating);
      g_assert_false (mock.reset_asserted);
      if (tx[1] == 5 || tx[1] == 4)
        {
          gsize offset = ((gsize) be16 (tx + 3) - 0x2000) * 4;
          gsize size = ((gsize) be16 (tx + 5) + 1) * 4;
          g_assert_cmpuint (offset + size, <=, sizeof mock.ram);
          if (tx[1] == 5)
            {
              g_assert_cmpuint (length, ==, size + 7);
              g_assert_cmpuint (offset, ==, mock.written);
              g_assert_cmpmem (tx + 7, size, mock.pram + offset, size);
              memcpy (mock.ram + offset, tx + 7, size);
              mock.written += size;
              if (mock.cancel_pram)
                g_cancellable_cancel (mock.cancellable);
            }
          else
            {
              g_assert_cmpuint (length, ==, 7);
              mock.selected = offset;
              mock.selected_length = size;
            }
        }
      else if (tx[1] == 7)
        {
          g_assert_cmpuint (length, ==, 11);
          guint16 address = be16 (tx + 3);
          g_assert_true ((address == 4 && be16 (tx + 7) == 0xaa55) ||
                         (address == 0x48 && be16 (tx + 7) == 0x0110) ||
                         (address == 7 && be16 (tx + 7) == 0x5a5a));
        }
      else
        {
          g_assert_cmpuint (length, ==, 3);
          g_assert_true (tx[1] == 0x55 || tx[1] == 0x0a);
          g_assert_cmpuint (tx[2], ==, (guint8) ~tx[1]);
        }
      return;
    }
  if (tx[0] == 0x71)
    {
      g_assert_cmpuint (length, ==, mock.selected_length + 1);
      g_assert_cmpuint (mock.selected, ==, mock.verified);
      memcpy (rx + 1, mock.ram + mock.selected, mock.selected_length);
      mock.verified += mock.selected_length;
      if (mock.corrupt_pram)
        rx[1] ^= 1;
      return;
    }
  g_assert_cmpuint (length, >=, 4);
  command = be16 (tx);
  payload = be16 (tx + 2);
  g_assert_cmpuint (length, ==, payload ? payload + 7 : 4);
  if (command == 0xff00)
    {
      mock.wake_time = g_get_monotonic_time ();
      return;
    }
  if (command == 0x9180)
    {
      if (payload == 4)
        {
          g_assert_cmpint (g_get_monotonic_time () - mock.wake_time, >=, 10000);
          mock.wake_checks++;
          if (mock.unready_wakes)
            {
              memset (rx + 7, 0x55, 4);
              mock.unready_wakes--;
            }
          else if (!mock.zero_wake)
            {
              rx[7] = 1;
              rx[8] = 2;
            }
          if (mock.cancel_wakecheck)
            g_cancellable_cancel (mock.cancellable);
          return;
        }
      g_assert_cmpuint (payload, ==, 32);
      mock.info_reads++;
      if (mock.wrong_info)
        return;
      rx[7 + 19] = 0x93;
      rx[7 + 20] = 0x68;
      rx[7 + 21] = mock.wrong_version ? 0x12 : 0x13;
      rx[7 + 23] = 64;
      rx[7 + 24] = 80;
      if (mock.finger)
        rx[7 + 1] = rx[7 + 2] = 0x11;
      if (mock.info_reads == mock.inject_info_read)
        {
          put_status (rx + 7 + 19, mock.injected_id);
          if (mock.injected_unhealthy)
            rx[7 + 2] = 0x22;
          mock.identity_transaction = mock.transactions;
        }
      return;
    }
  if (command == 0xf680)
    {
      g_assert_cmpuint (payload, ==, 4);
      return;
    }
  if (command == 0x9080)
    {
      if (payload == 2)
        {
          put_status (rx + 7, mock.wrong_boot_id ? 0xffff : 0x56a2);
        }
      else if (payload == 6)
        {
          mock.clears++;
        }
      else
        {
          g_assert_cmpuint (payload, ==, 5120);
          g_assert_true (mock.finger);
          for (gsize i = 0; i < payload; i++)
            rx[7 + i] = (i * 37 + 11) & 0xff;
          mock.images++;
          if (mock.cancel_image)
            g_cancellable_cancel (mock.cancellable);
        }
      return;
    }
  g_assert_true (mock.updating);
  if (command == 0x5500 || command == 0x6400 || command == 0x0700)
    {
      g_assert_cmpuint (payload, ==, 0);
      return;
    }
  if (command == 0x0900 || command == 0x1000)
    {
      g_assert_cmpuint (payload, ==, 1);
      g_assert_cmpuint (tx[7], ==, command == 0x0900 ? 0x0a : 0x0c);
      return;
    }
  if (command == 0x6100)
    {
      g_assert_cmpuint (mock.written, ==, sizeof mock.pram);
      g_assert_cmpuint (mock.verified, ==, sizeof mock.pram);
      g_assert_cmpuint (payload, ==, 1);
      g_assert_cmpuint (tx[7], ==, 0);
      mock.erased = TRUE;
      mock.status = mock.reject_erase ? 0 : 0xf0aa;
      if (mock.cancel_erase)
        g_cancellable_cancel (mock.cancellable);
      return;
    }
  if (command == 0xab00)
    {
      g_assert_true (mock.erased);
      g_assert_cmpuint (payload, ==, 3);
      mock.flash_address = ((gsize) tx[7] << 16) | ((gsize) tx[8] << 8) | tx[9];
      g_assert_cmpuint (mock.flash_address, ==, mock.program_length);
      return;
    }
  if (command == 0xbf00)
    {
      g_assert_cmpuint (mock.flash_address + payload, <=, sizeof mock.flash);
      g_assert_cmpmem (tx + 7, payload, mock.app + mock.flash_address, payload);
      memcpy (mock.flash + mock.flash_address, tx + 7, payload);
      mock.program_length += payload;
      /* Independent final-tail fixture: the observed acknowledgment is 1070. */
      mock.status = mock.reject_program ? 0 :
                    payload == 240 ? 0x1070 : 0x1000 + mock.flash_address / 256;
      return;
    }
  if (command == 0x6a80)
    {
      put_status (rx + 7, mock.status);
      return;
    }
  if (command == 0x6500)
    {
      const guint8 expected[] = { 0, 0, 0, 0, 0x69, 0xf0 };
      g_assert_cmpuint (payload, ==, sizeof expected);
      g_assert_cmpmem (tx + 7, payload, expected, sizeof expected);
      return;
    }
  if (command == 0x6680)
    {
      /* Golden for the generated 27120-byte pattern (i*29+3) mod 256. */
      put_status (rx + 7, mock.wrong_checksum ? 0 : 0x41e1);
      return;
    }
  g_error ("Unexpected simulated FT9368 command %04x", command);
}

typedef struct
{
  FpiSpiTransfer        *transfer;
  GCancellable          *cancellable;
  FpiSpiTransferCallback callback;
  gpointer               user_data;
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
  if (!error && be16 (transfer->buffer_wr) == 0x9180 &&
      be16 (transfer->buffer_wr + 2) == 4 && (mock.fail_wakecheck || mock.short_wakecheck))
    error = g_error_new_literal (G_IO_ERROR,
                                 mock.short_wakecheck ? G_IO_ERROR_PARTIAL_INPUT : G_IO_ERROR_FAILED,
                                 "Injected wake-check failure");
  if (!error && (mock.transactions == mock.fail_transaction ||
                 ((mock.fail_clear || (mock.fail_clear_number && mock.clears + 1 == mock.fail_clear_number)) && transfer->length_wr == 13 &&
                  be16 (transfer->buffer_wr) == 0x9080)))
    {
      mock.fail_clear = FALSE;
      error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Injected SPI failure");
    }
  if (!error)
    {
      emulate_transfer (transfer);
      if (mock.short_info && be16 (transfer->buffer_wr) == 0x9180 &&
          be16 (transfer->buffer_wr + 2) == 32)
        error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT,
                                     "Injected short INFO with untrusted identity bytes");
    }
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

  if (mock.erased)
    g_assert_null (cancellable);
  pending->transfer = transfer;
  pending->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  pending->callback = callback;
  pending->user_data = user_data;
  g_idle_add (complete_transfer, pending);
}

void
fpi_fte3600_submit_transfer (FpiSsm *ssm, FpiSpiTransfer *transfer,
                             gboolean cancellable)
{
  transfer->ssm = ssm;
  __wrap_fpi_spi_transfer_submit (transfer, cancellable ? mock.cancellable : NULL,
                                  fpi_ssm_spi_transfer_cb, NULL);
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
  g_assert_false (mock.reset_asserted);
}

static FpiDeviceFte3600 *
setup (gboolean update)
{
  FpiDeviceFte3600 *self;
  GError *error = NULL;

  memset (&mock, 0, sizeof mock);
  mock.cancellable = g_cancellable_new ();
  mock.updating = update;
  for (gsize i = 0; i < sizeof mock.app; i++)
    mock.app[i] = (i * 29 + 3) & 0xff;
  for (gsize i = 0; i < sizeof mock.pram; i++)
    mock.pram[i] = (i * 13 + 7) & 0xff;
  g_setenv ("FTE3600_FT9368_UPDATE", update ? "1" : "0", TRUE);
  self = g_object_new (fpi_device_fte3600_get_type (), NULL);
  self->sensor = fpi_fte3600_sensor_get (FTE3600_SENSOR_FT9368);
  self->identity = fpi_fte3600_identify_special (0x9368);
  self->backend = fpi_fte3600_ft9368_backend ();
  self->max_transfer = 16384;
  self->image_size = 5120;
  self->capture_frame_size = 5127;
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
  g_unsetenv ("FTE3600_FT9368_UPDATE");
}

static void
test_warm (void)
{
  FpiDeviceFte3600 *self = setup (FALSE);

  mock.wrong_version = TRUE; /* An older healthy application must not reflash. */
  run_ssm (self->backend->create_init (self));
  g_assert_no_error (mock.error);
  g_assert_true (self->idle_verified);
  g_assert_cmpuint (mock.loads, ==, 0);
  for (guint i = 0; i < 2; i++)
    {
      mock.finger = FALSE;
      run_ssm (self->backend->create_capture (self));
      g_assert_no_error (mock.error);
      g_assert_true (self->idle_verified);
      g_assert_nonnull (self->captured_image);
      for (gsize j = 0; j < 5120; j++)
        g_assert_cmpuint (self->captured_image->data[j], ==, (guint8) (j * 37 + 11));
      for (gsize j = 0; j < self->capture_frame_size; j++)
        g_assert_cmpuint (self->capture_rx[j], ==, 0);
    }
  g_assert_cmpuint (mock.images, ==, 2);
  g_assert_cmpuint (mock.irqs, ==, 2);
  teardown (self);
}

static void
test_capture_fault (gconstpointer scenario)
{
  FpiDeviceFte3600 *self = setup (FALSE);

  switch (GPOINTER_TO_UINT (scenario))
    {
    case 0: mock.cancel_irq = TRUE;
      break;

    case 1: mock.cancel_image = TRUE;
      break;

    case 2: mock.fail_transaction = 4;
      break;

    case 3: mock.fail_clear = TRUE;
      break;

    case 4: mock.fail_clear_number = 2;
      break;

    default: g_assert_not_reached ();
    }
  run_ssm (self->backend->create_capture (self));
  g_assert_nonnull (mock.error);
  g_assert_cmpint (self->idle_verified, ==, GPOINTER_TO_UINT (scenario) != 4);
  g_assert_null (self->captured_image);
  g_assert_cmpuint (mock.clears, >=, 1);
  g_cancellable_reset (mock.cancellable);
  mock.cancel_irq = mock.cancel_image = FALSE;
  mock.fail_transaction = 0;
  mock.fail_clear_number = 0;
  run_ssm (self->backend->create_capture (self));
  g_assert_no_error (mock.error);
  g_assert_nonnull (self->captured_image);
  teardown (self);
}

static void
test_cleanup_fault (void)
{
  FpiDeviceFte3600 *self = setup (FALSE);

  self->idle_verified = TRUE;
  mock.fail_clear = TRUE;
  run_ssm (self->backend->create_reset (self));
  g_assert_nonnull (mock.error);
  g_assert_false (self->idle_verified);
  teardown (self);
}

static void
test_identity_loss (gconstpointer scenario_ptr)
{
  guint scenario = GPOINTER_TO_UINT (scenario_ptr);
  FpiDeviceFte3600 *self = setup (FALSE);

  if (scenario != 0)
    {
      run_ssm (self->backend->create_init (self));
      g_assert_no_error (mock.error);
    }
  mock.injected_id = 0x9365;
  if (scenario == 0)
    {
      mock.inject_info_read = mock.info_reads + 1;
      run_ssm (self->backend->create_init (self));
    }
  else if (scenario <= 5)
    {
      /* Capture wake, pre-IRQ check, IRQ check, image wake and first cleanup
       * INFO must all stop at the first positively conflicting response. */
      mock.inject_info_read = mock.info_reads + scenario;
      run_ssm (self->backend->create_capture (self));
    }
  else
    {
      /* Both early cleanup reads, not just its final health check. */
      mock.inject_info_read = mock.info_reads + scenario - 5;
      run_ssm (self->backend->create_reset (self));
    }
  g_assert_error (mock.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_assert_true (self->session_failed);
  g_assert_false (self->idle_verified);
  g_assert_false (self->armed);
  g_assert_null (self->captured_image);
  g_assert_cmpuint (mock.identity_transaction, >, 0);
  g_assert_cmpuint (mock.transactions, ==, mock.identity_transaction);
  /* Even an accidental direct retry of a backend action cannot write to the
  * conflicting chip. The public driver also rejects this failed session. */
  run_ssm (self->backend->create_capture (self));
  g_assert_error (mock.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  run_ssm (self->backend->create_reset (self));
  g_assert_error (mock.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  run_ssm (self->backend->create_init (self));
  g_assert_error (mock.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_assert_cmpuint (mock.transactions, ==, mock.identity_transaction);
  teardown (self);
}

static void
test_ambiguous_cleanup_info (gconstpointer scenario_ptr)
{
  guint scenario = GPOINTER_TO_UINT (scenario_ptr);
  FpiDeviceFte3600 *self = setup (FALSE);

  mock.inject_info_read = 1;
  mock.injected_id = scenario == 0 ? 0 : scenario == 1 ? 0xffff : 0x9368;
  mock.injected_unhealthy = scenario == 2;
  run_ssm (self->backend->create_reset (self));
  g_assert_no_error (mock.error);
  g_assert_false (self->session_failed);
  g_assert_true (self->idle_verified);
  g_assert_cmpuint (mock.info_reads, ==, 3);
  g_assert_cmpuint (mock.clears, ==, 1);
  teardown (self);
}

static void
test_wake_readiness (gconstpointer scenario_ptr)
{
  guint scenario = GPOINTER_TO_UINT (scenario_ptr);
  FpiDeviceFte3600 *self = setup (FALSE);

  switch (scenario)
    {
    case 0: mock.unready_wakes = 2;
      break;

    case 1: mock.unready_wakes = 100;
      break;

    case 2: mock.zero_wake = TRUE;
      break;

    case 3: mock.zero_wake = mock.wrong_info = TRUE;
      break;

    case 4: mock.fail_wakecheck = TRUE;
      break;

    case 5: mock.short_wakecheck = TRUE;
      break;

    case 6: mock.cancel_wakecheck = TRUE;
      break;

    default: g_assert_not_reached ();
    }
  run_ssm (self->backend->create_init (self));
  g_assert_cmpuint (mock.loads, ==, 0);
  if (scenario == 0 || scenario == 2)
    {
      g_assert_no_error (mock.error);
      g_assert_cmpuint (mock.wake_checks, ==, scenario == 0 ? 3 : 1);
      g_assert_cmpuint (mock.info_reads, >=, 1);
      /* Both capture wake sites reuse the readiness check. */
      guint checks = mock.wake_checks;
      mock.unready_wakes = 1;
      run_ssm (self->backend->create_capture (self));
      g_assert_no_error (mock.error);
      g_assert_cmpuint (mock.wake_checks - checks, ==, 3);
      g_assert_cmpuint (mock.images, ==, 1);
    }
  else
    {
      g_assert_nonnull (mock.error);
      g_assert_false (self->idle_verified);
      g_assert_cmpuint (mock.images, ==, 0);
      if (scenario == 1)
        {
          g_assert_cmpuint (mock.wake_checks, ==, 3);
          g_assert_cmpuint (mock.info_reads, ==, 0);
        }
      if (scenario >= 4)
        {
          g_assert_error (mock.error, G_IO_ERROR,
                          (scenario == 4 ? G_IO_ERROR_FAILED :
                           scenario == 5 ? G_IO_ERROR_PARTIAL_INPUT : G_IO_ERROR_CANCELLED));
          g_assert_cmpuint (mock.info_reads, ==, 0);
        }
    }
  teardown (self);
}

static void
test_wake_predicate (void)
{
  guint8 bytes[4] = { 0 };

  g_assert_false (fpi_fte3600_ft9368_wake_ready (NULL, 4));
  g_assert_false (fpi_fte3600_ft9368_wake_ready (bytes, 3));
  g_assert_true (fpi_fte3600_ft9368_wake_ready (bytes, 4));
  for (guint value = 1; value <= 255; value++)
    {
      memset (bytes, value, sizeof bytes);
      g_assert_false (fpi_fte3600_ft9368_wake_ready (bytes, 4));
      for (guint i = 0; i < 4; i++)
        {
          bytes[i] = 0;
          g_assert_true (fpi_fte3600_ft9368_wake_ready (bytes, 4));
          bytes[i] = value;
        }
    }
}

static void
test_update (gconstpointer scenario)
{
  FpiDeviceFte3600 *self = setup (TRUE);
  guint which = GPOINTER_TO_UINT (scenario);

  switch (which)
    {
    case 0: break;

    case 1: mock.cancel_pram = TRUE;
      break;

    case 2: mock.cancel_erase = TRUE;
      break;

    case 3: mock.corrupt_pram = TRUE;
      break;

    case 4: mock.wrong_boot_id = TRUE;
      break;

    case 5: mock.reject_erase = TRUE;
      break;

    case 6: mock.reject_program = TRUE;
      break;

    case 7: mock.wrong_checksum = TRUE;
      break;

    case 8: mock.wrong_version = TRUE;
      break;

    case 9: mock.fail_transaction = 1;
      break;

    case 10: mock.cancel_reset = TRUE;
      break;

    case 11:
      /* The parent init first confirms FT9368, then update verification sees
       * another chip. It must not proceed to START or permit runtime cleanup. */
      mock.inject_info_read = 2;
      mock.injected_id = 0x9365;
      break;

    case 12:
      mock.inject_info_read = 1;
      mock.injected_id = 0x9365;
      mock.short_info = TRUE;
      break;

    default: g_assert_not_reached ();
    }
  run_ssm (which == 11 ? self->backend->create_init (self) :
           fpi_fte3600_ft9368_update_new (self));
  if (which == 0 || which == 2)
    {
      g_assert_no_error (mock.error);
      g_assert_cmpuint (mock.program_length, ==, sizeof mock.app);
      g_assert_cmpmem (mock.flash, sizeof mock.flash, mock.app, sizeof mock.app);
    }
  else
    {
      g_assert_nonnull (mock.error);
    }
  if (which == 1 || which == 3 || which == 4 || which == 9 || which == 10)
    g_assert_false (mock.erased);
  if (which == 1 || which == 10)
    g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpint (self->session_failed, ==, which == 11);
  if (which == 11)
    {
      g_assert_error (mock.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
      g_assert_cmpuint (mock.identity_transaction, >, 0);
      g_assert_cmpuint (mock.transactions, ==, mock.identity_transaction);
      run_ssm (self->backend->create_reset (self));
      g_assert_error (mock.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
      g_assert_cmpuint (mock.transactions, ==, mock.identity_transaction);
      g_assert_false (self->idle_verified);
    }
  if (which == 12)
    g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT);
  g_assert_cmpuint (mock.resets, ==, 6);
  g_assert_cmpuint (mock.loads, ==, 2);
  teardown (self);
}

static void
test_update_gate (gconstpointer scenario)
{
  FpiDeviceFte3600 *self = setup (TRUE);

  switch (GPOINTER_TO_UINT (scenario))
    {
    case 0: g_unsetenv ("FTE3600_FT9368_UPDATE");
      break;

    case 1: self->identity.response = 0;
      break;

    case 2: self->identity.evidence = FTE3600_IDENTITY_RUNTIME_GEOMETRY;
      break;

    case 3: mock.missing_firmware = TRUE;
      break;

    default: g_assert_not_reached ();
    }
  run_ssm (fpi_fte3600_ft9368_update_new (self));
  g_assert_nonnull (mock.error);
  g_assert_cmpuint (mock.transactions, ==, 0);
  g_assert_cmpuint (mock.resets, ==, 0);
  teardown (self);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/ft9368-backend/warm-capture", test_warm);
  g_test_add_func ("/ft9368-backend/cleanup-failure", test_cleanup_fault);
  g_test_add_func ("/ft9368-backend/wake-predicate", test_wake_predicate);
  for (guint i = 0; i < 9; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/ft9368-backend/identity-loss/%u", i);
      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_identity_loss);
    }
  for (guint i = 0; i < 3; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/ft9368-backend/ambiguous-cleanup-info/%u", i);
      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_ambiguous_cleanup_info);
    }
  for (guint i = 0; i < 7; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/ft9368-backend/wake/%u", i);
      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_wake_readiness);
    }
  for (guint i = 0; i < 5; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/ft9368-backend/capture-fault/%u", i);
      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_capture_fault);
      if (i < 4)
        {
          g_clear_pointer (&name, g_free);
          name = g_strdup_printf ("/ft9368-backend/update-gate/%u", i);
          g_test_add_data_func (name, GUINT_TO_POINTER (i), test_update_gate);
        }
    }
  for (guint i = 0; i < 13; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/ft9368-backend/update/%u", i);
      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_update);
    }
  return g_test_run ();
}
