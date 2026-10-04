// SPDX-License-Identifier: GPL-2.0-only
/* Independent ACPI resource bridge for FTE3600 fingerprint sensors.
 * No platform model, GPIO controller name, pin number or firmware is embedded.
 */
#include <linux/acpi.h>
#include <linux/compat.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>
#include <linux/uaccess.h>

#include "fte3600-bridge.h"
#include "fte3600-policy.h"

struct fte3600 {
	struct spi_device *spi;
	struct miscdevice misc;
	struct kref ref;
	struct mutex lock;
	wait_queue_head_t wait;
	atomic_t pending;
	struct gpio_desc *reset;
	struct acpi_gpio_params reset_param;
	struct acpi_gpio_mapping mapping[2];
	struct fte3600_bridge_info info;
	int irq;
	bool opened;
	bool suspended;
	bool invalidated;
	bool configuration_invalid;
	bool has_gpio_mapping;
	char *name;
};

static acpi_status fte3600_resource(struct acpi_resource *res, void *data)
{
	struct fte3600_resources *r = data;
	struct acpi_resource_gpio *gpio;
	enum fte3600_resource_kind kind;
	bool valid;

	if (res->type == ACPI_RESOURCE_TYPE_SERIAL_BUS &&
	    res->data.common_serial_bus.type == ACPI_RESOURCE_SERIAL_TYPE_SPI) {
		fte3600_resource_add(r, FTE3600_RESOURCE_SPI,
			res->data.common_serial_bus.producer_consumer == ACPI_CONSUMER);
		return AE_OK;
	}
	if (res->type != ACPI_RESOURCE_TYPE_GPIO)
		return AE_OK;

	gpio = &res->data.gpio;
	valid = gpio->pin_table_length == 1 &&
		gpio->producer_consumer == ACPI_CONSUMER;
	if (gpio->connection_type == ACPI_RESOURCE_GPIO_TYPE_IO) {
		kind = FTE3600_RESOURCE_RESET;
		valid &= gpio->io_restriction == ACPI_IO_RESTRICT_OUTPUT ||
			 gpio->io_restriction == ACPI_IO_RESTRICT_NONE;
		switch (gpio->pin_config) {
		case ACPI_PIN_CONFIG_DEFAULT:
			r->reset_bias = FTE3600_RESET_BIAS_DEFAULT;
			break;
		case ACPI_PIN_CONFIG_PULLUP:
			r->reset_bias = FTE3600_RESET_BIAS_PULL_UP;
			break;
		case ACPI_PIN_CONFIG_NOPULL:
			r->reset_bias = FTE3600_RESET_BIAS_DISABLED;
			break;
		case ACPI_PIN_CONFIG_PULLDOWN:
			r->reset_bias = FTE3600_RESET_BIAS_PULL_DOWN;
			break;
		default:
			r->reset_bias = FTE3600_RESET_BIAS_UNKNOWN;
		}
	} else if (gpio->connection_type == ACPI_RESOURCE_GPIO_TYPE_INT) {
		kind = FTE3600_RESOURCE_IRQ;
		/* A level interrupt needs a sensor-specific acknowledge protocol. */
		valid &= gpio->triggering == ACPI_EDGE_SENSITIVE &&
			 (gpio->polarity == ACPI_ACTIVE_HIGH ||
			  gpio->polarity == ACPI_ACTIVE_LOW);
	} else {
		kind = FTE3600_RESOURCE_INVALID_GPIO;
	}
	/* This index includes both GpioIo and GpioInt, as gpiolib requires. */
	fte3600_resource_add(r, kind, valid);
	return AE_OK;
}

/* Validate firmware metadata before acquiring a descriptor can drive a pin.
 * Return 1 for an accepted property, 0 when a driver mapping is needed.
 */
static int fte3600_reset_property(struct device *dev,
				 const struct fte3600_resources *r)
{
	struct fwnode_handle *fwnode = dev_fwnode(dev);
	struct fwnode_reference_args args = { };
	bool plural = fwnode_property_present(fwnode, "reset-gpios");
	bool singular = fwnode_property_present(fwnode, "reset-gpio");
	const char *property;
	bool valid;
	int ret;

	if (!plural && !singular)
		return 0;
	if (plural && singular)
		return -EINVAL;
	if (gpiod_count(dev, "reset") != 1)
		return -EINVAL;
	property = plural ? "reset-gpios" : "reset-gpio";
	ret = fwnode_property_get_reference_args(fwnode, property, NULL, 3, 0,
					       &args);
	if (ret)
		return ret;
	valid = fte3600_reset_reference_valid(args.fwnode == fwnode, args.nargs,
		args.args[0], args.args[1], args.args[2], r->reset_index);
	fwnode_handle_put(args.fwnode);
	return valid ? 1 : -EINVAL;
}

static void fte3600_free(struct kref *ref)
{
	struct fte3600 *f = container_of(ref, struct fte3600, ref);

	kfree(f->name);
	kfree(f);
}

static irqreturn_t fte3600_irq(int irq, void *data)
{
	struct fte3600 *f = data;

	atomic_set(&f->pending, 1);
	wake_up_interruptible(&f->wait);
	return IRQ_HANDLED;
}

static int fte3600_open(struct inode *inode, struct file *file)
{
	struct fte3600 *f = container_of(file->private_data, struct fte3600, misc);
	int ret = 0;

	mutex_lock(&f->lock);
	if (!f->spi)
		ret = -ENODEV;
	else if (f->suspended)
		ret = -EHOSTDOWN;
	else if (f->opened)
		ret = -EBUSY;
	else {
		/* Reopening cannot make an unknown electrical configuration valid. */
		if (f->configuration_invalid) {
			ret = spi_setup(f->spi);
			if (ret)
				goto unlock;
		}
		kref_get(&f->ref);
		f->opened = true;
		f->invalidated = false;
		f->configuration_invalid = false;
		atomic_set(&f->pending, 0);
		file->private_data = f;
	}
unlock:
	mutex_unlock(&f->lock);
	return ret;
}

static int fte3600_release(struct inode *inode, struct file *file)
{
	struct fte3600 *f = file->private_data;

	mutex_lock(&f->lock);
	if (f->spi && !f->suspended)
		gpiod_set_value_cansleep(f->reset, FTE3600_RESET_DEASSERTED);
	f->opened = false;
	mutex_unlock(&f->lock);
	kref_put(&f->ref, fte3600_free);
	return 0;
}

static long fte3600_message(struct fte3600 *f, void __user *arg)
{
	struct spi_ioc_transfer user;
	struct spi_transfer xfer = { };
	struct spi_message message;
	void *tx = NULL, *rx = NULL;
	int ret;

	if (copy_from_user(&user, arg, sizeof(user)))
		return -EFAULT;
	ret = fte3600_check_transfer(&user, f->info.max_transfer);
	if (ret)
		return ret;
	if (user.tx_buf) {
		tx = memdup_user(u64_to_user_ptr(user.tx_buf), user.len);
		if (IS_ERR(tx))
			return PTR_ERR(tx);
	}
	if (user.rx_buf) {
		rx = kzalloc(user.len, GFP_KERNEL);
		if (!rx) {
			ret = -ENOMEM;
			goto out;
		}
	}
	xfer.tx_buf = tx;
	xfer.rx_buf = rx;
	xfer.len = user.len;
	spi_message_init_with_transfers(&message, &xfer, 1);
	ret = spi_sync(f->spi, &message);
	ret = fte3600_transfer_result(ret, message.actual_length, user.len);
	if (ret > 0 && rx && copy_to_user(u64_to_user_ptr(user.rx_buf), rx, user.len))
		ret = -EFAULT;
out:
	kfree_sensitive(tx);
	kfree_sensitive(rx);
	return ret;
}

static long fte3600_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct fte3600 *f = file->private_data;
	void __user *ptr = (void __user *)arg;
	u32 value;
	u32 old_mode;
	long ret = 0;

	mutex_lock(&f->lock);
	if (!f->spi) {
		ret = -ENODEV;
		goto out;
	}
	if (f->suspended || f->invalidated) {
		ret = -EHOSTDOWN;
		goto out;
	}
	switch (cmd) {
	case FTE3600_IOC_SET_CS_POLARITY:
		if (copy_from_user(&value, ptr, sizeof(value))) {
			ret = -EFAULT;
			break;
		}
		if (value > 1) {
			ret = -EINVAL;
			break;
		}
		old_mode = f->spi->mode;
		f->spi->mode = (old_mode & ~SPI_CS_HIGH) |
			(value ? SPI_CS_HIGH : 0);
		ret = spi_setup(f->spi);
		if (ret) {
			f->spi->mode = old_mode;
			/* A failed rollback leaves electrical state unknown. */
			if (spi_setup(f->spi)) {
				f->invalidated = true;
				f->configuration_invalid = true;
			}
		} else {
			f->info.mode = f->spi->mode;
			atomic_set(&f->pending, 0);
		}
		break;
	case FTE3600_IOC_GET_INFO:
		if (copy_to_user(ptr, &f->info, sizeof(f->info)))
			ret = -EFAULT;
		break;
	case FTE3600_IOC_SET_RESET:
		if (copy_from_user(&value, ptr, sizeof(value)))
			ret = -EFAULT;
		else if (value > FTE3600_RESET_ASSERTED)
			ret = -EINVAL;
		else
			gpiod_set_value_cansleep(f->reset, value);
		break;
	case FTE3600_IOC_GET_EVENTS:
		value = atomic_xchg(&f->pending, 0);
		if (copy_to_user(ptr, &value, sizeof(value))) {
			if (value)
				atomic_set(&f->pending, 1);
			ret = -EFAULT;
		}
		break;
	case SPI_IOC_MESSAGE(1):
		ret = fte3600_message(f, ptr);
		break;
	default:
		ret = -ENOTTY;
	}
out:
	mutex_unlock(&f->lock);
	return ret;
}

static __poll_t fte3600_poll(struct file *file, poll_table *wait)
{
	struct fte3600 *f = file->private_data;
	__poll_t events = 0;

	poll_wait(file, &f->wait, wait);
	mutex_lock(&f->lock);
	if (!f->spi)
		events = EPOLLERR | EPOLLHUP;
	else if (f->suspended || f->invalidated)
		events = EPOLLERR;
	else if (atomic_read(&f->pending))
		events = EPOLLIN | EPOLLRDNORM;
	mutex_unlock(&f->lock);
	return events;
}

static const struct file_operations fte3600_fops = {
	.owner = THIS_MODULE,
	.open = fte3600_open,
	.release = fte3600_release,
	.unlocked_ioctl = fte3600_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
	.poll = fte3600_poll,
	.llseek = NULL,
};

static ssize_t fte3600_abi_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", FTE3600_BRIDGE_ABI);
}
static DEVICE_ATTR_RO(fte3600_abi);
static struct attribute *fte3600_attrs[] = {
	&dev_attr_fte3600_abi.attr,
	NULL,
};
ATTRIBUTE_GROUPS(fte3600);

static int fte3600_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct fte3600_resources r = { };
	struct fte3600 *f;
	acpi_status status;
	bool reset_property;
	int ret;

	if (!ACPI_COMPANION(dev))
		return -ENODEV;
	status = acpi_walk_resources(ACPI_HANDLE(dev), METHOD_NAME__CRS,
				     fte3600_resource, &r);
	if (ACPI_FAILURE(status) || !fte3600_resources_valid(&r))
		return dev_err_probe(dev, -EINVAL,
			"Need one SPI, one reset GpioIo and one edge GpioInt resource\n");
	/* Linux 6.8 ACPI may replace even GPIOD_ASIS/OUT_LOW with an initial
	 * value inferred from OutputOnly and pull bias. A pull-down would assert
	 * reset before protocol timing begins; refuse it before GPIO acquisition.
	 */
	if (!fte3600_reset_bias_valid(r.reset_bias))
		return dev_err_probe(dev, -EINVAL,
			"Reset bias conflicts with the verified inactive-high line\n");
	ret = fte3600_reset_property(dev, &r);
	if (ret < 0)
		return dev_err_probe(dev, ret,
			"Reset property must identify the active-low GpioIo resource\n");
	reset_property = ret == 1;
	if ((spi->bits_per_word && spi->bits_per_word != 8) ||
	    (spi->mode & ~(SPI_CPOL | SPI_CPHA | SPI_CS_HIGH)))
		return dev_err_probe(dev, -EINVAL, "Unsupported SPI resource format\n");
	if (!spi->max_speed_hz)
		return -EINVAL;
	spi->bits_per_word = 8;
	spi->max_speed_hz = min(spi->max_speed_hz, 1000000U);
	ret = spi_setup(spi);
	if (ret)
		return ret;
	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	kref_init(&f->ref);
	mutex_init(&f->lock);
	init_waitqueue_head(&f->wait);
	atomic_set(&f->pending, 0);
	f->spi = spi;
	f->info.abi_version = FTE3600_BRIDGE_ABI;
	f->info.max_transfer = min_t(size_t, FTE3600_BRIDGE_MAX_TRANSFER,
		min(spi_max_transfer_size(spi), spi_max_message_size(spi)));
	if (!f->info.max_transfer) {
		ret = -EMSGSIZE;
		goto free;
	}
	f->info.speed_hz = spi->max_speed_hz;
	f->info.mode = spi->mode;
	f->info.bits_per_word = spi->bits_per_word;
	f->info.capabilities = FTE3600_BRIDGE_CAP_CS_POLARITY;

	/* Windows writes physical H/L/H through IOCTL_GPIO_WRITE_PINS. Our ABI
	 * instead uses logical deassert/assert/deassert (0/1/0), so active_low
	 * must be true. Conflicting _DSD polarity was rejected before any write.
	 * GpioIo alone has no polarity field; use this independently established
	 * protocol fact only when firmware has no named reset property.
	 * crs_entry_index counts GPIO resources, not controllers or pin numbers.
	 */
	f->reset_param.crs_entry_index = r.reset_index;
	f->reset_param.line_index = 0;
	f->reset_param.active_low = true;
	f->mapping[0].name = "reset-gpios";
	f->mapping[0].data = &f->reset_param;
	f->mapping[0].size = 1;
	if (!reset_property) {
		ret = acpi_dev_add_driver_gpios(ACPI_COMPANION(dev), f->mapping);
		if (ret)
			goto free;
		f->has_gpio_mapping = true;
	}
	f->reset = gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(f->reset)) {
		ret = PTR_ERR(f->reset);
		goto unmap;
	}
	if (!gpiod_is_active_low(f->reset)) {
		ret = -EINVAL;
		goto gpio;
	}
	ret = gpiod_direction_output(f->reset, FTE3600_RESET_DEASSERTED);
	if (ret)
		goto gpio;
	f->irq = acpi_dev_gpio_irq_get(ACPI_COMPANION(dev), 0);
	if (f->irq < 0) {
		ret = f->irq;
		goto gpio;
	}
	ret = request_threaded_irq(f->irq, NULL, fte3600_irq, IRQF_ONESHOT,
				  dev_name(dev), f);
	if (ret)
		goto gpio;
	f->name = kasprintf(GFP_KERNEL, "fte3600-%s", dev_name(dev));
	if (!f->name) {
		ret = -ENOMEM;
		goto irq;
	}
	f->misc.minor = MISC_DYNAMIC_MINOR;
	f->misc.name = f->name;
	f->misc.fops = &fte3600_fops;
	f->misc.parent = dev;
	f->misc.mode = 0600;
	f->misc.groups = fte3600_groups;
	spi_set_drvdata(spi, f);
	ret = misc_register(&f->misc);
	if (!ret)
		return 0;
irq:
	free_irq(f->irq, f);
gpio:
	gpiod_put(f->reset);
unmap:
	if (f->has_gpio_mapping)
		acpi_dev_remove_driver_gpios(ACPI_COMPANION(dev));
free:
	kref_put(&f->ref, fte3600_free);
	return dev_err_probe(dev, ret, "Cannot acquire ACPI sensor resources\n");
}

static void fte3600_remove(struct spi_device *spi)
{
	struct fte3600 *f = spi_get_drvdata(spi);

	mutex_lock(&f->lock);
	f->spi = NULL;
	if (!f->suspended)
		gpiod_set_value_cansleep(f->reset, FTE3600_RESET_DEASSERTED);
	mutex_unlock(&f->lock);
	wake_up_interruptible(&f->wait);
	misc_deregister(&f->misc);
	free_irq(f->irq, f);
	gpiod_put(f->reset);
	if (f->has_gpio_mapping)
		acpi_dev_remove_driver_gpios(ACPI_COMPANION(&spi->dev));
	kref_put(&f->ref, fte3600_free);
}

static int fte3600_suspend(struct device *dev)
{
	struct fte3600 *f = dev_get_drvdata(dev);

	mutex_lock(&f->lock);
	if (!f->suspended) {
		f->suspended = true;
		f->invalidated = f->opened;
		/* The IRQ thread never takes this mutex or accesses the sensor. */
		disable_irq(f->irq);
		gpiod_set_value_cansleep(f->reset, FTE3600_RESET_DEASSERTED);
		atomic_set(&f->pending, 0);
	}
	mutex_unlock(&f->lock);
	wake_up_interruptible(&f->wait);
	return 0;
}

static int fte3600_resume(struct device *dev)
{
	struct fte3600 *f = dev_get_drvdata(dev);

	mutex_lock(&f->lock);
	if (f->suspended) {
		gpiod_set_value_cansleep(f->reset, FTE3600_RESET_DEASSERTED);
		atomic_set(&f->pending, 0);
		f->suspended = false;
		enable_irq(f->irq);
	}
	mutex_unlock(&f->lock);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(fte3600_pm, fte3600_suspend, fte3600_resume);
static const struct acpi_device_id fte3600_acpi_ids[] = {
	{ "FTE3600", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, fte3600_acpi_ids);
static const struct spi_device_id fte3600_spi_ids[] = {
	{ "fte3600", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, fte3600_spi_ids);

static struct spi_driver fte3600_driver = {
	.driver = {
		.name = "fte3600",
		.acpi_match_table = fte3600_acpi_ids,
		.pm = pm_sleep_ptr(&fte3600_pm),
	},
	.probe = fte3600_probe,
	.remove = fte3600_remove,
	.id_table = fte3600_spi_ids,
};
module_spi_driver(fte3600_driver);

MODULE_DESCRIPTION("FTE3600 ACPI SPI/reset/interrupt resource bridge");
MODULE_AUTHOR("FTE3600 Linux contributors");
MODULE_LICENSE("GPL");
