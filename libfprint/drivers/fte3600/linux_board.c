/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _XOPEN_SOURCE 700
#include "fte3600_linux_board.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

struct identity
{
  char                            sys[PATH_MAX], dev[PATH_MAX], physical[PATH_MAX], glue[PATH_MAX];
  char                            nodes[3][PATH_MAX], classes[3][PATH_MAX];
  struct stat                     dirs[5], devices[3];
  struct fte3600_linux_board_info info;
};
struct fte3600_linux_board
{
  struct identity                   id;
  struct fte3600_linux_board_config config;
  char                              names[3][64];
  int                               chip, reset, uio, spi, error, configured;
  uint8_t                           old_bits;
  uint32_t                          old_mode, old_speed, mode, last_clock;
  uint64_t                          last_monotonic;
  uint8_t                          *receive;
  struct fte3600_device_io          io;
};
static int
bad (int e)
{
  errno = e;
  return -1;
}
static int
join (char *out, const char *a, const char *b)
{
  int n = snprintf (out, PATH_MAX, "%s/%s", a, b);

  return n < 0 || n >= PATH_MAX ? bad (ENAMETOOLONG) : 0;
}
static int
resolve (char *out, const char *a, const char *b, const char *root)
{
  char p[PATH_MAX];
  size_t n = strlen (root);

  if (join (p, a, b) || !realpath (p, out))
    return -1;
  if (strncmp (out, root, n) || (out[n] && out[n] != '/'))
    return bad (EXDEV);
  return 0;
}
static int
parent (char *out, const char *p)
{
  char *last;

  if (strlen (p) >= PATH_MAX)
    return bad (ENAMETOOLONG);
  strcpy (out, p);
  last = strrchr (out, '/');
  if (!last || last == out)
    return bad (EINVAL);
  *last = 0;
  return 0;
}
static int
read_text (const char *a, const char *b, char *out, size_t capacity)
{
  char p[PATH_MAX];
  int fd, saved;
  ssize_t n;

  if (join (p, a, b))
    return -1;
  fd = open (p, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  n = read (fd, out, capacity - 1);
  saved = errno;
  if (close (fd) && n >= 0)
    return -1;
  if (n < 0)
    return bad (saved);
  if (!n || (size_t) n == capacity - 1)
    return bad (EPROTO);
  out[n] = 0;
  while (n && (out[n - 1] == '\n' || out[n - 1] == '\r'))
    out[--n] = 0;
  return 0;
}
static int
exact (const char *a, const char *b, const char *value)
{
  char text[128];

  return read_text (a, b, text, sizeof (text)) ? -1 : strcmp (text, value) ? bad (EPROTO) : 0;
}
static int
number (const char *a, const char *b, uint64_t *out)
{
  char text[64], *end;

  if (read_text (a, b, text, sizeof (text)))
    return -1;
  if (text[0] < '0' || text[0] > '9')
    return bad (EPROTO);
  errno = 0;
  *out = strtoull (text, &end, 10);
  return errno || *end ? bad (EPROTO) : 0;
}
static int
name_ok (const char *name, const char *prefix, int dot)
{
  size_t n = strlen (prefix);
  int seen_dot = 0, digits = 0;

  if (!name || strncmp (name, prefix, n) || strlen (name) >= 64)
    return 0;
  for (name += n; *name; ++name)
    {
      if (*name >= '0' && *name <= '9')
        {
          ++digits;
          continue;
        }
      if (*name == '.' && dot && !seen_dot && digits)
        {
          seen_dot = 1;
          digits = 0;
          continue;
        }
      return 0;
    }
  return digits && seen_dot == dot;
}
static int
same_stat (const struct stat *a, const struct stat *b)
{
  return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_rdev == b->st_rdev && a->st_mode == b->st_mode;
}
static int
driver (const char *p, const char *root, const char *name)
{
  char path[PATH_MAX];
  const char *base;

  if (resolve (path, p, "driver", root))
    return -1;
  base = strrchr (path, '/');
  return !base || strcmp (base + 1, name) ? bad (ENODEV) : 0;
}
static int
inspect (const struct fte3600_linux_board_config *c, struct identity *id)
{
  char p[PATH_MAX], q[PATH_MAX], text[128], rel[128];
  const char *names[3];
  uint64_t n;
  unsigned maj, min;
  char tail;

  if (!c || !name_ok (c->spi, "spidev", 1) || !name_ok (c->gpio, "gpiochip", 0) ||
      !name_ok (c->uio, "uio", 0))
    return bad (EINVAL);
  memset (id, 0, sizeof (*id));
  if (!realpath (c->sys_root ? c->sys_root : "/sys", id->sys) ||
      !realpath (c->dev_root ? c->dev_root : "/dev", id->dev))
    return -1;
  if (number (id->sys, "module/spidev/parameters/bufsiz", &n) || !n || n > UINT32_MAX)
    return bad (EPROTO);
  id->info.spi_bufsiz = (uint32_t) n;
  names[0] = c->spi;
  names[1] = c->gpio;
  names[2] = c->uio;
  snprintf (rel, sizeof (rel), "class/spidev/%s", c->spi);
  if (resolve (id->classes[0], id->sys, rel, id->sys) ||
      resolve (id->physical, id->classes[0], "device", id->sys) ||
      driver (id->physical, id->sys, "spidev") ||
      resolve (p, id->physical, "firmware_node", id->sys) || exact (p, "hid", "FTE3600"))
    return -1;
  snprintf (rel, sizeof (rel), "bus/gpio/devices/%s", c->gpio);
  if (resolve (id->classes[1], id->sys, rel, id->sys) ||
      parent (id->glue, id->classes[1]) || parent (p, id->glue) ||
      strcmp (p, id->physical) || driver (id->glue, id->sys, "fte3600-glue"))
    return bad (ENODEV);
  snprintf (rel, sizeof (rel), "class/uio/%s", c->uio);
  if (resolve (id->classes[2], id->sys, rel, id->sys) ||
      resolve (p, id->classes[2], "device", id->sys) || strcmp (p, id->glue) ||
      exact (id->classes[2], "name", "fte3600-irq") || exact (id->classes[2], "version", "2"))
    return bad (ENODEV);
  if (exact (id->glue, "fte3600_glue_abi", "2") || exact (id->glue, "fte3600_ngpio", "1") ||
      exact (id->glue, "fte3600_status", "ready") ||
      number (id->glue, "fte3600_generation", &id->info.generation))
    return -1;
  if (number (id->glue, "fte3600_acpi_mode", &n) || n > 7)
    return bad (EPROTO);
  id->info.acpi_mode = (uint32_t) n;
  if (number (id->glue, "fte3600_acpi_speed_hz", &n) || !n || n > UINT32_MAX)
    return bad (EPROTO);
  id->info.acpi_speed_hz = (uint32_t) n;
  if (number (id->glue, "fte3600_cs_control", &n) || n > 1)
    return bad (EPROTO);
  id->info.cs_control = (unsigned) n;
  if (number (id->glue, "fte3600_irq_active_low", &n) || n > 1)
    return bad (EPROTO);
  id->info.irq_active_low = (unsigned) n;
  if (read_text (id->glue, "fte3600_irq_source", text, sizeof (text)) ||
      (strcmp (text, "gpio") && strcmp (text, "acpi")))
    return bad (EPROTO);
  for (unsigned i = 0; i < 3; ++i)
    {
      if (read_text (id->classes[i], "dev", text, sizeof (text)) ||
          sscanf (text, "%u:%u%c", &maj, &min, &tail) != 2 || join (id->nodes[i], id->dev, names[i]) ||
          stat (id->nodes[i], &id->devices[i]) || !S_ISCHR (id->devices[i].st_mode) ||
          id->devices[i].st_rdev != makedev (maj, min))
        return bad (ENODEV);
      snprintf (rel, sizeof (rel), "dev/char/%u:%u", maj, min);
      if (resolve (q, id->sys, rel, id->sys) || strcmp (q, id->classes[i]) ||
          stat (id->classes[i], &id->dirs[i]))
        return bad (ENODEV);
    }
  if (stat (id->physical, &id->dirs[3]) || stat (id->glue, &id->dirs[4]))
    return -1;
  return 0;
}
int
fte3600_linux_board_validate (const struct fte3600_linux_board_config *c,
                              struct fte3600_linux_board_info         *info)
{
  struct identity id;

  if (!info)
    return bad (EINVAL);
  if (inspect (c, &id))
    return -1;
  *info = id.info;
  return 0;
}
static int
latch (struct fte3600_linux_board *b, int e)
{
  if (!b->error)
    b->error = e ? e : EIO;
  return bad (b->error);
}
static int
current (struct fte3600_linux_board *b, int ignore_error)
{
  struct identity now;

  if (!ignore_error && b->error)
    return bad (b->error);
  if (!ignore_error && ((b->config.cancelled && *b->config.cancelled) ||
                        (b->config.is_cancelled && b->config.is_cancelled (b->config.cancel_context))))
    return latch (b, ECANCELED);
  if (inspect (&b->config, &now))
    return latch (b, ESTALE);
  if (strcmp (now.physical, b->id.physical) || strcmp (now.glue, b->id.glue) ||
      memcmp (&now.info, &b->id.info, sizeof (now.info)))
    return latch (b, ESTALE);
  for (unsigned i = 0; i < 5; ++i)
    if (!same_stat (&now.dirs[i], &b->id.dirs[i]))
      return latch (b, ESTALE);
  for (unsigned i = 0; i < 3; ++i)
    if (!same_stat (&now.devices[i], &b->id.devices[i]))
      return latch (b, ESTALE);
  return 0;
}
static int
checked_ioctl (struct fte3600_linux_board *b, int fd, unsigned long cmd, void *p)
{
  int r, e;

  if (current (b, 0))
    return -1;
  r = ioctl (fd, cmd, p);
  e = errno;
  if (current (b, 0))
    return -1;
  return r < 0 ? latch (b, e) : r;
}
static int
open_node (struct fte3600_linux_board *b, unsigned i, int flags)
{
  struct stat st;
  int fd = open (b->id.nodes[i], flags | O_CLOEXEC | O_NOFOLLOW), e;

  if (fd < 0)
    return -1;
  if (fstat (fd, &st) || !same_stat (&st, &b->id.devices[i]))
    {
      e = ENODEV;
      close (fd);
      return bad (e);
    }
  return fd;
}
static uint32_t
spi_cb (void *context, uint8_t *buffer, size_t size)
{
  struct fte3600_linux_board *b = context;
  struct spi_ioc_transfer tr = {0};
  int r;

  if (!buffer || !size || size > b->config.transfer_capacity)
    {
      latch (b, EMSGSIZE);
      return UINT32_C (0x80000000);
    }
  tr.tx_buf = (uintptr_t) buffer;
  tr.rx_buf = (uintptr_t) b->receive;
  tr.len = (uint32_t) size;
  tr.speed_hz = b->id.info.acpi_speed_hz;
  tr.bits_per_word = 8;
  r = checked_ioctl (b, b->spi, SPI_IOC_MESSAGE (1), &tr);
  if (r != (int) size)
    {
      if (r >= 0)
        latch (b, EIO);
      return UINT32_C (0x80000000);
    }
  memcpy (buffer, b->receive, size);
  return 0;
}
static uint32_t
gpio_cb (void *context, int physical)
{
  struct fte3600_linux_board *b = context;
  struct gpio_v2_line_values v = { .mask = 1, .bits = physical ? 0 : 1 };

  if (physical != 0 && physical != 1)
    {
      latch (b, EINVAL);
      return UINT32_C (0x80000000);
    }
  return checked_ioctl (b, b->reset, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) < 0 ? UINT32_C (0x80000000) : 0;
}
static int
now_ms (struct fte3600_linux_board *b, uint64_t *ms)
{
  struct timespec t;

  if (clock_gettime (CLOCK_MONOTONIC, &t))
    return latch (b, errno);
  if (t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000L)
    return latch (b, EIO);
  *ms = (uint64_t) t.tv_sec * 1000 + (uint64_t) t.tv_nsec / 1000000;
  if (*ms < b->last_monotonic)
    return latch (b, EIO);
  b->last_monotonic = *ms;
  return 0;
}
static uint32_t
clock_cb (void *context)
{
  struct fte3600_linux_board *b = context;
  uint64_t t;

  if (!b->error && !now_ms (b, &t))
    b->last_clock = (uint32_t) t;
  return b->last_clock;
}
static void
wait_duration (struct fte3600_linux_board *b, uint64_t ns)
{
  /* Small slices make void protocol waits cancellable without restarting
   * a full delay after signals. A clock failure is sticky and fatal. */
  while (ns && !current (b, 0))
    {
      uint64_t piece = ns > UINT64_C (50000000) ? UINT64_C (50000000) : ns;
      struct timespec req = {0, (long) piece}, left = {0, 0};
      int e;
      do
        {
          e = clock_nanosleep (CLOCK_MONOTONIC, 0, &req, &left);
          if (e == EINTR)
            {
              if (current (b, 0))
                return;
              req = left;
            }
        }
      while (e == EINTR);
      if (e)
        {
          latch (b, e);
          return;
        }
      ns -= piece;
    }
  if (!b->error)
    (void) current (b, 0);
}
static void
wait_ms_cb (void *c, uint32_t t)
{
  wait_duration (c, (uint64_t) t * 1000000);
}
static void
wait_us_cb (void *c, uint32_t t)
{
  wait_duration (c, (uint64_t) t * 1000);
}
static int
error_cb (void *c)
{
  return ((struct fte3600_linux_board *) c)->error;
}
static void
clear_cb (void *c)
{
  (void) c;                              /* All transport failures require a new session. */
}
static uint32_t
boundary_cb (void *context, enum fte3600_factory_boundary which)
{
  struct fte3600_linux_board *b = context;

  if (which < FTE3600_BOUNDARY_PLATFORM || which > FTE3600_BOUNDARY_FRAMEWORK_FINISH)
    {
      latch (b, EINVAL);
      return UINT32_C (0x80000000);
    }
  /* These are Linux resource checks, not emulation of Windows objects.
   * Registry/framework have no Linux state and explicitly do no extra work.
   * Every boundary verifies the actual claimed lease and prepared resources. */
  if (b->spi < 0 || b->reset < 0 || b->uio < 0 || !b->configured || current (b, 0))
    return UINT32_C (0x80000000);
  if (which == FTE3600_BOUNDARY_SETTINGS || which == FTE3600_BOUNDARY_PREPARE)
    {
      uint32_t mode = 0, speed = 0;
      uint8_t bits = 0;
      if (checked_ioctl (b, b->spi, SPI_IOC_RD_MODE32, &mode) < 0 ||
          checked_ioctl (b, b->spi, SPI_IOC_RD_BITS_PER_WORD, &bits) < 0 ||
          checked_ioctl (b, b->spi, SPI_IOC_RD_MAX_SPEED_HZ, &speed) < 0)
        return UINT32_C (0x80000000);
      if (mode != b->mode || bits != 8 || speed != b->id.info.acpi_speed_hz)
        {
          latch (b, EPROTO);
          return UINT32_C (0x80000000);
        }
    }
  return 0;
}
int
fte3600_linux_board_open (const struct fte3600_linux_board_config *c,
                          struct fte3600_linux_board             **out)
{
  struct fte3600_linux_board *b;
  struct gpiochip_info chip = {0};
  struct gpio_v2_line_info line = {.offset = 0};
  struct gpio_v2_line_request request = {0};
  uint32_t speed, mode;
  uint8_t bits = 8;
  int saved;

  if (out)
    *out = NULL;
  if (!out || !c || !c->transfer_capacity || c->transfer_capacity > INT_MAX)
    return bad (EINVAL);
  if (geteuid () != 0)
    return bad (EPERM);
  b = calloc (1, sizeof (*b));
  if (!b)
    return -1;
  b->chip = b->reset = b->uio = b->spi = -1;
  if (inspect (c, &b->id))
    goto fail;
  b->config = *c;
  if (b->config.transfer_capacity > b->id.info.spi_bufsiz)
    b->config.transfer_capacity = b->id.info.spi_bufsiz;
  strcpy (b->names[0], c->spi);
  strcpy (b->names[1], c->gpio);
  strcpy (b->names[2], c->uio);
  b->config.spi = b->names[0];
  b->config.gpio = b->names[1];
  b->config.uio = b->names[2];
  b->config.sys_root = b->id.sys;
  b->config.dev_root = b->id.dev;
  b->receive = malloc (b->config.transfer_capacity);
  if (!b->receive)
    goto fail;
  b->chip = open_node (b, 1, O_RDONLY);
  if (b->chip < 0)
    goto fail;
  if (checked_ioctl (b, b->chip, GPIO_GET_CHIPINFO_IOCTL, &chip) < 0 || chip.lines != 1 ||
      checked_ioctl (b, b->chip, GPIO_V2_GET_LINEINFO_IOCTL, &line) < 0 || strcmp (line.name, "reset"))
    {
      if(!b->error)
        errno = EPROTO;
      goto fail;
    }
  request.num_lines = 1;
  request.offsets[0] = 0;
  strcpy (request.consumer, "fte3600-hw");
  request.config.flags = GPIO_V2_LINE_FLAG_OUTPUT | GPIO_V2_LINE_FLAG_ACTIVE_LOW;
  request.config.num_attrs = 1;
  request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
  request.config.attrs[0].attr.values = 0;
  request.config.attrs[0].mask = 1;
  /* Store the fd even if a post-claim metadata check fails. */
  if (current (b, 0))
    goto fail;
  if (ioctl (b->chip, GPIO_V2_GET_LINE_IOCTL, &request) < 0)
    goto fail;
  b->reset = request.fd;
  if (current (b, 0))
    goto fail;
  b->uio = open_node (b, 2, O_RDONLY | O_NONBLOCK);
  if (b->uio < 0 || current (b, 0))
    goto fail;
  b->spi = open_node (b, 0, O_RDWR);
  if (b->spi < 0 || current (b, 0))
    goto fail;
  if (checked_ioctl (b, b->spi, SPI_IOC_RD_MODE32, &b->old_mode) < 0 ||
      checked_ioctl (b, b->spi, SPI_IOC_RD_BITS_PER_WORD, &b->old_bits) < 0 ||
      checked_ioctl (b, b->spi, SPI_IOC_RD_MAX_SPEED_HZ, &b->old_speed) < 0)
    goto fail;
  b->configured = 1;
  b->mode = b->old_mode;
  if (b->id.info.cs_control)
    {
      mode = b->id.info.acpi_mode;
      if (checked_ioctl (b, b->spi, SPI_IOC_WR_MODE32, &mode) < 0)
        goto fail;
      b->mode = mode;
    }
  else if (b->old_mode & ~(uint32_t) (SPI_CPHA | SPI_CPOL | SPI_CS_HIGH))
    {
      errno = EPROTO;
      goto fail;
    }
  speed = b->id.info.acpi_speed_hz;
  if (checked_ioctl (b, b->spi, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
      checked_ioctl (b, b->spi, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0 ||
      boundary_cb (b, FTE3600_BOUNDARY_SETTINGS))
    goto fail;
  b->io = (struct fte3600_device_io){b, spi_cb, wait_ms_cb, wait_us_cb, gpio_cb, boundary_cb, clock_cb, error_cb, clear_cb};
  *out = b;
  return 0;
fail:
  saved = errno ? errno : EIO;
  fte3600_linux_board_close (b);
  return bad (saved);
}
const struct fte3600_device_io *
fte3600_linux_board_callbacks (struct fte3600_linux_board *b)
{
  if (!b)
    {
      errno = EINVAL;
      return NULL;
    }
  return &b->io;
}
int
fte3600_linux_board_get_info (const struct fte3600_linux_board *b, struct fte3600_linux_board_info *i)
{
  if(!b || !i)
    return bad (EINVAL);
  *i = b->id.info;
  return 0;
}
int
fte3600_linux_board_set_cs (struct fte3600_linux_board *b, int high)
{
  uint32_t mode, readback;

  if (!b || (high != 0 && high != 1))
    return bad (EINVAL);
  if (!b->id.info.cs_control)
    return bad (ENOTSUP);
  mode = (b->id.info.acpi_mode & ~(uint32_t) SPI_CS_HIGH) | (high ? SPI_CS_HIGH : 0);
  if (checked_ioctl (b, b->spi, SPI_IOC_WR_MODE32, &mode) < 0 ||
      checked_ioctl (b, b->spi, SPI_IOC_RD_MODE32, &readback) < 0)
    return -1;
  if (readback != mode)
    return latch (b, EPROTO);
  b->mode = mode;
  return 0;
}
int
fte3600_linux_board_wait_irq (struct fte3600_linux_board *b, uint32_t timeout, uint32_t *counter)
{
  uint64_t start, now, remaining;
  struct pollfd p;
  int r, e;
  ssize_t got;
  uint32_t value;

  if (!b || !counter)
    return bad (EINVAL);
  if (current (b, 0) || now_ms (b, &start))
    return -1;
  for (;;)
    {
      if (current (b, 0) || now_ms (b, &now))
        return -1;
      if (now < start)
        return latch (b, EIO);
      remaining = now - start >= timeout ? 0 : timeout - (now - start);
      p = (struct pollfd){b->uio, POLLIN, 0};
      r = poll (&p, 1, (int) (remaining > 50 ? 50 : remaining));
      if (r < 0 && errno != EINTR)
        return latch (b, errno);
      if (current (b, 0))
        return -1;
      if (r > 0)
        {
          if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
            return latch (b, ENODEV);
          if (p.revents & POLLIN)
            {
              got = read (b->uio, &value, sizeof (value));
              e = errno;
              if (current (b, 0))
                return -1;
              if (got == (ssize_t) sizeof (value))
                {
                  *counter = value;
                  return 1;
                }
              if (got < 0 && (e == EAGAIN || e == EINTR))
                continue;
              return latch (b, got < 0 ? e : EIO);
            }
        }
      if (!remaining)
        return 0;
    }
}
int
fte3600_linux_board_close (struct fte3600_linux_board *b)
{
  int saved = 0;
  int fds[4];

  if (!b)
    return 0;
  /* Do not perform recovery SPI after stale instance/removal. A prior
   * ordinary failure/cancel does not suppress restoration of a live lease. */
  if (b->configured && !current (b, 1))
    {
      uint32_t mode = b->id.info.acpi_mode;
      if (b->id.info.cs_control && ioctl (b->spi, SPI_IOC_WR_MODE32, &mode) < 0)
        saved = errno;
      if (!current (b, 1) && ioctl (b->spi, SPI_IOC_WR_BITS_PER_WORD, &b->old_bits) < 0 && !saved)
        saved = errno;
      if (!current (b, 1) && ioctl (b->spi, SPI_IOC_WR_MAX_SPEED_HZ, &b->old_speed) < 0 && !saved)
        saved = errno;
      if (current (b, 1) && !saved)
        saved = errno;
    }
  fds[0] = b->uio;
  fds[1] = b->reset;
  fds[2] = b->spi;
  fds[3] = b->chip;
  for(unsigned i = 0; i < 4; ++i)
    if(fds[i] >= 0 && close (fds[i]) < 0 && !saved)
      saved = errno;
  free (b->receive);
  free (b);
  return saved ? bad (saved) : 0;
}
