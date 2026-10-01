/*
 * Unit tests for the FocalTech legacy 93xx SPI protocol primitives
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>

#include "../libfprint/drivers/fte3600-legacy-proto.h"

typedef struct
{
  Fte3600LegacyStep   step;
  const guint8       *tx;
  guint               tx_length;
  guint               rx_length;
  guint               delay_after_us;
  Fte3600LegacyRisk   risk;
} TransactionFixture;

static void
test_crc16_golden (void)
{
  static const struct
  {
    guint8  bytes[2];
    guint16 crc;
  } fixtures[] = {
    { { 0x93, 0x62 }, 0x1c53 },
    { { 0x93, 0x91 }, 0xc32f },
    { { 0x93, 0x92 }, 0xf34c },
    { { 0x93, 0x95 }, 0x83ab },
    { { 0x93, 0x96 }, 0xb3c8 },
    { { 0x93, 0x97 }, 0xa3e9 },
    { { 0x93, 0x98 }, 0x5206 },
    { { 0x93, 0x63 }, 0x0c72 },
    { { 0x93, 0x72 }, 0x0e62 },
    { { 0x93, 0x49 }, 0x895a },
    { { 0x93, 0x65 }, 0x6cb4 },
  };

  for (guint i = 0; i < G_N_ELEMENTS (fixtures); i++)
    g_assert_cmphex (fte3600_legacy_crc16 (fixtures[i].bytes, 2), ==,
                     fixtures[i].crc);
}

static void
test_fw9362_identity (void)
{
  const guint8 response[] = { 0x93, 0x62, 0x00, 0x00 };
  Fte3600LegacyIdentity identity;

  fte3600_legacy_parse_identity (FTE3600_LEGACY_BRANCH_FW9362,
                                 response, &identity);

  g_assert_cmphex (identity.identity, ==, 0x9362);
  g_assert_cmphex (identity.trailer, ==, 0x0000);
  g_assert_cmpint (identity.family, ==, FTE3600_LEGACY_FAMILY_FW9362);
  g_assert_cmpint (identity.crc_status, ==, FTE3600_LEGACY_CRC_NOT_USED);
  g_assert_true (identity.recognized);
  g_assert_false (identity.shifted_fw9362);
  g_assert_false (identity.needs_9391_variant_read);
  g_assert_true (fte3600_legacy_identity_is_accepted (&identity));
}

static void
test_fw9362_shifted_identity (void)
{
  const guint8 response[] = { 0x26, 0xc4, 0x00, 0x00 };
  Fte3600LegacyIdentity identity;

  fte3600_legacy_parse_identity (FTE3600_LEGACY_BRANCH_FW9362,
                                 response, &identity);

  g_assert_cmphex (identity.identity, ==, 0x26c4);
  g_assert_cmpint (identity.family, ==, FTE3600_LEGACY_FAMILY_UNKNOWN);
  g_assert_false (identity.recognized);
  g_assert_true (identity.shifted_fw9362);
  g_assert_false (fte3600_legacy_identity_is_accepted (&identity));
}

static void
test_other_93xx_identities (void)
{
  static const guint16 supported[] = {
    0x9391, 0x9392, 0x9395, 0x9396, 0x9397, 0x9398,
    0x9363, 0x9372, 0x9349, 0x9365,
  };

  for (guint i = 0; i < G_N_ELEMENTS (supported); i++)
    {
      guint8 response[4] = {
        supported[i] >> 8,
        supported[i] & 0xff,
        0,
        0,
      };
      Fte3600LegacyIdentity identity;
      guint16 crc = fte3600_legacy_crc16 (response, 2);

      response[2] = crc >> 8;
      response[3] = crc & 0xff;
      fte3600_legacy_parse_identity (FTE3600_LEGACY_BRANCH_OTHER_93XX,
                                     response, &identity);

      g_assert_cmphex (identity.identity, ==, supported[i]);
      g_assert_cmpint (identity.family, ==,
                       FTE3600_LEGACY_FAMILY_OTHER_93XX);
      g_assert_cmpint (identity.crc_status, ==, FTE3600_LEGACY_CRC_VALID);
      g_assert_true (identity.recognized);
      g_assert_cmpint (identity.needs_9391_variant_read, ==,
                       supported[i] == 0x9391);
      g_assert_true (fte3600_legacy_identity_is_accepted (&identity));
    }
}

static void
test_other_93xx_crc_policy (void)
{
  const guint8 zero_crc[] = { 0x93, 0x91, 0x00, 0x00 };
  const guint8 wrong_crc[] = { 0x93, 0x91, 0x12, 0x34 };
  Fte3600LegacyIdentity identity;

  fte3600_legacy_parse_identity (FTE3600_LEGACY_BRANCH_OTHER_93XX,
                                 zero_crc, &identity);
  g_assert_true (identity.recognized);
  g_assert_cmpint (identity.crc_status, ==, FTE3600_LEGACY_CRC_ZERO);
  g_assert_false (identity.needs_9391_variant_read);
  g_assert_false (fte3600_legacy_identity_is_accepted (&identity));

  fte3600_legacy_parse_identity (FTE3600_LEGACY_BRANCH_OTHER_93XX,
                                 wrong_crc, &identity);
  g_assert_true (identity.recognized);
  g_assert_cmpint (identity.crc_status, ==, FTE3600_LEGACY_CRC_INVALID);
  g_assert_false (identity.needs_9391_variant_read);
  g_assert_false (fte3600_legacy_identity_is_accepted (&identity));
}

static void
test_unknown_identity (void)
{
  const guint8 valid_9391[] = { 0x93, 0x91, 0xc3, 0x2f };
  const gint invalid_branches[] = { -1, 2, G_MAXINT };
  Fte3600LegacyIdentity invalid_branch_identity;

  for (guint i = 0; i < G_N_ELEMENTS (invalid_branches); i++)
    {
      /* First populate a valid result, then ensure invalid input clears it. */
      fte3600_legacy_parse_identity (FTE3600_LEGACY_BRANCH_OTHER_93XX,
                                     valid_9391, &invalid_branch_identity);
      g_assert_true (fte3600_legacy_identity_is_accepted (&invalid_branch_identity));
      fte3600_legacy_parse_identity ((Fte3600LegacyBranch) invalid_branches[i],
                                     valid_9391, &invalid_branch_identity);
      g_assert_false (fte3600_legacy_identity_is_accepted (&invalid_branch_identity));
      g_assert_false (invalid_branch_identity.needs_9391_variant_read);
      g_assert_cmpint (invalid_branch_identity.family, ==, FTE3600_LEGACY_FAMILY_UNKNOWN);
    }

  static const guint8 responses[][4] = {
    { 0x00, 0x00, 0x00, 0x00 },
    { 0xff, 0xff, 0xff, 0xff },
    { 0x93, 0x62, 0x1c, 0x53 },
  };

  for (guint i = 0; i < G_N_ELEMENTS (responses); i++)
    {
      Fte3600LegacyIdentity identity;

      fte3600_legacy_parse_identity (FTE3600_LEGACY_BRANCH_OTHER_93XX,
                                     responses[i], &identity);
      g_assert_cmpint (identity.family, ==, FTE3600_LEGACY_FAMILY_UNKNOWN);
      g_assert_false (identity.recognized);
      g_assert_false (fte3600_legacy_identity_is_accepted (&identity));
    }
}

static void
test_transactions (void)
{
  static const guint8 c6_write[] = { 0x09, 0xf6, 0xc6, 0x01 };
  static const guint8 c6_read[] = { 0x08, 0xf7, 0xc6, 0x00 };
  static const guint8 identity_read[] = {
    0x04, 0xfb, 0x9a, 0x8b, 0x00, 0x00,
  };
  static const guint8 enable_1a84[] = {
    0x05, 0xfa, 0x9a, 0x84, 0x00, 0x00, 0xff, 0xff,
  };
  static const guint8 fd_write[] = { 0x09, 0xf6, 0xfd, 0x0a };
  static const guint8 fe_write[] = { 0x09, 0xf6, 0xfe, 0x7f };
  static const guint8 fe_read[] = { 0x08, 0xf7, 0xfe, 0x00 };
  static const guint8 variant_read[] = {
    0x04, 0xfb, 0x98, 0x16, 0x00, 0x00,
  };
  static const guint8 idle_enter[] = { 0x5a, 0xa5, 0x00 };
  static const guint8 idle_status[] = { 0x08, 0xf7, 0x80, 0x00 };
  static const guint8 idle_poll[] = { 0xc0, 0x3f, 0x00 };
  static const guint8 idle_exit[] = { 0xa5, 0x5a, 0x00 };
  static const TransactionFixture fixtures[] = {
    { FTE3600_LEGACY_STEP_C6_WRITE, c6_write, sizeof (c6_write), 0, 4000,
      FTE3600_LEGACY_RISK_CONFIGURE },
    { FTE3600_LEGACY_STEP_C6_READ, c6_read, sizeof (c6_read), 1, 0,
      FTE3600_LEGACY_RISK_OBSERVE },
    { FTE3600_LEGACY_STEP_IDENTITY_READ, identity_read,
      sizeof (identity_read), 4, 0, FTE3600_LEGACY_RISK_OBSERVE },
    { FTE3600_LEGACY_STEP_ENABLE_1A84, enable_1a84,
      sizeof (enable_1a84), 0, 0, FTE3600_LEGACY_RISK_CONFIGURE },
    { FTE3600_LEGACY_STEP_FD_WRITE, fd_write, sizeof (fd_write), 0, 0,
      FTE3600_LEGACY_RISK_ELECTRICAL },
    { FTE3600_LEGACY_STEP_FE_WRITE, fe_write, sizeof (fe_write), 0, 1000,
      FTE3600_LEGACY_RISK_ELECTRICAL },
    { FTE3600_LEGACY_STEP_FE_READ, fe_read, sizeof (fe_read), 1, 0,
      FTE3600_LEGACY_RISK_OBSERVE },
    { FTE3600_LEGACY_STEP_9391_VARIANT_READ, variant_read,
      sizeof (variant_read), 4, 0, FTE3600_LEGACY_RISK_OBSERVE },
    { FTE3600_LEGACY_STEP_IDLE_ENTER, idle_enter, sizeof (idle_enter), 0, 1000,
      FTE3600_LEGACY_RISK_RECOVERY },
    { FTE3600_LEGACY_STEP_IDLE_STATUS_READ, idle_status,
      sizeof (idle_status), 1, 0, FTE3600_LEGACY_RISK_OBSERVE },
    { FTE3600_LEGACY_STEP_IDLE_POLL, idle_poll, sizeof (idle_poll), 0, 1000,
      FTE3600_LEGACY_RISK_RECOVERY },
    { FTE3600_LEGACY_STEP_IDLE_EXIT, idle_exit, sizeof (idle_exit), 0, 0,
      FTE3600_LEGACY_RISK_RECOVERY },
  };

  g_assert_cmpuint (G_N_ELEMENTS (fixtures), ==, FTE3600_LEGACY_STEP_LAST);
  for (guint i = 0; i < G_N_ELEMENTS (fixtures); i++)
    {
      Fte3600LegacyTransaction transaction;

      g_assert_true (fte3600_legacy_get_transaction (fixtures[i].step,
                                                     &transaction));
      g_assert_cmpuint (transaction.tx_length, ==, fixtures[i].tx_length);
      g_assert_cmpmem (transaction.tx, transaction.tx_length,
                       fixtures[i].tx, fixtures[i].tx_length);
      g_assert_cmpuint (transaction.rx_length, ==, fixtures[i].rx_length);
      g_assert_cmpuint (transaction.delay_after_us, ==,
                        fixtures[i].delay_after_us);
      g_assert_cmpint (transaction.risk, ==, fixtures[i].risk);
    }
}

static void
test_invalid_transaction (void)
{
  Fte3600LegacyTransaction transaction = {
    .tx = { 0xff },
    .tx_length = 1,
  };

  g_assert_false (fte3600_legacy_get_transaction (FTE3600_LEGACY_STEP_LAST,
                                                  &transaction));
  g_assert_cmpuint (transaction.tx_length, ==, 0);
  g_assert_cmphex (transaction.tx[0], ==, 0x00);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/fte3600/legacy-proto/crc16-golden", test_crc16_golden);
  g_test_add_func ("/fte3600/legacy-proto/fw9362", test_fw9362_identity);
  g_test_add_func ("/fte3600/legacy-proto/fw9362-shifted", test_fw9362_shifted_identity);
  g_test_add_func ("/fte3600/legacy-proto/other-93xx", test_other_93xx_identities);
  g_test_add_func ("/fte3600/legacy-proto/other-93xx-crc", test_other_93xx_crc_policy);
  g_test_add_func ("/fte3600/legacy-proto/unknown", test_unknown_identity);
  g_test_add_func ("/fte3600/legacy-proto/transactions", test_transactions);
  g_test_add_func ("/fte3600/legacy-proto/invalid-transaction", test_invalid_transaction);

  return g_test_run ();
}
