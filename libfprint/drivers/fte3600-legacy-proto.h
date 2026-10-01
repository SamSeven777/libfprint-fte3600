/*
 * FocalTech legacy 93xx SPI protocol primitives
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define FTE3600_LEGACY_MAX_TX_LENGTH 8
#define FTE3600_LEGACY_ID_RESPONSE_LENGTH 4
#define FTE3600_LEGACY_SPI_SPEED_HZ 4000000U

typedef enum
{
  FTE3600_LEGACY_BRANCH_FW9362,
  FTE3600_LEGACY_BRANCH_OTHER_93XX,
} Fte3600LegacyBranch;

typedef enum
{
  FTE3600_LEGACY_FAMILY_UNKNOWN,
  FTE3600_LEGACY_FAMILY_FW9362,
  FTE3600_LEGACY_FAMILY_OTHER_93XX,
} Fte3600LegacyFamily;

typedef enum
{
  FTE3600_LEGACY_CRC_NOT_USED,
  FTE3600_LEGACY_CRC_ZERO,
  FTE3600_LEGACY_CRC_VALID,
  FTE3600_LEGACY_CRC_INVALID,
} Fte3600LegacyCrcStatus;

typedef struct
{
  guint16                  identity;
  guint16                  trailer;
  Fte3600LegacyFamily      family;
  Fte3600LegacyCrcStatus   crc_status;
  gboolean                 recognized;
  gboolean                 shifted_fw9362;
  gboolean                 needs_9391_variant_read;
} Fte3600LegacyIdentity;

typedef enum
{
  FTE3600_LEGACY_RISK_OBSERVE,
  FTE3600_LEGACY_RISK_CONFIGURE,
  FTE3600_LEGACY_RISK_ELECTRICAL,
  FTE3600_LEGACY_RISK_RECOVERY,
} Fte3600LegacyRisk;

typedef enum
{
  FTE3600_LEGACY_STEP_C6_WRITE,
  FTE3600_LEGACY_STEP_C6_READ,
  FTE3600_LEGACY_STEP_IDENTITY_READ,
  FTE3600_LEGACY_STEP_ENABLE_1A84,
  FTE3600_LEGACY_STEP_FD_WRITE,
  FTE3600_LEGACY_STEP_FE_WRITE,
  FTE3600_LEGACY_STEP_FE_READ,
  FTE3600_LEGACY_STEP_9391_VARIANT_READ,
  FTE3600_LEGACY_STEP_IDLE_ENTER,
  FTE3600_LEGACY_STEP_IDLE_STATUS_READ,
  FTE3600_LEGACY_STEP_IDLE_POLL,
  FTE3600_LEGACY_STEP_IDLE_EXIT,
  FTE3600_LEGACY_STEP_LAST,
} Fte3600LegacyStep;

typedef struct
{
  guint8              tx[FTE3600_LEGACY_MAX_TX_LENGTH];
  guint               tx_length;
  guint               rx_length;
  guint               delay_after_us;
  Fte3600LegacyRisk   risk;
} Fte3600LegacyTransaction;

guint16  fte3600_legacy_crc16 (const guint8 *data,
                               gsize         length);

void     fte3600_legacy_parse_identity (Fte3600LegacyBranch    branch,
                                        const guint8           response[FTE3600_LEGACY_ID_RESPONSE_LENGTH],
                                        Fte3600LegacyIdentity *result);

gboolean fte3600_legacy_identity_is_accepted (const Fte3600LegacyIdentity *identity);

gboolean fte3600_legacy_get_transaction (Fte3600LegacyStep         step,
                                         Fte3600LegacyTransaction *transaction);

G_END_DECLS
