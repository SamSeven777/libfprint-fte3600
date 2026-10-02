/* SPDX-License-Identifier: GPL-2.0-only
 * Compile the extracted reference bodies, not a reimplementation of them. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#define SPI_MODE_0 0
#define GPIOD_OUT_LOW 0
#define EPROBE_DEFER 517
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define PTR_ERR(p) ((int)(intptr_t)(p))
struct gpio_desc { int active_low; };
struct device { int unused; };
struct spi_device { struct device dev; unsigned mode, bits_per_word, max_speed_hz; };
struct focal_fp_data { struct gpio_desc *gpiod_rst; struct spi_device *spi; };
static struct gpio_desc gpio;
static int available, fail_at, lookup_count, values[16], writes, setup_result;
static unsigned elapsed;
static void dev_log(struct device *d, const char *fmt, ...) { (void)d; (void)fmt; }
#define dev_dbg dev_log
#define dev_err dev_log
static struct gpio_desc *devm_gpiod_get_optional(struct device *d, const char *name, int flags)
{
  static const char *names[] = {"reset", "power", "enable"};
  (void)d; assert(flags == GPIOD_OUT_LOW); assert(lookup_count < 3);
  assert(!strcmp(name, names[lookup_count++]));
  if (lookup_count == fail_at) return (void *)(intptr_t)-EPROBE_DEFER;
  return lookup_count == available ? &gpio : NULL;
}
static void gpiod_set_value(struct gpio_desc *g, int value)
{
  if (g) { assert(writes < 16); values[writes++] = (!!value) ^ g->active_low; }
}
#define gpiod_set_value_cansleep gpiod_set_value
static void msleep(unsigned n) { elapsed += 1000 * n; }
static void usleep_range(unsigned lo, unsigned hi) { assert(hi >= lo); elapsed += lo; }
static int spi_setup(struct spi_device *s) { (void)s; return setup_result; }
#include "ctfdavis-core.inc"
int main(void)
{
  struct spi_device spi = {0};
  struct focal_fp_data data = {.spi = &spi};
  for (int mapping = 0; mapping < 3; mapping++) {
    gpio.active_low = mapping == 2;
    data.gpiod_rst = mapping ? &gpio : NULL;
    for (int ioctl_phase = 0; ioctl_phase < 2; ioctl_phase++) {
      writes = elapsed = 0;
      if (ioctl_phase) focal_spi_reset(&data); else focal_spi_hw_reset(&data);
      assert(writes == (mapping ? (ioctl_phase ? 2 : 3) : 0));
      assert(elapsed == (ioctl_phase ? 10000U : mapping ? 60000U : 0U));
      if (mapping) {
        assert(values[0] == gpio.active_low && values[1] != gpio.active_low);
        if (!ioctl_phase) assert(values[2] == gpio.active_low);
      }
    }
    writes = 0;
    focal_spi_power_off(&data); focal_spi_power_on(&data);
    assert(writes == (mapping ? 2 : 0));
  }
  for (available = 0; available <= 3; available++) {
    writes = lookup_count = fail_at = 0;
    assert(focal_spi_get_gpio_config(&data) == 0);
    assert(lookup_count == (available ? available : 3));
    assert(data.gpiod_rst == (available ? &gpio : NULL));
    assert(writes == !!available);
  }
  available = 0;
  for (fail_at = 1; fail_at <= 3; fail_at++) {
    lookup_count = 0;
    assert(focal_spi_get_gpio_config(&data) == -EPROBE_DEFER);
    assert(lookup_count == fail_at);
  }
  unsigned speeds[] = {0, 500000, 4000000};
  for (unsigned i = 0; i < 3; i++) {
    spi.mode = 4; spi.bits_per_word = 16; spi.max_speed_hz = speeds[i];
    assert(focal_spi_configure_spi(&data) == 0);
    assert(spi.mode == 0 && spi.bits_per_word == 8);
    assert(spi.max_speed_hz == (speeds[i] == 500000 ? 500000 : 1000000));
  }
  setup_result = -EIO;
  assert(focal_spi_configure_spi(&data) == -EIO);
  puts("PASS: archived GPIO lookup, polarity, reset phases, power helpers and SPI setup");
}
