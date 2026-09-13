/*
 * Exercise the real driver without enrollment, capture, or template access.
 * SPDX-FileCopyrightText: 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include <stdio.h>
#include <string.h>
#include "fp-context.h"

int main(void)
{
  g_autoptr(FpContext) context = fp_context_new ();
  fp_context_enumerate (context);
  GPtrArray *devices = fp_context_get_devices (context);
  for (guint i = 0; i < devices->len; i++)
    {
      FpDevice *dev = g_ptr_array_index (devices, i);
      if (strcmp (fp_device_get_driver (dev), "fte3600"))
        continue;
      for (guint cycle = 1; cycle <= 3; cycle++)
        {
          g_autoptr(GError) error = NULL;
          if (!fp_device_open_sync (dev, NULL, &error) ||
              !fp_device_close_sync (dev, NULL, &error))
            {
              fprintf (stderr, "Cycle %u failed: %s\n", cycle, error->message);
              return 1;
            }
          printf ("Cycle %u: FT9361 opened and closed successfully\n", cycle);
        }
      return 0;
    }
  fputs ("FT9361 not found\n", stderr);
  return 1;
}
