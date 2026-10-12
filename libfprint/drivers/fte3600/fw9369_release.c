/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_fw9369_release.h"
#include <string.h>

static int
valid_state (const struct fte3600_fw9369_startup_state *s)
{
  return s && s->config.channel_mode <= 1 && s->config.rate && s->config.fast_rate;
}
static int
valid_io (const struct fte3600_capture_io *io)
{
  return io && io->spi && io->wait_ms;
}
static size_t
channels (const struct fte3600_fw9369_startup_state *s)
{
  return s->config.channel_mode ? 4 : 8;
}
static uint32_t
add (uint32_t a, uint32_t b)
{
  return UINT32_MAX - a < b ? UINT32_MAX : a + b;
}
static void
accumulate (struct fte3600_fw9369_release_diagnostics *d,
            const struct fte3600_capture_diagnostics  *in)
{
  d->transport.failed_attempts = add (d->transport.failed_attempts, in->failed_attempts);
  d->transport.exhausted_transactions = add (d->transport.exhausted_transactions, in->exhausted_transactions);
  if (in->last_failure)
    d->transport.last_failure = in->last_failure;
  d->transport.ready_timeout |= in->ready_timeout;
}
static enum fte3600_fw9369_release_status
transport_status (
  const struct fte3600_fw9369_release_diagnostics *d)
{
  if (d->transport.exhausted_transactions)
    return FTE3600_FW9369_RELEASE_TRANSPORT_ERROR;
  return d->transport.ready_timeout ? FTE3600_FW9369_RELEASE_NOT_READY : FTE3600_FW9369_RELEASE_OK;
}
static int
transfer (uint8_t *b, size_t n, const struct fte3600_capture_io *io,
          struct fte3600_fw9369_release_diagnostics *d)
{
  for (unsigned i = 0; i < 3; ++i)
    {
      uint32_t rc = io->spi (io->context, b, n);
      if (!(rc >> 31))
        return 1;
      d->transport.failed_attempts = add (d->transport.failed_attempts, 1);
      d->transport.last_failure = rc;
      io->wait_ms (io->context, 10);
    }
  d->transport.exhausted_transactions = add (d->transport.exhausted_transactions, 1);
  return 0;
}
static int
word (uint16_t address, uint16_t *value, const struct fte3600_capture_io *io,
      struct fte3600_fw9369_release_diagnostics *d)
{
  uint8_t b[12] = {4, 0xfb, (uint8_t) (0x80 | (address >> 8)), (uint8_t) address, 0, 1};

  if (!transfer (b, sizeof (b), io, d))
    return 0;
  *value = (uint16_t) ((uint16_t) b[6] << 8 | b[7]);
  return 1;
}
static int
write_word (uint16_t address, uint16_t value, const struct fte3600_capture_io *io,
            struct fte3600_fw9369_release_diagnostics *d)
{
  uint8_t b[8] = {5, 0xfa, (uint8_t) (0x80 | (address >> 8)), (uint8_t) address, 0, 1,
                  (uint8_t) (value >> 8), (uint8_t) value};

  return transfer (b, sizeof (b), io, d);
}
static uint16_t
crc (const uint16_t *words, size_t count)
{
  uint16_t value = 0xffff;

  for (size_t i = 0; i < count; ++i)
    for (unsigned byte = 0; byte < 2; ++byte)
      {
        value ^= (uint16_t) ((uint16_t) (uint8_t) (words[i] >> (8 * byte)) << 8);
        for (unsigned bit = 0; bit < 8; ++bit)
          value = (uint16_t) ((value << 1) ^ (value & 0x8000 ? 0x1021 : 0));
      }
  return value;
}
static enum fte3600_fw9369_release_status
observe (
  struct fte3600_fw9369_startup_state *s, enum fte3600_fw9369_detection selection,
  struct fte3600_fw9369_observation *out, const struct fte3600_capture_io *io,
  struct fte3600_fw9369_release_diagnostics *d)
{
  struct fte3600_fw9369_observation next = {0};
  struct fte3600_capture_diagnostics fd;
  const struct fte3600_fw9369_fdt_image_config image = {s->image_dac, s->shift, s->period};
  enum fte3600_fw9369_fdt_status status = fte3600_fw9369_fdt_run (
    FTE3600_FW9369_FDT_MEASURE, &s->fdt, &s->config, &image, next.samples, 8, io, &fd);

  accumulate (d, &fd);
  if (status == FTE3600_FW9369_FDT_WORD_READ_ERROR)
    return FTE3600_FW9369_RELEASE_WORD_READ_ERROR;
  if (status == FTE3600_FW9369_FDT_ADC_READ_ERROR)
    return FTE3600_FW9369_RELEASE_ADC_READ_ERROR;
  if (status || transport_status (d))
    return status ? FTE3600_FW9369_RELEASE_TRANSPORT_ERROR : transport_status (d);
  unsigned count = 0;
  int invalid = 0;
  for (size_t i = 0; i < channels (s); ++i)
    {
      uint32_t sample = next.samples[i];
      uint16_t bits = (uint16_t) (s->fdt_down[i] - sample);
      next.differences[i] = bits < 0x8000 ? bits : (int32_t) bits - 65536;
      if (selection == FTE3600_FW9369_DOWN)
        {
          if (sample > (uint32_t) s->fdt_down[i] + s->config.threshold_low)
            invalid = 1;
          if ((uint32_t) s->fdt_down[i] > sample + s->config.threshold_low)
            ++count;
        }
      else if (sample + s->config.threshold_high > s->fdt_up[i])
        {
          ++count;
        }
    }
  next.contact = selection == FTE3600_FW9369_DOWN ?
                 (invalid ? FTE3600_FW9369_CONTACT_INVALID : count >= s->config.repeat ?
                  FTE3600_FW9369_CONTACT_DOWN : FTE3600_FW9369_CONTACT_NONE) :
                 (count >= s->config.average ? FTE3600_FW9369_CONTACT_UP : FTE3600_FW9369_CONTACT_NONE);
  ++d->observations;
  *out = next;
  return FTE3600_FW9369_RELEASE_OK;
}
enum fte3600_fw9369_release_status
fte3600_fw9369_observe (
  struct fte3600_fw9369_startup_state *s, enum fte3600_fw9369_detection selection,
  struct fte3600_fw9369_observation *out, const struct fte3600_capture_io *io,
  struct fte3600_fw9369_release_diagnostics *diagnostics)
{
  struct fte3600_fw9369_release_diagnostics local, *d = diagnostics ? diagnostics : &local;

  if (!valid_state (s) || !valid_io (io) || !out ||
      (selection != FTE3600_FW9369_DOWN && selection != FTE3600_FW9369_UP))
    return FTE3600_FW9369_RELEASE_INVALID_ARGUMENT;
  memset (d, 0, sizeof (*d));
  return observe (s, selection, out, io, d);
}
static enum fte3600_fw9369_release_status
confirm (
  struct fte3600_fw9369_startup_state *s, uint16_t samples[3][8], int *confirmed,
  const struct fte3600_capture_io *io, struct fte3600_fw9369_release_diagnostics *d)
{
  uint16_t accepted[3][8];

  for (unsigned n = 0; n < 3; ++n)
    {
      struct fte3600_fw9369_observation o;
      enum fte3600_fw9369_release_status status = observe (s, FTE3600_FW9369_UP, &o, io, d);
      if (status)
        return status;
      unsigned quiet = 0;
      for (unsigned c = 0; c < 4; ++c)
        if (o.differences[c] >= -49 && o.differences[c] <= 49)
          ++quiet;
      if (o.contact != FTE3600_FW9369_CONTACT_UP || quiet < 3)
        {
          *confirmed = 0;
          return FTE3600_FW9369_RELEASE_OK;
        }
      memcpy (accepted[n], o.samples, sizeof (accepted[n]));
    }
  memcpy (samples, accepted, sizeof (accepted));
  *confirmed = 1;
  d->confirmed = 1;
  return FTE3600_FW9369_RELEASE_OK;
}
enum fte3600_fw9369_release_status
fte3600_fw9369_confirm_release (
  struct fte3600_fw9369_startup_state *s, uint16_t samples[3][8], int *confirmed,
  const struct fte3600_capture_io *io, struct fte3600_fw9369_release_diagnostics *diagnostics)
{
  struct fte3600_fw9369_release_diagnostics local, *d = diagnostics ? diagnostics : &local;

  if (!valid_state (s) || !valid_io (io) || !samples || !confirmed)
    return FTE3600_FW9369_RELEASE_INVALID_ARGUMENT;
  memset (d, 0, sizeof (*d));
  return confirm (s, samples, confirmed, io, d);
}
enum fte3600_fw9369_release_status
fte3600_fw9369_update_fdt (
  struct fte3600_fw9369_startup_state *s, const uint16_t samples[3][8])
{
  if (!valid_state (s) || !samples)
    return FTE3600_FW9369_RELEASE_INVALID_ARGUMENT;
  for (size_t i = 0; i < channels (s); ++i)
    {
      uint32_t mean = ((uint32_t) samples[0][i] + samples[1][i] + samples[2][i]) / 3;
      uint32_t minimum = (uint32_t) s->fdt_down[i] + 30;
      uint16_t value = (uint16_t) (mean > minimum ? mean : minimum);
      value = value < 31 ? 0 : (uint16_t) (value - 30);
      s->fdt_reference[i] = s->fdt_down[i] = s->fdt_up[i] = value;
    }
  s->fdt_down_crc = crc (s->fdt_down, channels (s));
  s->fdt_up_crc = crc (s->fdt_up, channels (s));
  return FTE3600_FW9369_RELEASE_OK;
}
enum fte3600_fw9369_release_status
fte3600_fw9369_baseline_needs_reset (
  const struct fte3600_fw9369_startup_state *s, int *needed)
{
  if (!valid_state (s) || !needed)
    return FTE3600_FW9369_RELEASE_INVALID_ARGUMENT;
  unsigned down = 0, up = 0;
  for (unsigned i = 0; i < 4; ++i)
    {
      down += s->fdt_down[i] == 512;
      up += s->fdt_up[i] == 512;
    }
  struct fte3600_image_statistics baseline, work;
  (void) fte3600_image_measure (s->baseline, &baseline);
  (void) fte3600_image_measure (s->work_image, &work);
  uint32_t sum = 0, spread = 0;
  for (size_t i = 0; i < FTE3600_IMAGE_PIXELS; ++i)
    sum += (uint32_t) ((int32_t) s->baseline[i] - s->work_image[i]);
  uint32_t mean = sum / FTE3600_IMAGE_PIXELS;
  for (size_t i = 0; i < FTE3600_IMAGE_PIXELS; ++i)
    {
      uint32_t diff = (uint32_t) ((int32_t) s->baseline[i] - s->work_image[i]) - mean;
      spread += diff * diff;
    }
  spread /= FTE3600_IMAGE_PIXELS;
  *needed = down == 4 || up == 4 || crc (s->fdt_down, channels (s)) != s->fdt_down_crc ||
            crc (s->fdt_up, channels (s)) != s->fdt_up_crc ||
            (int) baseline.median > (int) s->image_median + 30 ||
            ((int) work.median < (int) s->image_median - 30 && spread < 150);
  return FTE3600_FW9369_RELEASE_OK;
}
enum fte3600_fw9369_release_status
fte3600_fw9369_arm_detection (
  struct fte3600_fw9369_startup_state *s, enum fte3600_fw9369_detection selection,
  const struct fte3600_capture_io *io, struct fte3600_fw9369_release_diagnostics *diagnostics)
{
  struct fte3600_fw9369_release_diagnostics local, *d = diagnostics ? diagnostics : &local;

  if (!valid_state (s) || !valid_io (io) ||
      (selection != FTE3600_FW9369_DOWN && selection != FTE3600_FW9369_UP))
    return FTE3600_FW9369_RELEASE_INVALID_ARGUMENT;
  memset (d, 0, sizeof (*d));
  const struct fte3600_fw9369_fdt_image_config image = {s->image_dac, s->shift, s->period};
  struct fte3600_capture_diagnostics fd;
  enum fte3600_fw9369_fdt_status fs = fte3600_fw9369_fdt_run (FTE3600_FW9369_FDT_CONFIGURE,
                                                              &s->fdt, &s->config, &image, NULL, 0, io, &fd);
  accumulate (d, &fd);
  if (fs)
    return FTE3600_FW9369_RELEASE_WORD_READ_ERROR;
  if (transport_status (d))
    return transport_status (d);
  s->arm_mode = selection == FTE3600_FW9369_UP ? 0 : 1;
  uint8_t unlock[4] = {9, 0xf6, 0x9a, 0x5a}, lock[4] = {9, 0xf6, 0x9a, 0};
  uint8_t bank[22] = {5, 0xfa, 0x80, s->config.foundry == 0xaa ? 0xe0 : 0xb0, 0, 8};
  const uint16_t *thresholds = selection == FTE3600_FW9369_UP ? s->fdt_up : s->fdt_down;
  for (unsigned c = 0; c < 8; ++c)
    {
      bank[6 + c * 2] = (uint8_t) (thresholds[c] >> 8);
      bank[7 + c * 2] = (uint8_t) thresholds[c];
    }
  if (!transfer (unlock, sizeof (unlock), io, d) || !transfer (bank, sizeof (bank), io, d) || !transfer (lock, sizeof (lock), io, d))
    return FTE3600_FW9369_RELEASE_TRANSPORT_ERROR;
  uint16_t value;
  if (!word (0x1881, &value, io, d))
    return FTE3600_FW9369_RELEASE_WORD_READ_ERROR;
  value = (uint16_t) ((value & 0xff7c) | (selection == FTE3600_FW9369_DOWN ? 0x81 : 0));
  uint8_t trigger[3] = {0xc2, 0x3d, 0};
  if (!write_word (0x1881, value, io, d) || !write_word (0x1a84, 0x2f, io, d) || !transfer (trigger, sizeof (trigger), io, d))
    return FTE3600_FW9369_RELEASE_TRANSPORT_ERROR;
  io->wait_ms (io->context, 1);
  return FTE3600_FW9369_RELEASE_OK;
}
enum fte3600_fw9369_release_status
fte3600_fw9369_read_irq (uint16_t *status,
                         const struct fte3600_capture_io *io, struct fte3600_fw9369_release_diagnostics *diagnostics)
{
  struct fte3600_fw9369_release_diagnostics local, *d = diagnostics ? diagnostics : &local;

  if (!status || !valid_io (io))
    return FTE3600_FW9369_RELEASE_INVALID_ARGUMENT;
  memset (d, 0, sizeof (*d));
  uint16_t value;
  if (!word (0x1a82, &value, io, d))
    return FTE3600_FW9369_RELEASE_WORD_READ_ERROR;
  if (!write_word (0x1a84, value, io, d))
    return FTE3600_FW9369_RELEASE_TRANSPORT_ERROR;
  *status = value;
  return FTE3600_FW9369_RELEASE_OK;
}
static enum fte3600_fw9369_release_status
image_update (struct fte3600_fw9369_startup_state *s,
              uint8_t reset, struct fte3600_fw9369_startup_workspace *w, size_t capacity,
              const struct fte3600_capture_io *io, struct fte3600_fw9369_release_diagnostics *d)
{
  struct fte3600_fw9369_startup_diagnostics id;
  enum fte3600_fw9369_startup_status status = fte3600_fw9369_image_baseline_checked (s, reset, w, capacity, io, &id);

  accumulate (d, &id.transport);
  d->image_status = status;
  if (status == FTE3600_FW9369_STARTUP_UNDEFINED_NEIGHBORHOOD)
    return FTE3600_FW9369_RELEASE_UNDEFINED_NEIGHBORHOOD;
  if (status)
    return FTE3600_FW9369_RELEASE_IMAGE_ERROR;
  return transport_status (d);
}
enum fte3600_fw9369_release_status
fte3600_fw9369_release_run (
  struct fte3600_fw9369_startup_state *s, int maintenance_pending,
  struct fte3600_fw9369_startup_workspace *w, size_t capacity,
  enum fte3600_fw9369_release_outcome *out, const struct fte3600_capture_io *io,
  struct fte3600_fw9369_release_diagnostics *diagnostics)
{
  struct fte3600_fw9369_release_diagnostics local, *d = diagnostics ? diagnostics : &local;

  if (!valid_state (s) || s->config.channel_mode != 1 || !valid_io (io) || !w || capacity < sizeof (*w) || !out)
    return FTE3600_FW9369_RELEASE_INVALID_ARGUMENT;
  memset (d, 0, sizeof (*d));
  uint16_t samples[3][8];
  int confirmed;
  enum fte3600_fw9369_release_status status = confirm (s, samples, &confirmed, io, d);
  if (status)
    return status;
  if (!confirmed)
    {
      *out = FTE3600_FW9369_RELEASE_PENDING;
      return FTE3600_FW9369_RELEASE_OK;
    }
  if (maintenance_pending)
    {
      (void) fte3600_fw9369_update_fdt (s, (const uint16_t (*)[8]) samples);
      struct fte3600_fw9369_observation first, second;
      status = observe (s, FTE3600_FW9369_DOWN, &first, io, d);
      if (status)
        return status;
      status = image_update (s, 0, w, capacity, io, d);
      if (status)
        return status;
      status = observe (s, FTE3600_FW9369_DOWN, &second, io, d);
      if (status)
        return status;
      d->maintained = 1;
      if (second.contact == FTE3600_FW9369_CONTACT_DOWN)
        {
          *out = FTE3600_FW9369_RELEASE_RENEWED_TOUCH;
          return FTE3600_FW9369_RELEASE_OK;
        }
      if (first.contact != FTE3600_FW9369_CONTACT_DOWN)
        {
          int needed;
          (void) fte3600_fw9369_baseline_needs_reset (s, &needed);
          if (needed)
            {
              d->recalibrated = 1;
              status = image_update (s, 1, w, capacity, io, d);
              if (status)
                return status;
            }
        }
    }
  *out = FTE3600_FW9369_RELEASE_CONFIRMED;
  return FTE3600_FW9369_RELEASE_OK;
}

static void
merge (struct fte3600_fw9369_release_diagnostics       *d,
       const struct fte3600_fw9369_release_diagnostics *child)
{
  accumulate (d, &child->transport);
  d->observations += child->observations;
  d->confirmed |= child->confirmed;
  d->maintained |= child->maintained;
  d->recalibrated |= child->recalibrated;
  if (child->image_status)
    d->image_status = child->image_status;
}
static int
byte_read (uint8_t address, uint8_t *value, const struct fte3600_capture_io *io,
           struct fte3600_fw9369_release_diagnostics *d)
{
  uint8_t b[5] = {8, 0xf7, address, 0, 0};

  if (!transfer (b, sizeof (b), io, d))
    return 0;
  *value = b[4];
  return 1;
}
static int
communication (const struct fte3600_capture_io           *io,
               struct fte3600_fw9369_release_diagnostics *d)
{
  for (unsigned check = 0; check < 3; ++check)
    {
      for (unsigned attempt = 0; attempt < 31; ++attempt)
        {
          uint8_t set[4] = {9, 0xf6, 0xc6, 1}, value;
          if (!transfer (set, sizeof (set), io, d))
            return -1;
          io->wait_ms (io->context, 4);
          if (!byte_read (0xc6, &value, io, d))
            return -1;
          if (value == 1)
            break;
        }
      uint16_t id;
      if (!word (0x1a8b, &id, io, d))
        return -1;
      if (id != 0x9362)
        return 0;
    }
  return 1;
}
static int
idle (const struct fte3600_capture_io           *io,
      struct fte3600_fw9369_release_diagnostics *d)
{
  uint8_t wake[3] = {0x5a, 0xa5, 0}, stop[3] = {0xc0, 0x3f, 0}, end[3] = {0xa5, 0x5a, 0}, value;

  if (!transfer (wake, sizeof (wake), io, d))
    return 0;
  io->wait_ms (io->context, 1);
  if (!byte_read (0x80, &value, io, d))
    return 0;
  if (value != 0x50)
    {
      if (!transfer (stop, sizeof (stop), io, d))
        return 0;
      io->wait_ms (io->context, 1);
    }
  return transfer (end, sizeof (end), io, d);
}
static enum fte3600_fw9369_release_status
handle_up (
  struct fte3600_fw9369_startup_state *s, int maintenance, unsigned *rejections, int late,
  struct fte3600_fw9369_startup_workspace *w, size_t capacity,
  enum fte3600_fw9369_event *event, const struct fte3600_capture_io *io,
  struct fte3600_fw9369_release_diagnostics *d)
{
  struct fte3600_fw9369_release_diagnostics child;
  enum fte3600_fw9369_release_outcome outcome;
  enum fte3600_fw9369_release_status status = fte3600_fw9369_release_run (s, maintenance,
                                                                          w, capacity, &outcome, io, &child);

  merge (d, &child);
  if (status)
    return status;
  if (outcome == FTE3600_FW9369_RELEASE_PENDING)
    {
      *event = FTE3600_FW9369_EVENT_PENDING;
      if (!late && ++*rejections >= 11)
        {
          *rejections = 0;
          *event = FTE3600_FW9369_EVENT_RECOVERY;
        }
      else
        {
          status = fte3600_fw9369_arm_detection (s, FTE3600_FW9369_UP, io, &child);
          merge (d, &child);
          if (status)
            return status;
        }
    }
  else
    {
      *event = outcome == FTE3600_FW9369_RELEASE_RENEWED_TOUCH ?
               FTE3600_FW9369_EVENT_TOUCH : FTE3600_FW9369_EVENT_RELEASED;
    }
  return FTE3600_FW9369_RELEASE_OK;
}
enum fte3600_fw9369_release_status
fte3600_fw9369_event_run (
  struct fte3600_fw9369_startup_state *s, int maintenance, unsigned *rejections,
  struct fte3600_fw9369_startup_workspace *w, size_t capacity,
  enum fte3600_fw9369_event *out, const struct fte3600_capture_io *io,
  struct fte3600_fw9369_release_diagnostics *diagnostics)
{
  struct fte3600_fw9369_release_diagnostics local, *d = diagnostics ? diagnostics : &local;

  if (!valid_state (s) || s->config.channel_mode != 1 || !valid_io (io) || !rejections ||
      !w || capacity < sizeof (*w) || !out)
    return FTE3600_FW9369_RELEASE_INVALID_ARGUMENT;
  memset (d, 0, sizeof (*d));
  int ready = communication (io, d);
  for (unsigned retry = 0; ready == 0 && retry < 10; ++retry)
    {
      if (!idle (io, d))
        return FTE3600_FW9369_RELEASE_TRANSPORT_ERROR;
      io->wait_ms (io->context, 20);
      ready = communication (io, d);
    }
  if (ready < 0)
    return FTE3600_FW9369_RELEASE_TRANSPORT_ERROR;
  if (!ready)
    {
      *out = FTE3600_FW9369_EVENT_RECOVERY;
      return FTE3600_FW9369_RELEASE_OK;
    }
  uint16_t initial, late;
  if (!word (0x1a82, &initial, io, d))
    return FTE3600_FW9369_RELEASE_WORD_READ_ERROR;
  if (!write_word (0x1a84, initial, io, d))
    return FTE3600_FW9369_RELEASE_TRANSPORT_ERROR;
  enum fte3600_fw9369_event event = FTE3600_FW9369_EVENT_PENDING;
  enum fte3600_fw9369_release_status status;
  if (initial & 0x610)
    {
      event = FTE3600_FW9369_EVENT_RECOVERY;
    }
  else if (initial & 2)
    {
      uint16_t value;
      if (!word (0x1a83, &value, io, d))
        return FTE3600_FW9369_RELEASE_WORD_READ_ERROR;
      if (!write_word (0x1a83, (uint16_t) (value | 0x20), io, d))
        return FTE3600_FW9369_RELEASE_TRANSPORT_ERROR;
      event = FTE3600_FW9369_EVENT_TOUCH;
    }
  else if (initial & 4)
    {
      status = handle_up (s, maintenance, rejections, 0, w, capacity, &event, io, d);
      if (status)
        return status;
    }
  else if (initial & 0x20)
    {
      uint16_t value;
      if (!word (0x1a83, &value, io, d))
        return FTE3600_FW9369_RELEASE_WORD_READ_ERROR;
      if (!write_word (0x1a83, (uint16_t) (value | 0x20), io, d))
        return FTE3600_FW9369_RELEASE_TRANSPORT_ERROR;
    }
  else
    {
      struct fte3600_fw9369_release_diagnostics child;
      status = fte3600_fw9369_arm_detection (s, s->arm_mode ? FTE3600_FW9369_DOWN : FTE3600_FW9369_UP, io, &child);
      merge (d, &child);
      if (status)
        return status;
    }
  if (!word (0x1a82, &late, io, d))
    return FTE3600_FW9369_RELEASE_WORD_READ_ERROR;
  if (late & 8)
    for (unsigned ack = 0; ack < 2; ++ack)
      if (!write_word (0x1a84, late, io, d))
        return FTE3600_FW9369_RELEASE_TRANSPORT_ERROR;
  if (late & 4)
    {
      if (!write_word (0x1a84, late, io, d))
        return FTE3600_FW9369_RELEASE_TRANSPORT_ERROR;
      status = handle_up (s, 1, rejections, 1, w, capacity, &event, io, d);
      if (status)
        return status;
    }
  *out = event;
  return FTE3600_FW9369_RELEASE_OK;
}
