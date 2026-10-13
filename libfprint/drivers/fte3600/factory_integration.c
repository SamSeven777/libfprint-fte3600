/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_factory_integration.h"
#include "fte3600_ft9368.h"
#include <string.h>
#define FATAL UINT32_C (0xc0000001)
struct operation
{
  const struct fte3600_factory_integration_input    *in;
  struct fte3600_factory_integration_state          *s;
  struct fte3600_factory_startup_storage            *w;
  const fte3600_factory_integration_io              *io;
  const struct fte3600_factory_integration_observer *o;
  struct fte3600_factory_integration_result         *r;
  int                                                unavailable, modern_phase, optional_firmware;
  enum fte3600_factory_model firmware_model;
};
static int
negative (uint32_t s)
{
  return (s >> 31) != 0;
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
static void
clear_selection (struct fte3600_factory_integration_state *s)
{
  memset (&s->word.selected, 0, sizeof s->word.selected);
  memset (&s->byte.selected, 0, sizeof s->byte.selected);
  memset (&s->legacy.selected, 0, sizeof s->legacy.selected);
  s->selected_model = FTE3600_MODEL_UNKNOWN;
  s->board = NULL;
}
enum fte3600_factory_integration_error
fte3600_factory_integration_state_defaults (
  struct fte3600_factory_integration_state *s)
{
  if (!aligned (s, _Alignof (struct fte3600_factory_integration_state)))
    return FTE3600_FACTORY_INTEGRATION_INVALID_ARGUMENT;
  memset (s, 0, sizeof *s);
  (void) fte3600_factory_startup_state_defaults (&s->word);
  (void) fte3600_factory_legacy_state_defaults (&s->legacy);
  return FTE3600_FACTORY_INTEGRATION_OK;
}
static int
valid (const struct fte3600_factory_integration_input *in,
       struct fte3600_factory_integration_state *s, struct fte3600_factory_startup_storage *w,
       size_t capacity, const fte3600_factory_integration_io *io,
       const struct fte3600_factory_integration_observer *o, struct fte3600_factory_integration_result *r, int optional)
{
  const void *p[] = {in, s, w, io, o, r};
  const size_t n[] = {sizeof *in, sizeof *s, sizeof *w, sizeof *io, sizeof *o, sizeof *r};
  const size_t a[] = {_Alignof (struct fte3600_factory_integration_input),
                      _Alignof (struct fte3600_factory_integration_state), _Alignof (struct fte3600_factory_startup_storage),
                      _Alignof (fte3600_factory_integration_io), _Alignof (struct fte3600_factory_integration_observer),
                      _Alignof (struct fte3600_factory_integration_result)};

  for (size_t i = 0; i < 6; ++i)
    {
      if (i == 4 && !o)
        continue;
      if (!aligned (p[i], a[i]) || n[i] > UINTPTR_MAX - (uintptr_t) p[i])
        return 0;
      for (size_t j = 0; j < i; ++j)
        if (p[j] && overlap (p[i], n[i], p[j], n[j]))
          return 0;
    }
  if (capacity < sizeof *w || !io->spi || !io->wait_ms || !io->wait_us || !io->gpio || !io->boundary ||
      (o && !o->phase) || in->initial_model < FTE3600_MODEL_UNKNOWN || in->initial_model > FTE3600_MODEL_FT9365 ||
      (!optional && !in->pram.length) || in->pram.length > FTE3600_FT9368_PRAM_MAX || in->pram.length % 4 ||
      in->pram.capacity < (in->pram.length + 127) / 128 * 128 ||
      (!in->pram.length && (in->pram.data || in->pram.capacity)) || (!optional && !in->application.length) ||
      in->application.length > FTE3600_FT9368_APPLICATION_MAX || in->application.length % 2 ||
      in->application.capacity < in->application.length ||
      (!in->application.length && (in->application.data || in->application.capacity)) || (!optional && !in->firmware_length) || in->firmware_length > 14184 ||
      s->word.modern.preparation.state.check_registers > 2 || s->word.modern.fdt.analog.threshold_mode > 1 ||
      s->word.modern.fdt.dac_calibrated > 1 || s->word.modern.fdt.damaged > 1 ||
      s->word.modern.image.initialized > 1 || s->word.fw.config.channel_mode > 1)
    return 0;
  const void *payload[] = {in->pram.data, in->application.data, in->firmware};
  const size_t extent[] = {in->pram.capacity, in->application.capacity, in->firmware_length};
  for (size_t i = 0; i < 3; ++i)
    {
      if (!extent[i] && !payload[i] && optional)
        continue;
      if (!payload[i] || !extent[i] || extent[i] > UINTPTR_MAX - (uintptr_t) payload[i])
        return 0;
      for (size_t j = 0; j < 6; ++j)
        if (p[j] && overlap (payload[i], extent[i], p[j], n[j]))
          return 0;
      for (size_t j = 0; j < i; ++j)
        if (overlap (payload[i], extent[i], payload[j], extent[j]))
          return 0;
    }
  return 1;
}
static void
phase (struct operation *op, enum fte3600_factory_integration_phase p, int entry, int defined, uint32_t status)
{
  if (op->o)
    op->o->phase (op->o->context, p, entry, defined, status, op->r);
}
static uint32_t
spi (void *c, uint8_t *b, size_t n)
{
  struct operation *op = c;

  ++op->r->physical_calls;
  ++op->r->event_count;
  uint32_t status = op->io->spi (op->io->context, b, n);
  if (negative (status))
    {
      ++op->r->transport_failures;
      op->r->last_transport_failure = status;
    }
  if (status == FTE3600_FACTORY_INTEGRATION_UNAVAILABLE)
    op->unavailable = 1;
  return status;
}
static uint32_t
byte_spi (void *c, uint8_t *b, size_t n)
{
  struct operation *op = c;

  if (n && b[0] != 0x70 && b[0] != 0x71)
    ++op->r->byte_protocol_calls;
  if (n >= 2 && b[0] == 0x91 && b[1] == 0x80)
    ++op->r->information_replies_consumed;
  return spi (c, b, n);
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
static uint32_t
release_and_sync (void *c, uint32_t *gpio_status)
{
  struct operation *op = c;
  uint32_t status = op->io->release_and_sync (op->io->context, gpio_status);

  ++op->r->event_count;
  if (negative (*gpio_status))
    {
      ++op->r->gpio_failures;
      op->r->last_gpio_failure = *gpio_status;
    }
  else
    {
      ++op->r->physical_calls;
      ++op->r->event_count;
      if (negative (status))
        {
          ++op->r->transport_failures;
          op->r->last_transport_failure = status;
        }
      if (status == FTE3600_FACTORY_INTEGRATION_UNAVAILABLE)
        op->unavailable = 1;
    }
  return status;
}
static uint32_t
word_gpio (void *c, unsigned int level)
{
  return gpio (c, (int) level);
}
static uint32_t
boundary (void *c, enum fte3600_factory_boundary b)
{
  struct operation *op = c;

  return op->io->boundary (op->io->context, b);
}
static void
word_phase (void *c, enum fte3600_factory_startup_phase p, int entry, int defined, uint32_t status,
            const struct fte3600_factory_startup_result *r)
{
  (void) r;
  phase (c, (enum fte3600_factory_integration_phase) p, entry, defined, status);
}
static enum fte3600_factory_integration_error
word (struct operation *op,
      enum fte3600_factory_word_operation which, uint32_t settle, uint16_t *id)
{
  const struct fte3600_factory_startup_io io = {op, spi, wait_ms, word_gpio, boundary,
                                                op->io->release_and_sync ? release_and_sync : NULL};
  const struct fte3600_factory_startup_observer observer = {op, word_phase};
  enum fte3600_factory_startup_error e = fte3600_factory_word_operation (which, settle, &io, &observer, &op->r->word, id);

  if (op->unavailable)
    return FTE3600_FACTORY_INTEGRATION_RESPONSE_UNAVAILABLE;
  return e ? FTE3600_FACTORY_INTEGRATION_WORD_READ_ERROR : FTE3600_FACTORY_INTEGRATION_OK;
}
static uint16_t
byte_probe (struct operation *op)
{
  const struct fte3600_ft9368_io io = {op, byte_spi, wait_ms};

  phase (op, FTE3600_FI_BYTE_PROBE, 1, 0, 0);
  uint16_t id = fte3600_ft9368_probe (&io);
  phase (op, FTE3600_FI_BYTE_PROBE, 0, 1, id);
  return id;
}
static void
byte_phase (void *c, enum fte3600_factory_byte_phase p, int entry, int defined, uint32_t status,
            const struct fte3600_factory_byte_result *r)
{
  (void) r;
  static const enum fte3600_factory_integration_phase map[] = {FTE3600_FI_SHORT, FTE3600_FI_BYTE_PROBE,
                                                               FTE3600_FI_DISCARD, FTE3600_FI_SELECT, FTE3600_FI_BYTE_BIND, FTE3600_FI_BYTE_LOAD,
                                                               FTE3600_FI_BYTE_VERSION, FTE3600_FI_PRAM, FTE3600_FI_APPLICATION, FTE3600_FI_RESET};
  phase (c, map[p], entry, defined, status);
}
static enum fte3600_factory_integration_error
byte_load (struct operation *op, int check)
{
  if (!op->in->pram.length || !op->in->application.length)
    return FTE3600_FACTORY_INTEGRATION_MISSING_FIRMWARE;
  const struct fte3600_factory_byte_io io = {op, byte_spi, wait_ms, wait_us, gpio, boundary, NULL};
  const struct fte3600_factory_byte_observer observer = {op, byte_phase};
  op->r->check_version = (uint8_t) check;
  enum fte3600_factory_byte_error e = fte3600_factory_byte_load_selected (&op->s->byte, check, &io, &observer, &op->r->byte);
  op->r->status = op->r->byte.status;
  op->r->child_status = e;
  if (op->unavailable)
    return FTE3600_FACTORY_INTEGRATION_RESPONSE_UNAVAILABLE;
  return e ? FTE3600_FACTORY_INTEGRATION_BYTE_ERROR : FTE3600_FACTORY_INTEGRATION_OK;
}
static int
legacy_model (enum fte3600_factory_model m)
{
  return m == FTE3600_MODEL_FT9338 || m == FTE3600_MODEL_FT9348 || m == FTE3600_MODEL_FT9361 || m == FTE3600_MODEL_FT9536;
}
static void
legacy_phase (void *c, enum fte3600_factory_legacy_phase p, int entry, int defined, uint32_t status,
              const struct fte3600_factory_legacy_result *r)
{
  (void) r;
  static const enum fte3600_factory_integration_phase map[] = {FTE3600_FI_SHORT, FTE3600_FI_BYTE_PROBE,
                                                               FTE3600_FI_WORD_WAKE, FTE3600_FI_WORD_PROBE, FTE3600_FI_RESET, FTE3600_FI_PING, FTE3600_FI_PING_ONCE,
                                                               FTE3600_FI_LEGACY_APPLICATION, FTE3600_FI_LEGACY_FACTORY, FTE3600_FI_LEGACY_RETRY, FTE3600_FI_LEGACY_IDENTITY,
                                                               FTE3600_FI_SELECT, FTE3600_FI_LEGACY_INITIALIZE, FTE3600_FI_LEGACY_IDLE, FTE3600_FI_LEGACY_VERSION,
                                                               FTE3600_FI_LEGACY_LOAD, FTE3600_FI_LEGACY_BOOT_A, FTE3600_FI_LEGACY_CONFIG, FTE3600_FI_LEGACY_ARM,
                                                               FTE3600_FI_DISCARD, FTE3600_FI_LEGACY_BOOT_B, FTE3600_FI_LEGACY_UPLOAD_A8};
  phase (c, map[p], entry, defined, status);
}
static enum fte3600_factory_integration_error
legacy (struct operation *op, enum fte3600_factory_legacy_child which)
{
  if (which == FTE3600_FL_CHILD_ARM && !legacy_model (op->r->model))
    return FTE3600_FACTORY_INTEGRATION_UNDEFINED_ARM;
  if (which == FTE3600_FL_CHILD_INITIALIZE && op->optional_firmware && op->in->firmware_length &&
      op->r->model != op->firmware_model)
    return FTE3600_FACTORY_INTEGRATION_FIRMWARE_MODEL_MISMATCH;
  if (which == FTE3600_FL_CHILD_INITIALIZE && !op->in->firmware_length)
    return FTE3600_FACTORY_INTEGRATION_MISSING_FIRMWARE;
  const struct fte3600_factory_legacy_input in = {op->in->scene, 0, op->r->model, op->in->firmware, op->in->firmware_length};
  const struct fte3600_factory_startup_io io = {op, spi, wait_ms, word_gpio, boundary,
                                                op->io->release_and_sync ? release_and_sync : NULL};
  const struct fte3600_factory_legacy_observer observer = {op, legacy_phase};
  op->r->legacy.model = op->r->model;
  op->r->legacy.clock_us = op->r->clock_us;
  enum fte3600_factory_legacy_error e = op->optional_firmware ?
                                        fte3600_factory_legacy_child_run_optional_firmware (which, &in, &op->s->legacy, &io, &observer, &op->r->legacy) :
                                        fte3600_factory_legacy_child_run (which, &in, &op->s->legacy, &io, &observer, &op->r->legacy);
  op->r->status = op->r->legacy.status;
  op->r->model = op->r->legacy.model;
  op->r->child_status = e;
  if (op->unavailable)
    return FTE3600_FACTORY_INTEGRATION_RESPONSE_UNAVAILABLE;
  return e ? FTE3600_FACTORY_INTEGRATION_LEGACY_ERROR : FTE3600_FACTORY_INTEGRATION_OK;
}
static void
discard (struct operation *op)
{
  phase (op, FTE3600_FI_DISCARD, 1, 0, 0);
  if (op->r->sensor_constructed)
    ++op->r->destructions;
  clear_selection (op->s);
  op->r->sensor_constructed = 0;
  phase (op, FTE3600_FI_DISCARD, 0, 0, 0);
}
static int
select_sensor (struct operation *op)
{
  enum fte3600_factory_model m = op->r->model;
  const struct fte3600_factory_startup_io io = {op, spi, wait_ms, word_gpio, boundary,
                                                op->io->release_and_sync ? release_and_sync : NULL};

  phase (op, FTE3600_FI_SELECT, 1, 0, 0);
  if (m == FTE3600_MODEL_UNKNOWN)
    {
      op->r->status = FATAL;
      phase (op, FTE3600_FI_SELECT, 0, 1, FATAL);
      return 0;
    }
  clear_selection (op->s);
  if (m == FTE3600_MODEL_FT9368)
    {
      (void) fte3600_factory_ft9368_defaults (&op->s->byte.selected, op->io);
      op->s->byte.selected.pram = op->in->pram;
      op->s->byte.selected.application = op->in->application;
    }
  else if (legacy_model (m))
    {
      (void) fte3600_factory_legacy_defaults (m, &op->s->legacy.selected, &io);
      op->s->legacy.selected.board = NULL;
      op->s->legacy.selected.application = op->in->firmware;
      op->s->legacy.selected.application_length = op->in->firmware_length;
      op->s->legacy.selected.bound = 1;
    }
  else
    {
      if (m == FTE3600_MODEL_FW9369)
        (void) fte3600_factory_fw9369_defaults (&op->s->word.selected, &io);
      else if (m == FTE3600_MODEL_FT9365)
        (void) fte3600_factory_ft9365_defaults (&op->s->word.selected, &io);
      else
        (void) fte3600_factory_ft9769_defaults (&op->s->word.selected, &io);
      op->s->word.selected.board = NULL;
    }
  op->s->selected_model = m;
  op->s->board = op->io;
  op->r->sensor_constructed = 1;
  ++op->r->constructions;
  phase (op, FTE3600_FI_SELECT, 0, 1, 0);
  return 1;
}
static void
bind_byte (struct operation *op)
{
  phase (op, FTE3600_FI_BYTE_BIND, 1, 0, 0);
  op->s->byte.selected.bound = 1;
  phase (op, FTE3600_FI_BYTE_BIND, 0, 0, 0);
}
static void
fw_phase (void *c, enum fte3600_fw9369_startup_phase p, int entry, enum fte3600_fw9369_startup_status s)
{
  static const enum fte3600_factory_integration_phase map[] = {FTE3600_FI_WORD_WAKE, FTE3600_FI_FW_FDT, FTE3600_FI_FW_IMAGE, FTE3600_FI_FW_ARM};
  int defined = !entry && (p == FTE3600_FW9369_PHASE_FDT || p == FTE3600_FW9369_PHASE_IMAGE) && s <= 1;

  phase (c, map[p], entry, defined, s == 1 ? UINT32_MAX : 0);
}
static void
modern_phase (void *c, enum fte3600_ft93xx_startup_phase p, const struct fte3600_ft93xx_device *s)
{
  struct operation *op = c;

  (void) s;
  if (op->modern_phase >= 0)
    phase (op, (enum fte3600_factory_integration_phase) op->modern_phase, 0, op->modern_phase != FTE3600_FI_MODERN_ARM, 0);
  op->modern_phase = FTE3600_FI_MODERN_FDT + (int) p;
  phase (op, (enum fte3600_factory_integration_phase) op->modern_phase, 1, 0, 0);
  if (p == FTE3600_FT93XX_STARTUP_FINALIZE)
    {
      phase (op, FTE3600_FI_MODERN_FINALIZE, 0, 0, 0);
      op->modern_phase = -1;
    }
}
static enum fte3600_factory_integration_error
startup (struct operation *op)
{
  if (op->r->model == FTE3600_MODEL_FW9369)
    {
      const struct fte3600_capture_io io = {op, spi, wait_ms};
      const struct fte3600_fw9369_startup_observer observer = {op, fw_phase};
      op->r->fw_ran = 1;
      phase (op, FTE3600_FI_FW_STARTUP, 1, 0, 0);
      op->r->child_status = (op->optional_firmware ? fte3600_fw9369_startup_linux :
                             fte3600_fw9369_startup_checked)(&op->s->word.fw, &op->w->fw, sizeof op->w->fw, &io, &observer, &op->r->word.fw);
      phase (op, FTE3600_FI_FW_STARTUP, 0, op->r->child_status <= 1, op->r->child_status == 1 ? UINT32_MAX : 0);
      if (op->r->child_status > 1)
        return FTE3600_FACTORY_INTEGRATION_FW_ERROR;
    }
  else
    {
      const struct fte3600_ft93xx_io io = {op, spi, wait_ms};
      const struct fte3600_ft93xx_startup_observer observer = {op, modern_phase};
      phase (op, FTE3600_FI_MODERN_BIND, 1, 0, 0);
      phase (op, FTE3600_FI_MODERN_BIND, 0, 0, 0);
      op->r->modern_ran = 1;
      phase (op, FTE3600_FI_MODERN_STARTUP, 1, 0, 0);
      op->r->child_status = fte3600_ft93xx_full_startup (&op->s->word.modern, op->w->pixels, FTE3600_FT93XX_STARTUP_SAMPLES, &io, &observer, &op->r->word.modern);
      if (op->modern_phase >= 0)
        phase (op, (enum fte3600_factory_integration_phase) op->modern_phase, 0, 0, 0);
      phase (op, FTE3600_FI_MODERN_STARTUP, 0, op->r->child_status == 0, 0);
      if (op->r->child_status)
        return FTE3600_FACTORY_INTEGRATION_MODERN_ERROR;
    }
  op->r->status = 0;
  return op->unavailable ? FTE3600_FACTORY_INTEGRATION_RESPONSE_UNAVAILABLE : FTE3600_FACTORY_INTEGRATION_OK;
}
static enum fte3600_factory_model
word_model (uint16_t id)
{
  return id == 0x9362 ? FTE3600_MODEL_FW9369 : id == 0x9365 ? FTE3600_MODEL_FT9365 : (id == 0x9391 || id == 0x9392) ? FTE3600_MODEL_FT9769 : FTE3600_MODEL_UNKNOWN;
}
static enum fte3600_factory_integration_error
run (
  const struct fte3600_factory_integration_input *in, struct fte3600_factory_integration_state *s,
  struct fte3600_factory_startup_storage *w, size_t capacity, const fte3600_factory_integration_io *io,
  const struct fte3600_factory_integration_observer *o, struct fte3600_factory_integration_result *r, int optional, enum fte3600_factory_model firmware_model)
{
  if (!valid (in, s, w, capacity, io, o, r, optional))
    return FTE3600_FACTORY_INTEGRATION_INVALID_ARGUMENT;
  clear_selection (s);
  memset (r, 0, sizeof *r);
  r->model = in->initial_model;
  struct operation op = {in, s, w, io, o, r, 0, -1, optional, firmware_model};
  const struct fte3600_ft9368_io byte_io = {&op, byte_spi, wait_ms};
  enum fte3600_factory_integration_error error;
  int initial_contact = 0;
#define CHECK(call) do { error = (call); if (error) return error; } while (0)
#define AVAILABLE() do { if (op.unavailable) return FTE3600_FACTORY_INTEGRATION_RESPONSE_UNAVAILABLE; } while (0)
  for (int b = FTE3600_BOUNDARY_PLATFORM; b <= FTE3600_BOUNDARY_RESOURCES; ++b)
    {
      uint32_t status = boundary (&op, (enum fte3600_factory_boundary) b);
      if (b == FTE3600_BOUNDARY_INTERFACE ? status == FATAL : negative (status))
        {
          r->status = status;
          return 0;
        }
    }
  if (in->scene == 2)
    {
      initial_contact = !negative (boundary (&op, FTE3600_BOUNDARY_INITIAL_CONTACT));
      r->contact_pending = (uint8_t) initial_contact;
    }
  for (unsigned round = 0; round < 5; ++round)
    {
      if (!round && in->scene == 2)
        {
          phase (&op, FTE3600_FI_SHORT, 1, 0, 0);
          int present = fte3600_ft9368_shortcut (&byte_io);
          phase (&op, FTE3600_FI_SHORT, 0, 1, (uint32_t) present);
          AVAILABLE ();
          if (present)
            {
              r->contact_pending = 1;
              goto byte_complete;
            }
        }
      uint16_t id = byte_probe (&op);
      AVAILABLE ();
      if (id != 0x9368)
        {
          if (round == 2)
            {
              CHECK (word (&op, FTE3600_FACTORY_WORD_RESET, 300, NULL));
              (void) byte_probe (&op);
              AVAILABLE ();
            }
          else if (round >= 3)
            {
              if (optional)
                return FTE3600_FACTORY_INTEGRATION_UNCONFIRMED_IDENTITY;
              discard (&op);
              r->model = FTE3600_MODEL_FT9368;
              if (!select_sensor (&op))
                return 0;
              bind_byte (&op);
              CHECK (byte_load (&op, 0));
            }
          id = byte_probe (&op);
          AVAILABLE ();
        }
      if (id == 0x9368)
        goto byte_complete;
      for (unsigned attempt = 0; attempt < 2; ++attempt)
        {
          CHECK (word (&op, FTE3600_FACTORY_WORD_WAKE, 0, NULL));
          CHECK (word (&op, FTE3600_FACTORY_WORD_PROBE, 0, &id));
          enum fte3600_factory_model m = word_model (id);
          if (m != FTE3600_MODEL_UNKNOWN)
            {
              if (in->scene == 2)
                {
                  r->status = FATAL;
                  return 0;
                }
              r->model = m;
              if (!select_sensor (&op))
                return 0;
              CHECK (word (&op, FTE3600_FACTORY_WORD_RESET, 10, NULL));
              CHECK (startup (&op));
              goto complete;
            }
          if (!attempt)
            CHECK (word (&op, FTE3600_FACTORY_WORD_RESET, 10, NULL));
        }
      if (!round)
        continue;
      int app = 0;
      if (round == 1)
        {
          CHECK (legacy (&op, FTE3600_FL_CHILD_PING));
          if (!negative (r->status))
            {
              wait_ms (&op, 350);
              CHECK (legacy (&op, FTE3600_FL_CHILD_APPLICATION));
              app = !negative (r->status);
            }
        }
      if (app)
        {
          if (!select_sensor (&op))
            return 0;
          if (!initial_contact)
            {
              CHECK (legacy (&op, FTE3600_FL_CHILD_ARM));
              if (negative (r->status))
                return 0;
            }
          goto complete;
        }
      CHECK (legacy (&op, FTE3600_FL_CHILD_FACTORY));
      if (negative (r->status))
        continue;
      if (!select_sensor (&op))
        return 0;
      if (r->model != FTE3600_MODEL_FT9338)
        goto complete;
      CHECK (legacy (&op, FTE3600_FL_CHILD_INITIALIZE));
      if (!negative (r->status))
        goto complete;
      discard (&op);
    }
  goto complete;
byte_complete:
  r->model = FTE3600_MODEL_FT9368;
  discard (&op);
  if (!select_sensor (&op))
    return 0;
  bind_byte (&op);
  CHECK (byte_load (&op, 1));
complete:
  if (r->model == FTE3600_MODEL_FT9368)
    {
      uint8_t data[4];
      (void) fte3600_ft9368_command (0xf680, 4, data, sizeof data, &byte_io);
      wait_ms (&op, 100);
      AVAILABLE ();
    }
  (void) boundary (&op, FTE3600_BOUNDARY_FRAMEWORK_INIT);
  (void) boundary (&op, FTE3600_BOUNDARY_REGISTRY_READ);
  if (in->resume_cleanup)
    {
      uint8_t data[32];
      wait_ms (&op, 10);
      (void) boundary (&op, FTE3600_BOUNDARY_REGISTRY_CLEAR);
      r->contact_pending = 0;
      (void) fte3600_ft9368_info (32, data, sizeof data, &byte_io);
      AVAILABLE ();
      (void) fte3600_ft9368_info (32, data, sizeof data, &byte_io);
      AVAILABLE ();
      (void) fte3600_ft9368_clear (&byte_io);
      AVAILABLE ();
    }
  (void) boundary (&op, FTE3600_BOUNDARY_FRAMEWORK_FINISH);
  return FTE3600_FACTORY_INTEGRATION_OK;
#undef CHECK
#undef AVAILABLE
}

enum fte3600_factory_integration_error
fte3600_factory_integration_run (
  const struct fte3600_factory_integration_input *in, struct fte3600_factory_integration_state *s,
  struct fte3600_factory_startup_storage *w, size_t capacity, const fte3600_factory_integration_io *io,
  const struct fte3600_factory_integration_observer *o, struct fte3600_factory_integration_result *r)
{
  return run (in, s, w, capacity, io, o, r, 0, FTE3600_MODEL_UNKNOWN);
}
enum fte3600_factory_integration_error
fte3600_factory_integration_run_optional_firmware (
  const struct fte3600_factory_integration_input *in, enum fte3600_factory_model firmware_model,
  struct fte3600_factory_integration_state *s,
  struct fte3600_factory_startup_storage *w, size_t capacity, const fte3600_factory_integration_io *io,
  const struct fte3600_factory_integration_observer *o, struct fte3600_factory_integration_result *r)
{
  return run (in, s, w, capacity, io, o, r, 1, firmware_model);
}
