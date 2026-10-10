/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Real identification/recovery SSMs with generated firmware and mock I/O. */
#include <string.h>
#include "drivers/fte3600-legacy-recovery.h"
#include "drivers/fte3600-legacy-recovery-protocol.h"

G_DEFINE_TYPE (FpiDeviceFte3600, fpi_device_fte3600, FP_TYPE_DEVICE)
static struct
{
  GCancellable *cancel;
  guint8        firmware[14184];
  gsize         size;
  guint8        otp;
  guint8        fe;
  guint         transactions, resets, assertions, uploads, reads, loads, polls;
  guint         application_reads;
  guint         config_writes;
  guint         fail_transaction;
  guint         fail_reset;
  gboolean      asserted, done, corrupt, busy, missing;
  gboolean      cancel_reset, cancel_upload, cancel_readback, cancel_start;
  gboolean      recovering;
  gint64        assertion_time;
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

  device->id = "legacy38-recovery-test";
  device->full_name = "Simulated legacy RAM recovery";
  device->type = FP_DEVICE_TYPE_VIRTUAL;
  device->features = FP_DEVICE_FEATURE_CAPTURE;
}

void
fpi_fte3600_clear_irq_source (FpiDeviceFte3600 *self)
{
  self->irq_wait_ssm = NULL;
}

gboolean
fpi_fte3600_fail_if_cancelled (FpiSsm *ssm, FpDevice *dev)
{
  GError *error = NULL;

  if (!g_cancellable_set_error_if_cancelled (mock.cancel, &error))
    return FALSE;
  fpi_ssm_mark_failed (ssm, error);
  return TRUE;
}

static gboolean
set_reset (FpiSsm *ssm, FpiDeviceFte3600 *self, gboolean asserted)
{
  if (!asserted && mock.asserted)
    g_assert_cmpint (g_get_monotonic_time () - mock.assertion_time, >=, 19000);
  mock.resets++;
  if (mock.resets == mock.fail_reset)
    {
      fpi_ssm_mark_failed (ssm, g_error_new_literal (
                             G_IO_ERROR, G_IO_ERROR_FAILED, "Injected reset release failure"));
      return FALSE;
    }
  mock.asserted = asserted;
  self->idle_verified = FALSE;
  if (asserted)
    {
      mock.assertions++;
      mock.assertion_time = g_get_monotonic_time ();
      if (mock.cancel_reset || (mock.cancel_start && mock.reads))
        g_cancellable_cancel (mock.cancel);
    }
  return TRUE;
}

void
fpi_fte3600_set_hardware_reset (FpiSsm *ssm, FpiDeviceFte3600 *self, gboolean asserted)
{
  if (set_reset (ssm, self, asserted))
    fpi_ssm_next_state (ssm);
}

void
fpi_fte3600_release_reset_and_sync (FpiSsm *ssm)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));
  FpiSpiTransfer *transfer;

  if (!set_reset (ssm, self, FALSE))
    return;
  transfer = fpi_spi_transfer_new_with_buffer_size (FP_DEVICE (self), self->spi_fd, self->max_transfer);
  fpi_spi_transfer_write (transfer, 2);
  transfer->buffer_wr[0] = 0x55;
  transfer->buffer_wr[1] = 0xaa;
  fpi_spi_transfer_read (transfer, 2);
  fpi_spi_transfer_set_full_duplex (transfer, TRUE);
  fpi_fte3600_submit_transfer (ssm, transfer, FALSE);
}

guint8
fpi_fte3600_read_result_byte (FpiDeviceFte3600 *self)
{
  return self->small_rx[4];
}

gboolean
fpi_fte3600_mcu_is_idle (FpiDeviceFte3600 *self)
{
  return self->small_rx_valid && self->small_rx[4] == 0xa5 && self->small_rx[5] == 0x5a;
}

void
fpi_fte3600_submit_reg_read (FpiSsm *ssm, guint8 reg, gsize length, gboolean cancellable)
{
  FpiDeviceFte3600 *self = FPI_DEVICE_FTE3600 (fpi_ssm_get_device (ssm));

  g_assert_false (cancellable);
  mock.application_reads++;
  memset (self->small_rx, 0, sizeof self->small_rx);
  self->small_rx_valid = TRUE;
  if (reg == 0x20)
    {
      g_assert_cmpuint (length, ==, 2);
      mock.polls++;
      self->small_rx[4] = mock.busy ? 0 : 0xa5;
      self->small_rx[5] = mock.busy ? 0 : 0x5a;
    }
  else
    {
      g_assert_cmpuint (length, ==, 1);
      g_assert_true (reg == 0x14 || reg == 0x15);
      self->small_rx[4] = reg == 0x14 ? self->sensor->width : self->sensor->height;
    }
  fpi_ssm_next_state (ssm);
}

void
fpi_fte3600_try_reg_read (FpiSsm *ssm, guint8 reg, gsize length, gboolean cancellable)
{
  fpi_fte3600_submit_reg_read (ssm, reg, length, cancellable);
}

GBytes *__wrap_fpi_fte3600_firmware_load (const Fte3600Firmware *firmware,
                                          const gchar           *path,
                                          GError               **error);
GBytes *
__wrap_fpi_fte3600_firmware_load (const Fte3600Firmware *firmware,
                                  const gchar *path, GError **error)
{
  mock.loads++;
  if (mock.missing)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Simulated missing firmware");
      return NULL;
    }
  g_assert_cmpuint (firmware->size, ==, mock.size);
  g_assert_true (g_str_has_suffix (path, mock.size == 14184 ? "ft9338.bin" : "ft9536.bin"));
  return g_bytes_new_static (mock.firmware, mock.size);
}

typedef struct { FpiSsm         *ssm;
                 FpiSpiTransfer *transfer;
                 gboolean        cancellable;
} Pending;
static gboolean
complete_transfer (gpointer user_data)
{
  Pending *pending = user_data;
  FpiSpiTransfer *transfer = pending->transfer;
  const guint8 *tx = transfer->buffer_wr;
  guint8 *rx = transfer->buffer_rd;
  gsize length = transfer->length_wr;
  GError *error = NULL;

  mock.transactions++;
  g_assert_true (transfer->full_duplex);
  g_assert_cmpint (transfer->length_rd, ==, transfer->length_wr);
  memset (rx, 0, length);
  if (pending->cancellable)
    g_cancellable_set_error_if_cancelled (mock.cancel, &error);
  if (!error && mock.transactions == mock.fail_transaction)
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Injected boot SPI failure");
  if (!error)
    {
      if (tx[0] == 0x55)
        {
          const guint8 expected[] = { 0x55, 0xaa };
          g_assert_cmpmem (tx, length, expected, sizeof expected);
        }
      else if (tx[0] == 8 || tx[0] == 9)
        {
          g_assert_cmpuint (length, ==, 4);
          g_assert_cmpuint (tx[1], ==, (guint8) ~tx[0]);
          if (tx[0] == 8)
            {
              rx[3] = tx[2] == 0xfe ? mock.fe : tx[2] == 0xf3 ? mock.otp : 0;
            }
          else if (mock.recovering)
            {
              const guint8 expected[][2] = {
                { 0xc8, 0xff }, { 0xca, 0xff }, { 0xcb, 0xff },
                { 0xb9, 0xbf }, { 0xb9, 0xff },
              };
              g_assert_cmpuint (mock.config_writes, <, G_N_ELEMENTS (expected));
              g_assert_cmpmem (tx + 2, 2, expected[mock.config_writes], 2);
              mock.config_writes++;
            }
        }
      else if (tx[0] == 5)
        {
          g_assert_cmpuint (length, ==, mock.size + 7);
          g_assert_cmpuint (mock.config_writes, ==, 5);
          g_assert_cmpuint (tx[1], ==, 0xfa);
          g_assert_cmpuint (tx[2] | tx[3], ==, 0);
          g_assert_cmpuint (((guint16) tx[4] << 8) | tx[5], ==, mock.size);
          g_assert_cmpmem (tx + 6, mock.size, mock.firmware, mock.size);
          g_assert_cmpuint (tx[length - 1], ==, 0);
          mock.uploads++;
          if (mock.cancel_upload)
            g_cancellable_cancel (mock.cancel);
        }
      else
        {
          g_assert_cmpuint (tx[0], ==, 4);
          g_assert_cmpuint (tx[1], ==, 0xfb);
          g_assert_cmpuint (length, ==, mock.size + 8);
          g_assert_cmpuint (((guint16) tx[4] << 8) | tx[5], ==, length);
          g_assert_cmpuint (tx[2] | tx[3], ==, 0);
          memcpy (rx + 6, mock.firmware, mock.size);
          rx[length - 1] = rx[length - 2] = 0xcc;
          if (mock.corrupt)
            rx[6 + mock.size - 1] ^= 1;
          mock.reads++;
          if (mock.cancel_readback)
            g_cancellable_cancel (mock.cancel);
        }
    }
  if (error)
    fpi_ssm_mark_failed (pending->ssm, error);
  else
    fpi_ssm_next_state (pending->ssm);
  fpi_spi_transfer_unref (transfer);
  g_free (pending);
  return G_SOURCE_REMOVE;
}

void
fpi_fte3600_submit_transfer (FpiSsm *ssm, FpiSpiTransfer *transfer, gboolean cancellable)
{
  Pending *pending = g_new0 (Pending, 1);

  pending->ssm = ssm;
  pending->transfer = transfer;
  pending->cancellable = cancellable;
  g_idle_add (complete_transfer, pending);
}

static void
complete_ssm (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  mock.done = TRUE;
  mock.error = error;
}

static void
run (FpiSsm *ssm)
{
  mock.done = FALSE;
  g_clear_error (&mock.error);
  fpi_ssm_start (ssm, complete_ssm);
  while (!mock.done)
    g_main_context_iteration (NULL, TRUE);
  g_assert_false (mock.asserted);
}

static FpiDeviceFte3600 *
setup (gboolean ft9338)
{
  FpiDeviceFte3600 *self;

  memset (&mock, 0, sizeof mock);
  mock.cancel = g_cancellable_new ();
  mock.otp = ft9338 ? 0x10 : 0x20;
  mock.fe = 2;
  mock.size = ft9338 ? 14184 : 11934;
  for (gsize i = 0; i < mock.size; i++)
    mock.firmware[i] = (i * 13 + 17) & 0xff;
  self = g_object_new (fpi_device_fte3600_get_type (), NULL);
  self->sensor = fpi_fte3600_sensor_get (ft9338 ? FTE3600_SENSOR_FT9338 : FTE3600_SENSOR_FT9536);
  self->identity = fpi_fte3600_identify_runtime (self->sensor->width, self->sensor->height);
  self->discovery_rx[2] = 0xef;
  self->max_transfer = 16384;
  return self;
}

static void
finish (FpiDeviceFte3600 *self)
{
  g_assert_false (self->armed);
  g_object_unref (self);
  g_clear_object (&mock.cancel);
  g_clear_error (&mock.error);
}

static void
test_identify (gconstpointer scenario)
{
  guint which = GPOINTER_TO_UINT (scenario);
  gboolean boot_a = which < 4;
  FpiDeviceFte3600 *self = setup (!boot_a);

  switch (which)
    {
    case 0: break;

    case 1: mock.fe = 0xff;
      break;

    case 2: self->discovery_rx[2] = 0;
      break;

    case 3: mock.cancel_reset = TRUE;
      break;

    case 4: break;

    case 5: mock.otp = 0x20;
      break;

    case 6: mock.otp = 0xff;
      break;

    case 7: self->identity = (Fte3600Identity){ 0 };
      break;

    case 8: mock.fail_transaction = 4;
      break;

    default: g_assert_not_reached ();
    }
  run (fpi_fte3600_legacy38_identify_new (self, boot_a));
  if (which == 0 || which == 4)
    {
      g_assert_no_error (mock.error);
      g_assert_cmpuint (self->rom_identity.sensor, ==, self->sensor->sensor);
      g_assert_cmpuint (self->rom_identity.response, ==, boot_a ? 2 : 0x5858);
      /* Only the entry pulse; successful OTP identification needs no reset. */
      g_assert_cmpuint (mock.resets, ==, 3);
    }
  else
    {
      g_assert_nonnull (mock.error);
      g_assert_cmpuint (self->rom_identity.sensor, ==, FTE3600_SENSOR_UNKNOWN);
    }
  if (which == 2 || which == 7)
    g_assert_cmpuint (mock.resets + mock.transactions, ==, 0);
  finish (self);
}

static void
test_recover (gconstpointer scenario)
{
  guint which = GPOINTER_TO_UINT (scenario);
  FpiDeviceFte3600 *self = setup (which == 0);

  run (fpi_fte3600_legacy38_identify_new (self, which != 0));
  g_assert_no_error (mock.error);
  mock.resets = mock.assertions = mock.transactions = 0;
  mock.recovering = TRUE;
  switch (which)
    {
    case 0:
    case 1: break;

    case 2: mock.corrupt = TRUE;
      break;

    case 3: self->max_transfer = mock.size + 7;
      break;

    case 4: mock.missing = TRUE;
      break;

    case 5: mock.cancel_upload = TRUE;
      break;

    case 6: mock.busy = TRUE;
      break;

    case 7: self->rom_identity.response = 0;
      break;

    case 8: mock.cancel_reset = TRUE;
      break;

    case 9: mock.fail_transaction = 2;
      break;

    case 10: mock.cancel_readback = TRUE;
      break;

    case 11: mock.cancel_start = TRUE;
      break;

    case 12:
      mock.fail_transaction = 2;
      mock.fail_reset = 4;
      break;

    default: g_assert_not_reached ();
    }
  run (fpi_fte3600_legacy38_recovery_new (self));
  if (which < 2)
    {
      g_assert_no_error (mock.error);
      g_assert_true (self->idle_verified);
      g_assert_false (self->session_failed);
      g_assert_cmpuint (mock.uploads, ==, 1);
      g_assert_cmpuint (mock.reads, ==, 1);
      g_assert_cmpuint (mock.assertions, ==, 3);
      /* Geometry belongs after MCU configuration, not inside RAM startup. */
      g_assert_cmpuint (mock.application_reads, ==, 1);
    }
  else
    {
      g_assert_nonnull (mock.error);
      g_assert_false (self->idle_verified);
      g_assert_true (self->session_failed);
      if (which != 6)
        g_assert_cmpuint (mock.application_reads, ==, 0);
    }
  if (which == 3 || which == 4 || which == 7)
    {
      g_assert_cmpuint (mock.resets + mock.transactions, ==, 0);
    }
  else if (which < 2)
    {
      g_assert_cmpuint (mock.resets, ==, 9);
    }
  else if (which == 6)
    {
      g_assert_cmpuint (mock.resets, ==, 10);
    }
  else if (which == 11)
    {
      /* Cancellation during the first startup pulse finishes its low hold,
       * then releases the line without a second pulse or application reads. */
      g_assert_cmpuint (mock.resets, ==, 7);
      g_assert_cmpuint (mock.assertions, ==, 2);
    }
  else
    {
      g_assert_cmpuint (mock.resets, ==, 4);
      g_assert_cmpuint (mock.assertions, ==, 1);
    }
  if (which == 6)
    {
      g_assert_cmpuint (mock.polls, ==, 20);
      g_assert_cmpuint (mock.application_reads, ==, 20);
    }
  if (which == 5 || which == 8 || which == 10 || which == 11)
    g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  if (which == 12)
    {
      g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_FAILED);
      g_assert_cmpstr (mock.error->message, ==, "Injected boot SPI failure");
    }
  finish (self);
}

static void
test_recovery_transfer_failure (gconstpointer scenario)
{
  guint which = GPOINTER_TO_UINT (scenario);
  FpiDeviceFte3600 *self = setup (which < 8);
  guint failed_transfer = which % 8 + 1;

  run (fpi_fte3600_legacy38_identify_new (self, which >= 8));
  g_assert_no_error (mock.error);
  mock.resets = mock.assertions = mock.transactions = 0;
  mock.recovering = TRUE;
  mock.fail_transaction = failed_transfer;
  run (fpi_fte3600_legacy38_recovery_new (self));
  g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpstr (mock.error->message, ==, "Injected boot SPI failure");
  g_assert_cmpuint (mock.transactions, ==, failed_transfer);
  g_assert_cmpuint (mock.resets, ==, 4);
  g_assert_cmpuint (mock.assertions, ==, 1);
  g_assert_cmpuint (mock.application_reads, ==, 0);
  g_assert_false (self->idle_verified);
  g_assert_true (self->session_failed);
  finish (self);
}

static void
test_wire (void)
{
  guint8 frame[32];
  const guint8 read[] = { 8, 0xf7, 0xfe, 0 };
  const guint8 write[] = { 9, 0xf6, 0xfd, 0x11 };
  const guint8 readback[] = { 4, 0xfb, 0, 0, 0, 24 };

  memset (frame, 0x42, sizeof frame);
  g_assert_cmpuint (fpi_fte3600_build_boot38_read (frame, 4, 0xfe), ==, 4);
  g_assert_cmpmem (frame, 4, read, sizeof read);
  g_assert_cmpuint (frame[4], ==, 0x42);
  g_assert_cmpuint (fpi_fte3600_build_boot38_write (frame, 4, 0xfd, 0x11), ==, 4);
  g_assert_cmpmem (frame, 4, write, sizeof write);
  g_assert_cmpuint (fpi_fte3600_build_boot38_readback (frame, sizeof frame, 16), ==, 24);
  g_assert_cmpmem (frame, 6, readback, sizeof readback);
  g_assert_cmpuint (fpi_fte3600_build_boot38_readback (frame, sizeof frame, G_MAXSIZE), ==, 0);
  g_assert_cmpuint (fpi_fte3600_build_boot38_readback (frame, 23, 16), ==, 0);
  g_assert_cmpuint (fpi_fte3600_build_boot38_read (frame, 3, 1), ==, 0);
  g_assert_cmpuint (fpi_fte3600_build_boot38_write (NULL, 4, 1, 1), ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/legacy38/wire", test_wire);
  for (guint i = 0; i < 9; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/legacy38/identity/%u", i);
      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_identify);
    }
  for (guint i = 0; i < 13; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/legacy38/recovery/%u", i);
      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_recover);
    }
  for (guint i = 0; i < 16; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/legacy38/transfer-failure/%s/%u",
                                                i < 8 ? "FT9338" : "FT9536", i % 8 + 1);
      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_recovery_transfer_failure);
    }
  return g_test_run ();
}
