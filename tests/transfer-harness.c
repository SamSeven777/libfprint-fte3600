/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#define __user
#define __packed __attribute__((packed))
#define LIMIT (32 * 1024)
#define ERESTARTSYS 512
typedef uint8_t u8;
typedef uint16_t __le16;
#define le16_to_cpu(x) (x)
struct file { int unused; };
struct focal_fp_data { void *spi; u8 wr_buf[LIMIT], rd_buf[LIMIT], cached[LIMIT]; };
static struct focal_fp_data data, *current_device = &data;
static int io_lock, locked, copy_failure, spi_error, calls;
static unsigned sent, received;
static u8 first_tx;
static int mutex_lock_interruptible(int *lock) { (void)lock; assert(!locked); locked = 1; return 0; }
static void mutex_unlock(int *lock) { (void)lock; assert(locked); locked = 0; }
static int copy_bytes(void *to, const void *from, size_t count)
{ if (copy_failure) return 1; memcpy(to, from, count); return 0; }
#define copy_from_user copy_bytes
#define copy_to_user copy_bytes
static int spi_write_then_read(void *spi, const void *tx, unsigned ntx, void *rx, unsigned nrx)
{
  (void)spi; assert(locked); calls++; sent = ntx; received = nrx;
  first_tx = ntx ? *(const u8 *)tx : 0;
  memset(rx, 0x42, nrx); return spi_error;
}
static int spi_read(void *spi, void *rx, unsigned nrx)
{ return spi_write_then_read(spi, NULL, 0, rx, nrx); }
static int spi_write(void *spi, const void *tx, unsigned ntx)
{ u8 ignored; return spi_write_then_read(spi, tx, ntx, &ignored, 0); }
static void trace_spi(struct focal_fp_data *d, const u8 *tx, unsigned ntx, unsigned nrx, int rc)
{ (void)d; (void)tx; (void)ntx; (void)nrx; (void)rc; }
#include "transfer-functions.inc"
int main(void)
{
  u8 buffer[LIMIT] = {0xa5, 4, 0, 1, 0, 8, 0xf7, 0xc6, 0};
  assert(sizeof(struct frame) == 5);
  assert(transfer_read(NULL, (char *)buffer, 9, NULL) == 9);
  assert(buffer[0] == 0x42 && sent == 4 && received == 1 && first_tx == 8);
  struct frame *h = (void *)buffer;
  h->type = 0x5a; h->tx = 0; h->rx = 300;
  assert(transfer_read(NULL, (char *)buffer, 300, NULL) == 300);
  assert(sent == 0 && received == 300);
  int before = calls;
  for (unsigned n = 0; n < 5; n++) assert(transfer_read(NULL, (char *)buffer, n, NULL) == -EINVAL);
  assert(transfer_read(NULL, (char *)buffer, LIMIT + 1, NULL) == -EINVAL);
  h->type = 0xa5; h->tx = 5; h->rx = 1;
  assert(transfer_read(NULL, (char *)buffer, 9, NULL) == -EINVAL);
  h->tx = 0; h->rx = 10;
  assert(transfer_read(NULL, (char *)buffer, 9, NULL) == -EINVAL);
  h->rx = 0;
  assert(transfer_read(NULL, (char *)buffer, 9, NULL) == -EINVAL);
  h->type = 0x5a; h->tx = 1; h->rx = 1;
  assert(transfer_read(NULL, (char *)buffer, 9, NULL) == -EINVAL);
  h->type = 0x55;
  assert(transfer_read(NULL, (char *)buffer, 9, NULL) == -EINVAL);
  assert(calls == before);
  h->type = 0xa5; h->tx = 4; h->rx = 1;
  spi_error = -ETIMEDOUT;
  assert(transfer_read(NULL, (char *)buffer, 9, NULL) == -ETIMEDOUT);
  assert(transfer_write(NULL, (char *)buffer, 9, NULL) == -ETIMEDOUT);
  spi_error = 0;
  assert(transfer_write(NULL, (char *)buffer, 9, NULL) == 9);
  assert(sent == 4 && received == 0 && first_tx == 0x42);
  buffer[0] = 0xb9; buffer[8] = 0x99;
  before = calls;
  assert(transfer_write(NULL, (char *)buffer, 9, NULL) == 9);
  buffer[8] = 0;
  assert(transfer_read(NULL, (char *)buffer, 9, NULL) == 9 && buffer[8] == 0x99);
  assert(calls == before);
  copy_failure = 1;
  assert(transfer_read(NULL, (char *)buffer, 9, NULL) == -EFAULT);
  assert(transfer_write(NULL, (char *)buffer, 9, NULL) == -EFAULT);
  copy_failure = 0; current_device = NULL;
  assert(transfer_read(NULL, (char *)buffer, 9, NULL) == -ENODEV);
  assert(transfer_write(NULL, (char *)buffer, 9, NULL) == -ENODEV);
  assert(!locked);
  puts("PASS: actual adapter read/write bodies, frame bounds, payload offsets, errors and cache");
}
