/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_factory_legacy_startup.h"
#include "fte3600_discovery.h"
#include "fte3600_ft9368.h"
#include <string.h>

static const struct fte3600_factory_legacy_identity identities[] = {
  {14184, "ca4490163a1754639e945da3bd6ecbb4a498138962d611fc825dc129819efc46"},
  {10312, "48d658d588c297a5d749c1f4bd6a0f5bd3d6ede59040e1674f95fe9db08eede2"},
  {10396, "027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f"},
  {11934, "4a62b5d9a8a7b4620bec7843763b030632a5a37a54c4cb7ed2cd96d5c7451e79"}
};
struct operation
{
  const struct fte3600_factory_startup_io      *io;
  const struct fte3600_factory_legacy_observer *observer;
  struct fte3600_factory_legacy_result         *r;
  struct fte3600_factory_startup_result         word;
  uint32_t                                      last_status;
  int                                           unavailable;
};
static int
negative (uint32_t n)
{
  return (n >> 31) != 0;
}
static int
aligned (const void *p, size_t a)
{
  return p && (uintptr_t) p % a == 0;
}
static int
overlap (const void *a, size_t an, const void *b, size_t bn)
{
  uintptr_t x = (uintptr_t) a, y = (uintptr_t) b;

  return x <= y ? y - x < an : x - y < bn;
}
static int
valid_io (const struct fte3600_factory_startup_io *io)
{
  return io->spi && io->wait_ms && io->gpio && io->boundary;
}
static enum fte3600_sensor
sensor (enum fte3600_factory_model m)
{
  switch (m)
    {
    case FTE3600_MODEL_FT9338: return FTE3600_FT9338;

    case FTE3600_MODEL_FT9348: return FTE3600_FT9348;

    case FTE3600_MODEL_FT9361: return FTE3600_FT9361;

    case FTE3600_MODEL_FT9536: return FTE3600_FT9536;

    case FTE3600_MODEL_UNKNOWN:
    case FTE3600_MODEL_FT9368:
    case FTE3600_MODEL_FW9369:
    case FTE3600_MODEL_FT9769:
    case FTE3600_MODEL_FT9365:
    default: return FTE3600_SENSOR_UNKNOWN;
    }
}
static enum fte3600_factory_model
model (enum fte3600_sensor s)
{
  return s == FTE3600_FT9536 ? FTE3600_MODEL_FT9536 : (enum fte3600_factory_model) s;
}
enum fte3600_factory_legacy_error
fte3600_factory_legacy_state_defaults (
  struct fte3600_factory_legacy_state *s)
{
  if (!aligned (s, _Alignof (struct fte3600_factory_legacy_state)))
    return FTE3600_FACTORY_LEGACY_INVALID_ARGUMENT;
  memset (s, 0, sizeof (*s));
  s->irq_enabled = 1;
  return FTE3600_FACTORY_LEGACY_OK;
}
enum fte3600_factory_legacy_error
fte3600_factory_legacy_defaults (
  enum fte3600_factory_model m, struct fte3600_factory_legacy_selected *s,
  const struct fte3600_factory_startup_io *io)
{
  enum fte3600_sensor sn = sensor (m);

  if (!aligned (s, _Alignof (struct fte3600_factory_legacy_selected)) ||
      !aligned (io, _Alignof (struct fte3600_factory_startup_io)) ||
      !valid_io (io) || !sn || overlap (s, sizeof (*s), io, sizeof (*io)))
    return FTE3600_FACTORY_LEGACY_INVALID_ARGUMENT;
  struct fte3600_legacy_boot_profile p = fte3600_legacy_boot_defaults (sn);
  struct fte3600_legacy_geometry g = fte3600_legacy_geometry (sn);
  *s = (struct fte3600_factory_legacy_selected){.model = m, .width = (uint16_t) g.width,
                                                .height = (uint16_t) g.height, .firmware_version = p.expected_firmware,
                                                .agc_version = p.expected_agc, .mode_register = sn == FTE3600_FT9536 ? 0x47 : 0x76,
                                                .startup_wait_ms = p.startup_wait_ms, .original_application = &identities[sn - 1], .board = io};
  return FTE3600_FACTORY_LEGACY_OK;
}
static int
valid (const struct fte3600_factory_legacy_input *in,
       struct fte3600_factory_legacy_state *s, const struct fte3600_factory_startup_io *io,
       const struct fte3600_factory_legacy_observer *o, struct fte3600_factory_legacy_result *r, int optional)
{
  if (!aligned (in, _Alignof (struct fte3600_factory_legacy_input)) ||
      !aligned (s, _Alignof (struct fte3600_factory_legacy_state)) ||
      !aligned (io, _Alignof (struct fte3600_factory_startup_io)) ||
      !aligned (r, _Alignof (struct fte3600_factory_legacy_result)) ||
      (o && !aligned (o, _Alignof (struct fte3600_factory_legacy_observer))))
    return 0;
  const void *fixed[] = {in, s, io, o, r};
  const size_t lengths[] = {sizeof (*in), sizeof (*s), sizeof (*io), sizeof (*o), sizeof (*r)};
  for (size_t i = 0; i < 5; ++i)
    if (fixed[i])
      {
        if (lengths[i] > UINTPTR_MAX - (uintptr_t) fixed[i])
          return 0;
        for (size_t j = 0; j < i; ++j)
          if (fixed[j] && overlap (fixed[i], lengths[i], fixed[j], lengths[j]))
            return 0;
      }
  if (!valid_io (io) || (o && !o->phase) ||
      in->initial_model < FTE3600_MODEL_UNKNOWN || in->initial_model > FTE3600_MODEL_FT9365 ||
      ((!in->firmware || !in->firmware_length) && !(optional && !in->firmware && !in->firmware_length)) || in->firmware_length > 14184 ||
      in->firmware_length > UINTPTR_MAX - (uintptr_t) in->firmware)
    return 0;
  for (size_t i = 0; i < 5; ++i)
    if (fixed[i] && in->firmware_length &&
        overlap (in->firmware, in->firmware_length, fixed[i], lengths[i]))
      return 0;
  return 1;
}
static void
phase (struct operation *op, enum fte3600_factory_legacy_phase p, int entry, int defined, uint32_t status)
{
  if (op->observer)
    op->observer->phase (op->observer->context, p, entry, defined, status, op->r);
}
static uint32_t
spi (void *c, uint8_t *f, size_t n)
{
  struct operation *op = c;

  ++op->r->physical_calls;
  ++op->r->event_count;
  op->last_status = op->io->spi (op->io->context, f, n);
  if (negative (op->last_status))
    {
      ++op->r->transport_failures;
      op->r->last_transport_failure = op->last_status;
      if (op->last_status == FTE3600_FACTORY_LEGACY_UNAVAILABLE)
        op->unavailable = 1;
    }
  return op->last_status;
}
static uint32_t
byte_spi (void *c, uint8_t *f, size_t n)
{
  struct operation *op = c;

  ++op->r->byte_protocol_calls;
  if (n >= 2 && f[0] == 0x91 && f[1] == 0x80)
    ++op->r->information_replies_consumed;
  return spi (c, f, n);
}
static void
wait_ms (void *c, uint32_t n)
{
  struct operation *op = c;

  ++op->r->event_count;
  op->r->clock_us += (uint64_t) n * 1000;
  op->io->wait_ms (op->io->context, n);
}
static uint32_t
gpio (void *c, unsigned int level)
{
  struct operation *op = c;

  ++op->r->event_count;
  op->last_status = op->io->gpio (op->io->context, level);
  if (negative (op->last_status))
    {
      ++op->r->gpio_failures;
      op->r->last_gpio_failure = op->last_status;
    }
  return op->last_status;
}
static int
raw (uint32_t status)
{
  return !negative (status) ? 0 : status == UINT32_C (0xc000000e) ? FTE3600_LEGACY_BOOT_RAW_DEVICE_ABSENT : 1;
}
static int
legacy_spi (void *c, const uint8_t *tx, uint8_t *rx, size_t n)
{
  memcpy (rx, tx, n);
  return raw (spi (c, rx, n));
}
static int
legacy_gpio (void *c, int level)
{
  return raw (gpio (c, (unsigned int) level));
}
static int
legacy_release_and_sync (void *c, int *gpio_status)
{
  struct operation *op = c;
  uint32_t gpio_result;

  op->last_status = op->io->release_and_sync (op->io->context, &gpio_result);
  ++op->r->event_count;
  if (negative (gpio_result))
    {
      ++op->r->gpio_failures;
      op->r->last_gpio_failure = gpio_result;
    }
  else
    {
      ++op->r->physical_calls;
      ++op->r->event_count;
      if (negative (op->last_status))
        {
          ++op->r->transport_failures;
          op->r->last_transport_failure = op->last_status;
          if (op->last_status == FTE3600_FACTORY_LEGACY_UNAVAILABLE)
            op->unavailable = 1;
        }
    }
  *gpio_status = raw (gpio_result);
  return raw (op->last_status);
}
static uint32_t
clock_ms (void *c)
{
  return (uint32_t) (((struct operation *) c)->r->clock_us / 1000);
}
static uint32_t
runtime_status (struct operation *op, enum fte3600_legacy_status s)
{
  return s == FTE3600_LEGACY_OK ? 0 : op->last_status;
}
static void
arm_idle (void *c, int entry, enum fte3600_legacy_status status)
{
  struct operation *op = c;

  phase (op, FTE3600_FL_IDLE, entry, !entry, entry ? 0 : runtime_status (op, status));
}
static uint32_t
boot_status (struct operation *op, enum fte3600_legacy_boot_status s)
{
  switch (s)
    {
    case FTE3600_LEGACY_BOOT_OK: return 0;

    case FTE3600_LEGACY_BOOT_DEVICE_ABSENT: return UINT32_C (0xc000000e);

    case FTE3600_LEGACY_BOOT_IO_ERROR: return op->last_status;

    case FTE3600_LEGACY_BOOT_VERSION_MISMATCH: return UINT32_C (0xc000000d);

    case FTE3600_LEGACY_BOOT_FAILED:
    case FTE3600_LEGACY_BOOT_INVALID_INPUT:
    case FTE3600_LEGACY_BOOT_NO_MEMORY:
    case FTE3600_LEGACY_BOOT_NOT_RUN:
    default: return UINT32_C (0xc0000001);
    }
}
static void
reset_observation (void *c, int entry, int status)
{
  struct operation *op = c;
  uint32_t result = status ? op->last_status : 0;

  if (entry)
    ++op->r->reset_calls;
  else
    op->r->last_reset_status = result;
  phase (op, FTE3600_FL_RESET, entry, !entry, result);
}
static void
word_observation (void *c, enum fte3600_factory_startup_phase p, int entry,
                  int defined, uint32_t status, const struct fte3600_factory_startup_result *r)
{
  struct operation *op = c;

  (void) r;
  enum fte3600_factory_legacy_phase dest = p == FTE3600_PHASE_WORD_WAKE ? FTE3600_FL_WORD_WAKE :
                                           p == FTE3600_PHASE_WORD_PROBE ? FTE3600_FL_WORD_PROBE : FTE3600_FL_RESET;
  if (p == FTE3600_PHASE_RESET)
    {
      if (entry)
        ++op->r->reset_calls;
      else
        op->r->last_reset_status = status;
    }
  phase (op, dest, entry, defined, status);
}
static enum fte3600_factory_legacy_error
word (struct operation *op,
      enum fte3600_factory_word_operation which, uint32_t settle, uint16_t *id)
{
  const struct fte3600_factory_startup_io io = {op, spi, wait_ms, gpio, op->io->boundary, NULL};
  const struct fte3600_factory_startup_observer o = {op, word_observation};
  enum fte3600_factory_startup_error error = fte3600_factory_word_operation (which, settle, &io, &o, &op->word, id);

  op->r->discovery_exhausted = op->word.discovery_exhausted;
  op->r->wake_unacknowledged = op->word.wake_unacknowledged;
  op->r->negotiation_exhausted = op->word.negotiation_exhausted;
  if (op->unavailable)
    return FTE3600_FACTORY_LEGACY_RESPONSE_UNAVAILABLE;
  return error ? FTE3600_FACTORY_LEGACY_WORD_READ_ERROR : FTE3600_FACTORY_LEGACY_OK;
}
static uint16_t
byte_probe (struct operation *op, const struct fte3600_ft9368_io *io)
{
  phase (op, FTE3600_FL_BYTE_PROBE, 1, 0, 0);
  uint16_t id = fte3600_ft9368_probe (io);
  phase (op, FTE3600_FL_BYTE_PROBE, 0, 1, id);
  return id;
}
static uint32_t
read_legacy (struct operation *op, uint8_t address, uint8_t *value, size_t n)
{
  uint8_t f[6] = {0x10, 0xef, address, 0, 0, 0};
  uint32_t status = spi (op, f, n + 4);

  if (!negative (status))
    memcpy (value, f + 4, n);
  return status;
}
static uint32_t
ping_once (struct operation *op)
{
  uint8_t tx = 0x70, reply[2];
  uint32_t status = spi (op, &tx, 1);

  if (!negative (status))
    {
      wait_ms (op, 5);
      tx = 0x70;
      (void) spi (op, &tx, 1);
    }
  status = read_legacy (op, 0x20, reply, 2);
  if (negative (status))
    return status;
  return reply[0] == 0xa5 && reply[1] == 0x5a ? 0 : UINT32_C (0xc0000001);
}
static uint32_t
ping (struct operation *op)
{
  uint32_t status = 0;

  phase (op, FTE3600_FL_PING, 1, 0, 0);
  for (unsigned int i = 0; i < 6; ++i)
    {
      if (i)
        wait_ms (op, 5);
      phase (op, FTE3600_FL_PING_ONCE, 1, 0, 0);
      status = ping_once (op);
      phase (op, FTE3600_FL_PING_ONCE, 0, 1, status);
      if (!negative (status) || op->unavailable)
        break;
    }
  phase (op, FTE3600_FL_PING, 0, 1, status);
  return status;
}
static uint32_t
application (struct operation *op)
{
  uint8_t width = 0, height = 0;

  phase (op, FTE3600_FL_APPLICATION, 1, 0, 0);
  uint32_t status = read_legacy (op, 0x14, &width, 1);
  if (!negative (status))
    status = read_legacy (op, 0x15, &height, 1);
  if (!negative (status))
    {
      if (width == 88 && height == 88)
        op->r->model = FTE3600_MODEL_FT9338;
      if (width == 96 && height == 96)
        op->r->model = FTE3600_MODEL_FT9348;
      if (width == 64 && height == 80)
        op->r->model = FTE3600_MODEL_FT9361;
      if (width == 64 && height == 128)
        op->r->model = FTE3600_MODEL_FT9536;
    }
  phase (op, FTE3600_FL_APPLICATION, 0, 1, status);
  return status;
}
static uint32_t
factory (struct operation *op, struct fte3600_factory_legacy_state *s,
         const struct fte3600_recovery_io *io, uint32_t scene, int retained)
{
  phase (op, FTE3600_FL_FACTORY, 1, 0, 0);
  phase (op, FTE3600_FL_FACTORY_RETRY, 1, 0, 0);
  phase (op, FTE3600_FL_IDENTITY, 1, 0, 0);
  const struct fte3600_reset_observer o = {op, reset_observation};
  struct fte3600_identity_result id = retained ?
                                      fte3600_discover_legacy_retained (io, &o, &s->boot_edition) : fte3600_discover_legacy_observed (io, &o);
  uint32_t status = id.status == FTE3600_OK ? 0 : id.status == FTE3600_TRANSPORT_ERROR ?
                    op->last_status : UINT32_C (0xc0000001);
  if (id.status == FTE3600_OK)
    {
      op->r->model = model (id.sensor);
      s->boot_edition = id.boot == FTE3600_BOOT_B;
    }
  phase (op, FTE3600_FL_IDENTITY, 0, 1, status);
  phase (op, FTE3600_FL_FACTORY_RETRY, 0, 1, status);
  if (!negative (status) && scene == 2 && (id.sensor == FTE3600_FT9338 || id.sensor == FTE3600_FT9536))
    status = UINT32_C (0xc0000001);
  phase (op, FTE3600_FL_FACTORY, 0, 1, status);
  return status;
}
static void
boot_observation (void *c, enum fte3600_legacy_boot_phase p, int entry, enum fte3600_legacy_boot_status status)
{
  struct operation *op = c;
  const enum fte3600_factory_legacy_phase map[] = {FTE3600_FL_IDLE, FTE3600_FL_VERSION,
                                                   FTE3600_FL_LOAD, FTE3600_FL_CONFIG, FTE3600_FL_ARM, FTE3600_FL_BOOT_A,
                                                   FTE3600_FL_BOOT_B, FTE3600_FL_UPLOAD_A8, FTE3600_FL_RESET};
  uint32_t value = entry ? 0 : boot_status (op, status);

  if (p == FTE3600_LEGACY_PHASE_RESET)
    {
      if (entry)
        ++op->r->reset_calls;
      else
        op->r->last_reset_status = value;
    }
  if (p == FTE3600_LEGACY_PHASE_BOOT_A)
    {
      if (entry)
        ++op->r->load_attempts;
      else if (negative (value))
        ++op->r->load_failures;
    }
  if (!entry && negative (value))
    {
      if (p == FTE3600_LEGACY_PHASE_VERSION)
        ++op->r->version_failures;
      if (p == FTE3600_LEGACY_PHASE_ARM)
        ++op->r->arm_failures;
    }
  phase (op, map[p], entry, !entry && status != FTE3600_LEGACY_BOOT_NO_MEMORY &&
         status != FTE3600_LEGACY_BOOT_INVALID_INPUT, value);
}
static void
construct (struct operation *op, struct fte3600_factory_legacy_state *s,
           const struct fte3600_factory_legacy_input *in)
{
  phase (op, FTE3600_FL_SELECT, 1, 0, 0);
  (void) fte3600_factory_legacy_defaults (op->r->model, &s->selected, op->io);
  s->selected.application = in->firmware;
  s->selected.application_length = in->firmware_length;
  s->selected.bound = 1;
  op->r->sensor_constructed = 1;
  ++op->r->constructions;
  phase (op, FTE3600_FL_SELECT, 0, 1, 0);
}
enum fte3600_factory_legacy_error
fte3600_factory_legacy_startup_run (
  const struct fte3600_factory_legacy_input *in, struct fte3600_factory_legacy_state *s,
  const struct fte3600_factory_startup_io *io, const struct fte3600_factory_legacy_observer *o,
  struct fte3600_factory_legacy_result *r)
{
  if (!valid (in, s, io, o, r, 0))
    return FTE3600_FACTORY_LEGACY_INVALID_ARGUMENT;
  memset (&s->selected, 0, sizeof (s->selected));
  memset (r, 0, sizeof (*r));
  r->model = in->initial_model;
  struct operation op = {.io = io, .observer = o, .r = r};
  const struct fte3600_ft9368_io byte_io = {&op, byte_spi, wait_ms};
  const struct fte3600_recovery_io legacy_io = {&op, legacy_gpio, legacy_spi, wait_ms, clock_ms,
                                                io->release_and_sync ? legacy_release_and_sync : NULL};
  for (int b = FTE3600_BOUNDARY_PLATFORM; b <= FTE3600_BOUNDARY_RESOURCES; ++b)
    {
      uint32_t status = io->boundary (io->context, (enum fte3600_factory_boundary) b);
      if (b == FTE3600_BOUNDARY_INTERFACE ? status == UINT32_C (0xc0000001) : negative (status))
        {
          r->status = status;
          return FTE3600_FACTORY_LEGACY_OK;
        }
    }
  if (in->scene == 2)
    {
      r->contact_pending = (uint8_t) !negative (io->boundary (io->context, FTE3600_BOUNDARY_INITIAL_CONTACT));
      phase (&op, FTE3600_FL_SHORT_DETECT, 1, 0, 0);
      int found = fte3600_ft9368_shortcut (&byte_io);
      phase (&op, FTE3600_FL_SHORT_DETECT, 0, 1, (uint32_t) found);
      if (op.unavailable)
        return FTE3600_FACTORY_LEGACY_RESPONSE_UNAVAILABLE;
      if (found)
        return FTE3600_FACTORY_LEGACY_UNSUPPORTED;
    }
  for (unsigned int round = 0; round < 3; ++round)
    {
      if (byte_probe (&op, &byte_io) == 0x9368)
        return FTE3600_FACTORY_LEGACY_UNSUPPORTED;
      if (op.unavailable)
        return FTE3600_FACTORY_LEGACY_RESPONSE_UNAVAILABLE;
      if (round == 2)
        {
          (void) word (&op, FTE3600_FACTORY_WORD_RESET, 300, NULL);
          (void) byte_probe (&op, &byte_io); /* The extra identity is intentionally ignored. */
          if (op.unavailable)
            return FTE3600_FACTORY_LEGACY_RESPONSE_UNAVAILABLE;
        }
      if (byte_probe (&op, &byte_io) == 0x9368)
        return FTE3600_FACTORY_LEGACY_UNSUPPORTED;
      if (op.unavailable)
        return FTE3600_FACTORY_LEGACY_RESPONSE_UNAVAILABLE;
      for (unsigned int i = 0; i < 2; ++i)
        {
          uint16_t id = 0;
          enum fte3600_factory_legacy_error error = word (&op, FTE3600_FACTORY_WORD_WAKE, 0, NULL);
          if (error)
            return error;
          error = word (&op, FTE3600_FACTORY_WORD_PROBE, 0, &id);
          if (error)
            return error;
          if (id == 0x9362 || id == 0x9365 || id == 0x9391 || id == 0x9392)
            return FTE3600_FACTORY_LEGACY_UNSUPPORTED;
          if (!i)
            (void) word (&op, FTE3600_FACTORY_WORD_RESET, 10, NULL);
        }
      if (!round)
        continue;
      int app = 0;
      if (round == 1 && !negative (ping (&op)))
        {
          wait_ms (&op, 350);
          app = !negative (application (&op));
        }
      if (op.unavailable)
        return FTE3600_FACTORY_LEGACY_RESPONSE_UNAVAILABLE;
      if (!app && negative (factory (&op, s, &legacy_io, in->scene, 0)))
        {
          if (op.unavailable)
            return FTE3600_FACTORY_LEGACY_RESPONSE_UNAVAILABLE;
          continue;
        }
      if (op.unavailable)
        return FTE3600_FACTORY_LEGACY_RESPONSE_UNAVAILABLE;
      if (!sensor (r->model))
        return FTE3600_FACTORY_LEGACY_UNSUPPORTED;
      construct (&op, s, in);
      if (app && !r->contact_pending)
        {
          const struct fte3600_legacy_arm_observer arm_observer = {&op, arm_idle};
          phase (&op, FTE3600_FL_ARM, 1, 0, 0);
          r->status = runtime_status (&op, fte3600_legacy_arm_observed (sensor (r->model), 1, &legacy_io, &arm_observer));
          if (negative (r->status))
            ++r->arm_failures;
          phase (&op, FTE3600_FL_ARM, 0, 1, r->status);
          if (op.unavailable)
            return FTE3600_FACTORY_LEGACY_RESPONSE_UNAVAILABLE;
          if (negative (r->status))
            return FTE3600_FACTORY_LEGACY_OK;
        }
      else if (!app && r->model == FTE3600_MODEL_FT9338)
        {
          struct fte3600_legacy_boot_profile profile = fte3600_legacy_boot_defaults (FTE3600_FT9338);
          profile.boot = s->boot_edition ? FTE3600_RECOVERY_BOOT_B : FTE3600_RECOVERY_BOOT_A;
          const struct fte3600_legacy_boot_observer observer = {&op, boot_observation};
          phase (&op, FTE3600_FL_INITIALIZE, 1, 0, 0);
          ++r->initializer_calls;
          enum fte3600_legacy_boot_status status = fte3600_legacy_boot_run_observed (&profile,
                                                                                     FTE3600_LEGACY_BOOT_INITIALIZE, in->firmware, in->firmware_length, &s->irq_enabled, &legacy_io, &observer);
          r->last_initializer_status = boot_status (&op, status);
          phase (&op, FTE3600_FL_INITIALIZE, 0, status != FTE3600_LEGACY_BOOT_NO_MEMORY &&
                 status != FTE3600_LEGACY_BOOT_INVALID_INPUT, r->last_initializer_status);
          if (status == FTE3600_LEGACY_BOOT_NO_MEMORY)
            return FTE3600_FACTORY_LEGACY_NO_MEMORY;
          if (op.unavailable)
            return FTE3600_FACTORY_LEGACY_RESPONSE_UNAVAILABLE;
          if (status != FTE3600_LEGACY_BOOT_OK)
            {
              ++r->initializer_failures;
              phase (&op, FTE3600_FL_DISCARD, 1, 0, 0);
              memset (&s->selected, 0, sizeof (s->selected));
              r->sensor_constructed = 0;
              ++r->destructions;
              phase (&op, FTE3600_FL_DISCARD, 0, 0, 0);
              continue;
            }
        }
      (void) io->boundary (io->context, FTE3600_BOUNDARY_FRAMEWORK_INIT);
      (void) io->boundary (io->context, FTE3600_BOUNDARY_REGISTRY_READ);
      if (in->resume_cleanup)
        {
          uint8_t discarded[32];
          wait_ms (&op, 10);
          (void) io->boundary (io->context, FTE3600_BOUNDARY_REGISTRY_CLEAR);
          r->contact_pending = 0;
          (void) fte3600_ft9368_info (32, discarded, sizeof (discarded), &byte_io);
          (void) fte3600_ft9368_info (32, discarded, sizeof (discarded), &byte_io);
          (void) fte3600_ft9368_clear (&byte_io);
        }
      if (op.unavailable)
        return FTE3600_FACTORY_LEGACY_RESPONSE_UNAVAILABLE;
      (void) io->boundary (io->context, FTE3600_BOUNDARY_FRAMEWORK_FINISH);
      return FTE3600_FACTORY_LEGACY_OK;
    }
  return FTE3600_FACTORY_LEGACY_UNSUPPORTED;
}

static enum fte3600_factory_legacy_error
child_run (
  enum fte3600_factory_legacy_child which,
  const struct fte3600_factory_legacy_input *in, struct fte3600_factory_legacy_state *s,
  const struct fte3600_factory_startup_io *io, const struct fte3600_factory_legacy_observer *o,
  struct fte3600_factory_legacy_result *r, int optional)
{
  if (!valid (in, s, io, o, r, optional && which != FTE3600_FL_CHILD_INITIALIZE) || which < FTE3600_FL_CHILD_PING || which > FTE3600_FL_CHILD_INITIALIZE ||
      (which >= FTE3600_FL_CHILD_ARM && !sensor (r->model)))
    return FTE3600_FACTORY_LEGACY_INVALID_ARGUMENT;
  struct operation op = {.io = io, .observer = o, .r = r};
  const struct fte3600_recovery_io legacy_io = {&op, legacy_gpio, legacy_spi, wait_ms, clock_ms,
                                                io->release_and_sync ? legacy_release_and_sync : NULL};
  if (which == FTE3600_FL_CHILD_PING)
    {
      r->status = ping (&op);
    }
  else if (which == FTE3600_FL_CHILD_APPLICATION)
    {
      r->status = application (&op);
    }
  else if (which == FTE3600_FL_CHILD_FACTORY)
    {
      r->status = factory (&op, s, &legacy_io, in->scene, 1);
    }
  else if (which == FTE3600_FL_CHILD_ARM)
    {
      const struct fte3600_legacy_arm_observer observer = {&op, arm_idle};
      phase (&op, FTE3600_FL_ARM, 1, 0, 0);
      r->status = runtime_status (&op, fte3600_legacy_arm_observed (sensor (r->model), 1, &legacy_io, &observer));
      if (negative (r->status))
        ++r->arm_failures;
      phase (&op, FTE3600_FL_ARM, 0, 1, r->status);
    }
  else
    {
      struct fte3600_legacy_boot_profile profile = fte3600_legacy_boot_defaults (sensor (r->model));
      profile.boot = s->boot_edition ? FTE3600_RECOVERY_BOOT_B : FTE3600_RECOVERY_BOOT_A;
      const struct fte3600_legacy_boot_observer observer = {&op, boot_observation};
      phase (&op, FTE3600_FL_INITIALIZE, 1, 0, 0);
      ++r->initializer_calls;
      enum fte3600_legacy_boot_status status = fte3600_legacy_boot_run_observed (&profile,
                                                                                 FTE3600_LEGACY_BOOT_INITIALIZE, in->firmware, in->firmware_length, &s->irq_enabled, &legacy_io, &observer);
      r->status = r->last_initializer_status = boot_status (&op, status);
      phase (&op, FTE3600_FL_INITIALIZE, 0, status != FTE3600_LEGACY_BOOT_NO_MEMORY &&
             status != FTE3600_LEGACY_BOOT_INVALID_INPUT, r->status);
      if (status == FTE3600_LEGACY_BOOT_NO_MEMORY)
        return FTE3600_FACTORY_LEGACY_NO_MEMORY;
      if (status != FTE3600_LEGACY_BOOT_OK)
        ++r->initializer_failures;
    }
  return op.unavailable ? FTE3600_FACTORY_LEGACY_RESPONSE_UNAVAILABLE : FTE3600_FACTORY_LEGACY_OK;
}

enum fte3600_factory_legacy_error
fte3600_factory_legacy_child_run (
  enum fte3600_factory_legacy_child which, const struct fte3600_factory_legacy_input *in,
  struct fte3600_factory_legacy_state *s, const struct fte3600_factory_startup_io *io,
  const struct fte3600_factory_legacy_observer *o, struct fte3600_factory_legacy_result *r)
{
  return child_run (which, in, s, io, o, r, 0);
}
enum fte3600_factory_legacy_error
fte3600_factory_legacy_child_run_optional_firmware (
  enum fte3600_factory_legacy_child which, const struct fte3600_factory_legacy_input *in,
  struct fte3600_factory_legacy_state *s, const struct fte3600_factory_startup_io *io,
  const struct fte3600_factory_legacy_observer *o, struct fte3600_factory_legacy_result *r)
{
  return child_run (which, in, s, io, o, r, 1);
}
