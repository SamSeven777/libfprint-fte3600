/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _XOPEN_SOURCE 700
#include "fte3600_linux_runtime.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct fte3600_linux_runtime
{
  struct fte3600_linux_board    *board;
  struct fte3600_device_session *session;
  struct fte3600_device_config   device;
  uint8_t                       *firmware[2];
  char                          *firmware_dir;
  GCancellable                  *cancellable;
  gint64                         deadline;
  int                            timed_out;
};
static enum fte3600_linux_runtime_error
result (struct fte3600_linux_runtime_diagnostic *d,
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
diagnostic_clear (struct fte3600_linux_runtime_diagnostic *d)
{
  if(d)
    memset (d, 0, sizeof (*d));
}
static int
path_join (char out[PATH_MAX], const char *a, const char *b)
{
  int n = snprintf (out, PATH_MAX, "%s/%s", a, b);

  if(n < 0 || n >= PATH_MAX)
    {
      errno = ENAMETOOLONG;
      return -1;
    }
  return 0;
}
static int
numbered (const char *p, const char *prefix, int dot)
{
  size_t n = strlen (prefix);
  int seen = 0, digits = 0;

  if(strncmp (p, prefix, n) || strlen (p) >= 64)
    return 0;
  for(p += n; *p; ++p)
    {
      if(*p >= '0' && *p <= '9')
        {
          ++digits;
          continue;
        }
      if(*p == '.' && dot && !seen && digits)
        {
          seen = 1;
          digits = 0;
          continue;
        }
      return 0;
    }
  return digits && seen == dot;
}
enum fte3600_linux_runtime_error
fte3600_linux_runtime_discover (
  const struct fte3600_linux_runtime_config *c, struct fte3600_linux_runtime_pair *out,
  struct fte3600_linux_runtime_diagnostic *d)
{
  char sys[PATH_MAX], dev[PATH_MAX], path[PATH_MAX], expected[PATH_MAX];
  const char *name;

  DIR *gp = NULL, *up = NULL;
  struct dirent *g, *u;
  struct fte3600_linux_runtime_pair found = {0};
  int matches = 0, saved = ENODEV;
  diagnostic_clear (d);
  if(!c || !out || !c->spidev_path || c->spidev_path[0] != '/')
    return result (d, FTE3600_RUNTIME_INVALID_ARGUMENT, EINVAL, FTE3600_DEVICE_OK);
  name = strrchr (c->spidev_path, '/');
  ++name;
  if(!numbered (name, "spidev", 1))
    return result (d, FTE3600_RUNTIME_INVALID_ARGUMENT, EINVAL, FTE3600_DEVICE_OK);
  if(!realpath (c->sys_root ? c->sys_root : "/sys", sys) ||
     !realpath (c->dev_root ? c->dev_root : "/dev", dev) || path_join (expected, dev, name))
    goto failed;
  if(strcmp (expected, c->spidev_path))
    {
      errno = EINVAL;
      goto failed;
    }
  if(path_join (path, sys, "bus/gpio/devices") || !(gp = opendir (path)))
    goto failed;
  if(path_join (path, sys, "class/uio") || !(up = opendir (path)))
    goto failed;
  while((g = readdir (gp)))
    {
      if(!numbered (g->d_name, "gpiochip", 0))
        continue;
      rewinddir (up);
      while((u = readdir (up)))
        {
          struct fte3600_linux_board_info info;
          struct fte3600_linux_board_config board = {.sys_root = sys, .dev_root = dev, .spi = name,
                                                     .gpio = g->d_name, .uio = u->d_name, .transfer_capacity = 65535};
          if(!numbered (u->d_name, "uio", 0))
            continue;
          if(fte3600_linux_board_validate (&board, &info))
            continue;
          if(++matches > 1)
            {
              errno = EEXIST;
              goto failed;
            }
          strcpy (found.spi, name);
          strcpy (found.gpio, g->d_name);
          strcpy (found.uio, u->d_name);
          found.board = info;
        }
    }
  if(!matches)
    {
      errno = ENODEV;
      goto failed;
    }
  if(closedir (gp))
    {
      gp = NULL;
      goto failed;
    }
  gp = NULL;
  if(closedir (up))
    {
      up = NULL;
      goto failed;
    }
  up = NULL;
  *out = found;
  return FTE3600_RUNTIME_OK;
failed:
  saved = errno ? errno : saved;
  if(gp)
    closedir (gp);
  if(up)
    closedir (up);
  return result (d, FTE3600_RUNTIME_PAIRING, saved, FTE3600_DEVICE_OK);
}
static int
cancelled (void *context)
{
  struct fte3600_linux_runtime *r = context;

  if(r->cancellable && g_cancellable_is_cancelled (r->cancellable))
    return 1;
  if(r->deadline && g_get_monotonic_time () >= r->deadline)
    {
      r->timed_out = 1;
      return 1;
    }
  return 0;
}
static void
begin (struct fte3600_linux_runtime *r, GCancellable *cancel, uint32_t timeout)
{
  r->cancellable = cancel ? g_object_ref (cancel) : NULL;
  r->timed_out = 0;
  r->deadline = timeout ? g_get_monotonic_time () + (gint64) timeout * 1000 : 0;
}
static void
end (struct fte3600_linux_runtime *r)
{
  g_clear_object (&r->cancellable);
  r->deadline = 0;
}
static enum fte3600_linux_runtime_error
session_error (struct fte3600_linux_runtime *r,
               enum fte3600_device_error e, struct fte3600_linux_runtime_diagnostic *d)
{
  struct fte3600_device_status s = {0};
  int error = 0;

  if(r->session)
    {
      fte3600_device_get_status (r->session, &s);
      error = s.platform_error;
    }
  if(!error && r->board)
    error = fte3600_linux_board_callbacks (r->board)->error (r->device.io.context);
  int stopped = cancelled (r);
  if(r->timed_out)
    return result (d, FTE3600_RUNTIME_TIMEOUT, ETIMEDOUT, e);
  if(stopped || error == ECANCELED)
    return result (d, FTE3600_RUNTIME_CANCELLED, ECANCELED, e);
  if(error)
    return result (d, FTE3600_RUNTIME_IO, error, e);
  return result (d, e == FTE3600_DEVICE_NO_MEMORY ? FTE3600_RUNTIME_NO_MEMORY : FTE3600_RUNTIME_SESSION, 0, e);
}
enum fte3600_linux_runtime_error
fte3600_linux_runtime_start (struct fte3600_linux_runtime *r,
                             GCancellable *cancel, struct fte3600_linux_runtime_diagnostic *d)
{
  enum fte3600_device_error e;
  enum fte3600_linux_runtime_error rc;
  struct fte3600_device_status s;

  diagnostic_clear (d);
  if(!r || !r->board)
    return result (d, FTE3600_RUNTIME_INVALID_ARGUMENT, EINVAL, FTE3600_DEVICE_OK);
  begin (r, cancel, 0);
  if(cancelled (r))
    {
      rc = session_error (r, FTE3600_DEVICE_CANCELLED, d);
      goto done;
    }
  if(!r->session)
    e = fte3600_device_create (&r->device, &r->session);
  else
    e = FTE3600_DEVICE_OK;
  if(!e)
    e = fte3600_device_start (r->session);
  if(e == FTE3600_DEVICE_MISSING_FIRMWARE)
    {
      fte3600_device_get_status (r->session, &s);
      rc = fte3600_linux_firmware_load (r->firmware_dir, s.model, &r->device, r->firmware, d);
      if(rc)
        goto done;
      fte3600_device_close (r->session);
      r->session = NULL;
      e = fte3600_device_create (&r->device, &r->session);
      if(!e)
        e = fte3600_device_start (r->session);
    }
  rc = e ? session_error (r, e, d) : FTE3600_RUNTIME_OK;
done:
  end (r);
  return rc;
}
enum fte3600_linux_runtime_error
fte3600_linux_runtime_open (
  const struct fte3600_linux_runtime_config *c, GCancellable *cancel,
  struct fte3600_linux_runtime **out, struct fte3600_linux_runtime_diagnostic *d)
{
  struct fte3600_linux_runtime_pair pair;
  struct fte3600_linux_runtime *r;
  enum fte3600_linux_runtime_error rc;
  struct fte3600_linux_board_config board = {0};

  diagnostic_clear (d);
  if(out)
    *out = NULL;
  if(!out || !c)
    return result (d, FTE3600_RUNTIME_INVALID_ARGUMENT, EINVAL, FTE3600_DEVICE_OK);
  if(cancel && g_cancellable_is_cancelled (cancel))
    return result (d, FTE3600_RUNTIME_CANCELLED, ECANCELED, FTE3600_DEVICE_CANCELLED);
  rc = fte3600_linux_runtime_discover (c, &pair, d);
  if(rc)
    return rc;
  r = calloc (1, sizeof (*r));
  if(!r)
    return result (d, FTE3600_RUNTIME_NO_MEMORY, ENOMEM, FTE3600_DEVICE_NO_MEMORY);
  r->firmware_dir = strdup (c->firmware_dir ? c->firmware_dir : FTE3600_FIRMWARE_DIR);
  if(!r->firmware_dir)
    {
      free (r);
      return result (d, FTE3600_RUNTIME_NO_MEMORY, ENOMEM, FTE3600_DEVICE_NO_MEMORY);
    }
  board.sys_root = c->sys_root;
  board.dev_root = c->dev_root;
  board.spi = pair.spi;
  board.gpio = pair.gpio;
  board.uio = pair.uio;
  board.transfer_capacity = 65535;
  board.is_cancelled = cancelled;
  board.cancel_context = r;
  begin (r, cancel, 0);
  if(fte3600_linux_board_open (&board, &r->board))
    {
      int saved = errno;
      rc = result (d, saved == ECANCELED ? FTE3600_RUNTIME_CANCELLED : FTE3600_RUNTIME_IO, saved, FTE3600_DEVICE_PLATFORM_ERROR);
      end (r);
      goto fail;
    }
  end (r);
  r->device.io = *fte3600_linux_board_callbacks (r->board);
  rc = fte3600_linux_runtime_start (r, cancel, d);
  if(rc)
    goto fail;
  *out = r;
  return FTE3600_RUNTIME_OK;
fail:
  fte3600_linux_runtime_close (r, NULL);
  return rc;
}
static enum fte3600_linux_runtime_error
wait_error (struct fte3600_linux_runtime *r, int error,
            struct fte3600_linux_runtime_diagnostic *d)
{
  int stopped = cancelled (r);

  if(r->timed_out || error == ETIMEDOUT)
    return result (d, FTE3600_RUNTIME_TIMEOUT, ETIMEDOUT, FTE3600_DEVICE_OK);
  if(error == ECANCELED || stopped)
    return result (d, FTE3600_RUNTIME_CANCELLED, ECANCELED, FTE3600_DEVICE_CANCELLED);
  return result (d, FTE3600_RUNTIME_IO, error, FTE3600_DEVICE_PLATFORM_ERROR);
}
static uint32_t
remaining (struct fte3600_linux_runtime *r)
{
  gint64 us = r->deadline - g_get_monotonic_time ();

  return us <= 0 ? 0 : (uint32_t) ((us + 999) / 1000);
}
/* Bounded drain excludes already pending notifications before a new arm. */
static int
drain (struct fte3600_linux_runtime *r)
{
  uint32_t counter;

  for(unsigned i = 0; i < 64; ++i)
    {
      int rc = fte3600_linux_board_wait_irq (r->board, 0, &counter);
      if(rc <= 0)
        return rc;
    }
  errno = EOVERFLOW;
  return -1;
}
static int
contact (struct fte3600_linux_runtime *r)
{
  uint8_t info[32];
  const struct fte3600_device_io *io = &r->device.io;
  const struct fte3600_ft9368_io byte = {io->context, io->spi, io->wait_ms};
  uint32_t status = fte3600_ft9368_info (sizeof (info), info, sizeof (info), &byte);
  int error = io->error (io->context);

  if(error || (status >> 31))
    {
      errno = error ? error : EIO;
      return -1;
    }
  /* Only a healthy information response can establish contact/absence.
   * The legacy detect operation also returns zero after reset/recovery;
   * that zero cannot be used as finger-up evidence. */
  if(info[19] != 0x93 || info[20] != 0x68 || info[2] == 0x22 ||
     (info[0] == 0xff && info[1] == 0xff))
    {
      errno = EPROTO;
      return -1;
    }
  return info[1] == 0x11 && info[2] == 0x11;
}
enum fte3600_linux_runtime_error
fte3600_linux_runtime_capture (struct fte3600_linux_runtime *r,
                               GCancellable *cancel, uint32_t timeout, struct fte3600_linux_runtime_frame *frame,
                               struct fte3600_linux_runtime_diagnostic *d)
{
  struct fte3600_linux_runtime_frame next = {0};
  enum fte3600_linux_runtime_error rc;
  enum fte3600_device_error e;
  uint32_t counter;

  diagnostic_clear (d);
  if(!r || !r->session || !frame || frame->pixels || !timeout)
    return result (d, FTE3600_RUNTIME_INVALID_ARGUMENT, EINVAL, FTE3600_DEVICE_OK);
  begin (r, cancel, timeout);
  if(drain (r) < 0)
    {
      rc = wait_error (r, errno, d);
      goto done;
    }
  e = fte3600_device_arm (r->session);
  if(e)
    {
      rc = session_error (r, e, d);
      goto done;
    }
  for(;;)
    {
      uint32_t ms = remaining (r);
      int n;
      if(!ms)
        {
          rc = wait_error (r, ETIMEDOUT, d);
          goto done;
        }
      n = fte3600_linux_board_wait_irq (r->board, ms, &counter);
      if(n <= 0)
        {
          rc = wait_error (r, n < 0 ? errno : ETIMEDOUT, d);
          goto done;
        }
      fte3600_device_get_status (r->session, &next.status);
      if(next.status.model == FTE3600_MODEL_FT9368)
        {
          n = contact (r);
          if(n < 0)
            {
              rc = wait_error (r, errno, d);
              goto done;
            }
          if(!n)
            continue;
        }
      e = fte3600_device_irq (r->session);
      if(e == FTE3600_DEVICE_NO_FRAME)
        continue;
      if(e)
        {
          rc = session_error (r, e, d);
          goto done;
        }
      break;
    }
  fte3600_device_get_status (r->session, &next.status);
  next.stride = next.status.width;
  next.length = next.stride * next.status.height;
  next.pixels = malloc (next.length);
  if(!next.pixels)
    {
      rc = result (d, FTE3600_RUNTIME_NO_MEMORY, ENOMEM, FTE3600_DEVICE_NO_MEMORY);
      goto done;
    }
  e = fte3600_device_copy_image (r->session, next.status.image_sequence, next.pixels, next.length);
  if(e)
    {
      rc = session_error (r, e, d);
      goto done;
    }
  if(cancelled (r))
    {
      rc = wait_error (r, ECANCELED, d);
      goto done;
    }
  *frame = next;
  memset (&next, 0, sizeof (next));
  rc = FTE3600_RUNTIME_OK;
done:
  if(rc)
    fte3600_device_cancel (r->session);
  fte3600_linux_runtime_frame_clear (&next);
  end (r);
  return rc;
}
enum fte3600_linux_runtime_error
fte3600_linux_runtime_wait_release (struct fte3600_linux_runtime *r,
                                    GCancellable *cancel, uint32_t timeout, struct fte3600_linux_runtime_diagnostic *d)
{
  struct fte3600_device_status status;
  enum fte3600_linux_runtime_error rc;

  diagnostic_clear (d);
  if(!r || !r->session || !timeout)
    return result (d, FTE3600_RUNTIME_INVALID_ARGUMENT, EINVAL, FTE3600_DEVICE_OK);
  fte3600_device_get_status (r->session, &status);
  if(status.model != FTE3600_MODEL_FT9368)
    return result (d, FTE3600_RUNTIME_RELEASE_UNSUPPORTED, ENOTSUP, FTE3600_DEVICE_UNSUPPORTED);
  if(status.state != FTE3600_DEVICE_IDLE)
    return result (d, FTE3600_RUNTIME_NOT_READY, 0, FTE3600_DEVICE_NOT_READY);
  begin (r, cancel, timeout);
  for(;;)
    {
      int state = contact (r);
      if(state < 0)
        {
          rc = wait_error (r, errno, d);
          break;
        }
      if(!state)
        {
          rc = FTE3600_RUNTIME_OK;
          break;
        }
      if(!remaining (r))
        {
          rc = wait_error (r, ETIMEDOUT, d);
          break;
        }
      r->device.io.wait_ms (r->device.io.context, 20);
    }
  end (r);
  return rc;
}
enum fte3600_linux_runtime_error
fte3600_linux_runtime_get_status (const struct fte3600_linux_runtime *r,
                                  struct fte3600_device_status       *status)
{
  if(!r || !r->session || !status)
    return FTE3600_RUNTIME_INVALID_ARGUMENT;
  return fte3600_device_get_status (r->session, status) == FTE3600_DEVICE_OK ? FTE3600_RUNTIME_OK : FTE3600_RUNTIME_SESSION;
}
void
fte3600_linux_runtime_frame_clear (struct fte3600_linux_runtime_frame *frame)
{
  if(frame)
    {
      free (frame->pixels);
      memset (frame, 0, sizeof (*frame));
    }
}
enum fte3600_linux_runtime_error
fte3600_linux_runtime_close (struct fte3600_linux_runtime            *r,
                             struct fte3600_linux_runtime_diagnostic *d)
{
  int saved = 0;

  diagnostic_clear (d);
  if(!r)
    return FTE3600_RUNTIME_OK;
  fte3600_device_close (r->session);
  if(fte3600_linux_board_close (r->board))
    saved = errno;
  free (r->firmware[0]);
  free (r->firmware[1]);
  free (r->firmware_dir);
  end (r);
  free (r);
  return saved ? result (d, FTE3600_RUNTIME_IO, saved, FTE3600_DEVICE_PLATFORM_ERROR) : FTE3600_RUNTIME_OK;
}
const char *
fte3600_linux_runtime_error_string (enum fte3600_linux_runtime_error e)
{
  static const char *const names[] = {"ok", "invalid argument", "out of memory", "device pairing failed",
                                      "platform I/O failed", "firmware validation failed", "device session failed", "cancelled", "timed out",
                                      "finger release detection unavailable", "device not ready"};

  return (unsigned) e < G_N_ELEMENTS (names) ? names[e] : "unknown runtime error";
}
