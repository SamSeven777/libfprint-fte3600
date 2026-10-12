/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_ft9368_boot.h"
#include "fte3600_ft9368_lifecycle.h"
#include <string.h>
struct session
{
  const struct fte3600_ft9368_boot_io       *io;
  int                                        read_failed;
  const struct fte3600_ft9368_boot_observer *observer;
};
static int
negative (uint32_t s)
{
  return (s >> 31) != 0;
}
static int
valid_io (const struct fte3600_ft9368_boot_io *io)
{
  return io && io->protocol.exchange && io->protocol.wait_ms && io->gpio && io->wait_us;
}
static int
valid_image (const struct fte3600_ft9368_boot_image *v, int pram)
{
  size_t required;

  if (!v || v->length > (pram ? FTE3600_FT9368_PRAM_MAX : FTE3600_FT9368_APPLICATION_MAX) ||
      v->length % (pram ? 4 : 2) || (v->length && !v->data))
    return 0;
  required = pram ? ((v->length + 127) / 128) * 128 : v->length;
  return v->capacity >= required;
}
static void
ms (struct session *s, uint32_t duration)
{
  if (!s->read_failed)
    s->io->protocol.wait_ms (s->io->protocol.context, duration);
}
static void
us (struct session *s)
{
  if (!s->read_failed)
    s->io->wait_us (s->io->microsecond_context, 1);
}
static uint32_t
exchange (struct session *s, uint8_t *frame, size_t length, int reading)
{
  uint32_t status;

  if (s->read_failed)
    return FTE3600_FT9368_ADAPTER_IO_ERROR;
  status = s->io->protocol.exchange (s->io->protocol.context, frame, length);
  if (reading && negative (status))
    s->read_failed = 1;
  return status;
}
/* Existing application helpers receive a fail-stop proxy so no later callback
 * occurs after a response outside the reviewed initialized-read domain. */
static uint32_t
proxy_exchange (void *c, uint8_t *frame, size_t n)
{
  return exchange (c, frame, n, n > 4 && (frame[1] & 0x80));
}
static void
proxy_wait (void *c, uint32_t n)
{
  ms (c, n);
}
static uint32_t
proxy_gpio (void *c, int level)
{
  struct session *s = c;

  return s->read_failed ? FTE3600_FT9368_ADAPTER_IO_ERROR :
         s->io->gpio (s->io->gpio_context, level);
}
static uint64_t
no_notification (void *c)
{
  (void) c;
  return 0;
}
static struct fte3600_ft9368_io
protocol (struct session *s)
{
  struct fte3600_ft9368_io io = {s, proxy_exchange, proxy_wait};

  return io;
}
static void
reset (struct session *s)
{
  const struct fte3600_ft9368_lifecycle_state state = {0, 0};
  struct fte3600_ft9368_lifecycle_io io = {protocol (s), s, proxy_gpio, NULL, no_notification};
  uint32_t status;

  if (s->observer)
    s->observer->reset (s->observer->context, 1, 0);
  status = fte3600_ft9368_lifecycle_reset (&state, &io);
  if (s->observer)
    s->observer->reset (s->observer->context, 0, status);
}
static void
mode (struct session *s, uint8_t value)
{
  uint8_t f[] = {0x70, value, (uint8_t) (value ^ 0xff)};

  (void) exchange (s, f, sizeof f, 0);
}
static void
sfr (struct session *s, uint16_t address, uint16_t value)
{
  uint8_t f[] = {0x70, 0x07, 0xf8, (uint8_t) (address >> 8), (uint8_t) address, 0, 0,
                 (uint8_t) (value >> 8), (uint8_t) value, 0, 0};

  (void) exchange (s, f, sizeof f, 0);
}
static void
ram_header (uint8_t *f, int writing, size_t offset, size_t length)
{
  uint16_t address = (uint16_t) (0x2000 + offset / 4), count = (uint16_t) (length / 4 - 1);

  f[0] = 0x70;
  f[1] = writing ? 5 : 4;
  f[2] = writing ? 0xfa : 0xfb;
  f[3] = (uint8_t) (address >> 8);
  f[4] = (uint8_t) address;
  f[5] = (uint8_t) (count >> 8);
  f[6] = (uint8_t) count;
}
static void
ram_write (struct session *s, size_t offset, size_t n, const uint8_t *source)
{
  uint8_t f[135];

  ram_header (f, 1, offset, n);
  memcpy (f + 7, source, n);
  (void) exchange (s, f, n + 7, 0);
}
static int
ram_read (struct session *s, size_t offset, size_t n, uint8_t *output)
{
  uint8_t select[7], f[257] = {0x71};

  ram_header (select, 0, offset, n);
  (void) exchange (s, select, sizeof select, 0);
  us (s);
  (void) exchange (s, f, n + 1, 1);
  if (s->read_failed)
    return 0;
  memcpy (output, f + 1, n);
  return 1;
}
static uint32_t
pram (struct session *s, const struct fte3600_ft9368_boot_image *v, uint32_t *counter)
{
  uint8_t verified[6144];
  size_t o;

  reset (s);
  if ((*counter & 1) == 0)
    ms (s, 1);
  ++*counter;
  mode (s, 0x55);
  sfr (s, 4, 0xaa55);
  sfr (s, 0x48, 0x0110);
  for(o = 0; o < v->length; o += 128)
    {
      ram_write (s, o, 128, v->data + o);
      if (v->length - o <= 128)
        ram_write (s, o, v->length - o, v->data + o);
    }
  for(o = 0; o < v->length; o += 256)
    {
      if (!ram_read (s, o, 256, verified + o))
        return 0;
      if (v->length - o <= 256 && !ram_read (s, o, v->length - o, verified + o))
        return 0;
    }
  if (v->length && memcmp (verified, v->data, v->length))
    return 1;
  sfr (s, 7, 0x5a5a);
  mode (s, 0x0a);
  ms (s, 10);
  return 0;
}
static void
app_command (struct session *s, uint8_t opcode, size_t n,
             const uint8_t *payload, uint8_t *response)
{
  uint8_t wake[4] = {0xff, 0, 0, 0}, f[263] = {0};
  size_t header = n ? 7 : 4;

  (void) exchange (s, wake, sizeof wake, 0);
  us (s);
  f[0] = opcode;
  f[1] = response ? 0x80 : 0;
  f[2] = (uint8_t) (n >> 8);
  f[3] = (uint8_t) n;
  if (n && payload)
    memcpy (f + header, payload, n);
  (void) exchange (s, f, header + n, response != NULL);
  if (response && !s->read_failed)
    memcpy (response, f + header, n);
}
static uint16_t
app_word (struct session *s, uint8_t opcode)
{
  uint8_t bytes[2] = {0};

  app_command (s, opcode, 2, NULL, bytes);
  return (uint16_t) (((uint16_t) bytes[0] << 8) | bytes[1]);
}
static int
erase_word (uint16_t v)
{
  return v == 0xf055 || v == 0xf0aa || v == 0xf02d || v == 0x2000 || v == 0x1000;
}
static uint16_t
checksum (const uint8_t *data, size_t n)
{
  uint16_t sum = 0;
  size_t i;
  unsigned int bit;

  for(i = 0; i < n; i += 2)
    {
      sum ^= (uint16_t) (((uint16_t) data[i] << 8) | data[i + 1]);
      for(bit = 0; bit < 16; ++bit)
        sum = (uint16_t) ((sum >> 1) ^ ((sum & 1) ? 0x8408 : 0));
    }
  return sum;
}
static uint32_t
application (struct session *s, const struct fte3600_ft9368_boot_image *v)
{
  uint8_t a = 0x0a, b = 0x0c, c = 0, range[6] = {0};
  uint16_t value = 0, expected;
  unsigned int poll;
  size_t o;

  app_command (s, 0x55, 0, NULL, NULL);
  ms (s, 10);
  value = app_word (s, 0x90);
  if (s->read_failed || value != 0x56a2)
    return 1;
  app_command (s, 0x09, 1, &a, NULL);
  app_command (s, 0x10, 1, &b, NULL);
  app_command (s, 0x61, 1, &c, NULL);
  for(poll = 0; poll < 100; ++poll)
    {
      value = app_word (s, 0x6a);
      if (s->read_failed)
        return 0;
      if (erase_word (value))
        break;
      ms (s, 10);
    }
  if(value != 0xf0aa)
    return 2;
  for(o = 0; o < v->length; o += 256)
    {
      size_t n = v->length - o < 256 ? v->length - o : 256;
      uint8_t address[] = {(uint8_t) (o >> 16), (uint8_t) (o >> 8), (uint8_t) o};
      app_command (s, 0xab, 3, address, NULL);
      app_command (s, 0xbf, n, v->data + o, NULL);
      expected = (uint16_t) (0x1000 + o / n);
      for(poll = 0; poll < 10; ++poll)
        {
          value = app_word (s, 0x6a);
          if (s->read_failed)
            return 0;
          if(value == expected)
            break;
          ms (s, 10);
        }
      if(value != expected)
        return 3;
    }
  app_command (s, 0x64, 0, NULL, NULL);
  range[3] = (uint8_t) (v->length >> 16);
  range[4] = (uint8_t) (v->length >> 8);
  range[5] = (uint8_t) v->length;
  app_command (s, 0x65, sizeof range, range, NULL);
  ms (s, 5);
  value = app_word (s, 0x66);
  if (s->read_failed)
    return 0;
  if (!(value & 0xff00) || !(value & 0xff) || value != checksum (v->data, v->length))
    return 5;
  app_command (s, 0x07, 0, NULL, NULL);
  return 0;
}
static enum fte3600_ft9368_boot_error
finish (struct session *s, uint32_t value, uint32_t *out)
{
  if (s->read_failed)
    return FTE3600_FT9368_BOOT_TRANSPORT_FAILURE;
  *out = value;
  return FTE3600_FT9368_BOOT_OK;
}
enum fte3600_ft9368_boot_error
fte3600_ft9368_boot_pram (
  const struct fte3600_ft9368_boot_image *v, uint32_t *counter,
  const struct fte3600_ft9368_boot_io *io, uint32_t *result)
{
  return fte3600_ft9368_boot_pram_observed (v, counter, io, NULL, result);
}
enum fte3600_ft9368_boot_error
fte3600_ft9368_boot_pram_observed (
  const struct fte3600_ft9368_boot_image *v, uint32_t *counter,
  const struct fte3600_ft9368_boot_io *io,
  const struct fte3600_ft9368_boot_observer *observer, uint32_t *result)
{
  struct session s = {io, 0, observer};
  uint32_t value;

  if (observer && ((uintptr_t) observer % _Alignof (struct fte3600_ft9368_boot_observer) ||
                   !observer->reset))
    return FTE3600_FT9368_BOOT_INVALID_ARGUMENT;
  if (!valid_io (io) || !valid_image (v, 1) || !counter || !result)
    return FTE3600_FT9368_BOOT_INVALID_ARGUMENT;
  value = pram (&s, v, counter);
  return finish (&s, value, result);
}
enum fte3600_ft9368_boot_error
fte3600_ft9368_boot_application (
  const struct fte3600_ft9368_boot_image *v,
  const struct fte3600_ft9368_boot_io *io, uint32_t *result)
{
  struct session s = {io, 0, NULL};
  uint32_t value;

  if (!valid_io (io) || !valid_image (v, 0) || !result)
    return FTE3600_FT9368_BOOT_INVALID_ARGUMENT;
  value = application (&s, v);
  return finish (&s, value, result);
}
enum fte3600_ft9368_boot_error
fte3600_ft9368_boot_load (
  const struct fte3600_ft9368_boot_image *ram, const struct fte3600_ft9368_boot_image *app,
  uint32_t *counter, int check_version, uint8_t expected_version,
  const struct fte3600_ft9368_boot_io *io, uint32_t *result)
{
  struct session s = {io, 0, NULL};
  struct fte3600_ft9368_io existing = protocol (&s);
  unsigned int attempt;
  uint32_t value;

  if (!valid_io (io) || !valid_image (ram, 1) || !valid_image (app, 0) || !counter || !result ||
      (check_version != 0 && check_version != 1))
    return FTE3600_FT9368_BOOT_INVALID_ARGUMENT;
  if (check_version)
    {
      value = fte3600_ft9368_check_version (expected_version, &existing);
      if (s.read_failed)
        return FTE3600_FT9368_BOOT_TRANSPORT_FAILURE;
      if (!negative (value))
        return finish (&s, 0, result);
    }
  for(attempt = 0; attempt < 5; ++attempt)
    {
      value = pram (&s, ram, counter);
      if (s.read_failed)
        return FTE3600_FT9368_BOOT_TRANSPORT_FAILURE;
      if (value)
        continue;
      value = application (&s, app);
      if (s.read_failed)
        return FTE3600_FT9368_BOOT_TRANSPORT_FAILURE;
      if (value)
        continue;
      reset (&s);
      ms (&s, 400);
      value = fte3600_ft9368_probe (&existing);
      if (s.read_failed)
        return FTE3600_FT9368_BOOT_TRANSPORT_FAILURE;
      if (value == 0x9368)
        return finish (&s, 0, result);
    }
  return finish (&s, UINT32_C (0xc0000001), result);
}
static uint32_t
recovery_gpio (void *c, int level)
{
  const struct fte3600_recovery_io *io = c;

  return io->gpio (io->context, level) == 0 ? 0 : FTE3600_FT9368_ADAPTER_IO_ERROR;
}
enum fte3600_ft9368_boot_error
fte3600_ft9368_boot_io_from_recovery (
  const struct fte3600_recovery_io *recovery, void *context, void (*wait_us)(void *, uint32_t),
  struct fte3600_ft9368_boot_io *out)
{
  struct fte3600_ft9368_boot_io io;

  if (!recovery || !recovery->gpio || !wait_us || !out ||
      fte3600_ft9368_io_from_recovery (recovery, &io.protocol))
    return FTE3600_FT9368_BOOT_INVALID_ARGUMENT;
  io.gpio_context = (void *) recovery;
  io.gpio = recovery_gpio;
  io.microsecond_context = context;
  io.wait_us = wait_us;
  *out = io;
  return FTE3600_FT9368_BOOT_OK;
}
