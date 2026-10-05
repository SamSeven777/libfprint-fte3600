/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Real asynchronous SSM and transfer ownership, with a synthetic cold device.
 * The model exposes identity only after wake and acknowledged C6 negotiation. */
#include <string.h>
#include "drivers/fte3600-special-probe.h"

G_DEFINE_TYPE (FpiDeviceFte3600, fpi_device_fte3600, FP_TYPE_DEVICE)

static struct
{
  GCancellable *cancellable;
  gboolean      completed;
  gboolean      awake;
  gboolean      mode;
  gboolean      idle;
  guint         bad_crc_read;
  gboolean      unstable_variant;
  guint16       ids[2];
  guint16       variant;
  guint         transfers;
  guint         id_reads;
  guint         variant_reads;
  guint         writes;
  guint         ignore_writes;
  guint         gpio_calls;
  guint         idle_commands;
  guint         fail_at;
  guint         cancel_at;
  guint         gpio_fail_at;
  GArray       *events;
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

  device->id = "special-probe-test";
  device->full_name = "Special-family cold probe simulation";
  device->type = FP_DEVICE_TYPE_VIRTUAL;
  device->features = FP_DEVICE_FEATURE_CAPTURE;
}

static void
record (guint event)
{
  g_array_append_val (mock.events, event);
}

void __real_fpi_ssm_next_state_delayed (FpiSsm *ssm,
                                        int     delay);
void __wrap_fpi_ssm_next_state_delayed (FpiSsm *ssm,
                                        int     delay);
void
__wrap_fpi_ssm_next_state_delayed (FpiSsm *ssm, int delay)
{
  record (delay);
  __real_fpi_ssm_next_state_delayed (ssm, 0);
}

void
fpi_fte3600_clear_irq_source (FpiDeviceFte3600 *self)
{
  self->irq_wait_ssm = NULL;
}

void
fpi_fte3600_set_hardware_reset (FpiSsm *ssm, FpiDeviceFte3600 *self,
                                gboolean asserted)
{
  record (asserted ? 1001 : 1000);
  mock.gpio_calls++;
  self->idle_verified = FALSE;
  if (mock.gpio_calls == mock.gpio_fail_at)
    {
      fpi_ssm_mark_failed (ssm, g_error_new_literal (
                             G_IO_ERROR, G_IO_ERROR_FAILED, "Injected GPIO error"));
    }
  else
    {
      if (asserted)
        {
          mock.awake = FALSE;
          mock.mode = FALSE;
        }
      fpi_ssm_next_state (ssm);
    }
}

gboolean
fpi_fte3600_fail_if_cancelled (FpiSsm *ssm, FpDevice *device)
{
  GError *error = NULL;

  if (!g_cancellable_set_error_if_cancelled (mock.cancellable, &error))
    return FALSE;
  fpi_ssm_mark_failed (ssm, error);
  return TRUE;
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
  p[1] = value;
}

static void
emulate (FpiSpiTransfer *transfer)
{
  const guint8 *tx = transfer->buffer_wr;
  guint8 *rx = transfer->buffer_rd;
  gsize length = transfer->length_wr;

  g_assert_true (transfer->full_duplex);
  g_assert_cmpint (transfer->length_wr, ==, transfer->length_rd);
  g_assert_cmpuint (length, >=, 3);
  g_assert_cmpuint (tx[0] ^ tx[1], ==, 0xff);
  memset (rx, 0, length);
  record (0x10000 | tx[0]);
  switch (tx[0])
    {
    case 0x5a:
      g_assert_cmpuint (length, ==, 3);
      mock.awake = TRUE;
      return;

    case 0xc0:
      g_assert_cmpuint (length, ==, 3);
      mock.idle_commands++;
      mock.idle = TRUE;
      return;

    case 0xa5:
      g_assert_cmpuint (length, ==, 3);
      return;

    case 0x09:
      /* Reject any unknown voltage, IRQ or firmware write in discovery. */
      g_assert_cmpuint (length, ==, 4);
      g_assert_cmpuint (tx[2], ==, 0xc6);
      g_assert_cmpuint (tx[3], ==, 1);
      mock.writes++;
      if (mock.awake && mock.writes > mock.ignore_writes)
        mock.mode = TRUE;
      return;

    case 0x08:
      g_assert_cmpuint (length, ==, 5);
      g_assert_true (tx[2] == 0x80 || tx[2] == 0xc6);
      rx[4] = tx[2] == 0x80 ? (mock.idle ? 0x50 : 0x51) : mock.mode;
      return;

    case 0x04:
      g_assert_true (mock.awake && mock.mode);
      if ((be16 (tx + 2) & 0x7fff) == 0x1a8b)
        {
          g_assert_cmpuint (length, ==, 12);
          g_assert_cmpuint (be16 (tx + 4), ==, 1);
          g_assert_cmpuint (mock.id_reads, <, 2);
          put16 (rx + 6, mock.ids[mock.id_reads++]);
        }
      else
        {
          g_assert_cmpuint (be16 (tx + 2) & 0x7fff, ==, 0x1816);
          g_assert_cmpuint (length, ==, 10);
          g_assert_cmpuint (be16 (tx + 4), ==, 0);
          put16 (rx + 6, mock.variant + (mock.unstable_variant && mock.variant_reads));
          if (mock.bad_crc_read == mock.variant_reads + 1)
            put16 (rx + 8, 1);
          mock.variant_reads++;
        }
      return;

    default:
      g_error ("Unexpected state-changing command %02x during special discovery", tx[0]);
    }
}

static gboolean
complete_transfer (gpointer user_data)
{
  FpiSpiTransfer *transfer = user_data;
  GError *error = NULL;

  mock.transfers++;
  g_cancellable_set_error_if_cancelled (mock.cancellable, &error);
  if (!error && mock.transfers == mock.fail_at)
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE, "Injected SPI error");
  if (!error)
    emulate (transfer);
  if (mock.transfers == mock.cancel_at)
    g_cancellable_cancel (mock.cancellable);
  fpi_ssm_spi_transfer_cb (transfer, transfer->device, NULL, error);
  fpi_spi_transfer_unref (transfer);
  return G_SOURCE_REMOVE;
}

void
fpi_fte3600_submit_transfer (FpiSsm *ssm, FpiSpiTransfer *transfer, gboolean cancellable)
{
  g_assert_true (cancellable);
  transfer->ssm = ssm;
  g_idle_add (complete_transfer, transfer);
}

static void
completed (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  mock.error = error;
  mock.completed = TRUE;
}

static void
run (FpiDeviceFte3600 *self, Fte3600Identity *result)
{
  fpi_ssm_start (fpi_fte3600_special_probe_new (self, result), completed);
  while (!mock.completed)
    g_main_context_iteration (NULL, TRUE);
}

static FpiDeviceFte3600 *
setup (guint16 id)
{
  FpiDeviceFte3600 *self;

  memset (&mock, 0, sizeof mock);
  mock.ids[0] = mock.ids[1] = id;
  mock.variant = 0x0123;
  mock.cancellable = g_cancellable_new ();
  mock.events = g_array_new (FALSE, FALSE, sizeof (guint));
  self = g_object_new (fpi_device_fte3600_get_type (), NULL);
  self->max_transfer = 16384;
  return self;
}

static void
check_cleanup (void)
{
  static const guint expected[] = { 1000, 10, 1001, 20, 1000, 160 };

  g_assert_cmpuint (mock.gpio_calls, ==, 3);
  g_assert_cmpuint (mock.events->len, >=, G_N_ELEMENTS (expected));
  for (guint i = 0; i < G_N_ELEMENTS (expected); i++)
    g_assert_cmpuint (g_array_index (mock.events, guint,
                                     mock.events->len - G_N_ELEMENTS (expected) + i), ==, expected[i]);
}

static void
teardown (FpiDeviceFte3600 *self)
{
  g_object_unref (self);
  g_clear_object (&mock.cancellable);
  g_array_unref (mock.events);
  g_clear_error (&mock.error);
}

static void
test_cold (gconstpointer param)
{
  guint16 id = GPOINTER_TO_UINT (param);
  FpiDeviceFte3600 *self = setup (id);
  Fte3600Identity result = { 0 };

  run (self, &result);
  g_assert_no_error (mock.error);
  g_assert_cmpuint (result.sensor, ==, fpi_fte3600_identify_special (id).sensor);
  g_assert_cmpuint (result.response, ==, id);
  g_assert_cmpuint (result.evidence, ==, FTE3600_IDENTITY_SPECIAL_CHIP_ID);
  g_assert_cmpuint (self->identity.sensor, ==, FTE3600_SENSOR_UNKNOWN);
  g_assert_cmpuint (mock.id_reads, ==, 2);
  g_assert_cmpuint (mock.variant_reads, ==, id == 0x9391 ? 2 : 0);
  g_assert_cmpuint (mock.gpio_calls, ==, 0);
  g_assert_cmpuint (mock.idle_commands, ==, 1);
  g_assert_true (mock.mode && mock.awake);
  teardown (self);
}

static void
test_unknown (gconstpointer param)
{
  guint16 id = GPOINTER_TO_UINT (param);
  FpiDeviceFte3600 *self = setup (id);
  Fte3600Identity result = { 0 };

  run (self, &result);
  g_assert_no_error (mock.error);
  g_assert_cmpuint (result.sensor, ==, FTE3600_SENSOR_UNKNOWN);
  g_assert_cmpuint (mock.id_reads, ==, 2);
  check_cleanup ();
  g_assert_false (mock.mode || mock.awake);
  teardown (self);
}

static void
test_unstable_id (gconstpointer param)
{
  static const guint16 ids[][2] = {
    { 0x9362, 0x9365 }, { 0x9362, 0 }, { 0, 0x9362 },
  };
  guint scenario = GPOINTER_TO_UINT (param);
  FpiDeviceFte3600 *self = setup (ids[scenario][0]);
  Fte3600Identity result = { 0 };

  mock.ids[1] = ids[scenario][1];
  run (self, &result);
  g_assert_error (mock.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  g_assert_cmpuint (result.sensor, ==, FTE3600_SENSOR_UNKNOWN);
  check_cleanup ();
  teardown (self);
}

static void
test_mode_retry (gconstpointer param)
{
  guint ignores = GPOINTER_TO_UINT (param);
  FpiDeviceFte3600 *self = setup (0x9362);
  Fte3600Identity result = { 0 };

  mock.ignore_writes = ignores;
  mock.idle = TRUE;
  run (self, &result);
  g_assert_no_error (mock.error);
  g_assert_cmpuint (mock.idle_commands, ==, 0);
  if (ignores < 4)
    {
      g_assert_cmpuint (result.sensor, ==, FTE3600_SENSOR_FT9369);
      g_assert_cmpuint (mock.writes, ==, ignores + 1);
    }
  else
    {
      g_assert_cmpuint (result.sensor, ==, FTE3600_SENSOR_UNKNOWN);
      g_assert_cmpuint (mock.writes, ==, 4);
      g_assert_cmpuint (mock.id_reads, ==, 0);
      check_cleanup ();
    }
  teardown (self);
}

static void
test_variant (gconstpointer param)
{
  guint scenario = GPOINTER_TO_UINT (param);
  FpiDeviceFte3600 *self = setup (0x9391);
  Fte3600Identity result = { 0 };

  mock.variant = scenario == 0 ? 0x0fff : 0x123;
  mock.unstable_variant = scenario == 1;
  mock.bad_crc_read = scenario >= 2 ? scenario - 1 : 0;
  run (self, &result);
  g_assert_cmpuint (result.sensor, ==, FTE3600_SENSOR_UNKNOWN);
  if (scenario == 0)
    {
      g_assert_no_error (mock.error);
      g_assert_cmpuint (result.evidence, ==, FTE3600_IDENTITY_KNOWN_UNMAPPED_ID);
      g_assert_cmpuint (result.response, ==, 0x9395);
    }
  else if (scenario == 1)
    {
      g_assert_error (mock.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
    }
  else
    {
      g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    }
  check_cleanup ();
  teardown (self);
}

static void
test_transfer_failures (void)
{
  for (guint i = 1; i <= 10; i++)
    {
      FpiDeviceFte3600 *self = setup (0x9391);
      Fte3600Identity result = { 0 };
      mock.fail_at = i;
      run (self, &result);
      g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE);
      g_assert_cmpuint (result.sensor, ==, FTE3600_SENSOR_UNKNOWN);
      check_cleanup ();
      teardown (self);
    }
}

static void
test_cancellation (void)
{
  for (guint i = 0; i <= 10; i++)
    {
      FpiDeviceFte3600 *self = setup (0x9391);
      Fte3600Identity result = { 0 };
      if (i == 0)
        g_cancellable_cancel (mock.cancellable);
      else
        mock.cancel_at = i;
      run (self, &result);
      g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      g_assert_cmpuint (result.sensor, ==, FTE3600_SENSOR_UNKNOWN);
      if (i)
        check_cleanup ();
      else
        g_assert_cmpuint (mock.gpio_calls, ==, 0);
      teardown (self);
    }
}

static void
test_gpio_failures (void)
{
  for (guint i = 1; i <= 3; i++)
    {
      FpiDeviceFte3600 *self = setup (0);
      Fte3600Identity result = { 0 };
      mock.gpio_fail_at = i;
      run (self, &result);
      g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_FAILED);
      check_cleanup ();
      teardown (self);
    }
}

static void
test_error_precedence (void)
{
  FpiDeviceFte3600 *self = setup (0x9362);
  Fte3600Identity result = { 0 };

  mock.fail_at = 1;
  mock.gpio_fail_at = 2;
  run (self, &result);
  g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE);
  check_cleanup ();
  teardown (self);

  self = setup (0x9362);
  mock.ids[1] = 0;
  mock.gpio_fail_at = 2;
  run (self, &result);
  g_assert_error (mock.error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  check_cleanup ();
  teardown (self);
}

static void
test_transport_limit (void)
{
  FpiDeviceFte3600 *self = setup (0x9362);
  Fte3600Identity result = fpi_fte3600_identify_special (0x9362);

  self->max_transfer = 11;
  run (self, &result);
  g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
  g_assert_cmpuint (result.sensor, ==, FTE3600_SENSOR_UNKNOWN);
  g_assert_cmpuint (mock.transfers, ==, 0);
  g_assert_cmpuint (mock.gpio_calls, ==, 0);
  teardown (self);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_data_func ("/special/cold/9362", GUINT_TO_POINTER (0x9362), test_cold);
  g_test_add_data_func ("/special/cold/9365", GUINT_TO_POINTER (0x9365), test_cold);
  g_test_add_data_func ("/special/cold/9391", GUINT_TO_POINTER (0x9391), test_cold);
  g_test_add_data_func ("/special/cold/9392", GUINT_TO_POINTER (0x9392), test_cold);
  g_test_add_data_func ("/special/unknown/zero", GUINT_TO_POINTER (0), test_unknown);
  g_test_add_data_func ("/special/unknown/ones", GUINT_TO_POINTER (0xffff), test_unknown);
  g_test_add_data_func ("/special/unknown/other-protocol", GUINT_TO_POINTER (0x9368), test_unknown);
  g_test_add_data_func ("/special/errors/changed-id", GUINT_TO_POINTER (0), test_unstable_id);
  g_test_add_data_func ("/special/errors/disappearing-id", GUINT_TO_POINTER (1), test_unstable_id);
  g_test_add_data_func ("/special/errors/appearing-id", GUINT_TO_POINTER (2), test_unstable_id);
  g_test_add_data_func ("/special/mode/retry", GUINT_TO_POINTER (2), test_mode_retry);
  g_test_add_data_func ("/special/mode/bounded", GUINT_TO_POINTER (4), test_mode_retry);
  g_test_add_data_func ("/special/variant/unmapped", GUINT_TO_POINTER (0), test_variant);
  g_test_add_data_func ("/special/variant/unstable", GUINT_TO_POINTER (1), test_variant);
  g_test_add_data_func ("/special/variant/bad-crc", GUINT_TO_POINTER (2), test_variant);
  g_test_add_data_func ("/special/variant/second-bad-crc", GUINT_TO_POINTER (3), test_variant);
  g_test_add_func ("/special/errors/transfer", test_transfer_failures);
  g_test_add_func ("/special/errors/cancellation", test_cancellation);
  g_test_add_func ("/special/errors/gpio", test_gpio_failures);
  g_test_add_func ("/special/errors/precedence", test_error_precedence);
  g_test_add_func ("/special/errors/transport-limit", test_transport_limit);
  return g_test_run ();
}
