/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_factory_byte_startup.h"
#include "fte3600_ft9368_lifecycle.h"
#include <string.h>
const struct fte3600_factory_byte_identity fte3600_factory_byte_application_identity = {
  27120, "9997aafac8eb9a1aecc7ba5b41212004fe1b325e7bdd1ea9ca08ae54b803e2e0"
};
const struct fte3600_factory_byte_identity fte3600_factory_byte_pram_identity = {
  6096, "c93a807eaaa34d9e79fbab77b86e910a419fd633ea98b628b72c64b285ffd191"
};
struct operation
{
  const struct fte3600_factory_byte_io       *io;
  const struct fte3600_factory_byte_observer *observer;
  struct fte3600_factory_byte_result         *r;
  int                                         version, application, polling;
  uint32_t                                    destination;
  uint16_t                                    acknowledgement;
};
static int
negative (uint32_t x)
{
  return (x >> 31) != 0;
}
static int
aligned (const void *p, size_t a)
{
  return p && (uintptr_t) p % a == 0;
}
static int
extent (const void *p, size_t n)
{
  return p && n && n <= UINTPTR_MAX - (uintptr_t) p;
}
static int
overlap (const void *a, size_t an, const void *b, size_t bn)
{
  uintptr_t x = (uintptr_t) a, y = (uintptr_t) b;

  return x <= y ? y - x < an : x - y < bn;
}
static int
valid_io (const struct fte3600_factory_byte_io *io)
{
  return io->spi && io->wait_ms && io->wait_us && io->gpio && io->boundary;
}
enum fte3600_factory_byte_error
fte3600_factory_byte_state_defaults (struct fte3600_factory_byte_state *s)
{
  if (!aligned (s, _Alignof (struct fte3600_factory_byte_state)))
    return FTE3600_FACTORY_BYTE_INVALID_ARGUMENT;
  memset (s, 0, sizeof (*s));
  return FTE3600_FACTORY_BYTE_OK;
}
enum fte3600_factory_byte_error
fte3600_factory_ft9368_defaults (
  struct fte3600_factory_byte_selected *s, const struct fte3600_factory_byte_io *io)
{
  if (!aligned (s, _Alignof (struct fte3600_factory_byte_selected)) ||
      !aligned (io, _Alignof (struct fte3600_factory_byte_io)) || !valid_io (io) ||
      overlap (s, sizeof (*s), io, sizeof (*io)))
    return FTE3600_FACTORY_BYTE_INVALID_ARGUMENT;
  *s = (struct fte3600_factory_byte_selected){.model = FTE3600_MODEL_FT9368,
                                              .width = 64, .height = 80, .expected_version = 19, .agc_version = 0, .board = io,
                                              .application_identity = &fte3600_factory_byte_application_identity,
                                              .pram_identity = &fte3600_factory_byte_pram_identity};
  return FTE3600_FACTORY_BYTE_OK;
}
static int
valid (const struct fte3600_factory_byte_input *in,
       struct fte3600_factory_byte_state *s, const struct fte3600_factory_byte_io *io,
       const struct fte3600_factory_byte_observer *o, struct fte3600_factory_byte_result *r)
{
  if (!aligned (in, _Alignof (struct fte3600_factory_byte_input)) ||
      !aligned (s, _Alignof (struct fte3600_factory_byte_state)) ||
      !aligned (io, _Alignof (struct fte3600_factory_byte_io)) ||
      !aligned (r, _Alignof (struct fte3600_factory_byte_result)) ||
      (o && !aligned (o, _Alignof (struct fte3600_factory_byte_observer))))
    return 0;
  const void *fixed[] = {in, s, io, o, r};
  const size_t fixed_sizes[] = {sizeof (*in), sizeof (*s), sizeof (*io), sizeof (*o), sizeof (*r)};
  for (size_t i = 0; i < 5; ++i)
    {
      if (i == 3 && !o)
        continue;
      if (!extent (fixed[i], fixed_sizes[i]))
        return 0;
      for (size_t j = 0; j < i; ++j)
        if (fixed[j] && overlap (fixed[i], fixed_sizes[i], fixed[j], fixed_sizes[j]))
          return 0;
    }
  if (!valid_io (io) || (o && !o->phase) ||
      in->initial_model < FTE3600_MODEL_UNKNOWN || in->initial_model > FTE3600_MODEL_FT9365 ||
      !in->pram.length || in->pram.length > FTE3600_FT9368_PRAM_MAX || in->pram.length % 4 ||
      in->pram.capacity < ((in->pram.length + 127) / 128) * 128 ||
      !in->application.length || in->application.length > FTE3600_FT9368_APPLICATION_MAX ||
      in->application.length % 2 || in->application.capacity < in->application.length)
    return 0;
  const void *objects[] = {in, s, io, o, r, in->pram.data, in->application.data};
  const size_t sizes[] = {sizeof (*in), sizeof (*s), sizeof (*io), sizeof (*o), sizeof (*r),
                          in->pram.capacity, in->application.capacity};
  for (size_t i = 0; i < 7; ++i)
    {
      if (i == 3 && !o)
        continue;
      if (!extent (objects[i], sizes[i]))
        return 0;
      for (size_t j = 0; j < i; ++j)
        if (objects[j] && overlap (objects[i], sizes[i], objects[j], sizes[j]))
          return 0;
    }
  return 1;
}
static void
phase (struct operation *op, enum fte3600_factory_byte_phase p, int entering, int has_status, uint32_t status)
{
  if (op->observer)
    op->observer->phase (op->observer->context, p, entering, has_status, status, op->r);
}
static uint32_t
spi (void *c, uint8_t *f, size_t n)
{
  struct operation *op = c;
  uint16_t command = n >= 4 ? (uint16_t) ((uint16_t) f[0] << 8 | f[1]) : 0;
  size_t payload = n >= 4 ? (size_t) f[2] * 256 + f[3] : 0;

  ++op->r->physical_calls;
  ++op->r->event_count;
  if (f[0] != 0x70 && f[0] != 0x71)
    ++op->r->byte_protocol_calls;
  if (command == 0x9180)
    ++op->r->information_replies_consumed;
  /* Diagnostic poll expectations follow the loader's requested block, not
   * assumptions about successful writes or physical device retention. */
  if (op->application)
    {
      if (command == 0x6100)
        op->polling = 1;
      if (command == 0xab00 && n == 10)
        op->destination = (uint32_t) f[7] * 65536 + (uint32_t) f[8] * 256 + f[9];
      if (command == 0xbf00 && payload)
        {
          op->polling = 2;
          op->acknowledgement = (uint16_t) (0x1000 + op->destination / payload);
        }
    }
  uint32_t status = op->io->spi (op->io->context, f, n);
  if (negative (status))
    {
      ++op->r->transport_failures;
      op->r->last_transport_failure = status;
    }
  else if (op->version && command == 0x9180 && payload == 32 &&
           (f[26] != 0x93 || f[27] != 0x68))
    {
      ++op->r->version_identity_mismatches;
    }
  else if (op->application && command == 0x6a80 && n == 9)
    {
      uint16_t value = (uint16_t) ((uint16_t) f[7] << 8 | f[8]);
      if (op->polling == 1 && value != 0xf0aa)
        ++op->r->erase_poll_mismatches;
      if (op->polling == 2 && value != op->acknowledgement)
        ++op->r->program_poll_mismatches;
    }
  return status;
}
static void
wait_ms (void *c, uint32_t n)
{
  struct operation *op = c;

  ++op->r->event_count;
  op->r->clock_us += (uint64_t) n * 1000;
  op->io->wait_ms (op->io->context, n);
}
static void
wait_us (void *c, uint32_t n)
{
  struct operation *op = c;

  ++op->r->event_count;
  op->r->clock_us += n;
  op->io->wait_us (op->io->context, n);
}
static uint32_t
gpio (void *c, int level)
{
  struct operation *op = c;

  ++op->r->event_count;
  uint32_t status = op->io->gpio (op->io->context, level);
  if (negative (status))
    {
      ++op->r->gpio_failures;
      op->r->last_gpio_failure = status;
    }
  return status;
}
static void
reset_observation (void *c, int entering, uint32_t status)
{
  struct operation *op = c;

  if (entering)
    ++op->r->reset_calls;
  else
    op->r->last_reset_status = status;
  phase (op, FTE3600_BYTE_RESET, entering, !entering, status);
}
static uint64_t
no_notification (void *c)
{
  (void) c;
  return 0;
}
static void
reset (struct operation *op, const struct fte3600_ft9368_io *io)
{
  const struct fte3600_ft9368_lifecycle_state state = {0, 0};
  const struct fte3600_ft9368_lifecycle_io reset_io = {*io, op, gpio, NULL, no_notification};

  reset_observation (op, 1, 0);
  uint32_t status = fte3600_ft9368_lifecycle_reset (&state, &reset_io);
  reset_observation (op, 0, status);
}
static uint16_t
probe (struct operation *op, const struct fte3600_ft9368_io *io)
{
  phase (op, FTE3600_BYTE_PROBE, 1, 0, 0);
  uint16_t id = fte3600_ft9368_probe (io);
  phase (op, FTE3600_BYTE_PROBE, 0, 1, id);
  return id;
}
static enum fte3600_factory_byte_error
load (struct operation *op,
      struct fte3600_factory_byte_state *state, const struct fte3600_ft9368_io *byte_io,
      int check_version)
{
  const struct fte3600_ft9368_boot_io boot_io = {*byte_io, op, gpio, op, wait_us};
  const struct fte3600_ft9368_boot_observer observer = {op, reset_observation};
  uint32_t result = 0;

  phase (op, FTE3600_BYTE_LOAD, 1, 0, 0);
  if (check_version)
    {
      phase (op, FTE3600_BYTE_VERSION, 1, 0, 0);
      op->version = 1;
      result = fte3600_ft9368_check_version (state->selected.expected_version, byte_io);
      op->version = 0;
      op->r->version_status = result;
      if (negative (result))
        ++op->r->version_failures;
      phase (op, FTE3600_BYTE_VERSION, 0, 1, result);
      if (!negative (result))
        goto done;
    }
  for (unsigned int i = 0; i < 5; ++i)
    {
      phase (op, FTE3600_BYTE_PRAM, 1, 0, 0);
      ++op->r->pram_attempts;
      enum fte3600_ft9368_boot_error error = fte3600_ft9368_boot_pram_observed (
        &state->selected.pram, &state->attempt_counter, &boot_io, &observer, &result);
      phase (op, FTE3600_BYTE_PRAM, 0, error == FTE3600_FT9368_BOOT_OK, result);
      if (error)
        {
          phase (op, FTE3600_BYTE_LOAD, 0, 0, 0);
          return FTE3600_FACTORY_BYTE_PRAM_READ_ERROR;
        }
      op->r->last_pram_status = result;
      if (result)
        {
          ++op->r->pram_failures;
          continue;
        }
      phase (op, FTE3600_BYTE_APPLICATION, 1, 0, 0);
      ++op->r->application_attempts;
      op->application = 1;
      op->polling = 0;
      error = fte3600_ft9368_boot_application (&state->selected.application, &boot_io, &result);
      op->application = 0;
      phase (op, FTE3600_BYTE_APPLICATION, 0, error == FTE3600_FT9368_BOOT_OK, result);
      if (error)
        {
          phase (op, FTE3600_BYTE_LOAD, 0, 0, 0);
          return FTE3600_FACTORY_BYTE_APPLICATION_READ_ERROR;
        }
      op->r->last_application_status = result;
      if (result)
        {
          ++op->r->application_failures;
          if (result == 1)
            ++op->r->boot_identity_failures;
          if (result == 2)
            ++op->r->erase_failures;
          if (result == 3)
            ++op->r->program_failures;
          if (result == 5)
            ++op->r->checksum_failures;
          continue;
        }
      reset (op, byte_io);
      wait_ms (op, 400);
      if (probe (op, byte_io) == 0x9368)
        goto done;
      ++op->r->postload_identity_failures;
    }
  op->r->status = UINT32_C (0xc0000001);
done:
  phase (op, FTE3600_BYTE_LOAD, 0, 1, op->r->status);
  return FTE3600_FACTORY_BYTE_OK;
}
enum fte3600_factory_byte_error
fte3600_factory_byte_startup_run (
  const struct fte3600_factory_byte_input *in, struct fte3600_factory_byte_state *s,
  const struct fte3600_factory_byte_io *io, const struct fte3600_factory_byte_observer *o,
  struct fte3600_factory_byte_result *r)
{
  if (!valid (in, s, io, o, r))
    return FTE3600_FACTORY_BYTE_INVALID_ARGUMENT;
  memset (&s->selected, 0, sizeof (s->selected));
  memset (r, 0, sizeof (*r));
  r->model = in->initial_model;
  struct operation op = {.io = io, .observer = o, .r = r};
  const struct fte3600_ft9368_io byte_io = {&op, spi, wait_ms};
  for (int b = FTE3600_BOUNDARY_PLATFORM; b <= FTE3600_BOUNDARY_RESOURCES; ++b)
    {
      uint32_t status = io->boundary (io->context, (enum fte3600_factory_boundary) b);
      if (b == FTE3600_BOUNDARY_INTERFACE ? status == UINT32_C (0xc0000001) : negative (status))
        {
          r->status = status;
          return FTE3600_FACTORY_BYTE_OK;
        }
    }
  int selected = 0;
  if (in->scene == 2)
    {
      r->contact_pending = (uint8_t) !negative (io->boundary (io->context, FTE3600_BOUNDARY_INITIAL_CONTACT));
      phase (&op, FTE3600_BYTE_SHORT_DETECT, 1, 0, 0);
      selected = fte3600_ft9368_shortcut (&byte_io);
      phase (&op, FTE3600_BYTE_SHORT_DETECT, 0, 1, (uint32_t) selected);
      if (selected)
        r->contact_pending = 1;
    }
  for (unsigned int i = 0; !selected && i < 2; ++i)
    selected = probe (&op, &byte_io) == 0x9368;
  if (!selected)
    return FTE3600_FACTORY_BYTE_UNSUPPORTED;
  r->model = FTE3600_MODEL_FT9368;
  phase (&op, FTE3600_BYTE_DISCARD, 1, 0, 0);
  memset (&s->selected, 0, sizeof (s->selected));
  phase (&op, FTE3600_BYTE_DISCARD, 0, 0, 0);
  phase (&op, FTE3600_BYTE_SELECT, 1, 0, 0);
  (void) fte3600_factory_ft9368_defaults (&s->selected, io);
  r->sensor_constructed = 1;
  phase (&op, FTE3600_BYTE_SELECT, 0, 1, 0);
  s->selected.pram = in->pram;
  s->selected.application = in->application;
  phase (&op, FTE3600_BYTE_BIND, 1, 0, 0);
  s->selected.bound = 1;
  phase (&op, FTE3600_BYTE_BIND, 0, 0, 0);
  enum fte3600_factory_byte_error error = load (&op, s, &byte_io, 1);
  if (error)
    return error;
  uint8_t discarded[32];
  (void) fte3600_ft9368_command (0xf680, 4, discarded, sizeof (discarded), &byte_io);
  wait_ms (&op, 100);
  (void) io->boundary (io->context, FTE3600_BOUNDARY_FRAMEWORK_INIT);
  (void) io->boundary (io->context, FTE3600_BOUNDARY_REGISTRY_READ);
  if (in->resume_cleanup)
    {
      wait_ms (&op, 10);
      (void) io->boundary (io->context, FTE3600_BOUNDARY_REGISTRY_CLEAR);
      r->contact_pending = 0;
      (void) fte3600_ft9368_info (32, discarded, sizeof (discarded), &byte_io);
      (void) fte3600_ft9368_info (32, discarded, sizeof (discarded), &byte_io);
      (void) fte3600_ft9368_clear (&byte_io);
    }
  (void) io->boundary (io->context, FTE3600_BOUNDARY_FRAMEWORK_FINISH);
  return FTE3600_FACTORY_BYTE_OK;
}

enum fte3600_factory_byte_error
fte3600_factory_byte_load_selected (
  struct fte3600_factory_byte_state *s, int check_version,
  const struct fte3600_factory_byte_io *io,
  const struct fte3600_factory_byte_observer *o,
  struct fte3600_factory_byte_result *r)
{
  if (!aligned (s, _Alignof (struct fte3600_factory_byte_state)))
    return FTE3600_FACTORY_BYTE_INVALID_ARGUMENT;
  const struct fte3600_factory_byte_input in = {0, 0, FTE3600_MODEL_FT9368,
                                                s->selected.pram, s->selected.application};
  if (!valid (&in, s, io, o, r) || (check_version != 0 && check_version != 1) ||
      s->selected.model != FTE3600_MODEL_FT9368 || !s->selected.bound)
    return FTE3600_FACTORY_BYTE_INVALID_ARGUMENT;
  struct operation op = {.io = io, .observer = o, .r = r};
  const struct fte3600_ft9368_io byte_io = {&op, spi, wait_ms};
  r->status = 0;
  return load (&op, s, &byte_io, check_version);
}
