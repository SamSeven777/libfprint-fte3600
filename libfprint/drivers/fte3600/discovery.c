/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_discovery.h"
#include "fte3600_reset_sync.h"

static struct fte3600_identity_result
failed (enum fte3600_status status)
{
  const struct fte3600_identity_result result = {
    status, FTE3600_SENSOR_UNKNOWN, FTE3600_BOOT_UNKNOWN
  };

  return result;
}

/* Every exchange in this bounded protocol is at most eleven bytes. */
static int
send_frame (const struct fte3600_recovery_io *io,
            const uint8_t *tx, size_t length)
{
  uint8_t rx[11];

  return io->spi (io->context, tx, rx, length);
}

static int
read_register (const struct fte3600_recovery_io *io,
               uint8_t address, int extra_byte, uint8_t *value)
{
  const uint8_t tx[] = {0x08, 0xf7, address, 0, 0};
  uint8_t rx[sizeof tx];
  const size_t length = extra_byte ? 5 : 4;

  if (io->spi (io->context, tx, rx, length) != 0)
    return 1;
  *value = rx[length - 1];
  return 0;
}

static int
write_register (const struct fte3600_recovery_io *io,
                uint8_t address, uint8_t value, int extra_byte)
{
  const uint8_t tx[] = {0x09, 0xf6, address, value, 0};

  return send_frame (io, tx, extra_byte ? 5 : 4);
}

static struct fte3600_identity_result
discover_fe (const struct fte3600_recovery_io *io, const struct fte3600_reset_observer *o)
{
  uint8_t cb, fe;

  if (fte3600_reset_and_sync (io, o) != 0 ||
      read_register (io, 0xcb, 0, &cb) != 0 ||
      write_register (io, 0xcb, (uint8_t) (cb | 0x20), 0) != 0 ||
      write_register (io, 0xfd, 0x11, 0) != 0 ||
      write_register (io, 0xfe, 0x11, 0) != 0 ||
      read_register (io, 0xfe, 0, &fe) != 0)
    return failed (FTE3600_TRANSPORT_ERROR);
  return fte3600_classify_boot_a (fe, 1);
}

static struct fte3600_identity_result
discover_otp (const struct fte3600_recovery_io *io, uint16_t family,
              const struct fte3600_reset_observer *o)
{
  const int a8 = fte3600_classify_family (family, 1).family ==
                 FTE3600_FAMILY_A8;
  uint8_t c8, f4, otp;

  if ((!a8 && fte3600_reset_and_sync (io, o) != 0) ||
      read_register (io, 0xc8, a8, &c8) != 0 ||
      write_register (io, 0xc8, a8 ? 0xdf : (uint8_t) (c8 | 1), a8) != 0 ||
      write_register (io, 0xf1, 0x1d, a8) != 0 ||
      read_register (io, 0xf4, a8, &f4) != 0 ||
      write_register (io, 0xf4, (uint8_t) (f4 | 1), a8) != 0 ||
      read_register (io, 0xf3, a8, &otp) != 0 ||
      write_register (io, 0xf4, 0, a8) != 0)
    return failed (FTE3600_TRANSPORT_ERROR);
  return fte3600_classify_otp (family, otp, 1);
}

static struct fte3600_identity_result
discover (const struct fte3600_recovery_io *io,
          const struct fte3600_reset_observer *o, uint32_t *boot)
{
  static const uint8_t marker_tx[] = {0x90, 0, 0};
  static const uint8_t family_start[] = {0x06, 0xf9, 0};
  static const uint8_t family_prepare[] = {
    0x05, 0xfa, 0x85, 0xc0, 0, 4, 0x11, 0xee, 2, 0, 0
  };
  static const uint8_t family_read[] = {0x04, 0xfb, 0x85, 0xc0, 0, 0, 0, 0};
  uint8_t marker_rx[sizeof marker_tx];
  uint8_t family_rx[sizeof family_read];
  uint16_t family;

  if (io == NULL || io->gpio == NULL || io->spi == NULL ||
      io->wait_ms == NULL || (o && !o->reset))
    return failed (FTE3600_INVALID_INPUT);
  if (boot)
    *boot = 1;
  if (io->spi (io->context, marker_tx, marker_rx, sizeof marker_tx) != 0)
    return failed (FTE3600_TRANSPORT_ERROR);
  if (boot)
    *boot = 0;
  if (marker_rx[sizeof marker_rx - 1] == 0xef)
    return discover_fe (io, o);

  if (send_frame (io, family_start, sizeof family_start) != 0 ||
      send_frame (io, family_prepare, sizeof family_prepare) != 0)
    return failed (FTE3600_TRANSPORT_ERROR);
  io->wait_ms (io->context, 2);
  if (write_register (io, 0xa4, 1, 0) != 0 ||
      io->spi (io->context, family_read, family_rx, sizeof family_read) != 0)
    return failed (FTE3600_TRANSPORT_ERROR);
  family = (uint16_t) ((uint16_t) family_rx[6] << 8 | family_rx[7]);
  struct fte3600_identity_result result = discover_otp (io, family, o);
  if (boot && result.status == FTE3600_OK)
    *boot = result.boot == FTE3600_BOOT_B;
  return result;
}

struct fte3600_identity_result
fte3600_discover_legacy_observed (const struct fte3600_recovery_io    *io,
                                  const struct fte3600_reset_observer *o)
{
  return discover (io, o, NULL);
}

struct fte3600_identity_result
fte3600_discover_legacy_retained (
  const struct fte3600_recovery_io *io, const struct fte3600_reset_observer *o,
  uint32_t *boot)
{
  const void *p[] = {io, o, boot};
  const size_t n[] = {sizeof *io, sizeof *o, sizeof *boot};
  const size_t a[] = {_Alignof (struct fte3600_recovery_io), _Alignof (struct fte3600_reset_observer), _Alignof (uint32_t)};

  for (size_t i = 0; i < 3; ++i)
    {
      if (i == 1 && !o)
        continue;
      uintptr_t x = (uintptr_t) p[i];
      if (!p[i] || x % a[i] || n[i] > UINTPTR_MAX - x)
        return failed (FTE3600_INVALID_INPUT);
      for (size_t j = 0; j < i; ++j)
        if (p[j])
          {
            uintptr_t y = (uintptr_t) p[j];
            if (x <= y ? y - x < n[i] : x - y < n[j])
              return failed (FTE3600_INVALID_INPUT);
          }
    }
  return discover (io, o, boot);
}

struct fte3600_identity_result
fte3600_discover_legacy (const struct fte3600_recovery_io *io)
{
  return fte3600_discover_legacy_observed (io, NULL);
}
