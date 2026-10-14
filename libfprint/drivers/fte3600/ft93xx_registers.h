/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_FT93XX_REGISTERS_H
#define FTE3600_FT93XX_REGISTERS_H

#include "ft93xx_wire.h"

struct ft93xx_profile
{
  uint16_t id, width, height, window, extension;
  uint8_t  dac, gain, polarity;
};

FT93XX_INTERNAL const struct ft93xx_profile *ft93xx_lookup_profile (uint16_t id);
FT93XX_INTERNAL int ft93xx_idle (const struct ft93xx_operation *op);
FT93XX_INTERNAL int ft93xx_spi_mode (const struct ft93xx_operation *op);
FT93XX_INTERNAL int ft93xx_configure_image (const struct ft93xx_operation *op,
                                            const struct ft93xx_profile   *profile,
                                            struct fte3600_ft93xx_state   *state,
                                            uint8_t                        mode);
FT93XX_INTERNAL int ft93xx_configure_fdt (const struct ft93xx_operation   *operation,
                                          uint16_t                         chip_id,
                                          uint8_t                          foundry,
                                          struct fte3600_ft93xx_state     *state,
                                          struct fte3600_ft93xx_fdt_state *fdt,
                                          uint16_t                         replacement);

#endif
