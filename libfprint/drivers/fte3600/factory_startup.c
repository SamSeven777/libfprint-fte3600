/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_factory_startup.h"
#include "fte3600_ft9368.h"
#include <string.h>

struct operation
{
  const struct fte3600_factory_startup_io       *io;
  const struct fte3600_factory_startup_observer *observer;
  struct fte3600_factory_startup_result         *r;
  int                                            modern_phase;
};
static int
negative (uint32_t status)
{
  return (status >> 31) != 0;
}
static int
valid_io (const struct fte3600_factory_startup_io *io)
{
  return io && io->spi && io->wait_ms && io->gpio && io->boundary;
}
static int
overlap (const void *a, size_t an, const void *b, size_t bn)
{
  uintptr_t x = (uintptr_t) a, y = (uintptr_t) b;

  return x <= y ? y - x < an : x - y < bn;
}
static int
aligned (const void *p, size_t alignment)
{
  return p && (uintptr_t) p % alignment == 0;
}
static enum fte3600_factory_startup_error
construct (
  struct fte3600_factory_selected *s, const struct fte3600_factory_startup_io *io,
  enum fte3600_factory_model model, uint16_t width, uint16_t height)
{
  if (!aligned (s, _Alignof (struct fte3600_factory_selected)) ||
      !aligned (io, _Alignof (struct fte3600_factory_startup_io)) ||
      !valid_io (io) || overlap (s, sizeof (*s), io, sizeof (*io)))
    return FTE3600_FACTORY_STARTUP_INVALID_ARGUMENT;
  *s = (struct fte3600_factory_selected){model, width, height, NULL, NULL, 0, 0, io};
  return FTE3600_FACTORY_STARTUP_OK;
}
enum fte3600_factory_startup_error
fte3600_factory_fw9369_defaults (
  struct fte3600_factory_selected *s, const struct fte3600_factory_startup_io *io)
{
  return construct (s, io, FTE3600_MODEL_FW9369, 64, 80);
}
enum fte3600_factory_startup_error
fte3600_factory_ft9365_defaults (
  struct fte3600_factory_selected *s, const struct fte3600_factory_startup_io *io)
{
  return construct (s, io, FTE3600_MODEL_FT9365, 64, 80);
}
enum fte3600_factory_startup_error
fte3600_factory_ft9769_defaults (
  struct fte3600_factory_selected *s, const struct fte3600_factory_startup_io *io)
{
  return construct (s, io, FTE3600_MODEL_FT9769, 40, 196);
}
enum fte3600_factory_startup_error
fte3600_factory_startup_state_defaults (
  struct fte3600_factory_startup_state *s)
{
  if (!aligned (s, _Alignof (struct fte3600_factory_startup_state)))
    return FTE3600_FACTORY_STARTUP_INVALID_ARGUMENT;
  memset (s, 0, sizeof (*s));
  (void) fte3600_ft93xx_device_defaults (&s->modern);
  s->fw = (struct fte3600_fw9369_startup_state) FTE3600_FW9369_STARTUP_STATE_INITIALIZER;
  return FTE3600_FACTORY_STARTUP_OK;
}
static void
phase (struct operation *op, enum fte3600_factory_startup_phase p,
       int entering, int defined, uint32_t status)
{
  if (op->observer && op->observer->phase)
    op->observer->phase (op->observer->context, p, entering, defined, status, op->r);
}
static uint32_t
spi (void *context, uint8_t *bytes, size_t count)
{
  struct operation *op = context;

  ++op->r->physical_calls;
  ++op->r->event_count;
  uint32_t status = op->io->spi (op->io->context, bytes, count);
  if (negative (status))
    {
      ++op->r->transport_failures;
      op->r->last_transport_failure = status;
    }
  return status;
}
static uint32_t
byte_spi (void *context, uint8_t *bytes, size_t count)
{
  struct operation *op = context;

  ++op->r->byte_protocol_calls;
  return spi (context, bytes, count);
}
static void
wait_ms (void *context, uint32_t ms)
{
  struct operation *op = context;

  ++op->r->event_count;
  op->r->clock_ms += ms;
  op->io->wait_ms (op->io->context, ms);
}
static int
transfer (struct operation *op, uint8_t *bytes, size_t size)
{
  uint8_t original[12];

  memcpy (original, bytes, size);
  for (unsigned int i = 0; i < 3; ++i)
    {
      memcpy (bytes, original, size);
      if (!negative (spi (op, bytes, size)))
        return 1;
      wait_ms (op, 10);
    }
  ++op->r->discovery_exhausted;
  return 0;
}
static void
command (struct operation *op, uint8_t a, uint8_t b)
{
  uint8_t bytes[3] = {a, b, 0};

  (void) transfer (op, bytes, sizeof (bytes));
}
static uint8_t
read_byte (struct operation *op, uint8_t address)
{
  uint8_t bytes[5] = {8, 0xf7, address, 0, 0};

  (void) transfer (op, bytes, sizeof (bytes));
  return bytes[4];
}
static void
wake (struct operation *op)
{
  phase (op, FTE3600_PHASE_WORD_WAKE, 1, 0, 0);
  command (op, 0x5a, 0xa5);
  wait_ms (op, 1);
  if (read_byte (op, 0x80) != 0x50)
    {
      ++op->r->wake_unacknowledged;
      command (op, 0xc0, 0x3f);
      wait_ms (op, 1);
    }
  command (op, 0xa5, 0x5a);
  phase (op, FTE3600_PHASE_WORD_WAKE, 0, 0, 0);
}
static int
probe (struct operation *op, uint16_t *id)
{
  uint8_t bytes[12] = {4, 0xfb, 0x9a, 0x8b, 0, 1};

  phase (op, FTE3600_PHASE_WORD_PROBE, 1, 0, 0);
  for (unsigned int round = 0; round < 2; ++round)
    {
      unsigned int i;
      for (i = 0; i < 31; ++i)
        {
          uint8_t write[4] = {9, 0xf6, 0xc6, 1};
          (void) transfer (op, write, sizeof (write));
          wait_ms (op, 4);
          if (read_byte (op, 0xc6) == 1)
            break;
        }
      if (i == 31)
        ++op->r->negotiation_exhausted;
    }
  if (!transfer (op, bytes, sizeof (bytes)))
    {
      phase (op, FTE3600_PHASE_WORD_PROBE, 0, 0, 0);
      return 0;
    }
  *id = (uint16_t) ((uint16_t) bytes[6] << 8 | bytes[7]);
  phase (op, FTE3600_PHASE_WORD_PROBE, 0, 1, *id);
  return 1;
}
static void
reset (struct operation *op, uint32_t settle)
{
  uint32_t status = 0;

  phase (op, FTE3600_PHASE_RESET, 1, 0, 0);
  ++op->r->reset_calls;
  for (unsigned int i = 0; i < 3; ++i)
    {
      ++op->r->event_count;
      status = op->io->gpio (op->io->context, i != 1);
      if (negative (status))
        {
          ++op->r->gpio_failures;
          op->r->last_gpio_failure = status;
          break;
        }
      if (i < 2)
        wait_ms (op, i ? 20 : 10);
    }
  op->r->last_reset_status = status;
  phase (op, FTE3600_PHASE_RESET, 0, 1, status);
  if (settle)
    wait_ms (op, settle);
}
static enum fte3600_factory_model
model (uint16_t id)
{
  if (id == 0x9362)
    return FTE3600_MODEL_FW9369;
  if (id == 0x9365)
    return FTE3600_MODEL_FT9365;
  if (id == 0x9391 || id == 0x9392)
    return FTE3600_MODEL_FT9769;
  return FTE3600_MODEL_UNKNOWN;
}
static void
fw_phase (void *context, enum fte3600_fw9369_startup_phase p,
          int entering, enum fte3600_fw9369_startup_status status)
{
  struct operation *op = context;
  const enum fte3600_factory_startup_phase map[] = {
    FTE3600_PHASE_WORD_WAKE, FTE3600_PHASE_FW_FDT,
    FTE3600_PHASE_FW_IMAGE, FTE3600_PHASE_FW_ARM
  };
  int defined = !entering && (p == FTE3600_FW9369_PHASE_FDT ||
                              p == FTE3600_FW9369_PHASE_IMAGE) && status <= FTE3600_FW9369_STARTUP_FAILED;

  phase (op, map[p], entering, defined, status == FTE3600_FW9369_STARTUP_FAILED ? UINT32_MAX : 0);
}
static void
modern_phase (void *context, enum fte3600_ft93xx_startup_phase p,
              const struct fte3600_ft93xx_device *device)
{
  struct operation *op = context;

  (void) device;
  if (op->modern_phase >= 0)
    phase (op, (enum fte3600_factory_startup_phase) op->modern_phase, 0,
           op->modern_phase != FTE3600_PHASE_MODERN_ARM, 0);
  op->modern_phase = FTE3600_PHASE_MODERN_FDT + (int) p;
  phase (op, (enum fte3600_factory_startup_phase) op->modern_phase, 1, 0, 0);
  if (p == FTE3600_FT93XX_STARTUP_FINALIZE)
    {
      phase (op, FTE3600_PHASE_MODERN_FINALIZE, 0, 0, 0);
      op->modern_phase = -1;
    }
}
static int
valid (const struct fte3600_factory_startup_input *in,
       struct fte3600_factory_startup_state *s,
       struct fte3600_factory_startup_storage *w, size_t capacity,
       const struct fte3600_factory_startup_io *io,
       const struct fte3600_factory_startup_observer *o,
       struct fte3600_factory_startup_result *r)
{
  const void *objects[] = {in, s, w, io, r, o};
  const size_t sizes[] = {sizeof (*in), sizeof (*s), sizeof (*w), sizeof (*io), sizeof (*r), sizeof (*o)};

  if (!aligned (in, _Alignof (struct fte3600_factory_startup_input)) ||
      !aligned (s, _Alignof (struct fte3600_factory_startup_state)) ||
      !aligned (w, _Alignof (struct fte3600_factory_startup_storage)) ||
      !aligned (io, _Alignof (struct fte3600_factory_startup_io)) ||
      !aligned (r, _Alignof (struct fte3600_factory_startup_result)) ||
      (o && !aligned (o, _Alignof (struct fte3600_factory_startup_observer))) ||
      capacity < sizeof (*w))
    return 0;
  for (size_t i = 0; i < 6; ++i)
    for (size_t j = i + 1; j < 6; ++j)
      if (objects[i] && objects[j] && overlap (objects[i], sizes[i], objects[j], sizes[j]))
        return 0;
  return valid_io (io) && in->initial_model >= FTE3600_MODEL_UNKNOWN &&
         in->initial_model <= FTE3600_MODEL_FT9365 &&
         s->modern.preparation.state.check_registers <= 2 &&
         s->modern.fdt.analog.threshold_mode <= 1 && s->modern.fdt.dac_calibrated <= 1 &&
         s->modern.fdt.damaged <= 1 && s->modern.image.initialized <= 1 &&
         s->fw.config.channel_mode <= 1;
}
enum fte3600_factory_startup_error
fte3600_factory_startup_run (
  const struct fte3600_factory_startup_input *in,
  struct fte3600_factory_startup_state *s,
  struct fte3600_factory_startup_storage *w, size_t capacity,
  const struct fte3600_factory_startup_io *io,
  const struct fte3600_factory_startup_observer *observer,
  struct fte3600_factory_startup_result *r)
{
  if (!valid (in, s, w, capacity, io, observer, r))
    return FTE3600_FACTORY_STARTUP_INVALID_ARGUMENT;
  memset (&s->selected, 0, sizeof (s->selected));
  memset (r, 0, sizeof (*r));
  r->model = in->initial_model;
  struct operation op = {io, observer, r, -1};
  struct fte3600_ft9368_io byte_io = {&op, byte_spi, wait_ms};
  for (int b = FTE3600_BOUNDARY_PLATFORM; b <= FTE3600_BOUNDARY_RESOURCES; ++b)
    {
      uint32_t status = io->boundary (io->context, (enum fte3600_factory_boundary) b);
      if (b == FTE3600_BOUNDARY_INTERFACE ? status == UINT32_C (0xc0000001) : negative (status))
        {
          r->status = status;
          return FTE3600_FACTORY_STARTUP_OK;
        }
    }
  if (in->scene == 2)
    {
      r->contact_pending = (uint8_t) !negative (io->boundary (io->context, FTE3600_BOUNDARY_INITIAL_CONTACT));
      phase (&op, FTE3600_PHASE_SHORT_DETECT, 1, 0, 0);
      int present = fte3600_ft9368_shortcut (&byte_io);
      phase (&op, FTE3600_PHASE_SHORT_DETECT, 0, 1, (uint32_t) present);
      if (present)
        {
          r->contact_pending = 1;
          return FTE3600_FACTORY_STARTUP_UNSUPPORTED;
        }
    }
  for (unsigned int i = 0; i < 2; ++i)
    {
      phase (&op, FTE3600_PHASE_BYTE_PROBE, 1, 0, 0);
      uint16_t id = fte3600_ft9368_probe (&byte_io);
      phase (&op, FTE3600_PHASE_BYTE_PROBE, 0, 1, id);
      if (id == 0x9368)
        return FTE3600_FACTORY_STARTUP_UNSUPPORTED;
    }
  for (unsigned int i = 0; i < 2; ++i)
    {
      wake (&op);
      if (!probe (&op, &r->outer_raw_id))
        return FTE3600_FACTORY_STARTUP_WORD_READ_ERROR;
      if (model (r->outer_raw_id) != FTE3600_MODEL_UNKNOWN)
        break;
      if (i == 0)
        reset (&op, 10);
    }
  if (model (r->outer_raw_id) == FTE3600_MODEL_UNKNOWN)
    return FTE3600_FACTORY_STARTUP_UNSUPPORTED;
  if (in->scene == 2)
    {
      r->status = UINT32_C (0xc0000001);
      return FTE3600_FACTORY_STARTUP_OK;
    }
  r->model = model (r->outer_raw_id);
  phase (&op, FTE3600_PHASE_SELECT, 1, 0, 0);
  if (r->model == FTE3600_MODEL_FW9369)
    (void) fte3600_factory_fw9369_defaults (&s->selected, io);
  else if (r->model == FTE3600_MODEL_FT9365)
    (void) fte3600_factory_ft9365_defaults (&s->selected, io);
  else
    (void) fte3600_factory_ft9769_defaults (&s->selected, io);
  r->sensor_constructed = 1;
  phase (&op, FTE3600_PHASE_SELECT, 0, 1, 0);
  reset (&op, 10);
  if (r->model == FTE3600_MODEL_FW9369)
    {
      struct fte3600_capture_io child_io = {&op, spi, wait_ms};
      struct fte3600_fw9369_startup_observer child_observer = {&op, fw_phase};
      r->fw_ran = 1;
      phase (&op, FTE3600_PHASE_FW_STARTUP, 1, 0, 0);
      r->child_status = fte3600_fw9369_startup_checked (&s->fw, &w->fw,
                                                        sizeof (w->fw), &child_io, &child_observer, &r->fw);
      phase (&op, FTE3600_PHASE_FW_STARTUP, 0, r->child_status <= 1,
             r->child_status == 1 ? UINT32_MAX : 0);
      if (r->child_status > 1)
        return FTE3600_FACTORY_STARTUP_FW_ERROR;
    }
  else
    {
      struct fte3600_ft93xx_io child_io = {&op, spi, wait_ms};
      struct fte3600_ft93xx_startup_observer child_observer = {&op, modern_phase};
      phase (&op, FTE3600_PHASE_MODERN_BIND, 1, 0, 0);
      phase (&op, FTE3600_PHASE_MODERN_BIND, 0, 0, 0);
      r->modern_ran = 1;
      phase (&op, FTE3600_PHASE_MODERN_STARTUP, 1, 0, 0);
      r->child_status = fte3600_ft93xx_full_startup (&s->modern, w->pixels,
                                                     FTE3600_FT93XX_STARTUP_SAMPLES, &child_io, &child_observer, &r->modern);
      if (op.modern_phase >= 0)
        phase (&op, (enum fte3600_factory_startup_phase) op.modern_phase, 0, 0, 0);
      phase (&op, FTE3600_PHASE_MODERN_STARTUP, 0, r->child_status == 0, 0);
      if (r->child_status != 0)
        return FTE3600_FACTORY_STARTUP_MODERN_ERROR;
    }
  (void) io->boundary (io->context, FTE3600_BOUNDARY_FRAMEWORK_INIT);
  (void) io->boundary (io->context, FTE3600_BOUNDARY_REGISTRY_READ);
  if (in->resume_cleanup)
    {
      uint8_t discarded[32];
      wait_ms (&op, 10);
      (void) io->boundary (io->context, FTE3600_BOUNDARY_REGISTRY_CLEAR);
      r->contact_pending = 0;
      (void) fte3600_ft9368_command (0x9180, 32, discarded, sizeof (discarded), &byte_io);
      (void) fte3600_ft9368_command (0x9180, 32, discarded, sizeof (discarded), &byte_io);
      (void) fte3600_ft9368_command (0x9080, 6, discarded, sizeof (discarded), &byte_io);
    }
  (void) io->boundary (io->context, FTE3600_BOUNDARY_FRAMEWORK_FINISH);
  return FTE3600_FACTORY_STARTUP_OK;
}

enum fte3600_factory_startup_error
fte3600_factory_word_operation (
  enum fte3600_factory_word_operation which, uint32_t settle,
  const struct fte3600_factory_startup_io *io,
  const struct fte3600_factory_startup_observer *o,
  struct fte3600_factory_startup_result *r, uint16_t *identity)
{
  if (!aligned (io, _Alignof (struct fte3600_factory_startup_io)) ||
      !aligned (r, _Alignof (struct fte3600_factory_startup_result)) ||
      which < FTE3600_FACTORY_WORD_WAKE || which > FTE3600_FACTORY_WORD_RESET ||
      (which == FTE3600_FACTORY_WORD_PROBE && !aligned (identity, _Alignof (uint16_t))) ||
      (o && !aligned (o, _Alignof (struct fte3600_factory_startup_observer))))
    return FTE3600_FACTORY_STARTUP_INVALID_ARGUMENT;
  const void *objects[] = {io, o, r, identity};
  const size_t sizes[] = {sizeof (*io), sizeof (*o), sizeof (*r), sizeof (*identity)};
  for (size_t i = 0; i < 4; ++i)
    if (objects[i])
      {
        if (sizes[i] > UINTPTR_MAX - (uintptr_t) objects[i])
          return FTE3600_FACTORY_STARTUP_INVALID_ARGUMENT;
        for (size_t j = 0; j < i; ++j)
          if (objects[j] && overlap (objects[i], sizes[i], objects[j], sizes[j]))
            return FTE3600_FACTORY_STARTUP_INVALID_ARGUMENT;
      }
  if (!valid_io (io) || (o && !o->phase))
    return FTE3600_FACTORY_STARTUP_INVALID_ARGUMENT;
  struct operation op = {.io = io, .observer = o, .r = r};
  if (which == FTE3600_FACTORY_WORD_WAKE)
    wake (&op);
  else if (which == FTE3600_FACTORY_WORD_RESET)
    reset (&op, settle);
  else if (!probe (&op, identity))
    return FTE3600_FACTORY_STARTUP_WORD_READ_ERROR;
  return FTE3600_FACTORY_STARTUP_OK;
}
