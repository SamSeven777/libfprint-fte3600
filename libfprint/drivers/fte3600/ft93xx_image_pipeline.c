/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_ft93xx_image_pipeline.h"

#include <fenv.h>
#include <float.h>
#include <limits.h>
#include <string.h>

_Static_assert (CHAR_BIT == 8 && sizeof (float) == 4 && FLT_RADIX == 2 &&
                FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
                "The reviewed normalization contract requires IEEE binary32");

/* Volatile stores make each primitive rounding point explicit, including
 * targets with excess precision. The build disables contraction/fast-math. */
static float
addf (float a, float b)
{
  volatile float r = a + b;

  return r;
}
static float
subf (float a, float b)
{
  volatile float r = a - b;

  return r;
}
static float
mulf (float a, float b)
{
  volatile float r = a * b;

  return r;
}
static float
divf (float a, float b)
{
  volatile float r = a / b;

  return r;
}
static float
intf (int32_t a)
{
  volatile float r = (float) a;

  return r;
}

static int32_t
signed_bits (uint32_t a)
{
  return a <= INT32_MAX ? (int32_t) a : -1 - (int32_t) (UINT32_MAX - a);
}

static int32_t
wrap_sub (int32_t a, int32_t b)
{
  return signed_bits ((uint32_t) a - (uint32_t) b);
}

static int32_t
trunc_int (float a)
{
  /* 2^31 is exact in binary32; INT_MAX is not. NaNs also take this branch. */
  if (!(a >= -2147483648.0f && a < 2147483648.0f))
    return INT32_MIN;
  return (int32_t) a;
}

static float
float_bits (uint32_t bits)
{
  float f;

  memcpy (&f, &bits, sizeof (f));
  return f;
}

static int
shape (uint16_t chip, struct fte3600_ft93xx_geometry *g)
{
  static const uint16_t profiles[][3] = {
    {0x9349, 80, 128}, {0x9363, 64, 80}, {0x9365, 64, 80}, {0x9372, 56, 56},
    {0x9391, 40, 196}, {0x9392, 40, 196}, {0x9395, 36, 176}, {0x9396, 36, 96},
    {0x9397, 32, 176}, {0x9398, 36, 144}
  };

  for (size_t i = 0; i < sizeof (profiles) / sizeof (profiles[0]); ++i)
    {
      if (chip == profiles[i][0])
        {
          *g = (struct fte3600_ft93xx_geometry){profiles[i][1], profiles[i][2],
                                                (chip == 0x9391 || chip == 0x9392) ? 4 : 0};
          return 1;
        }
    }
  return 0;
}

enum fte3600_ft93xx_pipeline_status
fte3600_ft93xx_processing_defaults (
  uint16_t chip, uint8_t version, struct fte3600_ft93xx_processing_state *p)
{
  struct fte3600_ft93xx_state s = {0};

  if (p == NULL)
    return FTE3600_FT93XX_PIPELINE_INVALID_ARGUMENT;
  if (fte3600_ft93xx_configuration (chip, version, &s) != FTE3600_FT93XX_OK)
    return FTE3600_FT93XX_PIPELINE_UNSUPPORTED_DEVICE;
  *p = (struct fte3600_ft93xx_processing_state){s.integration, 0};
  return FTE3600_FT93XX_PIPELINE_OK;
}

size_t
fte3600_ft93xx_pipeline_workspace_size (
  uint16_t chip, enum fte3600_ft93xx_pipeline_action action)
{
  struct fte3600_ft93xx_geometry g;

  if ((action != FTE3600_FT93XX_PIPELINE_CAPTURE &&
       action != FTE3600_FT93XX_PIPELINE_NORMALIZE) || !shape (chip, &g))
    return 0;
  /* queue:u32, horizontal:f32, means:f32, scaled:u16, base/cdf/mask:u8.
   * Every admitted N is divisible by four, preserving array alignment. */
  return (size_t) g.width * g.height * 17;
}

static void
blend_pixel (uint16_t *p, uint16_t neighbor)
{
  if (neighbor == 4092)
    return;
  uint16_t bits = (uint16_t) (neighbor - *p);
  int32_t difference = bits <= INT16_MAX ? bits : (int32_t) bits - 65536;
  *p = (uint16_t) ((int32_t) *p + (80 * difference) / 100);
}

static void
blend (uint16_t *p, size_t w, size_t h)
{
  for (size_t y = 0; y < h; ++y)
    blend_pixel (&p[y * w], p[y * w + 1]);
  for (size_t y = 0; y < h; ++y)
    blend_pixel (&p[y * w + w - 1], p[y * w + w - 2]);
  for (size_t x = 0; x < w; ++x)
    blend_pixel (&p[x], p[w + x]);
}

static void
white (uint16_t *p, size_t w, size_t h)
{
  for (size_t y = 1; y + 1 < h; ++y)
    {
      for (size_t x = 1; x + 1 < w; ++x)
        {
          size_t i = y * w + x;
          uint16_t neighbors[4] = {p[i - 1], p[i + 1], p[i - w], p[i + w]};
          unsigned int high = 0, low = 0, total = 0, skip = 0;
          unsigned int lower = p[i] < 200 ? 0 : p[i] - 200;
          for (unsigned int j = 0; j < 4; ++j)
            {
              skip |= neighbors[j] == 4092;
              high += neighbors[j] > (unsigned int) p[i] + 200;
              low += neighbors[j] < lower;
              total += neighbors[j];
            }
          if (!skip && (high >= 3 || low >= 3))
            p[i] = (uint16_t) (total / 4);
        }
    }
}

static void
noise (uint8_t *p, size_t w, size_t h)
{
  uint32_t rows[196], total = 0;

  for (size_t y = 0; y < h; ++y)
    {
      uint32_t row = 0;
      for (size_t x = 0; x < w; ++x)
        row += p[y * w + x];
      total += row;
      rows[y] = row / (uint32_t) w;
    }
  int32_t mean = (int32_t) (total / (uint32_t) (w * h));
  for (size_t y = 1; y + 1 < h; ++y)
    {
      for (size_t x = 0; x < w; ++x)
        {
          int32_t value = p[y * w + x] - (int32_t) rows[y] + mean;
          p[y * w + x] = (uint8_t) (value < 5 ? 5 : value > 250 ? 250 : value);
        }
    }
}

static size_t
reflect (int i, size_t length)
{
  int n = (int) length;

  while (i < 0 || i >= n)
    i = i < 0 ? -i : 2 * n - i - 2;
  return (size_t) i;
}

static float
sample (const uint8_t *p, size_t i, int squared)
{
  return intf (squared ? (int32_t) p[i] * p[i] : p[i]);
}

static void
box_horizontal (const uint8_t *p, size_t w, size_t h,
                int squared, float *horizontal)
{
  for (size_t y = 0; y < h; ++y)
    {
      float sum = 0.0f;
      for (int x = -4; x <= 4; ++x)
        sum = addf (sum, sample (p, y * w + reflect (x, w), squared));
      horizontal[y * w] = sum;
      for (size_t x = 1; x < w; ++x)
        {
          float entering = sample (p, y * w + reflect ((int) x + 4, w), squared);
          float leaving = sample (p, y * w + reflect ((int) x - 5, w), squared);
          sum = addf (sum, subf (entering, leaving));
          horizontal[y * w + x] = sum;
        }
    }
}

static float
deviation (float variance)
{
  if (variance <= 0.0f)
    return 0.0f;
  uint32_t bits;
  memcpy (&bits, &variance, sizeof (bits));
  float y = float_bits (UINT32_C (0x5f375a86) - (bits >> 1));
  float a = mulf (variance, 0.5f);
  float b = mulf (a, y);
  float d = mulf (b, y);
  float e = subf (1.5f, d);
  float f = mulf (e, y);
  return divf (1.0f, f);
}

static void
invalid_mask (const uint8_t *p, size_t w, size_t h,
              float *horizontal, float *means, uint8_t *mask)
{
  for (int squared = 0; squared <= 1; ++squared)
    {
      box_horizontal (p, w, h, squared, horizontal);
      for (size_t x = 0; x < w; ++x)
        {
          float acc = 0.0f;
          for (int y = -4; y <= 3; ++y)
            acc = addf (acc, horizontal[reflect (y, h) * w + x]);
          for (size_t y = 0; y < h; ++y)
            {
              float entering = horizontal[reflect ((int) y + 4, h) * w + x];
              float leaving = horizontal[reflect ((int) y - 4, h) * w + x];
              float mean = divf (addf (acc, entering), 81.0f);
              acc = addf (subf (entering, leaving), acc);
              size_t i = y * w + x;
              if (!squared)
                means[i] = mean;
              else
                mask[i] = deviation (subf (mean, mulf (means[i], means[i]))) < 5.0f;
            }
        }
    }
}

static void
components (uint8_t *mask, uint32_t *queue, size_t w, size_t h)
{
  size_t n = w * h;
  uint32_t counts[256] = {0};
  unsigned int label = 2;

  for (size_t seed = 0; seed < n && label <= 255; ++seed)
    {
      if (mask[seed] != 1)
        continue;
      size_t head = 0, tail = 0;
      queue[tail++] = (uint32_t) seed;
      mask[seed] = (uint8_t) label;
      while (head < tail)
        {
          size_t i = queue[head++], x = i % w, y = i / w;
          size_t neighbors[4] = {x ? i - 1 : i, x + 1 < w ? i + 1 : i,
                                 y ? i - w : i, y + 1 < h ? i + w : i};
          for (unsigned int j = 0; j < 4; ++j)
            {
              size_t neighbor = neighbors[j];
              if (mask[neighbor] == 1)
                {
                  mask[neighbor] = (uint8_t) label;
                  queue[tail++] = (uint32_t) neighbor;
                }
            }
        }
      ++label;
    }
  for (size_t i = 0; i < n; ++i)
    ++counts[mask[i]];
  int retained = 0;
  for (unsigned int l = 2; l < 256; ++l)
    {
      if (counts[l] < n / 50)
        counts[l] = 0;
      retained |= counts[l] != 0;
    }
  for (size_t i = 0; i < n; ++i)
    mask[i] = (uint8_t) (retained && mask[i] != 0 && counts[mask[i]] != 0);
}

static void
dilate (uint8_t *mask, uint8_t *temp, size_t w, size_t h)
{
  for (size_t y = 0; y < h; ++y)
    {
      for (size_t x = 0; x < w; ++x)
        {
          uint8_t value = 0;
          for (int dx = -9; dx <= 9; ++dx)
            value |= mask[y * w + reflect ((int) x + dx, w)];
          temp[y * w + x] = value;
        }
    }
  for (size_t y = 0; y < h; ++y)
    {
      for (size_t x = 0; x < w; ++x)
        {
          uint8_t value = y < 8 || y >= h - 8 || x == 0 || x == w - 1;
          for (int dy = -12; dy <= 12; ++dy)
            value |= temp[reflect ((int) y + dy, h) * w + x];
          mask[y * w + x] = value;
        }
    }
}

static int
cutoff (const uint32_t *hist, int32_t k, int upper)
{
  int32_t cumulative = 0;
  int bin = upper ? 255 : 0;

  for (; upper ? bin >= 1 : bin <= 255; bin += upper ? -1 : 1)
    {
      int32_t count = (int32_t) hist[bin];
      if (cumulative + count > k)
        return k - cumulative < count - k + cumulative ? bin + (upper ? 1 : -1) : bin;
      cumulative += count;
    }
  return 0;
}

static void
normalize (const uint16_t *p, uint8_t *out, size_t w, size_t h, void *workspace)
{
  size_t n = w * h;
  uint32_t *queue = workspace;
  float *horizontal = (float *) (queue + n), *means = horizontal + n;
  uint16_t *a = (uint16_t *) (means + n);
  uint8_t *b = (uint8_t *) (a + n), *c = b + n, *mask = c + n;
  uint16_t max = 0, min = 4092;

  for (size_t i = 0; i < n; ++i)
    if (p[i] != 4092)
      {
        if (p[i] > max)
          max = p[i];
        if (p[i] < min)
          min = p[i];
      }
  float scale16 = max > min ? divf (2048.0f, intf (max - min)) : 0.0f;
  float scale8 = max > min ? divf (254.0f, intf (max - min)) : 0.0f;
  uint32_t sum = 0, hist[256] = {0};
  for (size_t i = 0; i < n; ++i)
    {
      float t = intf ((int32_t) max - p[i]);
      a[i] = (uint16_t) trunc_int (mulf (t, scale16));
      b[i] = (uint8_t) trunc_int (mulf (t, scale8));
      sum += b[i];
    }
  uint32_t t = sum / (uint32_t) (n + 1);
  uint8_t cap = (uint8_t) (t >= 169 && t <= 199 ? t : 200), table[256];
  for (size_t i = 0; i < n; ++i)
    {
      c[i] = b[i] < cap ? b[i] : cap;
      ++hist[c[i]];
    }
  sum = 0;
  for (unsigned int v = 0; v < 256; ++v)
    {
      sum += hist[v];
      table[v] = (uint8_t) (255 * sum / n);
    }
  for (size_t i = 0; i < n; ++i)
    c[i] = table[c[i]];
  invalid_mask (c, w, h, horizontal, means, mask);
  components (mask, queue, w, h);
  dilate (mask, c, w, h);

  memset (hist, 0, sizeof (hist));
  int32_t bmin = INT32_MAX, bmax = INT32_MIN;
  int32_t amin = INT32_MAX, amax = INT32_MIN, valid = 0;
  for (size_t i = 0; i < n; ++i)
    if (!mask[i])
      {
        ++valid;
        ++hist[b[i]];
        if (b[i] < bmin)
          bmin = b[i];
        if (b[i] > bmax)
          bmax = b[i];
        if (a[i] < amin)
          amin = a[i];
        if (a[i] > amax)
          amax = a[i];
      }
  int32_t k = trunc_int (mulf (intf (valid), float_bits (UINT32_C (0x3c23d70a))));
  int upper = cutoff (hist, k, 1), lower = cutoff (hist, k, 0);
  int32_t brange = wrap_sub (bmax, bmin);
  float uratio = 1.0f, lratio = 0.0f;
  if (brange >= 1)
    {
      uratio = divf (intf (wrap_sub (upper > 255 ? 255 : upper, bmin)), intf (brange));
      lratio = divf (intf (wrap_sub (lower < 0 ? 0 : lower, bmin)), intf (brange));
    }
  float range = intf (wrap_sub (amax, amin));
  int32_t lo = trunc_int (addf (mulf (range, lratio), intf (amin)));
  int32_t hi = trunc_int (addf (mulf (range, uratio), intf (amin)));
  if (lo >= hi)
    {
      memset (out, 254, n);
      return;
    }
  float scale = divf (250.0f, intf (wrap_sub (hi, lo)));
  for (size_t i = 0; i < n; ++i)
    {
      int32_t x = trunc_int (mulf (intf (wrap_sub (a[i], lo)), scale));
      out[i] = (uint8_t) (x < 0 ? 0 : x >= 250 ? 254 : x);
    }
}

static int
valid_state (const struct fte3600_ft93xx_state *s)
{
  return s->gain <= 15 && s->dac_polarity <= 3 &&
         s->sample_time <= 15 && s->sample_mode <= 3 &&
         s->subtract_baseline <= 1 && s->configured <= 1 && s->check_registers <= 2;
}

enum fte3600_ft93xx_pipeline_status
fte3600_ft93xx_pipeline_run (
  uint16_t chip, uint16_t raw, const struct fte3600_ft93xx_geometry *g,
  struct fte3600_ft93xx_state *s, struct fte3600_ft93xx_processing_state *p,
  enum fte3600_ft93xx_pipeline_action action,
  const struct fte3600_ft93xx_pipeline_buffers *b, void *workspace,
  size_t workspace_bytes, const struct fte3600_ft93xx_io *io,
  struct fte3600_ft93xx_pipeline_diagnostics *d)
{
  struct fte3600_ft93xx_geometry expected;

  if (!shape (chip, &expected))
    return FTE3600_FT93XX_PIPELINE_UNSUPPORTED_DEVICE;
  if (g == NULL || s == NULL || p == NULL || b == NULL ||
      action < FTE3600_FT93XX_PIPELINE_CAPTURE || action > FTE3600_FT93XX_PIPELINE_NOISE ||
      g->width != expected.width || g->height != expected.height)
    return FTE3600_FT93XX_PIPELINE_INVALID_ARGUMENT;
  size_t n = (size_t) g->width * g->height, raw_bytes = 0;
  int physical = action == FTE3600_FT93XX_PIPELINE_CAPTURE || action == FTE3600_FT93XX_PIPELINE_DIFFERENCE;
  int bytes = action == FTE3600_FT93XX_PIPELINE_CAPTURE || action == FTE3600_FT93XX_PIPELINE_NORMALIZE || action == FTE3600_FT93XX_PIPELINE_NOISE;
  if ((action != FTE3600_FT93XX_PIPELINE_NOISE &&
       (b->processed == NULL || b->processed_samples < n)) ||
      (bytes && (b->image == NULL || b->image_bytes < n)))
    return FTE3600_FT93XX_PIPELINE_INVALID_ARGUMENT;
  if (physical && (!valid_state (s) || io == NULL || io->spi == NULL || io->wait_ms == NULL ||
                   b->raw == NULL || ((chip == 0x9391 || chip == 0x9392) && g->extra_rows != 4) ||
                   fte3600_ft93xx_payload_size (g, 1, &raw_bytes) != FTE3600_FT93XX_OK || b->raw_bytes < raw_bytes))
    return FTE3600_FT93XX_PIPELINE_INVALID_ARGUMENT;
  if (!physical && action != FTE3600_FT93XX_PIPELINE_NOISE)
    for (size_t i = 0; i < n; ++i)
      if (b->processed[i] > 4092)
        return FTE3600_FT93XX_PIPELINE_INVALID_ARGUMENT;
  size_t need = fte3600_ft93xx_pipeline_workspace_size (chip, action);
  if (need && (workspace == NULL || workspace_bytes < need ||
               (uintptr_t) workspace % _Alignof (float) != 0 || (uintptr_t) workspace % _Alignof (uint32_t) != 0))
    return FTE3600_FT93XX_PIPELINE_RESOURCE_ERROR;
  if (need && fegetround () != FE_TONEAREST)
    return FTE3600_FT93XX_PIPELINE_FLOAT_ENVIRONMENT_ERROR;
  if (d != NULL)
    memset (d, 0, sizeof (*d));
  int overflow = 0;
  if (physical)
    {
      if (d != NULL)
        d->captures = 1;
      enum fte3600_ft93xx_status status = fte3600_ft93xx_capture_retained (chip, raw, g, s, 1,
                                                                           b->raw, b->raw_bytes, io, d == NULL ? NULL : &d->transport);
      if (status != FTE3600_FT93XX_OK)
        return (enum fte3600_ft93xx_pipeline_status) status;
      memcpy (b->processed, b->raw, n * sizeof (uint16_t));
      size_t low = 0;
      for (size_t i = 0; i < n; ++i)
        low += b->processed[i] < 8;
      overflow = low > 9 * n / 100;
      if (overflow)
        {
          s->integration = (uint16_t) (s->integration - 32);
          int32_t lower = (int32_t) p->integration_default - 64;
          if ((int32_t) s->integration < lower)
            {
              s->integration = (uint16_t) lower;
              p->integration_limited = 1;
            }
        }
      if (d != NULL)
        d->overflow = (uint8_t) overflow;
    }
  if (physical || action == FTE3600_FT93XX_PIPELINE_EDGE_BLEND)
    blend (b->processed, g->width, g->height);
  if ((physical && !overflow) || action == FTE3600_FT93XX_PIPELINE_WHITE_POINTS)
    white (b->processed, g->width, g->height);
  if (need)
    normalize (b->processed, b->image, g->width, g->height, workspace);
  int correct_noise = action == FTE3600_FT93XX_PIPELINE_NOISE;
  if (action == FTE3600_FT93XX_PIPELINE_CAPTURE)
    {
      size_t noisy_pairs = 0;
      for (size_t y = 1; y < g->height; ++y)
        {
          size_t columns = 0;
          for (size_t x = 0; x < g->width; ++x)
            {
              int delta = b->image[y * g->width + x] - b->image[(y - 1) * g->width + x];
              columns += delta > 70 || delta < -70;
            }
          noisy_pairs += columns > g->width / 3;
        }
      correct_noise = noisy_pairs > g->height / 10;
    }
  if (correct_noise)
    {
      noise (b->image, g->width, g->height);
      if (d != NULL)
        d->noise_applied = 1;
    }
  if (action == FTE3600_FT93XX_PIPELINE_CAPTURE)
    for (size_t i = 0; i < n; ++i)
      if (b->processed[i] == 4092)
        b->image[i] = 255;
  if (d != NULL)
    d->completed = 1;
  return FTE3600_FT93XX_PIPELINE_OK;
}
