/*
 * Bounded validation of externally supplied FocalTech firmware
 *
 * Copyright (C) 2026 FTE3600 Linux contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "fte3600-firmware.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static gboolean
firmware_metadata_valid (const Fte3600Firmware *firmware)
{
  if (!firmware || firmware->size == 0 || firmware->size > FTE3600_FIRMWARE_MAX_SIZE ||
      !firmware->sha256 || strlen (firmware->sha256) != 64)
    return FALSE;

  for (gsize i = 0; i < 64; i++)
    if (!g_ascii_isxdigit (firmware->sha256[i]))
      return FALSE;

  return TRUE;
}

GBytes *
fpi_fte3600_firmware_load (const Fte3600Firmware *firmware,
                           const gchar           *path,
                           GError               **error)
{
  g_autofree guint8 *contents = NULL;
  g_autofree gchar *checksum = NULL;
  struct stat st;
  GBytes *result = NULL;
  gsize length = 0;
  gint fd;
  gint status;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!path || !*path || !firmware_metadata_valid (firmware))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Firmware loading requires a path, a bounded nonzero size and a SHA256");
      return NULL;
    }

  /* Inspect the opened fd, so pathname replacement cannot bypass the file type
   * and size checks. NONBLOCK prevents a FIFO from blocking before fstat. */
  do
    fd = open (path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  while (fd < 0 && errno == EINTR);

  if (fd < 0)
    {
      gint saved_errno = errno;

      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                   "Failed to open firmware %s: %s", path, g_strerror (saved_errno));
      return NULL;
    }

  do
    status = fstat (fd, &st);
  while (status < 0 && errno == EINTR);

  if (status < 0)
    {
      gint saved_errno = errno;

      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                   "Failed to inspect firmware %s: %s", path, g_strerror (saved_errno));
      goto out;
    }

  if (!S_ISREG (st.st_mode) || st.st_size != (off_t) firmware->size)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Firmware %s must be a regular %zu-byte file", path, firmware->size);
      goto out;
    }

  /* One extra byte detects growth after fstat. Shrinkage/short reads and all
  * content changes are checked against the same immutable catalog record. */
  contents = g_malloc (firmware->size + 1);
  while (length < firmware->size + 1)
    {
      ssize_t count = read (fd, contents + length, firmware->size + 1 - length);

      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0)
        {
          gint saved_errno = errno;

          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                       "Failed to read firmware %s: %s", path, g_strerror (saved_errno));
          goto out;
        }
      if (count == 0)
        break;
      length += count;
    }

  if (length != firmware->size)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Firmware %s changed size while being read", path);
      goto out;
    }

  checksum = g_compute_checksum_for_data (G_CHECKSUM_SHA256, contents, length);
  if (g_ascii_strcasecmp (checksum, firmware->sha256) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Firmware %s has an unexpected SHA256", path);
      goto out;
    }

  result = g_bytes_new_take (g_steal_pointer (&contents), length);

out:
  /* On Linux close releases the descriptor even when interrupted; retrying
   * could close a descriptor reused by another thread. */
  close (fd);
  return result;
}

GBytes *
fpi_fte3600_load_firmware (const gchar *path, GError **error)
{
  const Fte3600SensorDescriptor *sensor = fpi_fte3600_sensor_get (FTE3600_SENSOR_FT9361);

  return fpi_fte3600_firmware_load (&sensor->firmware[0], path, error);
}
