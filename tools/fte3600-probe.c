/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fte3600_linux_runtime.h"
#include <stdio.h>
#include <string.h>

static const char *
model_name (enum fte3600_factory_model model)
{
  switch (model)
    {
    case FTE3600_MODEL_FT9338: return "FT9338";
    case FTE3600_MODEL_FT9348: return "FT9348";
    case FTE3600_MODEL_FT9361: return "FT9361";
    case FTE3600_MODEL_FT9365: return "FT9365";
    case FTE3600_MODEL_FT9368: return "FT9368";
    case FTE3600_MODEL_FT9536: return "FT9536";
    case FTE3600_MODEL_FW9369: return "FW9369";
    case FTE3600_MODEL_FT9769: return "FT9769";
    case FTE3600_MODEL_UNKNOWN: return "unknown";
    default: return "unknown";
    }
}

static void
identity (enum fte3600_factory_model model, unsigned raw, unsigned chip)
{
  printf ("Driver profile: %s\nRaw ID: 0x%04x\nChip ID: 0x%04x\n",
          model_name (model), raw, chip);
}

static void
diagnostic (const struct fte3600_linux_runtime_diagnostic *d)
{
  printf ("Runtime: %s\nDevice: %s\nSystem errno: %d\n",
          fte3600_linux_runtime_error_string (d->code),
          fte3600_device_error_string (d->device_error), d->system_error);
  if (d->has_session_status)
    {
      identity (d->model, d->raw_id, d->chip_id);
      printf ("Component error: %u\n", d->component_error);
      if (d->component_error == FTE3600_FACTORY_INTEGRATION_UNCONFIRMED_IDENTITY)
        puts ("Identity unconfirmed; the profile above is not a confirmed chip identification.");
    }
  if (d->firmware_file[0])
    printf ("Firmware file: %s\n", d->firmware_file);
  printf ("SPI mode: ACPI=%u last=%u CS-control=%u CS-retry=%u\n",
          d->acpi_mode, d->last_configured_mode, d->cs_control, d->cs_retry_attempted);
}

int
main (int argc, char **argv)
{
  struct fte3600_linux_runtime_config config = {0};
  struct fte3600_linux_runtime_diagnostic d = {0};
  struct fte3600_linux_runtime *runtime = NULL;
  struct fte3600_device_status status = {0};
  enum fte3600_linux_runtime_error rc, closed;

  if (argc == 2 && (!strcmp (argv[1], "--help") || !strcmp (argv[1], "-h")))
    {
      puts ("Usage: fte3600-probe /dev/spidevB.C [firmware-directory]\n"
            "Uses the production driver's identification and full startup sequence.\n"
            "Requires root and the ABI 2 ACPI bridge. Stop other sensor clients first.\n"
            "Keep the sensor untouched: startup may upload firmware and calibrate.\n"
            "Does not request enrollment, verification or save fingerprint images.");
      return 0;
    }
  if (argc < 2 || argc > 3 ||
      !g_regex_match_simple ("^/dev/spidev[0-9]+\\.[0-9]+$", argv[1], 0, 0))
    {
      fputs ("Usage: fte3600-probe /dev/spidevB.C [firmware-directory]\n", stderr);
      return 2;
    }
  config.spidev_path = argv[1];
  config.firmware_dir = argc == 3 ? argv[2] : NULL;
  printf ("SPI device: %s\nFirmware directory: %s\n",
          config.spidev_path, config.firmware_dir ? config.firmware_dir : FTE3600_FIRMWARE_DIR);
  puts ("Running driver startup; keep the sensor untouched.");
  fflush (stdout);
  rc = fte3600_linux_runtime_open (&config, NULL, &runtime, &d);
  if (rc)
    {
      puts ("Startup: FAILED (this alone does not establish that the chip is unsupported)");
      diagnostic (&d);
      return 1;
    }
  rc = fte3600_linux_runtime_get_status (runtime, &status);
  if (!rc)
    {
      identity (status.model, status.raw_id, status.chip_id);
      printf ("Geometry: %ux%u (+%u rows)\nStartup complete: %u\nCalibration complete: %u\n",
              status.width, status.height, status.extra_rows,
              status.startup_complete, status.calibration_complete);
      puts ("Startup: OK (enrollment and verification have not been tested)");
    }
  else
    fprintf (stderr, "Status: %s\n", fte3600_linux_runtime_error_string (rc));
  closed = fte3600_linux_runtime_close (runtime, &d);
  if (closed)
    {
      puts ("Cleanup: FAILED");
      diagnostic (&d);
    }
  return rc || closed ? 1 : 0;
}
