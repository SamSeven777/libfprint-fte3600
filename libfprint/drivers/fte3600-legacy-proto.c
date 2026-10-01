/*
 * FocalTech legacy 93xx SPI protocol primitives
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "fte3600-legacy-proto.h"

#include <string.h>

typedef struct
{
  const guint8       *tx;
  guint               tx_length;
  guint               rx_length;
  guint               delay_after_us;
  Fte3600LegacyRisk   risk;
} LegacyTransactionDefinition;

static const guint8 c6_write[] = { 0x09, 0xf6, 0xc6, 0x01 };
static const guint8 c6_read[] = { 0x08, 0xf7, 0xc6, 0x00 };
static const guint8 identity_read[] = { 0x04, 0xfb, 0x9a, 0x8b, 0x00, 0x00 };
static const guint8 enable_1a84[] = { 0x05, 0xfa, 0x9a, 0x84,
                                     0x00, 0x00, 0xff, 0xff };
static const guint8 fd_write[] = { 0x09, 0xf6, 0xfd, 0x0a };
static const guint8 fe_write[] = { 0x09, 0xf6, 0xfe, 0x7f };
static const guint8 fe_read[] = { 0x08, 0xf7, 0xfe, 0x00 };
static const guint8 variant_9391_read[] = { 0x04, 0xfb, 0x98, 0x16, 0x00, 0x00 };
static const guint8 idle_enter[] = { 0x5a, 0xa5, 0x00 };
static const guint8 idle_status_read[] = { 0x08, 0xf7, 0x80, 0x00 };
static const guint8 idle_poll[] = { 0xc0, 0x3f, 0x00 };
static const guint8 idle_exit[] = { 0xa5, 0x5a, 0x00 };

static const LegacyTransactionDefinition transactions[] = {
  [FTE3600_LEGACY_STEP_C6_WRITE] = {
    c6_write, sizeof (c6_write), 0, 4000, FTE3600_LEGACY_RISK_CONFIGURE,
  },
  [FTE3600_LEGACY_STEP_C6_READ] = {
    c6_read, sizeof (c6_read), 1, 0, FTE3600_LEGACY_RISK_OBSERVE,
  },
  [FTE3600_LEGACY_STEP_IDENTITY_READ] = {
    identity_read, sizeof (identity_read), 4, 0, FTE3600_LEGACY_RISK_OBSERVE,
  },
  [FTE3600_LEGACY_STEP_ENABLE_1A84] = {
    enable_1a84, sizeof (enable_1a84), 0, 0, FTE3600_LEGACY_RISK_CONFIGURE,
  },
  [FTE3600_LEGACY_STEP_FD_WRITE] = {
    fd_write, sizeof (fd_write), 0, 0, FTE3600_LEGACY_RISK_ELECTRICAL,
  },
  [FTE3600_LEGACY_STEP_FE_WRITE] = {
    fe_write, sizeof (fe_write), 0, 1000, FTE3600_LEGACY_RISK_ELECTRICAL,
  },
  [FTE3600_LEGACY_STEP_FE_READ] = {
    fe_read, sizeof (fe_read), 1, 0, FTE3600_LEGACY_RISK_OBSERVE,
  },
  [FTE3600_LEGACY_STEP_9391_VARIANT_READ] = {
    variant_9391_read, sizeof (variant_9391_read), 4, 0, FTE3600_LEGACY_RISK_OBSERVE,
  },
  [FTE3600_LEGACY_STEP_IDLE_ENTER] = {
    idle_enter, sizeof (idle_enter), 0, 1000, FTE3600_LEGACY_RISK_RECOVERY,
  },
  [FTE3600_LEGACY_STEP_IDLE_STATUS_READ] = {
    idle_status_read, sizeof (idle_status_read), 1, 0, FTE3600_LEGACY_RISK_OBSERVE,
  },
  [FTE3600_LEGACY_STEP_IDLE_POLL] = {
    idle_poll, sizeof (idle_poll), 0, 1000, FTE3600_LEGACY_RISK_RECOVERY,
  },
  [FTE3600_LEGACY_STEP_IDLE_EXIT] = {
    idle_exit, sizeof (idle_exit), 0, 0, FTE3600_LEGACY_RISK_RECOVERY,
  },
};

static gboolean
is_other_93xx_identity (guint16 identity)
{
  static const guint16 supported[] = {
    0x9391, 0x9392, 0x9395, 0x9396, 0x9397, 0x9398,
    0x9363, 0x9372, 0x9349, 0x9365,
  };

  for (guint i = 0; i < G_N_ELEMENTS (supported); i++)
    if (identity == supported[i])
      return TRUE;

  return FALSE;
}

guint16
fte3600_legacy_crc16 (const guint8 *data,
                      gsize         length)
{
  guint16 crc = 0xffff;

  g_return_val_if_fail (data != NULL || length == 0, 0);

  for (gsize i = 0; i < length; i++)
    {
      crc ^= (guint16) data[i] << 8;
      for (guint bit = 0; bit < 8; bit++)
        crc = (crc & 0x8000) ? (guint16) ((crc << 1) ^ 0x1021) :
                              (guint16) (crc << 1);
    }

  return crc;
}

void
fte3600_legacy_parse_identity (Fte3600LegacyBranch    branch,
                               const guint8           response[FTE3600_LEGACY_ID_RESPONSE_LENGTH],
                               Fte3600LegacyIdentity *result)
{
  guint16 calculated_crc;

  g_return_if_fail (response != NULL);
  g_return_if_fail (result != NULL);

  *result = (Fte3600LegacyIdentity) { 0 };
  if (branch != FTE3600_LEGACY_BRANCH_FW9362 &&
      branch != FTE3600_LEGACY_BRANCH_OTHER_93XX)
    return;

  result->identity = ((guint16) response[0] << 8) | response[1];
  result->trailer = ((guint16) response[2] << 8) | response[3];

  if (branch == FTE3600_LEGACY_BRANCH_FW9362)
    {
      result->crc_status = FTE3600_LEGACY_CRC_NOT_USED;
      result->shifted_fw9362 = result->identity == 0x26c4;
      result->recognized = result->identity == 0x9362;
      if (result->recognized)
        result->family = FTE3600_LEGACY_FAMILY_FW9362;
      return;
    }

  result->recognized = is_other_93xx_identity (result->identity);
  if (result->recognized)
    result->family = FTE3600_LEGACY_FAMILY_OTHER_93XX;

  calculated_crc = fte3600_legacy_crc16 (response, 2);
  if (result->trailer == 0)
    result->crc_status = FTE3600_LEGACY_CRC_ZERO;
  else if (result->trailer == calculated_crc)
    result->crc_status = FTE3600_LEGACY_CRC_VALID;
  else
    result->crc_status = FTE3600_LEGACY_CRC_INVALID;

  result->needs_9391_variant_read = result->identity == 0x9391 &&
                                    result->crc_status == FTE3600_LEGACY_CRC_VALID;
}

gboolean
fte3600_legacy_identity_is_accepted (const Fte3600LegacyIdentity *identity)
{
  g_return_val_if_fail (identity != NULL, FALSE);

  if (!identity->recognized)
    return FALSE;

  if (identity->family == FTE3600_LEGACY_FAMILY_FW9362)
    return identity->crc_status == FTE3600_LEGACY_CRC_NOT_USED;

  return identity->family == FTE3600_LEGACY_FAMILY_OTHER_93XX &&
         identity->crc_status == FTE3600_LEGACY_CRC_VALID;
}

gboolean
fte3600_legacy_get_transaction (Fte3600LegacyStep         step,
                                Fte3600LegacyTransaction *transaction)
{
  const LegacyTransactionDefinition *definition;

  g_return_val_if_fail (transaction != NULL, FALSE);

  *transaction = (Fte3600LegacyTransaction) { 0 };
  if ((guint) step >= FTE3600_LEGACY_STEP_LAST)
    return FALSE;

  definition = &transactions[step];
  g_assert_cmpuint (definition->tx_length, <=, FTE3600_LEGACY_MAX_TX_LENGTH);

  memcpy (transaction->tx, definition->tx, definition->tx_length);
  transaction->tx_length = definition->tx_length;
  transaction->rx_length = definition->rx_length;
  transaction->delay_after_us = definition->delay_after_us;
  transaction->risk = definition->risk;
  return TRUE;
}
