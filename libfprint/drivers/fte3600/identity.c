/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_identity.h"

static struct fte3600_identity_result
no_identity (enum fte3600_status status)
{
  const struct fte3600_identity_result result = {
    status, FTE3600_SENSOR_UNKNOWN, FTE3600_BOOT_UNKNOWN
  };

  return result;
}

static struct fte3600_identity_result
identified (enum fte3600_sensor sensor,
            enum fte3600_boot   boot)
{
  const struct fte3600_identity_result result = {FTE3600_OK, sensor, boot};

  return result;
}

static int
valid_read_flag (int read_ok)
{
  return read_ok == 0 || read_ok == 1;
}

struct fte3600_family_result
fte3600_classify_family (uint32_t response_word, int read_ok)
{
  struct fte3600_family_result result = {
    FTE3600_INVALID_INPUT, FTE3600_FAMILY_UNKNOWN
  };

  if (response_word > UINT16_MAX || !valid_read_flag (read_ok))
    return result;
  if (!read_ok)
    {
      result.status = FTE3600_TRANSPORT_ERROR;
      return result;
    }
  result.status = FTE3600_OK;
  result.family = response_word == 0x2b50 || response_word == 0x95a8 ||
                  response_word == 0x23dd ? FTE3600_FAMILY_A8 :
                  FTE3600_FAMILY_LEGACY;
  return result;
}

struct fte3600_identity_result
fte3600_classify_boot_a (uint32_t response_byte, int read_ok)
{
  if (response_byte > UINT8_MAX || !valid_read_flag (read_ok))
    return no_identity (FTE3600_INVALID_INPUT);
  if (!read_ok)
    return no_identity (FTE3600_TRANSPORT_ERROR);
  return identified (response_byte == 2 ? FTE3600_FT9536 : FTE3600_FT9338,
                     FTE3600_BOOT_A);
}

struct fte3600_identity_result
fte3600_classify_otp (uint32_t family_word, uint32_t otp_byte, int read_ok)
{
  const struct fte3600_family_result family =
    fte3600_classify_family (family_word, read_ok);

  if (otp_byte > UINT8_MAX || family.status == FTE3600_INVALID_INPUT)
    return no_identity (FTE3600_INVALID_INPUT);
  if (family.status != FTE3600_OK)
    return no_identity (family.status);

  if (family.family == FTE3600_FAMILY_A8)
    {
      const uint32_t nibble = otp_byte & 15;
      if (nibble >= 1 && nibble <= 3)
        return identified (FTE3600_FT9348, FTE3600_BOOT_A);
      if (nibble == 4 || nibble == 14 || nibble == 15)
        return identified (FTE3600_FT9361, FTE3600_BOOT_A);
    }
  else
    {
      if (otp_byte == 255 || (otp_byte >> 4) == 1)
        return identified (FTE3600_FT9338, FTE3600_BOOT_A);
      if ((otp_byte >> 4) == 2)
        return identified (FTE3600_FT9536, FTE3600_BOOT_B);
    }
  return no_identity (FTE3600_UNRECOGNIZED);
}
