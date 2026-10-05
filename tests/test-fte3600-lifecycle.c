/*
 * Hardware-independent FTE3600 lifecycle and fault-injection tests.
 * SPDX-FileCopyrightText: 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Linker wrappers mock the device boundary and control decode scheduling.
 * The production driver,
 * SPI worker threads, state machines, cancellables and public FpDevice API run
 * unchanged. No real sensor, GPIO, firmware or biometric fixture is needed.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/spi/spidev.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include "drivers/fte3600-bridge.h"

#include "drivers/fte3600.h"
#include "drivers/fte3600-ft93xx-protocol.h"

#ifndef FTE3600_ENABLE_PERSONAL_AUTH
#define FTE3600_ENABLE_PERSONAL_AUTH 0
#endif

#if FTE3600_ENABLE_PERSONAL_AUTH
#include "drivers/fte3600-template.h"
#include "drivers/fte3600-ipa.h"
#include "fte3600-test-image.h"
#endif

GType fpi_device_fte3600_get_type (void);

/* Declarations also check wrapper signatures against the platform headers. */
#define WRAPPED(name) __typeof__ (name) __wrap_ ## name
WRAPPED (open);
WRAPPED (close);
WRAPPED (ioctl);
WRAPPED (g_file_get_contents);
WRAPPED (g_unix_fd_source_new);
#undef WRAPPED

__typeof__ (g_unix_fd_source_new) __real_g_unix_fd_source_new;
__typeof__ (close) __real_close;
__typeof__ (g_file_get_contents) __real_g_file_get_contents;
int __wrap_open64 (const char *path,
                   int         flags,
                   ...);

typedef enum {
  DELIVER_FINGER,
  CANCEL_WAIT,
} IrqAction;

typedef struct
{
  const gchar *name;
  guint8       width;
  guint8       height;
  guint8       firmware_version;
  guint8       agc_version;
  guint8       mode_register;
  guint8       otp;
  gboolean     cold_recovery;
  const gchar *firmware_path;
} MockModel;

typedef struct
{
  guint32 asserted;
  gint64  when;
} ResetEvent;

/* Independent protocol fixtures, not values copied from the runtime catalog. */
static const MockModel models[] = {
  { "FT9361", 64, 80, 0x30, 0x31, 0x76, 4, TRUE,
    "/usr/lib/firmware/fte3600/ft9361.bin" },
  { "FT9348", 96, 96, 0x30, 0x31, 0x76, 2, TRUE,
    "/usr/lib/firmware/fte3600/ft9348.bin" },
  { "FT9338", 88, 88, 0x40, 0x10, 0x76, 0, FALSE, "/usr/lib/firmware/fte3600/ft9338.bin" },
  { "FT9536", 64, 128, 0x23, 0x13, 0x47, 0, FALSE, "/usr/lib/firmware/fte3600/ft9536.bin" },
};

typedef struct
{
  const gchar *name;
  guint16      id;
  guint16      variant;
  gboolean     required_high;
  gboolean     cs_control;
  gboolean     unstable;
  gboolean     corrupt_crc;
  gboolean     fail_cs;
  guint32      limit;
  gboolean     success;
  gboolean     exercise_open_failure;
  gboolean     mode_required;
} ProbeFixture;

static const ProbeFixture *probe_fixture;
static gboolean boot38_fixture;
static gboolean wake_fixture;
static gboolean stale_wake_fixture;

typedef enum {
  LEGACY_WAKE_OK,
  LEGACY_WAKE_IO_ERROR,
  LEGACY_WAKE_SHORT,
  LEGACY_WAKE_CANCEL,
  LEGACY_WAKE_UNKNOWN,
  LEGACY_WAKE_UNSTABLE,
  LEGACY_WAKE_EXHAUSTED,
} LegacyWakeFault;

typedef struct
{
  const gchar    *name;
  guint           model;
  gboolean        required_high;
  gboolean        cs_control;
  guint           ready_attempt;
  LegacyWakeFault fault;
  guint           fault_command;
} LegacyWakeFixture;

static const LegacyWakeFixture *legacy_wake_fixture;

static struct
{
  GMutex           lock;
  GArray          *reset_events;
  GArray          *soft_reset_times;
  const MockModel *model;
  guint            spi_transactions;
  guint32          spi_mode;
  guint            cs_changes;
  guint            special_reads;
  guint            special_resets;
  guint8           special_mode;
  gboolean         special_wake;
  guint            wake_commands[2];
  guint            wake_status_reads[2];
  guint            wake_early_geometry_reads[2];
  guint            wake_geometry_reads;
  guint            sleeping_geometry_reads;
  guint            resets_before_wake;
  guint            wake_rom_queries;
  gint64           wake_command_time[2];
  gint64           wake_status_time[2];
  gint64           wake_ready_time;
  guint8           wake_previous_opcode;
  gboolean         wake_fault_injected;
  gboolean         probe_started;
  guint            ioctl_count;
  guint            mode_writes;
  guint            fail_cleanup_status;
  gboolean         cleanup_started;
  gboolean         short_image;
  guint32          buffer_size;
  gboolean         bad_abi;
  gboolean         bad_mode;
  gboolean         rom_probe;
  gboolean         persistent_cold;
  guint16          rom_family;
  guint8           otp;
  guint8           boot_reply;
  guint8           boot38_id;
  guint            probe_packets;
  guint            fail_probe_packet;
  guint            cancel_probe_packet;
  guint            firmware_opens;
  gsize            firmware_file_size;
  gint             firmware_fd;
  gboolean         otp_enabled;
  gint             spi_fd;
  gint             irq_pipe[2];
  guint            opens;
  guint            closes;
  guint            claims;
  guint            releases;
  guint            resets;
  gboolean         inactive;
  guint            inactive_wake_commands;
  gint64           wake_first;
  gint64           wake_last;
  guint            awake_id_reads;
  guint            fail_wake_at;
  gboolean         cancel_wake;
  gboolean         unstable_wake;
  guint            discovery_wakes;
  guint            hardware_asserts;
  guint            hardware_deasserts;
  guint            images;
  const guint8    *frames;
  guint            n_frames;
  guint            next_frame;
  guint            irq_source;
  guint8           registers[256];
  gboolean         claimed;
  gboolean         armed;
  gboolean         finger_ready;
  gboolean         bad_id;
  gboolean         fail_config;
  gboolean         fail_claim;
  gboolean         fail_image;
  gboolean         fail_reset;
  gboolean         cancel_image;
  gboolean         hardware_recovery;
  gboolean         cold_start;
  gboolean         reset_asserted;
  gboolean         cancel_hardware_reset;
  gboolean         fail_hardware_reset;
  IrqAction        irq_action;
  GCancellable    *cancellable;
} sensor;

int
__wrap_open (const char *path, int flags, ...)
{
  /* Fail closed: an unexpected firmware/hardware open is a test failure. */
  if (g_str_has_prefix (path, "/usr/lib/firmware/fte3600/"))
    {
      g_assert_nonnull (sensor.model->firmware_path);
      g_assert_cmpstr (path, ==, sensor.model->firmware_path);
      sensor.firmware_opens++;
      if (sensor.firmware_file_size)
        {
          g_autofree gchar *temporary_path = NULL;
          g_autofree guint8 *bytes = g_malloc0 (sensor.firmware_file_size);
          g_autoptr(GError) error = NULL;

          /* Ordinary generated bytes exercise size/hash validation without a
           * vendor payload. The loader owns and must close this regular fd. */
          g_assert_cmpint (sensor.firmware_fd, ==, -1);
          sensor.firmware_fd = g_file_open_tmp ("fte3600-test-firmware-XXXXXX",
                                                &temporary_path, &error);
          g_assert_no_error (error);
          g_assert_cmpint (sensor.firmware_fd, >=, 0);
          g_assert_cmpint (g_unlink (temporary_path), ==, 0);
          g_assert_cmpint (write (sensor.firmware_fd, bytes, sensor.firmware_file_size), ==,
                           sensor.firmware_file_size);
          g_assert_cmpint (lseek (sensor.firmware_fd, 0, SEEK_SET), ==, 0);
          return sensor.firmware_fd;
        }
      errno = ENOENT;
      return -1;
    }
  g_assert_cmpstr (path, ==, "/mock/fte3600-spi");
  if (sensor.fail_claim)
    {
      errno = EBUSY;
      return -1;
    }
  sensor.claimed = TRUE;
  sensor.claims++;
  g_assert_cmpint (sensor.spi_fd, ==, -1);
  sensor.spi_fd = dup (sensor.irq_pipe[0]);
  g_assert_cmpint (sensor.spi_fd, >=, 0);
  sensor.opens++;
  return sensor.spi_fd;
}

int
__wrap_open64 (const char *path, int flags, ...)
{
  return __wrap_open (path, flags);
}

int
__wrap_close (int fd)
{
  if (fd == sensor.firmware_fd)
    {
      sensor.firmware_fd = -1;
      return __real_close (fd);
    }
  g_assert_cmpint (fd, ==, sensor.spi_fd);
  /* Kernel close deasserts reset even after an interrupted operation. */
  sensor.reset_asserted = FALSE;
  sensor.claimed = FALSE;
  sensor.releases++;
  sensor.spi_fd = -1;
  sensor.closes++;
  return __real_close (fd);
}

gboolean
__wrap_g_file_get_contents (const gchar *path, gchar **contents,
                            gsize *length, GError **error)
{
  /* DMI/GPIO sysfs lookup must never return to the runtime path. */
  g_assert_false (g_str_has_prefix (path, "/sys/class/dmi/"));
  g_assert_false (g_str_has_prefix (path, "/mock/gpio"));
  return __real_g_file_get_contents (path, contents, length, error);
}

static int
set_reset (guint32 value)
{
  ResetEvent event = { value, g_get_monotonic_time () };

  g_array_append_val (sensor.reset_events, event);
  g_assert_true (sensor.claimed);
  if (value)
    {
      g_assert_true (sensor.hardware_recovery || sensor.rom_probe || sensor.special_wake);
      sensor.hardware_asserts++;
      sensor.special_resets += sensor.special_wake;
      if (sensor.fail_hardware_reset)
        {
          errno = EIO;
          return -1;
        }
      sensor.reset_asserted = TRUE;
      if (sensor.cancel_hardware_reset)
        g_cancellable_cancel (sensor.cancellable);
    }
  else if (sensor.reset_asserted)
    {
      sensor.hardware_deasserts++;
      sensor.reset_asserted = FALSE;
      sensor.otp_enabled = FALSE;
      sensor.special_wake = FALSE;
      sensor.special_mode = 0;
      if (!sensor.persistent_cold)
        {
          sensor.cold_start = FALSE;
          sensor.registers[FT9361_REG_SENSOR_ID_HIGH] = sensor.model->width;
          sensor.registers[FT9361_REG_SENSOR_ID_LOW] = sensor.model->height;
        }
    }
  return 0;
}

static gboolean
deliver_irq (gpointer unused)
{
  sensor.irq_source = 0;
  if (sensor.irq_action == CANCEL_WAIT)
    {
      g_cancellable_cancel (sensor.cancellable);
    }
  else
    {
      g_mutex_lock (&sensor.lock);
      sensor.armed = FALSE;
      sensor.finger_ready = TRUE;
      g_mutex_unlock (&sensor.lock);
      g_assert_cmpint (write (sensor.irq_pipe[1], "x", 1), ==, 1);
    }
  return G_SOURCE_REMOVE;
}

GSource *
__wrap_g_unix_fd_source_new (gint fd, GIOCondition condition)
{
  g_assert_cmpint (fd, ==, sensor.spi_fd);
  if (sensor.armed && sensor.irq_source == 0)
    sensor.irq_source = g_idle_add (deliver_irq, NULL);
  return __real_g_unix_fd_source_new (fd, condition);
}

/* The fake only exposes geometry after this chip's actual wake handshake.
 * It never borrows production timing constants or returns an identity merely
 * because the driver's state machine expects one. */
static int
sleeping_legacy_transfer (const guint8 *tx, guint8 *rx, guint length)
{
  const LegacyWakeFixture *fixture = legacy_wake_fixture;
  gboolean high = !!(sensor.spi_mode & SPI_CS_HIGH);
  gboolean selected = high == fixture->required_high;
  guint8 previous = sensor.wake_previous_opcode;
  gint64 now = g_get_monotonic_time ();

  if (sensor.wake_fault_injected)
    {
      /* Cancellation cannot split the two-command wake handshake. Nothing
       * after its final command may reach the transport. */
      g_assert_cmpint (fixture->fault, ==, LEGACY_WAKE_CANCEL);
      g_assert_cmpuint (tx[0], ==, 0x70);
      g_assert_cmpuint (sensor.wake_commands[high] % 2, ==, 1);
    }
  sensor.wake_previous_opcode = tx[0];
  if (rx)
    memset (rx, 0, length);
  switch (tx[0])
    {
    case 0x70:
      g_assert_cmpuint (length, ==, 1);
      if (!sensor.wake_commands[0] && !sensor.wake_commands[1])
        sensor.resets_before_wake = sensor.hardware_asserts;
      sensor.wake_commands[high]++;
      g_assert_cmpuint (sensor.wake_commands[high], <=, 12);
      if (sensor.wake_commands[high] % 2 == 0)
        {
          g_assert_cmpuint (previous, ==, 0x70);
          g_assert_cmpint (now - sensor.wake_command_time[high], >=, 5000);
        }
      else if (sensor.wake_commands[high] > 1)
        {
          g_assert_cmpuint (previous, ==, 0x10);
          g_assert_cmpint (now - sensor.wake_status_time[high], >=, 5000);
        }
      sensor.wake_command_time[high] = now;
      if (selected && sensor.wake_commands[high] == fixture->fault_command)
        {
          sensor.wake_fault_injected = TRUE;
          if (fixture->fault == LEGACY_WAKE_CANCEL)
            {
              g_cancellable_cancel (sensor.cancellable);
            }
          else if (fixture->fault == LEGACY_WAKE_SHORT)
            {
              return length - 1;
            }
          else
            {
              g_assert_cmpint (fixture->fault, ==, LEGACY_WAKE_IO_ERROR);
              errno = EIO;
              return -1;
            }
        }
      return length;

    case 0x10:
      g_assert_nonnull (rx);
      if (tx[2] == 0x20)
        {
          g_assert_cmpuint (length, ==, 6);
          g_assert_cmpuint (previous, ==, sensor.wake_status_reads[high] ? 0x70 : 0x10);
          if (!sensor.wake_status_reads[high])
            g_assert_cmpuint (sensor.wake_early_geometry_reads[high], ==, 2);
          g_assert_cmpint (now - sensor.wake_command_time[high], >=, 2000);
          g_assert_cmpuint (sensor.wake_commands[high], ==, 2 * (sensor.wake_status_reads[high] + 1));
          sensor.wake_status_reads[high]++;
          sensor.wake_status_time[high] = now;
          if (selected && fixture->fault != LEGACY_WAKE_EXHAUSTED &&
              sensor.wake_status_reads[high] >= fixture->ready_attempt)
            {
              rx[4] = 0xa5;
              rx[5] = 0x5a;
              sensor.wake_ready_time = now;
            }
        }
      else
        {
          g_assert_true (tx[2] == 0x14 || tx[2] == 0x15);
          g_assert_cmpuint (length, ==, 5);
          if (selected && sensor.wake_ready_time)
            {
              g_assert_cmpint (now - sensor.wake_ready_time, >=, 350000);
              sensor.wake_geometry_reads++;
              g_assert_cmpuint (tx[2], ==, sensor.wake_geometry_reads % 2 ? 0x14 : 0x15);
              rx[4] = tx[2] == 0x14 ? sensor.model->width : sensor.model->height;
              if (fixture->fault == LEGACY_WAKE_UNSTABLE && sensor.wake_geometry_reads > 2)
                rx[4] = 0x7f;
            }
          else if (sensor.wake_commands[high])
            {
              /* The immediate A1 fast path must remain blank for a sensor
               * which needs MCU readiness and the later settling interval. */
              g_assert_cmpuint (sensor.wake_commands[high], ==, 2);
              g_assert_cmpint (now - sensor.wake_command_time[high], >=, 2000);
              sensor.wake_early_geometry_reads[high]++;
              g_assert_cmpuint (sensor.wake_early_geometry_reads[high], <=, 2);
              g_assert_cmpuint (tx[2], ==, sensor.wake_early_geometry_reads[high] % 2 ? 0x14 : 0x15);
              if (selected && fixture->fault == LEGACY_WAKE_UNKNOWN)
                {
                  sensor.wake_geometry_reads++;
                  rx[4] = 0x7f;
                }
            }
          else
            {
              sensor.sleeping_geometry_reads++;
            }
        }
      return length;

    case 0x90:
      /* Only a deliberately exhausted wake test may reach ROM. Fail its
       * first read so the test cannot accidentally obtain a ROM identity. */
      g_assert_cmpint (fixture->fault, ==, LEGACY_WAKE_EXHAUSTED);
      sensor.wake_rom_queries++;
      errno = EIO;
      return -1;

    case 0xff:
    case 0x91:
    case 0x5a:
    case 0xa5:
    case 0xc0:
    case 0x08:
    case 0x09:
    case 0x04:
      /* Other application/factory protocols do not identify this device. */
      if (sensor.wake_commands[0] || sensor.wake_commands[1])
        g_assert_true (fixture->fault == LEGACY_WAKE_UNKNOWN ||
                       fixture->fault == LEGACY_WAKE_EXHAUSTED);
      return length;

    default:
      g_assert_not_reached ();
    }
}

int
__wrap_ioctl (int fd, unsigned long operation, ...)
{
  struct spi_ioc_transfer *transfer;
  const guint8 *tx;
  guint8 *rx;
  va_list args;
  int result;

  g_assert_cmpint (fd, ==, sensor.spi_fd);
  sensor.ioctl_count++;
  va_start (args, operation);
  gpointer argument = va_arg (args, gpointer);
  va_end (args);
  if (operation == FTE3600_IOC_GET_INFO)
    {
      struct fte3600_bridge_info *info = argument;
      if (sensor.fail_config)
        {
          errno = EIO;
          return -1;
        }
      *info = (struct fte3600_bridge_info){
        .abi_version = sensor.bad_abi ? 999 : FTE3600_BRIDGE_ABI,
        .max_transfer = sensor.buffer_size,
        .speed_hz = FTE3600_SPI_SPEED_HZ,
        .bits_per_word = 8,
        .mode = sensor.bad_mode ? SPI_MODE_3 : sensor.spi_mode,
        .capabilities = ((probe_fixture && probe_fixture->cs_control) ||
                         (legacy_wake_fixture && legacy_wake_fixture->cs_control)) ? FTE3600_BRIDGE_CAP_CS_POLARITY : 0,
      };
      return 0;
    }
  if (operation == FTE3600_IOC_SET_RESET)
    return set_reset (*(guint32 *) argument);
  if (operation == FTE3600_IOC_SET_CS_POLARITY)
    {
      g_assert_true ((probe_fixture && probe_fixture->cs_control) ||
                     (legacy_wake_fixture && legacy_wake_fixture->cs_control));
      g_assert_cmpuint (*(guint32 *) argument, <=, 1);
      sensor.cs_changes++;
      if (probe_fixture && probe_fixture->fail_cs)
        {
          errno = EIO;
          return -1;
        }
      sensor.spi_mode = *(guint32 *) argument ? SPI_CS_HIGH : SPI_MODE_0;
      return 0;
    }
  if (operation == FTE3600_IOC_GET_EVENTS)
    {
      struct pollfd event = { .fd = fd, .events = POLLIN };
      char byte;
      *(guint32 *) argument = 0;
      if (poll (&event, 1, 0) > 0)
        {
          g_assert_cmpint (read (fd, &byte, 1), ==, 1);
          *(guint32 *) argument = 1;
        }
      return 0;
    }
  g_assert_cmpuint (operation, ==, SPI_IOC_MESSAGE (1));
  transfer = argument;
  tx = (const guint8 *) (guintptr) transfer->tx_buf;
  rx = (guint8 *) (guintptr) transfer->rx_buf;
  result = transfer->len;
  g_mutex_lock (&sensor.lock);
  sensor.spi_transactions++;
  if (tx[0] == 0x5a)
    sensor.special_wake = TRUE;
  if (tx[0] == 0x90)
    sensor.probe_started = TRUE;
  if (probe_fixture && tx[0] == 0x70)
    sensor.resets++;
  if (legacy_wake_fixture)
    {
      result = sleeping_legacy_transfer (tx, rx, transfer->len);
      g_mutex_unlock (&sensor.lock);
      return result;
    }
  if (probe_fixture)
    {
      guint16 id = probe_fixture->id;
      gboolean selected = !!(sensor.spi_mode & SPI_CS_HIGH) == probe_fixture->required_high;

      if (rx)
        memset (rx, 0, transfer->len);
      if (selected && probe_fixture->mode_required && tx[0] == 0x09 && tx[2] == 0xc6)
        sensor.special_mode = tx[3];
      if (selected && tx[0] == 0x08 && tx[2] == 0xc6)
        rx[4] = sensor.special_mode;
      if (selected && (!probe_fixture->mode_required || sensor.special_mode == 1) &&
          tx[0] == 0x04 && tx[2] == 0x9a && tx[3] == 0x8b &&
          ((id == 0x9362 && transfer->len == 12 && tx[5] == 1) ||
           (sensor.special_mode == 1 && transfer->len == 12 && tx[5] == 1) ||
           (id != 0x9362 && id != 0x9368 && transfer->len == 10 && tx[5] == 0)))
        {
          sensor.special_reads++;
          if (probe_fixture->unstable && sensor.special_reads > 1)
            id = 0;
          rx[6] = id >> 8;
          rx[7] = id;
          if (transfer->len == 10)
            {
              guint16 crc = fpi_fte3600_ft93xx_crc16 (rx + 6, 2);
              rx[8] = crc >> 8;
              rx[9] = crc ^ (probe_fixture->corrupt_crc ? 1 : 0);
            }
        }
      else if (selected && tx[0] == 0x04 && tx[2] == 0x98 && tx[3] == 0x16)
        {
          rx[6] = probe_fixture->variant >> 8;
          rx[7] = probe_fixture->variant;
        }
      else if (selected && id == 0x9368 && tx[0] == 0x91 && transfer->len == 39)
        {
          sensor.special_reads++;
          rx[26] = 0x93;
          rx[27] = 0x68;
          rx[28] = 0x13;
          rx[30] = 64;
          rx[31] = probe_fixture->unstable && sensor.special_reads > 1 ? 0 : 80;
        }
      g_mutex_unlock (&sensor.lock);
      return result;
    }
  if (sensor.rom_probe && sensor.probe_started && tx[0] != 0x70 && tx[0] != 0x10 && tx[0] != 0x11 &&
      tx[0] != 0x5a && tx[0] != 0xa5 && tx[0] != 0xc0 &&
      !(tx[0] == 0x08 && (tx[2] == 0x80 || tx[2] == 0xc6)) &&
      !(tx[0] == 0x09 && tx[2] == 0xc6) &&
      tx[0] != 0xff && tx[0] != 0x91 && !(tx[0] == 0x04 && tx[2] == 0x9a))
    {
      sensor.probe_packets++;
      if (sensor.cancel_probe_packet == sensor.probe_packets)
        g_cancellable_cancel (sensor.cancellable);
      if (sensor.fail_probe_packet == sensor.probe_packets)
        {
          g_mutex_unlock (&sensor.lock);
          errno = EIO;
          return -1;
        }
    }
  switch (tx[0])
    {
    case 0xff:
    case 0x91:
    case 0x5a:
    case 0xa5:
    case 0xc0:
      /* These legacy mock sensors do not speak the FT9368 protocol. */
      g_assert_nonnull (rx);
      memset (rx, 0, transfer->len);
      break;

    case 0x70:
      /* ROM fixtures have no running application. A failed special-family
       * negotiation may pulse GPIO reset, but its cleanup must not fabricate
       * application readiness before the intended ROM identification. */
      if (sensor.rom_probe && !sensor.probe_started)
        {
          sensor.discovery_wakes++;
          g_assert_cmpuint (sensor.discovery_wakes, <=, 12);
          break;
        }
      {
        gint64 when = g_get_monotonic_time ();

        g_array_append_val (sensor.soft_reset_times, when);
        if (sensor.inactive)
          {
            sensor.inactive_wake_commands++;
            if (sensor.inactive_wake_commands == 1)
              sensor.wake_first = when;
            else
              g_assert_cmpint (when - sensor.wake_first, >=, 5000);
            sensor.wake_last = when;
            if (sensor.cancel_wake)
              g_cancellable_cancel (sensor.cancellable);
            if (sensor.fail_wake_at == sensor.inactive_wake_commands)
              {
                result = -1;
                break;
              }
          }
      }
      sensor.resets++;
      sensor.armed = FALSE;
      sensor.finger_ready = FALSE;
      if (sensor.images && sensor.fail_cleanup_status)
        sensor.cleanup_started = TRUE;
      if (sensor.fail_reset)
        result = -1;
      break;

    case 0x11:
      g_assert_cmpuint (transfer->len, ==, 5);
      sensor.registers[tx[2]] = tx[3];
      if (tx[2] == 0x47 || tx[2] == 0x76)
        {
          g_assert_cmpuint (tx[2], ==, sensor.model->mode_register);
          sensor.mode_writes++;
        }
      if (!sensor.model->cold_recovery)
        g_assert_true (tx[2] != 0x22 && tx[2] != 0x23);
      if (tx[2] == FT9361_REG_CAPTURE_START && tx[3] == 1)
        {
          g_assert_cmpuint (sensor.registers[sensor.model->mode_register], ==, 1);
          sensor.armed = TRUE;
        }
      if (tx[2] == FT9361_REG_QUICK_TRIGGER && tx[3] == 1)
        {
          g_assert_cmpuint (sensor.registers[sensor.model->mode_register], ==, 2);
          sensor.armed = TRUE;
        }
      break;

    case 0x10:
      g_assert_nonnull (rx);
      memset (rx, 0, transfer->len);
      if (sensor.inactive)
        {
          if (sensor.inactive_wake_commands < 2)
            {
              if (stale_wake_fixture)
                memset (rx, 1, transfer->len);
              break;
            }
          g_assert_cmpint (g_get_monotonic_time () - sensor.wake_last, >=, 2000);
          sensor.inactive = FALSE;
        }
      if (wake_fixture && (tx[2] == FT9361_REG_SENSOR_ID_HIGH || tx[2] == FT9361_REG_SENSOR_ID_LOW))
        {
          sensor.awake_id_reads++;
          if (sensor.unstable_wake && sensor.awake_id_reads > 2)
            break;
        }
      if (tx[2] == FT9361_REG_MCU_STATUS)
        {
          if (sensor.rom_probe && !sensor.probe_started)
            {
              /* Keep the six wake attempts unready. Only the separate ROM
               * scenario can make its application available later. */
            }
          else if (sensor.cleanup_started && sensor.fail_cleanup_status)
            {
              if (sensor.fail_cleanup_status == 2)
                result = -1;
            }
          else if (!sensor.armed && !sensor.cold_start)
            {
              rx[4] = 0xa5;
              rx[5] = 0x5a;
            }
        }
      else if (tx[2] == FT9361_REG_FINGER_STATUS)
        {
          rx[4] = sensor.finger_ready ? 1 : 0;
        }
      else if (tx[2] == FT9361_REG_SENSOR_ID_HIGH && sensor.bad_id)
        {
          rx[4] = 0xff;
        }
      else
        {
          rx[4] = sensor.registers[tx[2]];
        }
      break;

    case 0x90:
      g_assert_cmpuint (transfer->len, ==, 3);
      rx[2] = sensor.boot_reply;
      break;

    case 0x55:
      g_assert_cmpuint (transfer->len, ==, 2);
      g_assert_cmpuint (tx[1], ==, 0xaa);
      break;

    case 0x06:
      g_assert_cmpuint (transfer->len, ==, 3);
      g_assert_cmpuint (tx[1], ==, 0xf9);
      break;

    case 0x05:
      {
        const guint8 query[] = { 5, 0xfa, 0x85, 0xc0, 0, 4, 0x11, 0xee, 2, 0, 0 };
        /* A ROM query is allowed. A blind firmware upload is a test failure. */
        g_assert_cmpmem (tx, transfer->len, query, sizeof (query));
        break;
      }

    case 0x09:
      g_assert_cmpuint (tx[1], ==, 0xf6);
      if (tx[2] == 0xf4)
        sensor.otp_enabled = tx[3] & 1;
      break;

    case 0x08:
      g_assert_cmpuint (tx[1], ==, 0xf7);
      if (transfer->len == 4)
        {
          rx[3] = tx[2] == 0xfe ? sensor.boot38_id : tx[2] == 0xf3 ? sensor.otp : 0;
          break;
        }
      if (tx[2] == 0xf3)
        {
          g_assert_true (sensor.otp_enabled);
          rx[4] = sensor.otp;
        }
      break;

    case 0x04:
      if (tx[2] == 0x9a)
        {
          g_assert_nonnull (rx);
          memset (rx, 0, transfer->len);
          break;
        }
      if (transfer->len == 8 && tx[2] == 0x85 && tx[3] == 0xc0)
        {
          rx[6] = sensor.rom_family >> 8;
          rx[7] = sensor.rom_family & 0xff;
          break;
        }
      sensor.images++;
      g_assert_cmpuint (transfer->len, ==, (guint) sensor.model->width * sensor.model->height + 8);
      g_assert_cmpuint (tx[1], ==, 0xfb);
      g_assert_cmpuint (tx[2], ==, 0x34);
      g_assert_cmpuint (tx[3], ==, 0);
      g_assert_cmpuint (((guint) tx[4] << 8) | tx[5], ==, transfer->len);
      g_assert_nonnull (rx);
      memset (rx, 0xe7, 8);
      if (sensor.frames != NULL)
        g_assert_cmpuint (sensor.next_frame, <, sensor.n_frames);
      for (guint i = 0; i < (guint) sensor.model->width * sensor.model->height; i++)
        rx[FT9361_CAPTURE_DATA_OFFSET + i] = sensor.frames != NULL ?
                                             (guint8) ~sensor.frames[sensor.next_frame * sensor.model->width * sensor.model->height + i] :
                                             (i * 37 + 11) & 0xff;
      sensor.next_frame++;
      if (sensor.fail_image)
        result = -1;
      else if (sensor.short_image)
        result--;
      if (sensor.cancel_image)
        g_cancellable_cancel (sensor.cancellable);
      break;

    default:
      g_assert_not_reached ();
    }
  g_mutex_unlock (&sensor.lock);
  if (result < 0)
    errno = EIO;
  return result;
}

typedef struct
{
  gboolean complete;
  GError  *error;
} DeviceInit;

static void
init_complete (GObject *object, GAsyncResult *result, gpointer data)
{
  DeviceInit *initialized = data;
  gboolean success;

  success = g_async_initable_init_finish (G_ASYNC_INITABLE (object), result,
                                          &initialized->error);
  g_assert_cmpint (success, ==, initialized->error == NULL);
  initialized->complete = TRUE;
}

static FpDevice *
new_device_for_model_checked (guint model, guint32 buffer_size, GError **error)
{
  DeviceInit initialized = { 0 };
  FpDevice *device;

  memset (&sensor, 0, sizeof (sensor));
  g_mutex_init (&sensor.lock);
  sensor.reset_events = g_array_new (FALSE, FALSE, sizeof (ResetEvent));
  sensor.soft_reset_times = g_array_new (FALSE, FALSE, sizeof (gint64));
  g_assert_cmpuint (model, <, G_N_ELEMENTS (models));
  sensor.model = &models[model];
  sensor.buffer_size = buffer_size;
  sensor.rom_family = 0x95a8;
  sensor.otp = sensor.model->otp;
  sensor.spi_fd = -1;
  sensor.firmware_fd = -1;
  sensor.inactive = wake_fixture;
  sensor.registers[FT9361_REG_SENSOR_ID_HIGH] = sensor.model->width;
  sensor.registers[FT9361_REG_SENSOR_ID_LOW] = sensor.model->height;
  sensor.registers[FT9361_REG_FW_VERSION] = sensor.model->firmware_version;
  sensor.registers[FT9361_REG_AGC_VERSION] = sensor.model->agc_version;
  if (probe_fixture)
    {
      sensor.registers[FT9361_REG_SENSOR_ID_HIGH] = 0;
      sensor.registers[FT9361_REG_SENSOR_ID_LOW] = 0;
    }
  if (boot38_fixture)
    {
      sensor.rom_probe = TRUE;
      sensor.cold_start = TRUE;
      sensor.persistent_cold = TRUE;
      sensor.boot_reply = 0xef;
      sensor.boot38_id = 2;
      sensor.registers[FT9361_REG_SENSOR_ID_HIGH] = 0;
      sensor.registers[FT9361_REG_SENSOR_ID_LOW] = 0;
    }
  g_assert_cmpint (pipe (sensor.irq_pipe), ==, 0);
  sensor.cancellable = g_cancellable_new ();
  device = g_object_new (fpi_device_fte3600_get_type (),
                         "fpi-udev-data-spidev", "/mock/fte3600-spi", NULL);
  g_async_initable_init_async (G_ASYNC_INITABLE (device), G_PRIORITY_DEFAULT,
                               legacy_wake_fixture ? sensor.cancellable : NULL,
                               init_complete, &initialized);
  while (!initialized.complete)
    g_main_context_iteration (NULL, TRUE);
  if (initialized.error)
    g_propagate_error (error, initialized.error);
  return device;
}

static FpDevice *
new_device_checked (GError **error)
{
  return new_device_for_model_checked (0, FTE3600_BRIDGE_MAX_TRANSFER, error);
}

static FpDevice *
new_device_for_model (guint model)
{
  g_autoptr(GError) error = NULL;
  FpDevice *device = new_device_for_model_checked (model, FTE3600_BRIDGE_MAX_TRANSFER, &error);

  g_assert_no_error (error);
  return device;
}

static FpDevice *
new_device (void)
{
  g_autoptr(GError) error = NULL;
  FpDevice *device = new_device_checked (&error);

  g_assert_no_error (error);
  return device;
}

static void
finish_device (FpDevice *device)
{
  GError *error = NULL;

  if (fp_device_is_open (device))
    {
      g_assert_true (fp_device_close_sync (device, NULL, &error));
      g_assert_no_error (error);
    }
  g_object_unref (device);
  g_assert_cmpint (sensor.firmware_fd, ==, -1);
  g_assert_cmpint (sensor.spi_fd, ==, -1);
  g_assert_false (sensor.claimed);
  g_assert_cmpuint (sensor.opens, ==, sensor.closes);
  g_assert_cmpuint (sensor.claims, ==, sensor.releases);
  g_assert_cmpuint (sensor.irq_source, ==, 0);
  __real_close (sensor.irq_pipe[0]);
  __real_close (sensor.irq_pipe[1]);
  g_clear_object (&sensor.cancellable);
  g_array_unref (sensor.reset_events);
  g_array_unref (sensor.soft_reset_times);
  g_mutex_clear (&sensor.lock);
  probe_fixture = NULL;
  boot38_fixture = FALSE;
  legacy_wake_fixture = NULL;
}

static void
test_sleeping_legacy_discovery (gconstpointer data)
{
  const LegacyWakeFixture *fixture = data;

  g_autoptr(GError) error = NULL;
  FpDevice *device;
  guint expected_commands = fixture->fault == LEGACY_WAKE_CANCEL ? 2 :
                            fixture->fault_command ? fixture->fault_command :
                            fixture->fault == LEGACY_WAKE_EXHAUSTED ? 12 : 2 * fixture->ready_attempt;

  legacy_wake_fixture = fixture;
  device = new_device_for_model_checked (fixture->model, FTE3600_BRIDGE_MAX_TRANSFER, &error);
  switch (fixture->fault)
    {
    case LEGACY_WAKE_OK:
      g_assert_no_error (error);
      g_assert_nonnull (strstr (fp_device_get_name (device), models[fixture->model].name));
      g_assert_cmpuint (sensor.wake_geometry_reads, ==, 4);
      g_assert_cmpuint (sensor.spi_mode, ==, fixture->required_high ? SPI_CS_HIGH : 0);
      break;

    case LEGACY_WAKE_UNKNOWN:
      g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
      g_assert_cmpuint (sensor.wake_geometry_reads, ==, 2);
      break;

    case LEGACY_WAKE_UNSTABLE:
      g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
      g_assert_cmpuint (sensor.wake_geometry_reads, ==, 4);
      break;

    case LEGACY_WAKE_IO_ERROR:
    case LEGACY_WAKE_EXHAUSTED:
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
      g_assert_cmpuint (sensor.wake_geometry_reads, ==, 0);
      break;

    case LEGACY_WAKE_SHORT:
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT);
      g_assert_cmpuint (sensor.wake_geometry_reads, ==, 0);
      break;

    case LEGACY_WAKE_CANCEL:
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      g_assert_cmpuint (sensor.wake_geometry_reads, ==, 0);
      break;
    }
  if (fixture->fault != LEGACY_WAKE_OK)
    g_assert_cmpuint (sensor.spi_mode, ==, SPI_MODE_0);
  g_assert_cmpuint (sensor.wake_commands[fixture->required_high], ==, expected_commands);
  g_assert_cmpuint (sensor.wake_commands[!fixture->required_high], ==,
                    fixture->required_high || (fixture->cs_control && fixture->fault == LEGACY_WAKE_EXHAUSTED) ? 12 : 0);
  g_assert_cmpuint (sensor.wake_status_reads[fixture->required_high], ==,
                    fixture->fault_command || fixture->fault == LEGACY_WAKE_UNKNOWN ? 0 : expected_commands / 2);
  g_assert_cmpuint (sensor.sleeping_geometry_reads, ==, fixture->cs_control ? 4 : 2);
  g_assert_cmpuint (sensor.wake_early_geometry_reads[fixture->required_high], ==,
                    fixture->fault_command ? 0 : 2);
  g_assert_cmpuint (sensor.wake_early_geometry_reads[!fixture->required_high], ==,
                    fixture->required_high || (fixture->cs_control && fixture->fault == LEGACY_WAKE_EXHAUSTED) ? 2 : 0);
  if (fixture->fault == LEGACY_WAKE_CANCEL)
    g_assert_cmpint (g_get_monotonic_time () - sensor.wake_command_time[fixture->required_high], >=, 2000);
  g_assert_cmpuint (sensor.wake_rom_queries, ==, fixture->fault == LEGACY_WAKE_EXHAUSTED ? 1 : 0);
  /* Wake runs before the factory-mode negotiation. Only an unknown or fully
   * exhausted application can reach that later negotiation and its cleanup. */
  g_assert_cmpuint (sensor.resets_before_wake, ==, 0);
  g_assert_cmpuint (sensor.hardware_asserts, ==,
                    fixture->fault == LEGACY_WAKE_UNKNOWN || fixture->fault == LEGACY_WAKE_EXHAUSTED ?
                    (fixture->cs_control ? 2 : 1) : 0);
  g_assert_cmpuint (sensor.hardware_deasserts, ==, sensor.hardware_asserts);
  g_assert_false (sensor.reset_asserted);
  g_assert_cmpuint (sensor.firmware_opens, ==, 0);
  g_assert_cmpuint (sensor.images, ==, 0);
  finish_device (device);
}

static void
test_boot38_cold_enumeration (void)
{
  g_autoptr(GError) error = NULL;
  FpDevice *device;

  boot38_fixture = TRUE;
  device = new_device_for_model_checked (3, FTE3600_BRIDGE_MAX_TRANSFER, &error);
  g_assert_no_error (error);
  g_assert_nonnull (strstr (fp_device_get_name (device), "FT9536"));
  g_assert_cmpuint (sensor.firmware_opens, ==, 0);
  g_assert_cmpint (fp_device_has_feature (device, FP_DEVICE_FEATURE_VERIFY), ==,
                   FTE3600_ENABLE_PERSONAL_AUTH);
  g_test_expect_message ("libfprint-fte3600", G_LOG_LEVEL_WARNING,
                         "*Sensor reset after open failure also failed:*");
  g_assert_false (fp_device_open_sync (device, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_test_assert_expected_messages ();
  g_assert_cmpuint (sensor.firmware_opens, ==, 1);
  g_assert_false (sensor.reset_asserted);
  finish_device (device);
}

static void
test_special_discovery (gconstpointer data)
{
  const ProbeFixture *fixture = data;

  g_autoptr(GError) error = NULL;
  FpDevice *device;

  probe_fixture = fixture;
  device = new_device_for_model_checked (0, fixture->limit ? fixture->limit : FTE3600_BRIDGE_MAX_TRANSFER, &error);
  if (fixture->success)
    {
      g_assert_no_error (error);
      g_assert_nonnull (strstr (fp_device_get_name (device), fixture->name));
      g_assert_cmpuint (sensor.special_reads, ==, 2);
      g_assert_cmpuint (sensor.cs_changes, ==, fixture->required_high ? (fixture->mode_required ? 5 : 1) : 0);
      g_assert_cmpuint (sensor.resets, ==, fixture->mode_required ? (fixture->cs_control ? 24 : 12) : 0);
      g_assert_cmpuint (sensor.spi_mode, ==, fixture->required_high ? SPI_CS_HIGH : 0);
      g_assert_true (fp_device_has_feature (device, FP_DEVICE_FEATURE_CAPTURE));
      g_assert_cmpint (fp_device_has_feature (device, FP_DEVICE_FEATURE_VERIFY), ==,
                       FTE3600_ENABLE_PERSONAL_AUTH);
      if (fixture->exercise_open_failure)
        {
          /* The fake accepts identity but never enters the requested SPI/AFE
           * mode. Exercise core-owned error cleanup around a backend-owned
           * reset SSM with its own private context. */
          g_test_expect_message ("libfprint-fte3600", G_LOG_LEVEL_WARNING,
                                 "*Sensor reset after open failure also failed:*");
          g_assert_false (fp_device_open_sync (device, NULL, &error));
          g_assert_nonnull (error);
          g_test_assert_expected_messages ();
        }
    }
  else
    {
      g_assert_nonnull (error);
      g_assert_cmpuint (sensor.spi_mode, ==, SPI_MODE_0);
    }
  g_assert_cmpuint (sensor.firmware_opens, ==, 0);
  /* A known ID rejection and repeated-ID check must not pulse reset. */
  if (fixture->id && (!fixture->required_high || fixture->cs_control))
    g_assert_cmpuint (sensor.hardware_asserts, ==, fixture->mode_required && fixture->required_high ? 1 : 0);
  finish_device (device);
}

static void
open_device (FpDevice *device)
{
  GError *error = NULL;

  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (fp_device_is_open (device));
}

static void
test_inactive_discovery (gconstpointer data)
{
  guint scenario = GPOINTER_TO_UINT (data);

  g_autoptr(GError) error = NULL;
  FpDevice *device;

  wake_fixture = TRUE;
  stale_wake_fixture = scenario == 5;
  device = new_device_checked (&error);
  g_assert_no_error (error);
  g_assert_nonnull (strstr (fp_device_get_name (device), "FT9361"));
  g_assert_cmpuint (sensor.inactive_wake_commands, ==, 2);
  g_assert_cmpuint (sensor.awake_id_reads, ==, 4);
  g_assert_cmpuint (sensor.firmware_opens, ==, 0);
  g_assert_cmpuint (sensor.hardware_asserts, ==, 0);

  /* Enumeration's successful identity must not hide an inactive device at
   * the next open, nor make a failed/unstable wake authorize ROM recovery. */
  sensor.inactive = TRUE;
  sensor.inactive_wake_commands = 0;
  sensor.awake_id_reads = 0;
  sensor.fail_wake_at = scenario == 1 ? 1 : scenario == 2 ? 2 : 0;
  sensor.unstable_wake = scenario == 3;
  sensor.cancel_wake = scenario == 4;
  if (scenario == 0 || scenario == 5)
    {
      open_device (device);
      g_assert_cmpuint (sensor.inactive_wake_commands, ==, 2);
      g_assert_true (fp_device_close_sync (device, NULL, &error));
      g_assert_no_error (error);
      sensor.inactive = TRUE;
      sensor.inactive_wake_commands = 0;
      sensor.awake_id_reads = 0;
      open_device (device);
      g_assert_cmpuint (sensor.inactive_wake_commands, ==, 2);
    }
  else
    {
      g_assert_false (fp_device_open_sync (device, sensor.cancellable, &error));
      g_assert_nonnull (error);
      if (scenario == 3)
        g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
      if (scenario == 4)
        {
          g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
          g_assert_cmpuint (sensor.inactive_wake_commands, ==, 2);
        }
      g_assert_false (fp_device_is_open (device));
    }
  g_assert_cmpuint (sensor.firmware_opens, ==, 0);
  g_assert_false (sensor.probe_started);
  finish_device (device);
  wake_fixture = FALSE;
  stale_wake_fixture = FALSE;
}

static void
test_capture_reopen (void)
{
  g_autoptr(GError) init_error = NULL;
  FpDevice *device = new_device_checked (&init_error);

  g_assert_no_error (init_error);

  for (guint round = 0; round < 2; round++)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(FpImage) image = NULL;
      const guint8 *pixels;
      gsize size;
      guint resets;

      open_device (device);
      image = fp_device_capture_sync (device, TRUE, NULL, &error);
      g_assert_no_error (error);
      g_assert_nonnull (image);
      g_assert_cmpuint (fp_image_get_width (image), ==, FT9361_IMAGE_WIDTH);
      g_assert_cmpuint (fp_image_get_height (image), ==, FT9361_IMAGE_HEIGHT);
      pixels = fp_image_get_data (image, &size);
      g_assert_cmpuint (size, ==, FT9361_IMAGE_SIZE);
      for (guint i = 0; i < size; i++)
        g_assert_cmpuint (pixels[i], ==, (guint8) ~((i * 37 + 11) & 0xff));
      g_assert_cmpuint (sensor.resets, ==, (round + 1) * 4);
      resets = sensor.resets;
      g_assert_true (fp_device_close_sync (device, NULL, &error));
      g_assert_no_error (error);
      /* Terminal cleanup already reset the sensor; close must not repeat it. */
      g_assert_cmpuint (sensor.resets, ==, resets);
      g_assert_false (sensor.claimed);
    }
  g_assert_cmpuint (sensor.images, ==, 2);
  finish_device (device);
}

static void
assert_image_matches_model (FpImage *image)
{
  const guint8 *pixels;
  gsize size;

  g_assert_nonnull (image);
  g_assert_cmpuint (fp_image_get_width (image), ==, sensor.model->width);
  g_assert_cmpuint (fp_image_get_height (image), ==, sensor.model->height);
  pixels = fp_image_get_data (image, &size);
  g_assert_cmpuint (size, ==, (guint) sensor.model->width * sensor.model->height);
  for (gsize i = 0; i < size; i++)
    g_assert_cmpuint (pixels[i], ==, (guint8) ~((i * 37 + 11) & 0xff));
}

static void
test_family_warm_capture (gconstpointer data)
{
  guint model = GPOINTER_TO_UINT (data);
  FpDevice *device = new_device_for_model (model);

  g_assert_nonnull (strstr (fp_device_get_name (device), models[model].name));
  g_assert_cmpint (fp_device_has_feature (device, FP_DEVICE_FEATURE_VERIFY), ==,
                   FTE3600_ENABLE_PERSONAL_AUTH);
  g_assert_cmpint (fp_device_get_nr_enroll_stages (device), ==,
                   FTE3600_ENABLE_PERSONAL_AUTH ? 8 : 0);
  for (guint round = 0; round < 2; round++)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(FpImage) image = NULL;

      open_device (device);
      image = fp_device_capture_sync (device, TRUE, NULL, &error);
      g_assert_no_error (error);
      assert_image_matches_model (image);
      g_assert_true (fp_device_close_sync (device, NULL, &error));
      g_assert_no_error (error);
    }
  g_assert_cmpuint (sensor.images, ==, 2);
  g_assert_cmpuint (sensor.mode_writes, >=, 2);
  g_assert_cmpuint (sensor.hardware_asserts, ==, 0);
  g_assert_cmpuint (sensor.firmware_opens, ==, 0);
  finish_device (device);
}

static void
test_family_cancel_and_reuse (gconstpointer data)
{
  FpDevice *device = new_device_for_model (GPOINTER_TO_UINT (data));

  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;

  open_device (device);
  sensor.irq_action = CANCEL_WAIT;
  image = fp_device_capture_sync (device, TRUE, sensor.cancellable, &error);
  g_assert_null (image);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint (sensor.images, ==, 0);
  g_clear_error (&error);
  g_cancellable_reset (sensor.cancellable);
  sensor.irq_action = DELIVER_FINGER;
  image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_no_error (error);
  assert_image_matches_model (image);
  g_assert_cmpuint (sensor.hardware_asserts, ==, 0);
  g_assert_cmpuint (sensor.firmware_opens, ==, 0);
  finish_device (device);
}

static void
test_ft9348_rom_and_firmware (gconstpointer data)
{
  guint scenario = GPOINTER_TO_UINT (data);
  FpDevice *device = new_device_for_model (1);

  g_autoptr(GError) error = NULL;

  sensor.rom_probe = TRUE;
  sensor.cold_start = TRUE;
  sensor.hardware_recovery = TRUE;
  if (scenario == 0)
    {
      sensor.registers[FT9361_REG_SENSOR_ID_HIGH] = 0;
      sensor.registers[FT9361_REG_SENSOR_ID_LOW] = 0;
      g_assert_true (fp_device_open_sync (device, NULL, &error));
      g_assert_no_error (error);
      g_assert_cmpuint (sensor.probe_packets, ==, 12);
      g_assert_cmpuint (sensor.firmware_opens, ==, 0);
    }
  else
    {
      sensor.persistent_cold = TRUE;
      if (scenario == 1)
        sensor.buffer_size = 9500; /* Fits 9224-byte capture, not 10319-byte firmware. */
      if (scenario == 3)
        sensor.firmware_file_size = 10312; /* Correct FT9348 size, wrong SHA256. */
      if (scenario == 4)
        sensor.firmware_file_size = 10396; /* FT9361-sized bytes cannot stand in for FT9348. */
      if (scenario != 1)
        g_test_expect_message ("libfprint-fte3600", G_LOG_LEVEL_WARNING,
                               "*firmware loading failed:*");
      g_test_expect_message ("libfprint-fte3600", G_LOG_LEVEL_WARNING,
                             "*Sensor reset after open failure also failed:*");
      g_assert_false (fp_device_open_sync (device, NULL, &error));
      if (scenario == 1)
        g_assert_error (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
      else if (scenario == 2)
        g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
      else
        g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
      g_test_assert_expected_messages ();
      g_assert_cmpuint (sensor.firmware_opens, ==, scenario == 1 ? 0 : 1);
    }
  g_assert_false (sensor.reset_asserted);
  g_assert_false (sensor.otp_enabled);
  finish_device (device);
}

static void
test_family_transfer_limit (gconstpointer data)
{
  guint scenario = GPOINTER_TO_UINT (data);
  guint model = scenario / 2;
  gboolean accepted = scenario % 2;
  guint32 limit = models[model].width * models[model].height + 8 - !accepted;

  g_autoptr(GError) error = NULL;
  FpDevice *device = new_device_for_model_checked (model, limit, &error);

  if (accepted)
    {
      g_autoptr(FpImage) image = NULL;

      g_assert_no_error (error);
      open_device (device);
      image = fp_device_capture_sync (device, TRUE, NULL, &error);
      g_assert_no_error (error);
      assert_image_matches_model (image);
    }
  else
    {
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
      g_assert_cmpuint (sensor.resets, ==, 0);
      g_assert_cmpuint (sensor.images, ==, 0);
    }
  g_assert_cmpuint (sensor.hardware_asserts, ==, 0);
  g_assert_cmpuint (sensor.firmware_opens, ==, 0);
  finish_device (device);
}

static void
test_probe_open_identity_change (gconstpointer data)
{
  guint model = GPOINTER_TO_UINT (data);
  guint changed_model = model ^ 1;
  FpDevice *device = new_device_for_model (model);

  g_autoptr(GError) error = NULL;
  guint transactions = sensor.spi_transactions;

  sensor.model = &models[changed_model];
  sensor.registers[FT9361_REG_SENSOR_ID_HIGH] = sensor.model->width;
  sensor.registers[FT9361_REG_SENSOR_ID_LOW] = sensor.model->height;
  sensor.registers[FT9361_REG_FW_VERSION] = sensor.model->firmware_version;
  sensor.registers[FT9361_REG_AGC_VERSION] = sensor.model->agc_version;
  g_assert_false (fp_device_open_sync (device, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
  g_assert_cmpuint (sensor.spi_transactions, ==, transactions + 4);
  g_assert_cmpuint (sensor.resets, ==, 0);
  g_assert_cmpuint (sensor.mode_writes, ==, 0);
  g_assert_cmpuint (sensor.hardware_asserts, ==, 0);
  g_assert_cmpuint (sensor.firmware_opens, ==, 0);
  finish_device (device);
}

static void
test_family_short_frame (gconstpointer data)
{
  FpDevice *device = new_device_for_model (GPOINTER_TO_UINT (data));

  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;

  open_device (device);
  sensor.short_image = TRUE;
  image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_null (image);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT);
  sensor.short_image = FALSE;
  g_clear_error (&error);
  image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_no_error (error);
  assert_image_matches_model (image);
  finish_device (device);
}

static void
test_cleanup_invalidates_session (gconstpointer data)
{
  guint scenario = GPOINTER_TO_UINT (data);
  FpDevice *device = new_device_for_model (scenario / 2);

  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;
  guint transactions, ioctls;

  open_device (device);
  sensor.fail_cleanup_status = 1 + scenario % 2;
  image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_null (image);
  if (sensor.fail_cleanup_status == 1)
    g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO);
  else
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  transactions = sensor.spi_transactions;
  ioctls = sensor.ioctl_count;

  g_clear_error (&error);
  sensor.fail_cleanup_status = 0;
  image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_null (image);
  g_assert_nonnull (error);
  g_assert_cmpuint (sensor.spi_transactions, ==, transactions);
  g_assert_cmpuint (sensor.ioctl_count, ==, ioctls);
  g_clear_error (&error);
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (sensor.spi_transactions, ==, transactions);
  g_assert_cmpuint (sensor.ioctl_count, ==, ioctls);

  open_device (device);
  image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_no_error (error);
  assert_image_matches_model (image);
  finish_device (device);
}

static void
test_legacy_reject_unready_application (gconstpointer data)
{
  guint scenario = GPOINTER_TO_UINT (data);
  guint model = 2 + scenario / 3;
  guint failure = scenario % 3;
  FpDevice *device = new_device_for_model (model);

  g_autoptr(GError) error = NULL;

  if (failure == 0)
    {
      sensor.registers[FT9361_REG_FW_VERSION] ^= 1;
    }
  else if (failure == 1)
    {
      sensor.registers[FT9361_REG_AGC_VERSION] ^= 1;
    }
  else
    {
      sensor.cold_start = TRUE;
      sensor.rom_probe = TRUE;
      sensor.persistent_cold = TRUE;
      g_test_expect_message ("libfprint-fte3600", G_LOG_LEVEL_WARNING,
                             "*Sensor reset after open failure also failed:*");
    }

  g_assert_false (fp_device_open_sync (device, NULL, &error));
  g_assert_nonnull (error);
  if (failure == 2)
    g_test_assert_expected_messages ();
  g_assert_cmpuint (sensor.mode_writes, ==, 0);
  g_assert_cmpuint (sensor.hardware_asserts, ==, failure == 2 ? 2 : 0);
  if (failure != 2)
    g_assert_cmpuint (sensor.probe_packets, ==, 0);
  g_assert_cmpuint (sensor.firmware_opens, ==, 0);

  sensor.cold_start = FALSE;
  sensor.registers[FT9361_REG_FW_VERSION] = sensor.model->firmware_version;
  sensor.registers[FT9361_REG_AGC_VERSION] = sensor.model->agc_version;
  open_device (device);
  finish_device (device);
}

#if !FTE3600_ENABLE_PERSONAL_AUTH
static void
test_disabled_auth (gconstpointer data)
{
  FpDevice *device = new_device_for_model (GPOINTER_TO_UINT (data));

  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) print = g_object_ref_sink (fp_print_new (device));
  g_autoptr(FpPrint) enrolled = NULL;
  gboolean match = FALSE;
  guint transactions, ioctls;

  open_device (device);
  transactions = sensor.spi_transactions;
  ioctls = sensor.ioctl_count;
  enrolled = fp_device_enroll_sync (device, print, NULL, NULL, NULL, &error);
  g_assert_null (enrolled);
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
  g_clear_error (&error);
  g_assert_false (fp_device_verify_sync (device, print, NULL, NULL, NULL,
                                         &match, NULL, &error));
  g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
  g_assert_false (match);
  g_assert_cmpuint (sensor.spi_transactions, ==, transactions);
  g_assert_cmpuint (sensor.ioctl_count, ==, ioctls);
  g_assert_cmpuint (sensor.images, ==, 0);
  finish_device (device);
}
#endif

static void
test_bridge_reject (gconstpointer data)
{
  guint scenario = GPOINTER_TO_UINT (data);

  g_autoptr(GError) error = NULL;
  FpDevice *device = new_device ();

  sensor.bad_abi = scenario == 0;
  sensor.buffer_size = scenario == 1 ? 4096 : FTE3600_BRIDGE_MAX_TRANSFER;
  sensor.bad_mode = scenario == 2;
  g_assert_false (fp_device_open_sync (device, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_cmpuint (sensor.resets, ==, 0);
  g_assert_cmpuint (sensor.hardware_asserts, ==, 0);
  g_assert_cmpuint (sensor.firmware_opens, ==, 0);
  finish_device (device);
}

static void
test_rom_discovery (gconstpointer data)
{
  guint scenario = GPOINTER_TO_UINT (data);

  g_autoptr(GError) error = NULL;
  FpDevice *device = new_device ();

  sensor.rom_probe = TRUE;
  sensor.cold_start = TRUE;
  sensor.registers[FT9361_REG_SENSOR_ID_HIGH] = 0;
  sensor.registers[FT9361_REG_SENSOR_ID_LOW] = 0;
  if (scenario == 1)
    sensor.rom_family = 0xffff;
  if (scenario == 2)
    sensor.otp = 2;                  /* FT9348 conflicts with the probed FT9361. */
  if (scenario == 3)
    sensor.otp = 0;
  if (scenario == 4)
    sensor.boot_reply = 0xef;
  if (scenario == 5)
    sensor.fail_probe_packet = 11;                  /* OTP enabled, read fails. */
  if (scenario == 6)
    sensor.cancel_probe_packet = 10;
  if (scenario == 7)
    sensor.fail_probe_packet = 1;                  /* Pure boot query fails before state changes. */
  if (scenario == 8)
    sensor.cancel_probe_packet = 1;
  if (scenario == 0)
    {
      g_assert_true (fp_device_open_sync (device, sensor.cancellable, &error));
      g_assert_no_error (error);
      g_assert_cmpuint (sensor.probe_packets, ==, 12);
    }
  else
    {
      g_assert_false (fp_device_open_sync (device, sensor.cancellable, &error));
      if (scenario == 5 || scenario == 7)
        g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
      else if (scenario == 6 || scenario == 8)
        g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      else
        g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
      /* Application wake is counted separately from backend resets. */
      g_assert_cmpuint (sensor.resets, ==, 0);
    }
  g_assert_false (sensor.otp_enabled);
  g_assert_false (sensor.reset_asserted);
  g_assert_cmpuint (sensor.discovery_wakes, ==, 12);
  g_assert_cmpuint (sensor.hardware_asserts - sensor.special_resets, ==, scenario >= 7 ? 0 : scenario == 4 ? 2 : 1);
  g_assert_cmpuint (sensor.hardware_deasserts - sensor.special_resets, ==, scenario >= 7 ? 0 : scenario == 4 ? 2 : 1);
  g_assert_cmpuint (sensor.firmware_opens, ==, 0);
  finish_device (device);
}

static void
test_firmware_identity_gate (gconstpointer data)
{
  guint scenario = GPOINTER_TO_UINT (data);

  g_autoptr(GError) error = NULL;
  FpDevice *device = new_device ();

  /* A stale 40/50 signature does not authorize a firmware upload. */
  sensor.rom_probe = TRUE;
  sensor.hardware_recovery = TRUE;
  sensor.cold_start = TRUE;
  sensor.persistent_cold = TRUE;
  if (scenario == 0)
    sensor.rom_family = 0;
  if (scenario == 1)
    sensor.otp = 3;
  if (scenario == 2)
    sensor.buffer_size = 8192;
  if (scenario == 3)
    g_test_expect_message ("libfprint-fte3600", G_LOG_LEVEL_WARNING,
                           "*firmware loading failed:*");
  g_test_expect_message ("libfprint-fte3600", G_LOG_LEVEL_WARNING,
                         "*Sensor reset after open failure also failed:*");
  g_assert_false (fp_device_open_sync (device, NULL, &error));
  if (scenario < 2)
    g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
  else if (scenario == 2)
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
  else
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_test_assert_expected_messages ();
  g_assert_cmpuint (sensor.firmware_opens, ==, scenario == 3 ? 1 : 0);
  g_assert_false (sensor.reset_asserted);
  g_assert_false (sensor.otp_enabled);
  finish_device (device);
}

static void
assert_hardware_recovery_timing (void)
{
  const guint32 expected[] = { 0, 1, 0, 0, 1, 0 };
  const guint minimum_ms[] = { 10, 20, 10, 10, 20 };
  gint64 last_release;
  guint after_recovery = 0;

  g_assert_cmpuint (sensor.reset_events->len, >=, G_N_ELEMENTS (expected));
  for (gsize i = 0; i < G_N_ELEMENTS (expected); i++)
    {
      ResetEvent current = g_array_index (sensor.reset_events, ResetEvent, i);

      g_assert_cmpuint (current.asserted, ==, expected[i]);
      if (i)
        {
          ResetEvent previous = g_array_index (sensor.reset_events, ResetEvent, i - 1);

          /* Lower bounds only: scheduler latency must not cause flaky upper
           * limits, and a removed wait must not silently pass. */
          g_assert_cmpint (current.when - previous.when, >=, minimum_ms[i - 1] * 1000);
        }
    }
  last_release = g_array_index (sensor.reset_events, ResetEvent, 5).when;
  for (gsize i = 0; i < sensor.soft_reset_times->len; i++)
    {
      gint64 when = g_array_index (sensor.soft_reset_times, gint64, i);

      if (when > last_release)
        {
          if (after_recovery == 0)
            {
              g_assert_cmpint (when - last_release, >=, 160000);
            }
          else
            {
              gint64 previous = g_array_index (sensor.soft_reset_times, gint64, i - 1);

              g_assert_cmpint (when - previous, >=, 5000);
            }
          after_recovery++;
        }
    }
  g_assert_cmpuint (after_recovery, ==, 2);
}

static void
test_hardware_reset (gconstpointer data)
{
  guint scenario = GPOINTER_TO_UINT (data);
  FpDevice *device = new_device ();

  g_autoptr(GError) error = NULL;

  sensor.hardware_recovery = TRUE;
  sensor.cold_start = TRUE;
  sensor.cancel_hardware_reset = scenario == 1;
  sensor.fail_hardware_reset = scenario == 2;
  if (scenario == 2)
    g_test_expect_message ("libfprint-fte3600", G_LOG_LEVEL_WARNING,
                           "*Sensor reset after open failure also failed:*");

  if (scenario == 0)
    {
      g_assert_true (fp_device_open_sync (device, sensor.cancellable, &error));
      g_assert_no_error (error);
    }
  else
    {
      g_assert_false (fp_device_open_sync (device, sensor.cancellable, &error));
      g_assert_error (error, G_IO_ERROR,
                      (scenario == 1 ? G_IO_ERROR_CANCELLED : G_IO_ERROR_FAILED));
      g_assert_false (fp_device_is_open (device));
      g_assert_cmpint (sensor.spi_fd, ==, -1);
      g_assert_false (sensor.claimed);
    }

  if (scenario == 2)
    {
      g_test_assert_expected_messages ();
      g_assert_cmpuint (sensor.hardware_asserts, ==, 1);
      g_assert_cmpuint (sensor.hardware_deasserts, ==, 0);
    }
  else
    {
      /* Cancellation during the first pulse must not truncate either pulse. */
      g_assert_cmpuint (sensor.hardware_asserts, ==, 2);
      g_assert_cmpuint (sensor.hardware_deasserts, ==, 2);
      assert_hardware_recovery_timing ();
    }
  finish_device (device);
}

static void
test_capture_error (gconstpointer data)
{
  guint scenario = GPOINTER_TO_UINT (data);
  gboolean cancel = scenario != 0;
  gboolean cancel_wait = scenario == 1 || scenario == 3;
  FpDevice *device = new_device ();

  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;

  open_device (device);
  sensor.irq_action = cancel_wait ? CANCEL_WAIT : DELIVER_FINGER;
  sensor.fail_image = !cancel;
  sensor.cancel_image = scenario == 2;
  sensor.fail_reset = scenario == 3;
  image = fp_device_capture_sync (device, TRUE, sensor.cancellable, &error);
  g_assert_null (image);
  g_assert_error (error, G_IO_ERROR, (cancel ? G_IO_ERROR_CANCELLED : G_IO_ERROR_FAILED));
  g_assert_cmpuint (sensor.resets, ==, 4);
  g_assert_cmpuint (sensor.images, ==, cancel_wait ? 0 : 1);
  g_assert_cmpint (fpi_device_get_current_action (device), ==, FPI_DEVICE_ACTION_NONE);

  /* Cancellation/error must leave the same open device usable again. */
  g_clear_error (&error);
  g_cancellable_reset (sensor.cancellable);
  sensor.irq_action = DELIVER_FINGER;
  sensor.fail_image = FALSE;
  sensor.cancel_image = FALSE;
  sensor.fail_reset = FALSE;
  image = fp_device_capture_sync (device, TRUE, sensor.cancellable, &error);
  g_assert_no_error (error);
  g_assert_nonnull (image);
  finish_device (device);
}

#if FTE3600_ENABLE_PERSONAL_AUTH
static void
enroll_progress (FpDevice *device,
                 gint      completed_stages,
                 FpPrint  *print,
                 gpointer  user_data,
                 GError   *error)
{
  guint *stages = user_data;

  g_assert_no_error (error);
  g_assert_cmpint (completed_stages, ==, ++*stages);
}

static void
test_enroll_verify_images (gconstpointer fixture)
{
  static const Fte3600Sensor sensors[] = {
    FTE3600_SENSOR_FT9361, FTE3600_SENSOR_FT9348,
    FTE3600_SENSOR_FT9338, FTE3600_SENSOR_FT9536,
  };
  guint model = GPOINTER_TO_UINT (fixture);
  const Fte3600MatchProfile *profile = fpi_fte3600_match_profile_get (sensors[model]);
  FpDevice *device = new_device_for_model (model);
  gsize image_size = (gsize) profile->width * profile->height;
  guint8 patch[FTE3600_BRISK_IMAGE_SIZE];
  g_autofree guint8 *original = g_malloc (image_size);
  g_autofree guint8 *frames = g_malloc (FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES * image_size);
  g_autofree guint8 *blank = g_malloc0 (image_size);
  guint stages = 0;
  gboolean match = FALSE;

  g_autoptr(Fte3600Template) expected = fpi_fte3600_template_new_for_profile (profile);
  g_autoptr(GBytes) expected_wire = NULL;
  g_autoptr(GVariant) data = NULL;
  g_autoptr(FpPrint) print = g_object_ref_sink (fp_print_new (device));
  g_autoptr(FpPrint) enrolled = NULL;
  g_autoptr(GError) error = NULL;
  gsize actual_size, expected_size;
  const guint8 *actual_data, *expected_data;

  make_visual_pattern (patch);
  memset (original, 126, image_size);
  for (guint y = 0; y < FTE3600_BRISK_HEIGHT; y++)
    memcpy (original + (y + (profile->height - FTE3600_BRISK_HEIGHT) / 2) * profile->width +
            (profile->width - FTE3600_BRISK_WIDTH) / 2,
            patch + y * FTE3600_BRISK_WIDTH, FTE3600_BRISK_WIDTH);
  for (guint i = 0; i < FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES; i++)
    {
      Fte3600BriskFeatureSet features;
      const Fte3600IpaFeatureSet *p_ipa = NULL;
#if FTE3600_ENABLE_IPA_AUTH
      Fte3600IpaFeatureSet ipa_features;
#endif
      guint8 *frame = frames + i * image_size;
      const FpiBriskImage view = { frame, image_size, profile->width, profile->height, profile->width };

      if (model == 0)
        {
          translate_pattern (original, frame, i % 4, i / 4, FALSE);
        }
      else
        {
          for (guint y = 0; y < profile->height; y++)
            for (guint x = 0; x < profile->width; x++)
              frame[y * profile->width + x] = (x >= i % 4 && y >= i / 4) ?
                                              original[(y - i / 4) * profile->width + x - i % 4] : 126;
        }
      g_assert_cmpint (fpi_fte3600_brisk_extract_for_profile (profile, &view,
                                                              &features), ==, FTE3600_BRISK_OK);
#if FTE3600_ENABLE_IPA_AUTH
      if (profile->sensor == FTE3600_SENSOR_FT9361 &&
          fpi_fte3600_ipa_extract (view.data, view.length, &ipa_features) == FTE3600_IPA_OK)
        p_ipa = &ipa_features;
#endif
      g_assert_cmpint (fpi_fte3600_template_add_dual_features (expected, &features, p_ipa, NULL), ==,
                       i + 1 == FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES ? FTE3600_TEMPLATE_OK :
                       FTE3600_TEMPLATE_NEED_MORE_SAMPLES);
    }
  g_assert_cmpint (fpi_fte3600_template_encode (expected, &expected_wire), ==,
                   FTE3600_TEMPLATE_OK);

  open_device (device);
  sensor.frames = frames;
  sensor.n_frames = FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES;
  enrolled = fp_device_enroll_sync (device, print, NULL,
                                    enroll_progress, &stages, &error);
  g_assert_no_error (error);
  g_assert_nonnull (enrolled);
  g_assert_cmpuint (stages, ==, FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES);
  g_assert_cmpuint (sensor.next_frame, ==, FTE3600_TEMPLATE_REQUIRED_SUBTEMPLATES);

  /* The real asynchronous enrollment worker must produce the same canonical
  * template as the public extractor applied once to each captured image. */
  g_object_get (enrolled, "fpi-data", &data, NULL);
  g_assert_nonnull (data);
  g_assert_true (g_variant_is_of_type (data, G_VARIANT_TYPE ("ay")));
  actual_data = g_variant_get_fixed_array (data, &actual_size, sizeof (guint8));
  expected_data = g_bytes_get_data (expected_wire, &expected_size);
  g_assert_cmpmem (actual_data, actual_size, expected_data, expected_size);

  sensor.next_frame = 0;
  sensor.n_frames = 1;
  g_assert_true (fp_device_verify_sync (device, enrolled, NULL, NULL, NULL,
                                        &match, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (match);
  g_assert_cmpuint (sensor.next_frame, ==, 1);

  /* A low-contrast retry terminates this action and leaves the device reusable. */
  sensor.frames = blank;
  sensor.next_frame = 0;
  g_assert_false (fp_device_verify_sync (device, enrolled, NULL, NULL, NULL,
                                         &match, NULL, &error));
  g_assert_error (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_GENERAL);
  g_assert_cmpuint (sensor.next_frame, ==, 1);
  g_clear_error (&error);
  sensor.frames = frames;
  sensor.next_frame = 0;
  g_assert_true (fp_device_verify_sync (device, enrolled, NULL, NULL, NULL,
                                        &match, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (match);
  finish_device (device);
}

static void
test_enroll_cancel (void)
{
  FpDevice *device = new_device ();

  g_autoptr(FpPrint) print = g_object_ref_sink (fp_print_new (device));
  g_autoptr(FpPrint) enrolled = NULL;
  g_autoptr(GError) error = NULL;

  open_device (device);
  sensor.irq_action = CANCEL_WAIT;
  enrolled = fp_device_enroll_sync (device, print, sensor.cancellable,
                                    NULL, NULL, &error);
  g_assert_null (enrolled);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint (sensor.images, ==, 0);
  g_assert_cmpuint (sensor.resets, ==, 4);
  g_assert_cmpint (fpi_device_get_current_action (device), ==, FPI_DEVICE_ACTION_NONE);
  finish_device (device);
}
#endif

static void
test_open_error (gconstpointer data)
{
  guint failure = GPOINTER_TO_UINT (data);
  FpDevice *device = new_device ();

  g_autoptr(GError) error = NULL;

  sensor.fail_config = failure == 0;
  sensor.fail_claim = failure == 1;
  sensor.bad_id = failure == 2;
  g_assert_false (fp_device_open_sync (device, NULL, &error));
  g_assert_nonnull (error);
  g_assert_false (fp_device_is_open (device));
  g_assert_cmpint (sensor.spi_fd, ==, -1);
  g_assert_false (sensor.claimed);
  g_assert_cmpuint (sensor.claims, ==, sensor.releases);
  if (sensor.bad_id)
    {
      g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
      g_assert_cmpuint (sensor.resets, ==, 2);
      g_assert_cmpuint (sensor.firmware_opens, ==, 0);
      g_assert_false (sensor.probe_started);
    }

  sensor.fail_config = sensor.fail_claim = sensor.bad_id = FALSE;
  open_device (device);
  finish_device (device);
}

#include "fte3600-test-load.h"

int
main (int argc, char **argv)
{
  static const LegacyWakeFixture sleeping[] = {
    { .name = "FT9361-original", .model = 0, .ready_attempt = 1 },
    { .name = "FT9361-alternate", .model = 0, .ready_attempt = 1, .required_high = TRUE, .cs_control = TRUE },
    { .name = "FT9348-original", .model = 1, .ready_attempt = 2 },
    { .name = "FT9348-alternate", .model = 1, .ready_attempt = 2, .required_high = TRUE, .cs_control = TRUE },
    { .name = "FT9338-original", .model = 2, .ready_attempt = 3 },
    { .name = "FT9338-alternate", .model = 2, .ready_attempt = 3, .required_high = TRUE, .cs_control = TRUE },
    { .name = "FT9536-sixth-attempt", .model = 3, .ready_attempt = 6 },
    { .name = "FT9536-alternate-sixth-attempt", .model = 3, .ready_attempt = 6, .required_high = TRUE, .cs_control = TRUE },
    { .name = "first-wake-error", .fault = LEGACY_WAKE_IO_ERROR, .fault_command = 1 },
    { .name = "second-wake-error", .fault = LEGACY_WAKE_IO_ERROR, .fault_command = 2 },
    { .name = "alternate-wake-short", .fault = LEGACY_WAKE_SHORT, .fault_command = 2, .required_high = TRUE, .cs_control = TRUE },
    { .name = "first-wake-cancel", .fault = LEGACY_WAKE_CANCEL, .fault_command = 1 },
    { .name = "alternate-second-wake-cancel", .fault = LEGACY_WAKE_CANCEL, .fault_command = 2, .required_high = TRUE, .cs_control = TRUE },
    { .name = "unknown-geometry", .ready_attempt = 1, .fault = LEGACY_WAKE_UNKNOWN },
    { .name = "unstable-geometry", .ready_attempt = 1, .fault = LEGACY_WAKE_UNSTABLE },
    { .name = "six-attempts-exhausted", .fault = LEGACY_WAKE_EXHAUSTED },
    { .name = "both-polarities-exhausted", .fault = LEGACY_WAKE_EXHAUSTED, .cs_control = TRUE },
  };
  static const ProbeFixture probes[] = {
    { .name = "FT9369", .id = 0x9362, .required_high = TRUE, .cs_control = TRUE, .success = TRUE },
    { .name = "FT9365", .id = 0x9365, .success = TRUE },
    { .name = "FT9769", .id = 0x9391, .variant = 0x123, .success = TRUE },
    { .name = "FT9769", .id = 0x9392, .limit = 1798, .success = TRUE },
    { .name = "FT9368", .id = 0x9368, .required_high = TRUE, .cs_control = TRUE, .success = TRUE },
    { .name = "9362-unstable", .id = 0x9362, .required_high = TRUE, .cs_control = TRUE, .unstable = TRUE },
    { .name = "9395-rejected", .id = 0x9391, .variant = 0xfff },
    { .name = "9365-corrupt-crc", .id = 0x9365, .corrupt_crc = TRUE },
    { .name = "9368-bad-geometry", .id = 0x9368, .unstable = TRUE },
    { .name = "cs-setup-failure", .id = 0x9362, .required_high = TRUE, .cs_control = TRUE, .fail_cs = TRUE },
    { .name = "9362-transfer-too-small", .id = 0x9362, .limit = 8192 },
    { .name = "9392-transfer-too-small", .id = 0x9392, .limit = 1797 },
    { .name = "9395-unsupported", .id = 0x9395, .required_high = TRUE, .cs_control = TRUE },
    { .name = "FT9365", .id = 0x9365, .success = TRUE, .exercise_open_failure = TRUE },
    { .name = "FT9769", .id = 0x9392, .success = TRUE, .exercise_open_failure = TRUE },
    { .name = "FT9369", .id = 0x9362, .mode_required = TRUE, .success = TRUE },
    { .name = "FT9365", .id = 0x9365, .mode_required = TRUE, .success = TRUE },
    { .name = "FT9769", .id = 0x9392, .mode_required = TRUE, .required_high = TRUE, .cs_control = TRUE, .success = TRUE },
  };

  g_test_init (&argc, &argv, NULL);
  for (guint i = 0; i < G_N_ELEMENTS (sleeping); i++)
    {
      g_autofree gchar *path = g_strdup_printf ("/fte3600-lifecycle/discovery/sleeping/%s", sleeping[i].name);
      g_test_add_data_func (path, &sleeping[i], test_sleeping_legacy_discovery);
    }
  g_test_add_func ("/fte3600-lifecycle/discovery/FT9536-cold-boot-a", test_boot38_cold_enumeration);
  for (guint i = 0; i < G_N_ELEMENTS (probes); i++)
    {
      g_autofree gchar *path = g_strdup_printf ("/fte3600-lifecycle/discovery/%u-%s", i, probes[i].name);
      g_test_add_data_func (path, &probes[i], test_special_discovery);
    }
  g_test_add_func ("/fte3600-lifecycle/capture-reopen-without-dmi", test_capture_reopen);
  for (guint model = 0; model < G_N_ELEMENTS (models); model++)
    {
      g_autofree gchar *change = g_strdup_printf ("/fte3600-lifecycle/families/%s/changed-sensor", models[model].name);
      g_autofree gchar *short_frame = g_strdup_printf ("/fte3600-lifecycle/families/%s/short-frame", models[model].name);

      g_test_add_data_func (change, GUINT_TO_POINTER (model), test_probe_open_identity_change);
      g_test_add_data_func (short_frame, GUINT_TO_POINTER (model), test_family_short_frame);
      if (model)
        {
          g_autofree gchar *warm = g_strdup_printf ("/fte3600-lifecycle/families/%s/warm-capture", models[model].name);
          g_autofree gchar *cancel = g_strdup_printf ("/fte3600-lifecycle/families/%s/cancel-reuse", models[model].name);

          g_test_add_data_func (warm, GUINT_TO_POINTER (model), test_family_warm_capture);
          g_test_add_data_func (cancel, GUINT_TO_POINTER (model), test_family_cancel_and_reuse);
        }
#if !FTE3600_ENABLE_PERSONAL_AUTH
      {
        g_autofree gchar *auth = g_strdup_printf ("/fte3600-lifecycle/families/%s/no-auth", models[model].name);

        g_test_add_data_func (auth, GUINT_TO_POINTER (model), test_disabled_auth);
      }
#endif
      for (guint scenario = 0; scenario < 2; scenario++)
        {
          g_autofree gchar *limit = g_strdup_printf ("/fte3600-lifecycle/families/%s/transfer-limit-%u", models[model].name, scenario);
          g_autofree gchar *cleanup = g_strdup_printf ("/fte3600-lifecycle/families/%s/cleanup-failed-%u", models[model].name, scenario);

          g_test_add_data_func (limit, GUINT_TO_POINTER (model * 2 + scenario), test_family_transfer_limit);
          g_test_add_data_func (cleanup, GUINT_TO_POINTER (model * 2 + scenario), test_cleanup_invalidates_session);
        }
    }
  for (guint scenario = 0; scenario < 5; scenario++)
    {
      g_autofree gchar *path = g_strdup_printf ("/fte3600-lifecycle/ft9348/rom-firmware-%u", scenario);

      g_test_add_data_func (path, GUINT_TO_POINTER (scenario), test_ft9348_rom_and_firmware);
    }
  for (guint scenario = 0; scenario < 6; scenario++)
    {
      g_autofree gchar *path = g_strdup_printf ("/fte3600-lifecycle/legacy/unready-%u", scenario);

      g_test_add_data_func (path, GUINT_TO_POINTER (scenario), test_legacy_reject_unready_application);
    }
  for (guint scenario = 0; scenario < 3; scenario++)
    {
      g_autofree gchar *path = g_strdup_printf ("/fte3600-lifecycle/bridge/reject-%u", scenario);
      g_test_add_data_func (path, GUINT_TO_POINTER (scenario), test_bridge_reject);
    }
  for (guint scenario = 0; scenario < 9; scenario++)
    {
      g_autofree gchar *path = g_strdup_printf ("/fte3600-lifecycle/discovery/rom-%u", scenario);
      g_test_add_data_func (path, GUINT_TO_POINTER (scenario), test_rom_discovery);
    }
  for (guint scenario = 0; scenario < 6; scenario++)
    {
      g_autofree gchar *path = g_strdup_printf ("/fte3600-lifecycle/discovery/inactive-%u", scenario);
      g_test_add_data_func (path, GUINT_TO_POINTER (scenario), test_inactive_discovery);
    }
  for (guint scenario = 0; scenario < 4; scenario++)
    {
      g_autofree gchar *path = g_strdup_printf ("/fte3600-lifecycle/firmware/gate-%u", scenario);
      g_test_add_data_func (path, GUINT_TO_POINTER (scenario), test_firmware_identity_gate);
    }
  g_test_add_data_func ("/fte3600-lifecycle/hardware-reset/recovery", GUINT_TO_POINTER (0),
                        test_hardware_reset);
  g_test_add_data_func ("/fte3600-lifecycle/hardware-reset/cancel", GUINT_TO_POINTER (1),
                        test_hardware_reset);
  g_test_add_data_func ("/fte3600-lifecycle/hardware-reset/error", GUINT_TO_POINTER (2),
                        test_hardware_reset);
  g_test_add_data_func ("/fte3600-lifecycle/cancel-wait", GINT_TO_POINTER (TRUE),
                        test_capture_error);
  g_test_add_data_func ("/fte3600-lifecycle/capture-error", GINT_TO_POINTER (FALSE),
                        test_capture_error);
  g_test_add_data_func ("/fte3600-lifecycle/cancel-transfer", GUINT_TO_POINTER (2),
                        test_capture_error);
  g_test_add_data_func ("/fte3600-lifecycle/cancel-cleanup-failure", GUINT_TO_POINTER (3),
                        test_capture_error);
#if FTE3600_ENABLE_PERSONAL_AUTH
  g_test_add_func ("/fte3600-lifecycle/verify-load-cancel", test_verify_load_cancel);
  g_test_add_func ("/fte3600-lifecycle/verify-load-previous-policy", test_verify_load_previous_policy);
  for (guint model = 0; model < G_N_ELEMENTS (models); model++)
    {
      g_autofree gchar *path = g_strdup_printf ("/fte3600-lifecycle/families/%s/enroll-verify-images", models[model].name);
      g_test_add_data_func (path, GUINT_TO_POINTER (model), test_enroll_verify_images);
    }
  g_test_add_func ("/fte3600-lifecycle/enroll-cancel", test_enroll_cancel);
#endif
  g_test_add_data_func ("/fte3600-lifecycle/open/bridge-query-error", GUINT_TO_POINTER (0),
                        test_open_error);
  g_test_add_data_func ("/fte3600-lifecycle/open/bridge-busy", GUINT_TO_POINTER (1),
                        test_open_error);
  g_test_add_data_func ("/fte3600-lifecycle/open/id-error", GUINT_TO_POINTER (2),
                        test_open_error);
  return g_test_run ();
}
