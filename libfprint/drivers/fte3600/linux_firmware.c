/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include "fte3600_linux_runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static enum fte3600_linux_runtime_error
firmware_result (struct fte3600_linux_runtime_diagnostic *d,
                 enum fte3600_linux_runtime_error code, int system, enum fte3600_device_error device)
{
  if(d)
    {
      d->code = code;
      d->system_error = system;
      d->device_error = device;
    }
  return code;
}
static void
firmware_diagnostic_clear (struct fte3600_linux_runtime_diagnostic *d)
{
  if(d)
    memset (d, 0, sizeof (*d));
}
struct firmware_identity { enum fte3600_factory_model model;
                           const char *name;
                           size_t      size;
                           const char *hash;
};
/* Approved unchanged installer metadata; payloads remain external. */
static const struct firmware_identity firmware[] = {
  {FTE3600_MODEL_FT9338, "ft9338.bin", 14184, "ca4490163a1754639e945da3bd6ecbb4a498138962d611fc825dc129819efc46"},
  {FTE3600_MODEL_FT9348, "ft9348.bin", 10312, "48d658d588c297a5d749c1f4bd6a0f5bd3d6ede59040e1674f95fe9db08eede2"},
  {FTE3600_MODEL_FT9361, "ft9361.bin", 10396, "027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f"},
  {FTE3600_MODEL_FT9536, "ft9536.bin", 11934, "4a62b5d9a8a7b4620bec7843763b030632a5a37a54c4cb7ed2cd96d5c7451e79"},
  {FTE3600_MODEL_FT9368, "ft9368-app.bin", 27120, "9997aafac8eb9a1aecc7ba5b41212004fe1b325e7bdd1ea9ca08ae54b803e2e0"},
  {FTE3600_MODEL_FT9368, "ft9368-pramboot.bin", 6096, "c93a807eaaa34d9e79fbab77b86e910a419fd633ea98b628b72c64b285ffd191"}
};
static uint8_t *
load_image (int directory, const struct firmware_identity *id, size_t capacity,
            struct fte3600_linux_runtime_diagnostic *d)
{
  int fd, saved;
  struct stat st;
  uint8_t *data = NULL;
  size_t done = 0;
  GChecksum *sum = NULL;

  if(d)
    snprintf (d->firmware_file, sizeof (d->firmware_file), "%s", id->name);
  if(capacity < id->size)
    {
      errno = EINVAL;
      return NULL;
    }
  fd = openat (directory, id->name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if(fd < 0)
    return NULL;
  if(fstat (fd, &st) || !S_ISREG (st.st_mode) || st.st_size != (off_t) id->size)
    {
      errno = EINVAL;
      goto fail;
    }
  data = malloc (capacity);
  if(!data)
    goto fail;
  while(done < id->size)
    {
      ssize_t n = read (fd, data + done, id->size - done);
      if(n < 0 && errno == EINTR)
        continue;
      if(n <= 0)
        {
          if(!n)
            errno = EIO;
          goto fail;
        }
      done += (size_t) n;
    }
  { uint8_t extra;
    ssize_t n;
    do
      n = read (fd, &extra, 1);
    while(n < 0 && errno == EINTR);
    if(n != 0)
      {
        if(n > 0)
          errno = EINVAL;
        goto fail;
      }
  }
  sum = g_checksum_new (G_CHECKSUM_SHA256);
  if(!sum)
    {
      errno = ENOMEM;
      goto fail;
    }
  g_checksum_update (sum, data, id->size);
  if(strcmp (g_checksum_get_string (sum), id->hash))
    {
      errno = EBADMSG;
      goto fail;
    }
  g_checksum_free (sum);
  if(close (fd))
    {
      free (data);
      return NULL;
    }
  return data;
fail:
  saved = errno ? errno : EIO;
  if(sum)
    g_checksum_free (sum);
  close (fd);
  free (data);
  errno = saved;
  return NULL;
}
enum fte3600_linux_runtime_error
fte3600_linux_firmware_load (const char *directory,
                             enum fte3600_factory_model model, struct fte3600_device_config *config, uint8_t *owned[2],
                             struct fte3600_linux_runtime_diagnostic *d)
{
  struct fte3600_device_config next;

  uint8_t *data[2] = {0};
  int fd = -1, saved = ENOTSUP;
  firmware_diagnostic_clear (d);
  if(!directory || !config || !owned || owned[0] || owned[1] || config->startup.firmware ||
     config->startup.firmware_length || config->startup.application.data ||
     config->startup.application.length || config->startup.application.capacity ||
     config->startup.pram.data || config->startup.pram.length || config->startup.pram.capacity ||
     config->legacy_firmware_model != FTE3600_MODEL_UNKNOWN)
    return firmware_result (d, FTE3600_RUNTIME_INVALID_ARGUMENT, EINVAL, FTE3600_DEVICE_OK);
  next = *config;
  fd = open (directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if(fd < 0)
    goto fail;
  for(unsigned i = 0; i < G_N_ELEMENTS (firmware); ++i)
    {
      if(firmware[i].model != model)
        continue;
      if(model == FTE3600_MODEL_FT9368)
        {
          uint8_t *tail;
          gchar *hash;
          data[0] = load_image (fd, &firmware[4], 27120, d);
          if(!data[0])
            goto fail;
          data[1] = load_image (fd, &firmware[5], 6144, d);
          if(!data[1])
            goto fail;
          tail = load_image (fd, &firmware[3], 11934, d);
          if(!tail)
            goto fail;
          memcpy (data[1] + 6096, tail, 48);
          free (tail);
          hash = g_compute_checksum_for_data (G_CHECKSUM_SHA256, data[1], 6144);
          if(!hash)
            {
              errno = ENOMEM;
              goto fail;
            }
          if(strcmp (hash, "6dd63be4288cd7c248d4ab0e2aee73b8202bdf93df6617c7b70d9bb83182af79"))
            {
              g_free (hash);
              errno = EBADMSG;
              goto fail;
            }
          g_free (hash);
          next.startup.application = (struct fte3600_ft9368_boot_image){data[0], 27120, 27120};
          next.startup.pram = (struct fte3600_ft9368_boot_image){data[1], 6096, 6144};
        }
      else
        {
          data[0] = load_image (fd, &firmware[i], firmware[i].size, d);
          if(!data[0])
            goto fail;
          next.startup.firmware = data[0];
          next.startup.firmware_length = firmware[i].size;
          next.legacy_firmware_model = model;
        }
      if(close (fd))
        {
          fd = -1;
          goto fail;
        }
      fd = -1;
      *config = next;
      owned[0] = data[0];
      owned[1] = data[1];
      return firmware_result (d, FTE3600_RUNTIME_OK, 0, FTE3600_DEVICE_OK);
    }
  errno = ENOTSUP;
fail:
  saved = errno ? errno : saved;
  if(fd >= 0)
    close (fd);
  free (data[0]);
  free (data[1]);
  return firmware_result (d, FTE3600_RUNTIME_FIRMWARE, saved, FTE3600_DEVICE_MISSING_FIRMWARE);
}
