/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "ft93xx_registers.h"

static const struct ft93xx_profile profiles[] = {
  {0x9349, 80, 128, 0x7ffe, 0x003f, 122, 8, 0},
  {0x9363, 64, 80, 0x4ffe, 0, 60, 7, 0},
  {0x9365, 64, 80, 0x4ffe, 0, 72, 7, 0},
  {0x9372, 56, 56, 0x06fe, 0, 62, 7, 0},
  {0x9391, 40, 196, 0xc7fe, 0x7fff, 124, 5, 3},
  {0x9392, 40, 196, 0xc7fe, 0x7fff, 124, 5, 3},
  {0x9395, 36, 176, 0xaffe, 0x0fff, 120, 8, 0},
  {0x9396, 36, 96, 0x5ffe, 0x0003, 128, 8, 0},
  {0x9397, 32, 176, 0xaffe, 0x0fff, 97, 11, 3},
  {0x9398, 36, 144, 0x8ffe, 0x00ff, 128, 8, 0}
};

const struct ft93xx_profile *
ft93xx_lookup_profile (uint16_t id)
{
  for (size_t i = 0; i < sizeof (profiles) / sizeof (profiles[0]); ++i)
    if (profiles[i].id == id)
      return &profiles[i];
  return NULL;
}

static int
checked_write (const struct ft93xx_operation *op,
               struct fte3600_ft93xx_state *state,
               uint16_t address, uint16_t value)
{
  uint16_t observed;

  if (!ft93xx_write_word (op, address, value))
    return 0;
  if (state->check_registers == 1)
    {
      if (!ft93xx_read_word (op, address, &observed))
        return 0;
      if (observed != value)
        state->check_registers = 2;
    }
  return 1;
}

int
ft93xx_idle (const struct ft93xx_operation *op)
{
  uint8_t value;

  if (!ft93xx_command (op, 0x5a))
    return 0;
  ft93xx_wait_ms (op, 1);
  if (!ft93xx_read_byte (op, 0x80, &value))
    return 0;
  for (unsigned int i = 0; value != 0x50 && i < 5; ++i)
    {
      if (!ft93xx_command (op, 0xc0))
        return 0;
      ft93xx_wait_ms (op, 1);
      if (!ft93xx_read_byte (op, 0x80, &value))
        return 0;
      if (value != 0x50)
        ft93xx_wait_ms (op, 1);
    }
  if (op->diagnostics != NULL)
    op->diagnostics->idle_unacknowledged |= value != 0x50;
  return ft93xx_command (op, 0xa5);
}

int
ft93xx_configure_image (const struct ft93xx_operation *op, const struct ft93xx_profile *p,
                        struct fte3600_ft93xx_state *state, uint8_t mode)
{
  static const uint8_t byte_settings[][2] = {
    {0x8e, 0xf4}, {0x8d, 0x61}, {0x8f, 0xff}, {0x90, 0},
    {0x91, 7}, {0x92, 0xd0}, {0x90, 1}
  };
  uint16_t value;
  uint16_t dac = (uint16_t) (0x3000u | ((unsigned int) state->dac_polarity << 14) |
                             ((unsigned int) state->gain << 8) | state->dac);
  uint16_t timing = (uint16_t) (((unsigned int) state->integration - 1u) << 5 |
                                state->sample_time);
  uint8_t prior_mode = state->mode;

  if (!ft93xx_idle (op) || !checked_write (op, state, 0x1801, dac) ||
      !checked_write (op, state, 0x1800, p->window) ||
      !checked_write (op, state, 0x1816, p->extension) ||
      !checked_write (op, state, 0x1807, timing) ||
      !checked_write (op, state, 0x1806, 0x023b) ||
      !checked_write (op, state, 0x180a, 1) ||
      !checked_write (op, state, 0x180b, 0x0104) ||
      !ft93xx_read_word (op, 0x1812, &value))
    return 0;
  value = (uint16_t) ((value & 0x8000) |
                      ((unsigned int) state->subtract_baseline << 14) | 0x0100);
  if (!checked_write (op, state, 0x1812, value))
    return 0;
  state->mode = mode;
  if (prior_mode == 0xa3 || prior_mode == 0xa4)
    return 1;
  value = (uint16_t) (0x27c0 | state->sample_mode |
                      (state->sample_mode > 1 ? 8 : 0));
  if (!checked_write (op, state, 0x1804, value))
    return 0;
  if (state->configured == 1)
    return 1;
  if (!checked_write (op, state, 0x1811, 0x01fe))
    return 0;
  for (unsigned int bit = 0x20; bit <= 0x40; bit <<= 1)
    {
      if (!ft93xx_read_word (op, 0x1a83, &value))
        return 0;
      if ((value & bit) == 0 &&
          !ft93xx_write_word (op, 0x1a83, (uint16_t) (value | bit)))
        return 0;
    }
  state->configured = 1;
  for (size_t i = 0; i < sizeof (byte_settings) / sizeof (byte_settings[0]); ++i)
    if (!ft93xx_write_byte (op, byte_settings[i][0], byte_settings[i][1]))
      return 0;
  return ft93xx_write_word (op, 0x1a06, 1);
}

int
ft93xx_spi_mode (const struct ft93xx_operation *op)
{
  uint8_t byte = 0;

  for (unsigned int attempt = 0; attempt < 4; ++attempt)
    {
      if (!ft93xx_write_byte (op, 0xc6, 1))
        return 0;
      ft93xx_wait_ms (op, 4);
      if (!ft93xx_read_byte (op, 0xc6, &byte))
        return 0;
      if (byte == 1)
        break;
    }
  if (op->diagnostics != NULL)
    op->diagnostics->spi_mode_unacknowledged |= byte != 1;
  return 1;
}

static int
masked_write (const struct ft93xx_operation *op,
              struct fte3600_ft93xx_state *state,
              uint16_t address, uint16_t mask, uint16_t bits)
{
  uint16_t value;

  return ft93xx_read_word (op, address, &value) &&
         checked_write (op, state, address, (uint16_t) ((value & mask) | bits));
}

static int
analog_channels (const struct ft93xx_operation *op, uint16_t chip,
                 struct fte3600_ft93xx_state *state)
{
  if (chip == 0x9349)
    return checked_write (op, state, 0xc0, 0x0440) &&
           checked_write (op, state, 0xc1, 0x00c0) &&
           checked_write (op, state, 0xc2, 0x0044);
  if (chip == 0x9363 || chip == 0x9365)
    return masked_write (op, state, 0xc0, 0xe000, 0x0444) &&
           masked_write (op, state, 0xc1, 0xffc0, 0x0021) &&
           masked_write (op, state, 0xc2, 0xffff, 0x00c0);
  if (chip == 0x9372)
    return checked_write (op, state, 0xc0, 0x0920) &&
           checked_write (op, state, 0xc1, 0x0c8e);
  static const uint8_t channels[][4] = {
    {3, 9, 15, 20}, {2, 8, 13, 19}, {0, 4, 7, 11}, {2, 6, 11, 15}
  };
  unsigned int row = chip == 0x9391 || chip == 0x9392 ? 0 :
                     chip == 0x9396 ? 2 : chip == 0x9398 ? 3 : 1;
  uint16_t bits = row == 0 ? 0x0103 : chip == 0x9395 ? 0x0083 : 0x0080;
  for (size_t i = 0; i < 4; ++i)
    {
      uint16_t address = (uint16_t) (0xc0 + 2 * channels[row][i]);
      if (!masked_write (op, state, address, 0xe000, bits) ||
          !masked_write (op, state, (uint16_t) (address + 1), 0xe000, 0))
        return 0;
    }
  return 1;
}

int
ft93xx_configure_fdt (const struct ft93xx_operation *operation,
                      uint16_t chip_id, uint8_t foundry,
                      struct fte3600_ft93xx_state *state,
                      struct fte3600_ft93xx_fdt_state *fdt, uint16_t replacement)
{
  const struct ft93xx_operation op = *operation;
  uint16_t word;
  unsigned int gain = chip_id == 0x9391 || chip_id == 0x9392 ? 7 :
                      chip_id == 0x9397 ? 11 : 10;
  uint16_t dac = (uint16_t) (0x3000u | ((unsigned int) state->dac_polarity << 14) |
                             (gain << 8) | fdt->fdt_dac);

  if (!ft93xx_idle (&op) || !checked_write (&op, state, 0x1801, dac))
    return 0;
  if (fdt->threshold_cached != fdt->threshold_mode)
    {
      if (!checked_write (&op, state, 0x180c, fdt->threshold_mode ? 0 : (uint16_t) (0x0400 | replacement)))
        return 0;
      fdt->threshold_cached = fdt->threshold_mode;
    }
  int newer = chip_id >= 0x9391;
  if (!masked_write (&op, state, 0x1807, 0xfff0, 1) ||
      !checked_write (&op, state, 0x1806, newer ? 0x09bb : 0x04bb) ||
      !checked_write (&op, state, 0x180a, newer ? 0x0987 : 0x0482) ||
      !checked_write (&op, state, 0x180b, newer ? 0x0491 : 0x0207) ||
      !masked_write (&op, state, 0x1812, 0x8000,
                     (uint16_t) (((unsigned int) state->subtract_baseline << 14) |
                                 (newer ? 0x0493 : 0x0209))))
    return 0;
  if (!newer && !masked_write (&op, state, 0x1810, 0xffff, 0x0380))
    return 0;
  if (state->mode == 0xa1)
    return 1;
  int wide = chip_id == 0x9391 || chip_id == 0x9392 || chip_id == 0x9395;
  if (!checked_write (&op, state, 0x1881, wide ? 0x20ce : 0x0fce))
    return 0;
  state->mode = 0xa1;
  uint16_t window = 2, extension = 0;
  switch (chip_id)
    {
    case 0x9391:
    case 0x9392: window = 0x1c10;
      extension = 0x0420;
      break;

    case 0x9395:
    case 0x9397: window = 0x1a08;
      extension = 0x0208;
      break;

    case 0x9396: window = 0x1922;
      extension = 2;
      break;

    case 0x9398: window = 0x1888;
      extension = 0x0022;
      break;

    default: break;
    }
  if (!checked_write (&op, state, 0x1800, window) ||
      (newer && !checked_write (&op, state, 0x1816, extension)) ||
      !checked_write (&op, state, 0x1804, 0x07c0))
    return 0;
  if (fdt->fdt_configured)
    return 1;
  uint16_t mode = wide ? 0x084b : chip_id == 0x9349 ? 0x0803 :
                  chip_id == 0x9372 ? 0x080b : 0x0807;
  uint16_t bias = chip_id != 0x9372 || foundry == 2 ? 0x5f64 :
                  foundry == 3 ? 0x5055 : 0;
  uint16_t routing = wide ? 0 : chip_id >= 0x9396 ? 4 : 1;
  if (!checked_write (&op, state, 0x1808, mode) ||
      !checked_write (&op, state, 0x1815, 0) ||
      !checked_write (&op, state, 0x180d, 0x03e8) ||
      !checked_write (&op, state, 0x1880, bias) ||
      !checked_write (&op, state, 0x1884, 3) ||
      !checked_write (&op, state, 0x1887, routing) ||
      !checked_write (&op, state, 0x1888, 0) ||
      !ft93xx_write_byte (&op, 0x9a, 0x5a) || !analog_channels (&op, chip_id, state) ||
      !ft93xx_write_byte (&op, 0x9a, 0) || !ft93xx_read_word (&op, 0x1a83, &word))
    return 0;
  if ((word & 8) == 0 && !ft93xx_write_word (&op, 0x1a83, (uint16_t) (word | 8)))
    return 0;
  fdt->fdt_configured = 1;
  return 1;
}
