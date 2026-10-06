/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Model the reset-and-sync callback for protocol fixtures. These callbacks
 * only record synthetic operations; native ordering and guards are exercised
 * independently in test-medion-identify-io.c.
 */
#pragma once

#include "examples/fte3600-medion-identify.h"

static gboolean
medion_fixture_reset_and_sync (const Fte3600MedionIdentifyIo *io, GError **error)
{
  const guint8 tx[] = { 0x55, 0xaa };
  guint8 rx[sizeof tx] = { 0 };

  g_autoptr(GError) failure = NULL;
  g_autoptr(GError) next = NULL;

  if (!io->set_reset (io->user_data, FALSE, &failure))
    goto release;
  if (!io->wait (io->user_data, 10, &failure))
    {
      g_prefix_error (&failure, "wait: ");
      goto release;
    }
  io->set_reset (io->user_data, TRUE, &failure);
  if (!io->wait (io->user_data, 20, &next))
    g_prefix_error (&next, "wait: ");
  if (!failure)
    failure = g_steal_pointer (&next);
  g_clear_error (&next);

release:
  io->set_reset (io->user_data, FALSE, &next);
  if (!failure)
    failure = g_steal_pointer (&next);
  if (failure)
    {
      g_propagate_error (error, g_steal_pointer (&failure));
      return FALSE;
    }
  if (!io->exchange (io->user_data, tx, rx, sizeof tx, error))
    {
      g_prefix_error (error, "SPI exchange: ");
      return FALSE;
    }
  return TRUE;
}
