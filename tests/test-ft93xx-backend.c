/* Independent simulated hardware; production SSMs and transfer ownership.
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <string.h>
#include "drivers/fte3600-private.h"
#include "drivers/fte3600-ft93xx.h"

G_DEFINE_TYPE (FpiDeviceFte3600, fpi_device_fte3600, FP_TYPE_DEVICE)

static struct
{
  GCancellable *cancellable;
  guint16       id;
  guint16       registers[0x2000];
  guint8        sfr[256];
  guint16       variant;
  guint         width;
  guint         height;
  guint         initial_dac;
  guint         raw_size;
  gint          exposure_offset;
  gsize         fifo_offset;
  guint         transactions;
  guint         frames;
  gint64        frame_times[32];
  guint         sequence_start;
  guint         sequence_count;
  guint8        sequence[16];
  guint         cancel_after_frame;
  guint         fifo_reads;
  guint         longest_transfer;
  guint         dac_writes;
  guint         resets;
  guint         key_opens;
  guint         key_closes;
  guint         cleanup_after_cancel;
  guint         fail_transaction;
  guint16       bad_readback;
  gboolean      texture;
  gboolean      reject_idle;
  gboolean      reject_scan;
  gboolean      never_ready;
  gboolean      crc_fault;
  gboolean      cancel_fifo;
  gboolean      cancel_key;
  gboolean      cancel_empty;
  gboolean      short_fifo;
  gboolean      frozen_adc;
  gboolean      reject_pad_voltage;
  gboolean      fault_status;
  gboolean      completed;
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

  device->id = "ft93xx-backend-test";
  device->full_name = "FT93xx simulated hardware";
  device->type = FP_DEVICE_TYPE_VIRTUAL;
  device->features = FP_DEVICE_FEATURE_CAPTURE;
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

gboolean
fpi_fte3600_fail_if_cancelled (FpiSsm *ssm, FpDevice *dev)
{
  GError *error = NULL;

  if (!g_cancellable_set_error_if_cancelled (mock.cancellable, &error))
    return FALSE;
  fpi_ssm_mark_failed (ssm, error);
  return TRUE;
}

static guint16
be16 (const guint8 *bytes)
{
  return ((guint16) bytes[0] << 8) | bytes[1];
}

static void
put16 (guint8 *bytes, guint16 value)
{
  bytes[0] = value >> 8;
  bytes[1] = value;
}

static void
emulate (FpiSpiTransfer *transfer)
{
  const guint8 *tx = transfer->buffer_wr;
  guint8 *rx = transfer->buffer_rd;
  gsize size = transfer->length_wr;

  g_assert_true (transfer->sensitive);
  g_assert_cmpuint (size, <=, 1798);
  mock.longest_transfer = MAX (mock.longest_transfer, size);
  if (rx)
    {
      g_assert_true (transfer->full_duplex);
      g_assert_cmpint (transfer->length_rd, ==, size);
      memset (rx, 0, size);
    }

  if (size == 3)
    {
      g_assert_null (rx);
      g_assert_cmphex (tx[1], ==, (guint8) (tx[0] ^ 0xff));
      g_assert_cmphex (tx[2], ==, 0);
      switch (tx[0])
        {
        case 0x5a: break;

        case 0xc0:
          mock.resets++;
          mock.sfr[0x80] = mock.reject_idle ? 0x51 : 0x50;
          break;

        case 0xc4:
          g_assert_cmphex (mock.sfr[0x80], ==, 0x50);
          mock.sfr[0x80] = mock.reject_scan ? 0x50 : 0x54;
          break;

        default: g_assert_not_reached ();
        }
      return;
    }
  if (tx[0] == 0x09)
    {
      g_assert_cmpuint (size, ==, 4);
      g_assert_cmphex (tx[1], ==, 0xf6);
      g_assert_null (rx);
      mock.sfr[tx[2]] = tx[3];
      if (tx[2] == 0x9a)
        {
          if (tx[3])
            {
              g_assert_cmphex (tx[3], ==, 0x5a);
              mock.key_opens++;
              if (mock.cancel_key)
                g_cancellable_cancel (mock.cancellable);
            }
          else
            {
              mock.key_closes++;
            }
        }
      return;
    }
  if (tx[0] == 0x08)
    {
      g_assert_cmpuint (size, ==, 5);
      g_assert_cmphex (tx[1], ==, 0xf7);
      g_assert_cmphex (tx[3], ==, 0);
      g_assert_cmphex (tx[4], ==, 0);
      g_assert_nonnull (rx);
      rx[4] = mock.sfr[tx[2]];
      if (tx[2] == 0xfe && mock.reject_pad_voltage)
        rx[4] = 0xff;
      return;
    }

  guint16 address = be16 (tx + 2) & 0x7fff;
  g_assert_cmphex (tx[2] & 0x80, ==, 0x80);
  g_assert_cmpuint (address, <, G_N_ELEMENTS (mock.registers));
  if (tx[0] == 0x04)
    {
      guint16 value = mock.registers[address];
      g_assert_cmphex (tx[1], ==, 0xfb);
      g_assert_cmpuint (size, ==, 10);
      g_assert_cmphex (be16 (tx + 4), ==, 0);
      g_assert_nonnull (rx);
      if (address == 0x1a8b)
        value = mock.id;
      if (address == 0x1a82)
        value = mock.fault_status ? 0x0400 : mock.never_ready ? 0 : 0x0020;
      if (address == mock.bad_readback)
        value ^= 1;
      put16 (rx + 6, value);
      if (mock.crc_fault && address == 0x1a82)
        put16 (rx + 8, 1);
      /* Independent known nonzero ID checksum; other replies use the
       * reference's zero-checksum sentinel, not a second CRC implementation. */
      else if (address == 0x1a8b && mock.id == 0x9365)
        put16 (rx + 8, 0x6cb4);
      return;
    }
  if (tx[0] == 0x05)
    {
      guint16 value = be16 (tx + 6);
      g_assert_cmphex (tx[1], ==, 0xfa);
      g_assert_cmpuint (size, ==, 8);
      g_assert_cmphex (be16 (tx + 4), ==, 0);
      g_assert_null (rx);
      if (address >= 0xc0 && address <= 0xe9)
        g_assert_cmphex (mock.sfr[0x9a], ==, 0x5a);
      if (address == 0x1801)
        mock.dac_writes++;
      if (address == 0x1800 && (value & 1))
        {
          g_assert_cmphex (mock.registers[0x1800] & 1, ==, 0);
          g_assert_cmphex (mock.sfr[0x80], ==, 0x54);
          g_assert_cmphex (value, ==, mock.id == 0x9365 ? 0x4fff : 0xc7ff);
          mock.frames++;
          if (mock.frames <= G_N_ELEMENTS (mock.frame_times))
            mock.frame_times[mock.frames - 1] = g_get_monotonic_time ();
          mock.fifo_offset = 0;
        }
      mock.registers[address] = value;
      return;
    }
  g_assert_cmphex (tx[0], ==, 0x06);
  g_assert_cmphex (tx[1], ==, 0xf9);
  g_assert_cmphex (address, ==, 0x1a05);
  g_assert_nonnull (rx);
  g_assert_cmphex (mock.sfr[0x80], ==, 0x54);
  gsize payload = ((gsize) be16 (tx + 4) + 1) * 2;
  g_assert_cmpuint (size, ==, payload + 8);
  g_assert_cmpuint (payload, <=, 1790);
  g_assert_cmpuint (mock.fifo_offset + payload, <=, mock.raw_size);
  for (gsize i = 0; i < payload; i += 2)
    {
      gsize sample_index = (mock.fifo_offset + i) / 2;
      gint dac = mock.registers[0x1801] & (mock.id == 0x9365 ? 0x7f : 0xff);
      gint signal = mock.texture ? 3264 + (sample_index % 16) * 24 : 3472;
      if (mock.sequence_count && mock.frames > mock.sequence_start)
        {
          guint index = mock.frames - mock.sequence_start - 1;

          g_assert_cmpuint (index, <, mock.sequence_count);
          switch (mock.sequence[index])
            {
            case 0: signal = 3472; break;
            case 1: signal = 3264 + (sample_index % 16) * 24; break;
            /* Low range alone or low gradient alone must not prove empty. */
            case 2: signal = 3432 + (sample_index % 2) * 80; break;
            case 3: signal = 3344 + (sample_index % mock.width) * 4; break;
            default: g_assert_not_reached ();
            }
        }
      signal += mock.exposure_offset;
      if (!mock.frozen_adc)
        signal += (dac - (gint) mock.initial_dac) * 40;
      /* Extra rows carry deliberately unrelated data and must be cropped. */
      if (sample_index >= (gsize) mock.width * mock.height)
        signal = 12;
      put16 (rx + 6 + i, CLAMP (signal, 0, 4092));
    }
  rx[size - 2] = 0xde;
  rx[size - 1] = 0xad; /* Opaque FIFO trailer, not a register CRC. */
  mock.fifo_offset += payload;
  mock.fifo_reads++;
  if (mock.cancel_fifo || (mock.cancel_after_frame && mock.frames >= mock.cancel_after_frame &&
                           mock.fifo_offset == mock.raw_size) ||
      (mock.cancel_empty && mock.frames >= 3 &&
                           mock.fifo_offset == mock.raw_size))
    g_cancellable_cancel (mock.cancellable);
}

typedef struct
{
  FpiSsm         *ssm;
  FpiSpiTransfer *transfer;
  gboolean        cancellable;
} Pending;

static gboolean
complete_transfer (gpointer data)
{
  Pending *pending = data;
  GError *error = NULL;

  mock.transactions++;
  if (pending->cancellable)
    g_cancellable_set_error_if_cancelled (mock.cancellable, &error);
  else if (g_cancellable_is_cancelled (mock.cancellable))
    mock.cleanup_after_cancel++;
  if (!error && mock.transactions == mock.fail_transaction)
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Injected transport error");
  if (!error && mock.short_fifo && pending->transfer->buffer_wr[0] == 0x06)
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT, "Injected short FIFO transfer");
  if (!error)
    emulate (pending->transfer);
  if (error)
    fpi_ssm_mark_failed (pending->ssm, error);
  else
    fpi_ssm_next_state (pending->ssm);
  fpi_spi_transfer_unref (pending->transfer);
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
  mock.completed = TRUE;
  mock.error = error;
}

static void
run (FpiSsm *ssm)
{
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;

  mock.completed = FALSE;
  g_clear_error (&mock.error);
  fpi_ssm_start (ssm, complete_ssm);
  while (!mock.completed)
    {
      g_main_context_iteration (NULL, TRUE);
      g_assert_cmpint (g_get_monotonic_time (), <, deadline);
    }
}

static FpiDeviceFte3600 *
setup (guint16 id)
{
  FpiDeviceFte3600 *self;
  GError *error = NULL;

  memset (&mock, 0, sizeof mock);
  mock.id = id;
  mock.width = id == 0x9365 ? 64 : 40;
  mock.height = id == 0x9365 ? 80 : 196;
  mock.raw_size = id == 0x9365 ? 10240 : 16000;
  mock.initial_dac = id == 0x9365 ? 72 : 124;
  mock.cancellable = g_cancellable_new ();
  mock.sfr[0x80] = 0x50;
  mock.registers[0x1816] = id == 0x9365 ? 0 : 0x7fff;
  /* Undocumented AFE bits must survive read/modify/write. */
  for (guint i = 0xc0; i < 0xea; i++)
    mock.registers[i] = 0xa500;
  mock.registers[0x1812] = 0x8000;
  mock.registers[0x1a83] = 0x0800;
  self = g_object_new (fpi_device_fte3600_get_type (), NULL);
  self->sensor = fpi_fte3600_sensor_get (id == 0x9365 ? FTE3600_SENSOR_FT9365 : FTE3600_SENSOR_FT9769);
  self->identity = fpi_fte3600_identify_special (id);
  self->backend = fpi_fte3600_ft93xx_backend (self->sensor->sensor);
  self->max_transfer = 1798;
  self->image_size = mock.width * mock.height;
  self->capture_frame_size = mock.raw_size;
  self->capture_tx = g_malloc0 (mock.raw_size);
  self->capture_rx = g_malloc0 (mock.raw_size);
  g_assert_true (self->backend->prepare_capture (self, &error));
  g_assert_no_error (error);
  return self;
}

static void
teardown (FpiDeviceFte3600 *self)
{
  self->backend->destroy (self);
  self->backend->destroy (self);
  g_assert_null (self->backend_data);
  g_clear_object (&self->captured_image);
  g_free (self->capture_tx);
  g_free (self->capture_rx);
  g_object_unref (self);
  g_clear_object (&mock.cancellable);
  g_clear_error (&mock.error);
}

static void
test_cold_capture (gconstpointer data)
{
  guint16 id = GPOINTER_TO_UINT (data);
  FpiDeviceFte3600 *self = setup (id);

  run (self->backend->create_init (self));
  g_assert_no_error (mock.error);
  g_assert_true (self->idle_verified);
  g_assert_cmphex (mock.sfr[0x9a], ==, 0);
  g_assert_cmpuint (mock.key_opens, ==, 1);
  g_assert_cmphex (mock.registers[0x1812], ==, 0x8100);
  g_assert_cmphex (mock.registers[0x1a83], ==, 0x0860);
  g_assert_cmphex (mock.registers[0x1806], ==, 0x023b);
  g_assert_cmphex (mock.registers[0x1807], ==, 0x0fef);
  if (id == 0x9365)
    {
      g_assert_cmphex (mock.registers[0xc0], ==, 0xa444);
      g_assert_cmphex (mock.registers[0xc1], ==, 0xa521);
      g_assert_cmphex (mock.registers[0xc2], ==, 0xa5c0);
    }
  else
    {
      const guint channels[] = { 3, 9, 15, 20 };
      for (guint i = 0; i < G_N_ELEMENTS (channels); i++)
        {
          g_assert_cmphex (mock.registers[0xc0 + 2 * channels[i]], ==, 0xa103);
          g_assert_cmphex (mock.registers[0xc1 + 2 * channels[i]], ==, 0xa000);
        }
    }
  mock.texture = TRUE;
  for (guint round = 0; round < 2; round++)
    {
      guint previous_reads = mock.fifo_reads;
      run (self->backend->create_capture (self));
      g_assert_no_error (mock.error);
      g_assert_true (self->idle_verified);
      g_assert_nonnull (self->captured_image);
      g_assert_cmpuint (mock.fifo_reads - previous_reads, ==, id == 0x9365 ? 6 : 9);
      g_assert_cmpuint (fp_image_get_width (self->captured_image), ==, mock.width);
      g_assert_cmpuint (fp_image_get_height (self->captured_image), ==, mock.height);
      g_assert_cmpfloat (fp_image_get_ppmm (self->captured_image), ==, 0.0);
      for (gsize i = 0; i < self->image_size; i++)
        g_assert_cmpuint (self->captured_image->data[i], ==, ((id == 0x9392 ? i ^ 3 : i) % 16) * 17);
      for (gsize i = 0; i < self->capture_frame_size; i++)
        g_assert_cmpuint (self->capture_rx[i], ==, 0);
      g_clear_object (&self->captured_image);
    }
  g_assert_cmpuint (mock.longest_transfer, ==, 1798);
  teardown (self);
}

static void
test_calibration (gconstpointer data)
{
  guint scenario = GPOINTER_TO_UINT (data);
  FpiDeviceFte3600 *self = setup (scenario & 1 ? 0x9392 : 0x9365);

  mock.exposure_offset = -1072;
  mock.frozen_adc = scenario >= 2;
  run (self->backend->create_init (self));
  if (mock.frozen_adc)
    {
      g_assert_nonnull (mock.error);
      g_assert_cmpuint (mock.frames, <=, 9);
    }
  else
    {
      g_assert_no_error (mock.error);
      g_assert_cmpuint (mock.frames, >=, 2);
      g_assert_cmpuint (mock.frames, <=, 9);
      g_assert_cmpuint (mock.registers[0x1801] & (mock.id == 0x9365 ? 0x7f : 0xff), >, mock.initial_dac);
    }
  g_assert_true (self->idle_verified);
  g_assert_null (self->captured_image);
  teardown (self);
}

static void
test_capture_failure (gconstpointer data)
{
  FpiDeviceFte3600 *self = setup (0x9392);
  guint scenario = GPOINTER_TO_UINT (data);

  run (self->backend->create_init (self));
  g_assert_no_error (mock.error);
  mock.texture = TRUE;
  switch (scenario)
    {
    case 0: mock.cancel_fifo = TRUE;
      break;

    case 1: mock.short_fifo = TRUE;
      break;

    case 2: mock.crc_fault = TRUE;
      break;

    case 3: mock.never_ready = TRUE;
      break;

    case 4: mock.reject_scan = TRUE;
      break;

    case 5: mock.fault_status = TRUE;
      break;

    case 6:
      mock.texture = FALSE;
      mock.cancel_empty = TRUE;
      break;

    default: g_assert_not_reached ();
    }
  run (self->backend->create_capture (self));
  g_assert_nonnull (mock.error);
  g_assert_true (self->idle_verified);
  g_assert_null (self->captured_image);
  if (scenario == 0 || scenario == 6)
    {
      g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      g_assert_cmpuint (mock.cleanup_after_cancel, >, 0);
    }
  if (scenario == 1)
    g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT);
  if (scenario == 2)
    g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  if (scenario == 3)
    g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  if (scenario == 6)
    g_assert_cmpuint (mock.frames, ==, 3);
  mock.cancel_fifo = mock.short_fifo = mock.crc_fault = FALSE;
  mock.never_ready = mock.reject_scan = mock.fault_status = mock.cancel_empty = FALSE;
  mock.texture = TRUE;
  g_cancellable_reset (mock.cancellable);
  run (self->backend->create_capture (self));
  g_assert_no_error (mock.error);
  g_assert_nonnull (self->captured_image);
  teardown (self);
}

static void
test_init_failure (gconstpointer data)
{
  FpiDeviceFte3600 *self = setup (0x9391);
  guint scenario = GPOINTER_TO_UINT (data);

  switch (scenario)
    {
    case 0: mock.registers[0x1816] = 0x0fff;
      break;

    case 1: mock.id = 0x9365;
      break;

    case 2: mock.bad_readback = 0x1807;
      break;

    case 3: mock.cancel_key = TRUE;
      break;

    case 4: mock.reject_pad_voltage = TRUE;
      break;

    default: g_assert_not_reached ();
    }
  run (self->backend->create_init (self));
  g_assert_nonnull (mock.error);
  g_assert_cmphex (mock.sfr[0x9a], ==, 0);
  g_assert_true (self->idle_verified);
  g_assert_cmpuint (mock.frames, ==, 0);
  if (scenario == 0 || scenario == 1 || scenario == 4)
    g_assert_cmpuint (mock.key_opens, ==, 0);
  if (scenario == 3)
    g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  teardown (self);
}

static void
test_cleanup_failure (gconstpointer data)
{
  FpiDeviceFte3600 *self = setup (0x9365);
  guint scenario = GPOINTER_TO_UINT (data);

  if (scenario == 0)
    mock.reject_idle = TRUE;
  else
    mock.fail_transaction = 1;
  run (self->backend->create_reset (self));
  g_assert_nonnull (mock.error);
  g_assert_false (self->idle_verified);
  g_assert_cmpuint (mock.resets, >, 0);
  if (scenario == 0)
    g_assert_cmpuint (mock.resets, ==, 5);
  else
    g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  mock.reject_idle = FALSE;
  mock.fail_transaction = 0;
  run (self->backend->create_reset (self));
  g_assert_no_error (mock.error);
  g_assert_true (self->idle_verified);
  teardown (self);
}

static void
test_wait_release (gconstpointer data)
{
  guint scenario = GPOINTER_TO_UINT (data) / 2;
  FpiDeviceFte3600 *self = setup (GPOINTER_TO_UINT (data) & 1 ? 0x9392 : 0x9365);
  const guint8 sequence[] = { 1, 2, 3, 0, 0, 1, 0, 0, 0 };
  guint initial_frames;

  run (self->backend->create_init (self));
  g_assert_no_error (mock.error);
  g_assert_nonnull (self->backend->create_wait_release);
  initial_frames = mock.frames;
  if (scenario == 0)
    {
      mock.sequence_start = initial_frames;
      mock.sequence_count = G_N_ELEMENTS (sequence);
      memcpy (mock.sequence, sequence, sizeof sequence);
    }
  else if (scenario == 1)
    {
      mock.texture = TRUE;
      mock.cancel_after_frame = initial_frames + 3;
    }
  else if (scenario == 2)
    mock.short_fifo = TRUE;
  else if (scenario == 3)
    mock.reject_idle = TRUE;
  else
    {
      /* Match the public core's dispatch contract: it clears idle before
       * invoking the child, even when cancellation is already pending. */
      self->idle_verified = FALSE;
      g_cancellable_cancel (mock.cancellable);
    }
  run (self->backend->create_wait_release (self));
  g_assert_null (self->captured_image);
  g_assert_false (self->armed);
  if (scenario == 0)
    {
      g_assert_no_error (mock.error);
      g_assert_cmpuint (mock.frames - initial_frames, ==, G_N_ELEMENTS (sequence));
    }
  else if (scenario == 1 || scenario == 4)
    {
      g_assert_error (mock.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      g_assert_cmpuint (mock.frames - initial_frames, ==, scenario == 1 ? 3 : 0);
      g_assert_cmpuint (mock.cleanup_after_cancel, >, 0);
    }
  else
    g_assert_nonnull (mock.error);
  g_assert_cmpint (self->idle_verified, ==, scenario != 3);
  for (gsize i = 0; i < self->capture_frame_size; i++)
    g_assert_cmpuint (self->capture_rx[i], ==, 0);
  teardown (self);
}

static void
test_empty_backoff (void)
{
  FpiDeviceFte3600 *self = setup (0x9365);
  const guint8 sequence[] = { 0, 0, 0, 0, 0, 1 };
  const guint delays[] = { 100, 200, 400, 500, 500 };
  guint initial_frames;

  run (self->backend->create_init (self));
  g_assert_no_error (mock.error);
  initial_frames = mock.frames;
  mock.sequence_start = initial_frames;
  mock.sequence_count = G_N_ELEMENTS (sequence);
  memcpy (mock.sequence, sequence, sizeof sequence);
  run (self->backend->create_capture (self));
  g_assert_no_error (mock.error);
  g_assert_nonnull (self->captured_image);
  for (guint i = 0; i < G_N_ELEMENTS (delays); i++)
    g_assert_cmpint (mock.frame_times[initial_frames + i + 1] -
                    mock.frame_times[initial_frames + i], >=, delays[i] * 1000);
  /* A new capture starts with its own 100ms delay, not the previous cap. */
  mock.sequence_start = mock.frames;
  mock.sequence_count = 2;
  mock.sequence[0] = 0;
  mock.sequence[1] = 1;
  initial_frames = mock.frames;
  run (self->backend->create_capture (self));
  g_assert_no_error (mock.error);
  g_assert_cmpuint (mock.frames - initial_frames, ==, 2);
  g_assert_cmpint (mock.frame_times[initial_frames + 1] - mock.frame_times[initial_frames],
                   >=, 100 * 1000);
  teardown (self);
}

int
main (int argc, char **argv)
{
  const guint16 ids[] = { 0x9365, 0x9391, 0x9392 };

  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/ft93xx/empty-backoff", test_empty_backoff);
  for (guint i = 0; i < 10; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/ft93xx/release/%u", i);

      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_wait_release);
    }
  for (guint i = 0; i < G_N_ELEMENTS (ids); i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/ft93xx/cold-capture/%04x", ids[i]);
      g_test_add_data_func (name, GUINT_TO_POINTER (ids[i]), test_cold_capture);
    }
  for (guint i = 0; i < 4; i++)
    {
      g_autofree gchar *calibration = g_strdup_printf ("/ft93xx/calibration/%u", i);
      g_autofree gchar *init = g_strdup_printf ("/ft93xx/init-failure/%u", i);
      g_test_add_data_func (calibration, GUINT_TO_POINTER (i), test_calibration);
      g_test_add_data_func (init, GUINT_TO_POINTER (i), test_init_failure);
    }
  g_test_add_data_func ("/ft93xx/init-failure/4", GUINT_TO_POINTER (4), test_init_failure);
  for (guint i = 0; i < 7; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/ft93xx/capture-failure/%u", i);
      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_capture_failure);
    }
  for (guint i = 0; i < 2; i++)
    {
      g_autofree gchar *name = g_strdup_printf ("/ft93xx/cleanup/%u", i);
      g_test_add_data_func (name, GUINT_TO_POINTER (i), test_cleanup_failure);
    }
  return g_test_run ();
}
