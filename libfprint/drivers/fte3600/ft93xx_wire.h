/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FTE3600_FT93XX_WIRE_H
#define FTE3600_FT93XX_WIRE_H

#include "fte3600_ft93xx.h"

/* Private cross-file declarations; keep them out of the shared-library ABI. */
#if defined(__GNUC__)
#define FT93XX_INTERNAL __attribute__((visibility ("hidden")))
#else
#define FT93XX_INTERNAL
#endif

/* Public entry points validate I/O and storage before using an operation.
 * Helpers preserve completed I/O and set error only for a rejected reply CRC. */
struct ft93xx_operation
{
  const struct fte3600_ft93xx_io    *io;
  struct fte3600_ft93xx_diagnostics *diagnostics;
  enum fte3600_ft93xx_status *error;
};

/* CRC-16/CCITT, initial 0xffff, also used for the retained FDT baseline checksum. */
FT93XX_INTERNAL uint16_t ft93xx_crc16 (const uint8_t *bytes,
                                       size_t         length);
FT93XX_INTERNAL void ft93xx_wait_ms (const struct ft93xx_operation *op,
                                     uint32_t                       duration);
FT93XX_INTERNAL int ft93xx_command (const struct ft93xx_operation *op,
                                    uint8_t                        command_byte);
FT93XX_INTERNAL int ft93xx_read_byte (const struct ft93xx_operation *op,
                                      uint8_t                        address,
                                      uint8_t                       *value);
FT93XX_INTERNAL int ft93xx_write_byte (const struct ft93xx_operation *op,
                                       uint8_t                        address,
                                       uint8_t                        value);
FT93XX_INTERNAL int ft93xx_read_word (const struct ft93xx_operation *op,
                                      uint16_t                       address,
                                      uint16_t                      *value);
FT93XX_INTERNAL int ft93xx_write_word (const struct ft93xx_operation *op,
                                       uint16_t                       address,
                                       uint16_t                       value);

/* FIFO length is an even payload byte count; reply trailers remain unchecked. */
FT93XX_INTERNAL int ft93xx_read_fifo (const struct ft93xx_operation *op,
                                      uint8_t                       *payload,
                                      size_t                         length);
/* FDT blocks use prefix byte 0x80 and fixed four/eight-word sizes.
 * Register selection and protected-window control belong to the caller. */
FT93XX_INTERNAL int ft93xx_read_fdt_values (const struct ft93xx_operation *op,
                                            uint8_t                        address,
                                            uint16_t                       values[4]);
FT93XX_INTERNAL int ft93xx_write_fdt_baseline (const struct ft93xx_operation *op,
                                               uint8_t                        address,
                                               const uint16_t                 values[8]);

#endif
