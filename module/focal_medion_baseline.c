// SPDX-License-Identifier: GPL-2.0-only
/* Diagnostic adapter for the ctfdavis bridge ABI.
 * Copyright (c) 2024 Focaltech Systems (ShenZhen) Co., Ltd. (included core)
 * Copyright (c) 2026 FTE3600 Linux contributors (adapter)
 * See docs/provenance.md. Never binds outside the single Medion profile.
 */
#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>

#define LIMIT (32 * 1024)
#define INIT_SUCCESS 0x55AA
#define LOGD(...) do {} while (0)
struct focal_fp_data {
  int init;
  struct spi_device *spi;
  struct gpio_desc *gpiod_rst;
  u8 wr_buf[LIMIT] ____cacheline_aligned;
  u8 rd_buf[LIMIT] ____cacheline_aligned;
  u8 cached[LIMIT];
  unsigned seq;
  bool irq_enabled;
  u32 saved_mode, saved_speed;
  u8 saved_bits;
};
struct frame { u8 type; __le16 tx, rx; u8 payload[]; } __packed;
static DEFINE_MUTEX(io_lock);
static struct focal_fp_data *current_device;
static atomic_t opened = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(wake_queue);
static int wake_event = 5; /* archived FOCAL_WAKE_EVENT_DISABLE */

static struct gpio_desc *lookup_gpio(struct device *dev, const char *name,
                                     enum gpiod_flags flags)
{
  struct gpio_desc *gpio = devm_gpiod_get_optional(dev, name, flags);
  pr_info("MEDION_BASELINE:LOOKUP name=%s result=%s error=%ld\n", name,
          IS_ERR(gpio) ? "error" : gpio ? "present" : "absent",
          IS_ERR(gpio) ? PTR_ERR(gpio) : 0);
  if (!IS_ERR_OR_NULL(gpio))
    pr_info("MEDION_BASELINE:GPIO_MAP name=%s active_low=%d\n",
            name, gpiod_is_active_low(gpio));
  return gpio;
}
static void write_gpio(struct gpio_desc *gpio, int logical)
{
  if (!gpio) {
    pr_info("MEDION_BASELINE:GPIO logical=%d descriptor=absent no-write\n", logical);
    return;
  }
  pr_info("MEDION_BASELINE:GPIO logical=%d active_low=%d requested_raw=%d\n",
          logical, gpiod_is_active_low(gpio), logical ^ !!gpiod_is_active_low(gpio));
  gpiod_set_value_cansleep(gpio, logical);
}
/* These six bodies are extracted byte-for-byte from the pinned reference by
 * prepare.py, not rewritten from a description of the sensor protocol.
 * Instrument the descriptor boundary without altering logical values/delays.
 */
#define devm_gpiod_get_optional lookup_gpio
#define gpiod_set_value write_gpio
#define gpiod_set_value_cansleep write_gpio
#include "ctfdavis-core.inc"
#undef devm_gpiod_get_optional
#undef gpiod_set_value
#undef gpiod_set_value_cansleep

static void trace_spi(struct focal_fp_data *d, const u8 *tx,
                      unsigned ntx, unsigned nrx, int rc)
{
  static const u8 c6[] = {0x08, 0xf7, 0xc6, 0};
  static const u8 id[] = {0x04, 0xfb, 0x9a, 0x8b, 0, 0};
  unsigned n = ++d->seq;
  if (n > 64) return;
  pr_info("MEDION_BASELINE:SPI seq=%u tx=%u rx=%u rc=%d\n", n, ntx, nrx, rc);
  if (rc >= 0 && ntx == sizeof(c6) && nrx == 1 && !memcmp(tx, c6, sizeof(c6)))
    pr_info("MEDION_BASELINE:C6 seq=%u value=%02x\n", n, d->rd_buf[0]);
  if (rc >= 0 && ntx == sizeof(id) && nrx == 4 && !memcmp(tx, id, sizeof(id)))
    pr_info("MEDION_BASELINE:IDENTITY seq=%u bytes=%4ph\n", n, d->rd_buf);
}
static ssize_t transfer_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
  struct focal_fp_data *d;
  struct frame *header;
  unsigned tx, rx;
  ssize_t rc;
  if (count < sizeof(*header) || count > LIMIT) return -EINVAL;
  if (mutex_lock_interruptible(&io_lock)) return -ERESTARTSYS;
  d = current_device;
  if (!d) { rc = -ENODEV; goto out; }
  if (copy_from_user(d->wr_buf, buf, count)) { rc = -EFAULT; goto out; }
  header = (struct frame *)d->wr_buf;
  if (header->type == 0xb9) {
    rc = copy_to_user(buf, d->cached, count) ? -EFAULT : (ssize_t)count;
    goto out;
  }
  tx = le16_to_cpu(header->tx);
  rx = le16_to_cpu(header->rx);
  if ((header->type != 0xa5 && header->type != 0x5a) ||
      (header->type == 0x5a && tx) || !rx || rx > count ||
      tx > count - sizeof(*header) || tx + rx > LIMIT) {
    rc = -EINVAL; goto out;
  }
  memmove(d->wr_buf, header->payload, tx);
  /* Exactly the archived message shape: one message, TX then RX; no CS gap. */
  rc = tx ? spi_write_then_read(d->spi, d->wr_buf, tx, d->rd_buf, rx)
          : spi_read(d->spi, d->rd_buf, rx);
  trace_spi(d, d->wr_buf, tx, rx, rc);
  if (rc < 0) goto out; /* archived code incorrectly overwrote this error */
  rc = copy_to_user(buf, d->rd_buf, rx) ? -EFAULT : (ssize_t)count;
out:
  mutex_unlock(&io_lock);
  return rc;
}
static ssize_t transfer_write(struct file *f, const char __user *buf, size_t count, loff_t *pos)
{
  struct focal_fp_data *d;
  ssize_t rc;
  if (count < sizeof(struct frame) || count > LIMIT) return -EINVAL;
  if (mutex_lock_interruptible(&io_lock)) return -ERESTARTSYS;
  d = current_device;
  if (!d) { rc = -ENODEV; goto out; }
  if (copy_from_user(d->wr_buf, buf, count)) { rc = -EFAULT; goto out; }
  if (d->wr_buf[0] == 0xb9) {
    memcpy(d->cached, d->wr_buf, count); rc = count; goto out;
  }
  rc = spi_write(d->spi, d->wr_buf + sizeof(struct frame), count - sizeof(struct frame));
  trace_spi(d, d->wr_buf + sizeof(struct frame), count - sizeof(struct frame), 0, rc);
  if (rc >= 0) rc = count;
out:
  mutex_unlock(&io_lock);
  return rc;
}
static long control(struct file *f, unsigned int cmd, unsigned long arg)
{
  struct focal_fp_data *d;
  long rc = 0;
  if (mutex_lock_interruptible(&io_lock)) return -ERESTARTSYS;
  d = current_device;
  if (!d) { rc = -ENODEV; goto out; }
  switch (cmd) {
  case 0x8086:
    pr_info("MEDION_BASELINE:RESET_IOCTL\n"); focal_spi_reset(d); break;
  case 0x8087: focal_spi_power_off(d); break;
  case 0x8088: focal_spi_power_on(d); break;
  case 0x8089:
    if (!d->spi->irq) { rc = -ENXIO; break; }
    if (!!arg != d->irq_enabled) {
      if (arg) enable_irq(d->spi->irq); else disable_irq(d->spi->irq);
      d->irq_enabled = !!arg;
    }
    break;
  case 0x808a: break; /* vendor log switch; our bounded trace stays enabled */
  case 0x808b:
    WRITE_ONCE(wake_event, (int)arg);
    if (arg) wake_up_interruptible(&wake_queue);
    break;
  case 0x808c: break; /* archived CS control is a no-op */
  default: rc = -ENOTTY;
  }
out:
  mutex_unlock(&io_lock);
  return rc;
}
static int client_open(struct inode *i, struct file *f)
{
  int rc = 0;
  mutex_lock(&io_lock);
  if (!current_device) rc = -ENODEV;
  else if (atomic_cmpxchg(&opened, 0, 1)) rc = -EBUSY;
  mutex_unlock(&io_lock);
  pr_info("MEDION_BASELINE:DEVICE_OPEN rc=%d\n", rc);
  return rc;
}
static int client_close(struct inode *i, struct file *f)
{
  atomic_set(&opened, 0);
  pr_info("MEDION_BASELINE:DEVICE_CLOSE\n");
  return 0;
}
static __poll_t client_poll(struct file *f, poll_table *wait)
{
  int value;
  /* Preserve the archived event-number ABI, including its blocking poll. */
  if (wait_event_interruptible(wake_queue, READ_ONCE(wake_event) > 0)) return EPOLLERR;
  value = xchg(&wake_event, 0);
  return (__poll_t)value;
}
static const struct file_operations operations = {
  .owner = THIS_MODULE, .open = client_open, .release = client_close,
  .read = transfer_read, .write = transfer_write, .unlocked_ioctl = control,
  .compat_ioctl = control, .poll = client_poll,
};
static struct miscdevice device = {
  .minor = MISC_DYNAMIC_MINOR, .name = "focal_moh_spi", .fops = &operations, .mode = 0600,
};
static irqreturn_t interrupt(int irq, void *arg)
{
  if (READ_ONCE(wake_event) <= 1) {
    WRITE_ONCE(wake_event, 2);
    wake_up_interruptible(&wake_queue);
  }
  return IRQ_HANDLED;
}
static void restore_spi(void *arg)
{
  struct focal_fp_data *d = arg;
  d->spi->mode = d->saved_mode;
  d->spi->max_speed_hz = d->saved_speed;
  d->spi->bits_per_word = d->saved_bits;
  pr_info("MEDION_BASELINE:SPI_RESTORE rc=%d\n", spi_setup(d->spi));
}
static int probe(struct spi_device *spi)
{
  struct focal_fp_data *d;
  int rc;
  if (strcmp(dev_name(&spi->dev), "spi-FTE3600:00") ||
      !dmi_match(DMI_SYS_VENDOR, "MEDION") || !dmi_match(DMI_PRODUCT_NAME, "E3224") ||
      !dmi_match(DMI_PRODUCT_VERSION, "FT") || !dmi_match(DMI_BOARD_NAME, "YS13G"))
    return -ENODEV;
  if (current_device) return -EBUSY;
  d = devm_kzalloc(&spi->dev, sizeof(*d), GFP_KERNEL);
  if (!d) return -ENOMEM;
  d->spi = spi;
  d->saved_mode = spi->mode;
  d->saved_speed = spi->max_speed_hz;
  d->saved_bits = spi->bits_per_word;
  spi_set_drvdata(spi, d);
  pr_info("MEDION_BASELINE:PROBE ctfdavis-core port-v1\n");
  rc = focal_spi_get_gpio_config(d);
  if (rc) return rc;
  rc = devm_add_action_or_reset(&spi->dev, restore_spi, d);
  if (rc) return rc;
  rc = focal_spi_configure_spi(d);
  if (rc) return rc;
  pr_info("MEDION_BASELINE:PROBE_RESET\n");
  focal_spi_hw_reset(d);
  if (spi->irq) {
    rc = devm_request_threaded_irq(&spi->dev, spi->irq, NULL, interrupt,
                                   IRQF_ONESHOT | IRQF_TRIGGER_HIGH, "medion-baseline", d);
    if (rc) return rc;
    disable_irq(spi->irq);
  }
  d->init = INIT_SUCCESS;
  mutex_lock(&io_lock);
  current_device = d;
  rc = misc_register(&device);
  if (rc) current_device = NULL;
  mutex_unlock(&io_lock);
  pr_info("MEDION_BASELINE:BOUND rc=%d mode=%u bits=%u speed=%u gpio=%s\n",
          rc, spi->mode, spi->bits_per_word, spi->max_speed_hz, d->gpiod_rst ? "present" : "absent");
  return rc;
}
static void remove_device(struct spi_device *spi)
{
  misc_deregister(&device);
  mutex_lock(&io_lock);
  current_device = NULL;
  WRITE_ONCE(wake_event, 5);
  wake_up_interruptible(&wake_queue);
  mutex_unlock(&io_lock);
  pr_info("MEDION_BASELINE:REMOVE\n");
}
static const struct acpi_device_id ids[] = {{"FTE3600", 0}, {}};
MODULE_DEVICE_TABLE(acpi, ids);
static struct spi_driver medion_baseline_driver = {
  .driver = {.name = "focal-medion-baseline", .acpi_match_table = ids},
  .probe = probe, .remove = remove_device,
};
module_spi_driver(medion_baseline_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Medion-only ctfdavis ABI diagnostic port; not an authentication driver");
