// SPDX-License-Identifier: GPL-2.0-only
/*
 * FTE3600 ACPI glue for standard spidev, GPIO and UIO character interfaces.
 * No sensor commands, firmware, board names or physical pin numbers belong here.
 */
#include <linux/acpi.h>
#include <linux/cred.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/suspend.h>
#include <linux/uio_driver.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#include "fte3600-policy.h"

#define FTE3600_GLUE_NAME "fte3600-glue"

struct fte3600 {
	struct device *dev;
	struct acpi_device *adev;
	struct gpio_chip chip;
	struct gpio_desc *lines[FTE3600_LEASE_COUNT];
	struct acpi_gpio_params params[FTE3600_LEASE_COUNT];
	struct acpi_gpio_mapping mapping[FTE3600_LEASE_COUNT + 1];
	struct uio_info uio;
	struct fte3600_resources resources;
	struct fte3600_lease lease;
	struct mutex lock;
	struct notifier_block pm;
	struct kref ref;
	struct work_struct cleanup;
	int irq;
	bool irq_requested;
	bool uio_registered;
	bool mapped;
};

struct fte3600_binding {
	struct list_head node;
	struct platform_device *pdev;
	struct device *spi;
};

static LIST_HEAD(fte3600_bindings);
static DEFINE_MUTEX(fte3600_bindings_lock);
static struct workqueue_struct *fte3600_cleanup_queue;
static const char * const fte3600_line_names[] = { "reset", "irq" };

static void fte3600_last_reference(struct kref *ref);

static const struct acpi_device_id fte3600_acpi_ids[] = {
	{ "FTE3600", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, fte3600_acpi_ids);

static acpi_status fte3600_resource(struct acpi_resource *res, void *data)
{
	struct fte3600_resources *r = data;
	struct acpi_resource_spi_serialbus *spi;
	struct acpi_resource_gpio *gpio;
	enum fte3600_resource_kind kind;
	bool valid;

	if (res->type == ACPI_RESOURCE_TYPE_SERIAL_BUS &&
	    res->data.common_serial_bus.type == ACPI_RESOURCE_SERIAL_TYPE_SPI) {
		spi = &res->data.spi_serial_bus;
		valid = spi->producer_consumer == ACPI_CONSUMER &&
			spi->data_bit_length == 8 && spi->connection_speed &&
			spi->wire_mode == ACPI_SPI_4WIRE_MODE &&
			spi->clock_phase <= ACPI_SPI_SECOND_PHASE &&
			spi->clock_polarity <= ACPI_SPI_START_HIGH &&
			spi->device_polarity <= ACPI_SPI_ACTIVE_HIGH;
		fte3600_resource_add(r, FTE3600_RESOURCE_SPI, valid);
		r->speed_hz = spi->connection_speed;
		if (spi->clock_phase == ACPI_SPI_SECOND_PHASE)
			r->mode |= SPI_CPHA;
		if (spi->clock_polarity == ACPI_SPI_START_HIGH)
			r->mode |= SPI_CPOL;
		if (spi->device_polarity == ACPI_SPI_ACTIVE_HIGH)
			r->mode |= SPI_CS_HIGH;
		return AE_OK;
	}
	if (res->type == ACPI_RESOURCE_TYPE_IRQ) {
		struct acpi_resource_irq *irq = &res->data.irq;

		valid = irq->interrupt_count == 1 &&
			irq->triggering == ACPI_EDGE_SENSITIVE &&
			(irq->polarity == ACPI_ACTIVE_HIGH ||
			 irq->polarity == ACPI_ACTIVE_LOW);
		r->irq_active_low = irq->polarity == ACPI_ACTIVE_LOW;
		fte3600_resource_add(r, FTE3600_RESOURCE_IRQ_ACPI, valid);
		return AE_OK;
	}
	if (res->type == ACPI_RESOURCE_TYPE_EXTENDED_IRQ) {
		struct acpi_resource_extended_irq *irq = &res->data.extended_irq;

		valid = irq->producer_consumer == ACPI_CONSUMER &&
			irq->interrupt_count == 1 &&
			irq->triggering == ACPI_EDGE_SENSITIVE &&
			(irq->polarity == ACPI_ACTIVE_HIGH ||
			 irq->polarity == ACPI_ACTIVE_LOW);
		r->irq_active_low = irq->polarity == ACPI_ACTIVE_LOW;
		fte3600_resource_add(r, FTE3600_RESOURCE_IRQ_ACPI, valid);
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
		valid &= gpio->triggering == ACPI_EDGE_SENSITIVE &&
			 (gpio->polarity == ACPI_ACTIVE_HIGH ||
			  gpio->polarity == ACPI_ACTIVE_LOW);
		r->irq_active_low = gpio->polarity == ACPI_ACTIVE_LOW;
	} else {
		kind = FTE3600_RESOURCE_INVALID_GPIO;
	}
	fte3600_resource_add(r, kind, valid);
	return AE_OK;
}

/* Firmware properties must agree with the validated resource roles. GpioInt
 * defines its own polarity; the _DSD active_low argument for it must be zero.
 * GpioIo has no polarity: independently established reset polarity is low.
 */
static int fte3600_check_property(struct device *spi, unsigned int index,
				 unsigned int resource)
{
	struct fwnode_handle *fwnode = dev_fwnode(spi);
	struct fwnode_reference_args args = { };
	const char *plural = index == FTE3600_RESET ? "reset-gpios" : "irq-gpios";
	const char *singular = index == FTE3600_RESET ? "reset-gpio" : "irq-gpio";
	bool has_plural = fwnode_property_present(fwnode, plural);
	bool has_singular = fwnode_property_present(fwnode, singular);
	bool valid;
	int ret;

	if (!has_plural && !has_singular)
		return 0;
	if ((has_plural && has_singular) ||
	    gpiod_count(spi, fte3600_line_names[index]) != 1)
		return -EINVAL;
	ret = fwnode_property_get_reference_args(fwnode,
		has_plural ? plural : singular, NULL, 3, 0, &args);
	if (ret)
		return ret;
	valid = fte3600_gpio_reference_valid(args.fwnode == fwnode, args.nargs,
		args.args[0], args.args[1], args.args[2], resource,
		index == FTE3600_RESET);
	fwnode_handle_put(args.fwnode);
	return valid ? 0 : -EINVAL;
}

static int fte3600_deassert(struct fte3600 *f)
{
	/* The underlying descriptor is active-low; logical zero is physical H.
	 * direction_output also gives an error return on kernels predating 6.17.
	 */
	return gpiod_direction_output(f->lines[FTE3600_RESET], 0);
}

static irqreturn_t fte3600_irq_handler(int irq, void *data)
{
	struct fte3600 *f = data;

	/* No SPI access or sensor-specific acknowledgement belongs in this ISR.
	 * An edge IRQ is exclusively requested while the UIO session is open.
	 */
	uio_event_notify(&f->uio);
	return IRQ_HANDLED;
}

/* Caller holds f->lock. The ISR never takes that lock, so free_irq can
 * synchronize it here before reset release or UIO unregistration.
 */
static void fte3600_stop_irq(struct fte3600 *f)
{
	if (f->irq_requested) {
		free_irq(f->irq, f);
		f->irq_requested = false;
	}
}

static int fte3600_uio_open(struct uio_info *info, struct inode *inode)
{
	struct fte3600 *f = info->priv;
	unsigned long flags;
	int ret;

	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID))
		return -EPERM;
	mutex_lock(&f->lock);
	ret = fte3600_lease_request(&f->lease, FTE3600_IRQ);
	if (ret)
		goto unlock;
	flags = f->resources.irq_active_low ? IRQF_TRIGGER_FALLING : IRQF_TRIGGER_RISING;
	/* Do not use IRQF_SHARED: without sensor I/O the ISR cannot establish
	 * ownership of a shared interrupt. An occupied IRQ must fail with EBUSY.
	 */
	ret = request_irq(f->irq, fte3600_irq_handler, flags, FTE3600_GLUE_NAME, f);
	if (ret)
		fte3600_lease_free(&f->lease, FTE3600_IRQ);
	else
		f->irq_requested = true;
unlock:
	mutex_unlock(&f->lock);
	return ret;
}

static int fte3600_uio_release(struct uio_info *info, struct inode *inode)
{
	struct fte3600 *f = info->priv;

	mutex_lock(&f->lock);
	fte3600_stop_irq(f);
	fte3600_lease_free(&f->lease, FTE3600_IRQ);
	mutex_unlock(&f->lock);
	return 0;
}

static int fte3600_gpio_request(struct gpio_chip *chip, unsigned int offset)
{
	struct fte3600 *f = gpiochip_get_data(chip);
	int ret;

	/* Keep access root-only even if an unrelated generic GPIO udev rule
	 * accidentally grants this new subset device to a broader group.
	 */
	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID))
		return -EPERM;
	if (offset >= FTE3600_NGPIO)
		return -EINVAL;
	mutex_lock(&f->lock);
	ret = fte3600_lease_request(&f->lease, offset);
	if (!ret && offset == FTE3600_RESET) {
		ret = fte3600_deassert(f);
		if (ret)
			fte3600_lease_free(&f->lease, offset);
	}
	if (!ret)
		kref_get(&f->ref);
	mutex_unlock(&f->lock);
	return ret;
}

static void fte3600_gpio_free(struct gpio_chip *chip, unsigned int offset)
{
	struct fte3600 *f = gpiochip_get_data(chip);
	int ret = 0;

	mutex_lock(&f->lock);
	fte3600_stop_irq(f);
	if (offset == FTE3600_RESET && f->lease.online && !f->lease.suspended)
		ret = fte3600_deassert(f);
	fte3600_lease_free(&f->lease, offset);
	if (f->uio_registered && (f->lease.requested & (1U << FTE3600_IRQ)))
		uio_event_notify(&f->uio);
	mutex_unlock(&f->lock);
	if (ret)
		dev_warn(f->dev, "Could not deassert reset on GPIO close: %d\n", ret);
	kref_put(&f->ref, fte3600_last_reference);
}

static int fte3600_gpio_get_direction(struct gpio_chip *chip, unsigned int offset)
{
	/* Also called during gpiochip registration, before any user request. */
	if (offset >= FTE3600_NGPIO)
		return -EINVAL;
	return 0;
}

static int fte3600_gpio_direction_input(struct gpio_chip *chip, unsigned int offset)
{
	return -EINVAL;
}

static int fte3600_gpio_direction_output(struct gpio_chip *chip,
					unsigned int offset, int value)
{
	struct fte3600 *f = gpiochip_get_data(chip);
	int ret;

	if (offset != FTE3600_RESET)
		return -EINVAL;
	mutex_lock(&f->lock);
	ret = fte3600_lease_check(&f->lease, offset);
	if (!ret)
		ret = gpiod_direction_output_raw(f->lines[offset], value);
	mutex_unlock(&f->lock);
	return ret;
}

static int fte3600_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct fte3600 *f = gpiochip_get_data(chip);
	int ret;

	if (offset >= FTE3600_NGPIO)
		return -EINVAL;
	mutex_lock(&f->lock);
	ret = fte3600_lease_check(&f->lease, offset);
	if (!ret)
		ret = gpiod_get_raw_value_cansleep(f->lines[offset]);
	mutex_unlock(&f->lock);
	return ret;
}

static int fte3600_gpio_set_value(struct gpio_chip *chip,
				 unsigned int offset, int value)
{
	struct fte3600 *f = gpiochip_get_data(chip);
	int ret;

	if (offset != FTE3600_RESET)
		return -EINVAL;
	mutex_lock(&f->lock);
	ret = fte3600_lease_check(&f->lease, offset);
	if (!ret) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
		ret = gpiod_set_raw_value_cansleep(f->lines[offset], value);
#else
		gpiod_set_raw_value_cansleep(f->lines[offset], value);
#endif
	}
	mutex_unlock(&f->lock);
	return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
static int fte3600_gpio_set(struct gpio_chip *chip, unsigned int offset, int value)
{
	return fte3600_gpio_set_value(chip, offset, value);
}
#else
static void fte3600_gpio_set(struct gpio_chip *chip, unsigned int offset, int value)
{
	/* Older GPIO setters cannot return errors. A stale session never drives
	 * reset; the userspace generation checks are mandatory on these kernels.
	 */
	fte3600_gpio_set_value(chip, offset, value);
}
#endif

static int fte3600_pm_notify(struct notifier_block *nb, unsigned long event,
			     void *unused)
{
	struct fte3600 *f = container_of(nb, struct fte3600, pm);
	int ret = 0;

	mutex_lock(&f->lock);
	switch (event) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
		fte3600_lease_suspend(&f->lease);
		fte3600_stop_irq(f);
		ret = fte3600_deassert(f);
		if (f->uio_registered)
			uio_event_notify(&f->uio);
		break;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		ret = fte3600_deassert(f);
		/* On failure stay unavailable; no old request becomes usable. */
		if (!ret)
			f->lease.suspended = false;
		break;
	default:
		break;
	}
	mutex_unlock(&f->lock);
	if (ret)
		dev_warn(f->dev, "Reset release during power transition failed: %d\n", ret);
	/* A failed release leaves this glue unavailable. Never stop another
	 * device's POST notifier: all suppliers have already resumed there.
	 */
	return NOTIFY_OK;
}

static ssize_t fte3600_glue_abi_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", FTE3600_GLUE_ABI);
}
static DEVICE_ATTR_RO(fte3600_glue_abi);

static ssize_t fte3600_ngpio_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", FTE3600_NGPIO);
}
static DEVICE_ATTR_RO(fte3600_ngpio);

static ssize_t fte3600_generation_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct fte3600 *f = dev_get_drvdata(dev);
	u64 generation;

	mutex_lock(&f->lock);
	generation = f->lease.generation;
	mutex_unlock(&f->lock);
	return sysfs_emit(buf, "%llu\n", generation);
}
static DEVICE_ATTR_RO(fte3600_generation);

static ssize_t fte3600_status_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct fte3600 *f = dev_get_drvdata(dev);
	const char *status;

	mutex_lock(&f->lock);
	status = !f->lease.online ? "removed" :
		 f->lease.suspended ? "suspended" : "ready";
	mutex_unlock(&f->lock);
	return sysfs_emit(buf, "%s\n", status);
}
static DEVICE_ATTR_RO(fte3600_status);

static ssize_t fte3600_acpi_mode_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct fte3600 *f = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", f->resources.mode);
}
static DEVICE_ATTR_RO(fte3600_acpi_mode);

static ssize_t fte3600_cs_control_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct fte3600 *f = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", f->resources.cs_control);
}
static DEVICE_ATTR_RO(fte3600_cs_control);

static ssize_t fte3600_acpi_speed_hz_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	struct fte3600 *f = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", f->resources.speed_hz);
}
static DEVICE_ATTR_RO(fte3600_acpi_speed_hz);

static ssize_t fte3600_irq_active_low_show(struct device *dev,
					  struct device_attribute *attr, char *buf)
{
	struct fte3600 *f = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", f->resources.irq_active_low);
}
static DEVICE_ATTR_RO(fte3600_irq_active_low);

static ssize_t fte3600_irq_source_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct fte3600 *f = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", f->resources.irq_is_gpio ? "gpio" : "acpi");
}
static DEVICE_ATTR_RO(fte3600_irq_source);

static struct attribute *fte3600_attrs[] = {
	&dev_attr_fte3600_glue_abi.attr,
	&dev_attr_fte3600_ngpio.attr,
	&dev_attr_fte3600_generation.attr,
	&dev_attr_fte3600_status.attr,
	&dev_attr_fte3600_acpi_mode.attr,
	&dev_attr_fte3600_cs_control.attr,
	&dev_attr_fte3600_acpi_speed_hz.attr,
	&dev_attr_fte3600_irq_active_low.attr,
	&dev_attr_fte3600_irq_source.attr,
	NULL,
};
static const struct attribute_group fte3600_attr_group = {
	.attrs = fte3600_attrs,
};

static int fte3600_change_uevent(struct device *dev, void *unused)
{
	kobject_uevent(&dev->kobj, KOBJ_CHANGE);
	return 0;
}

static void fte3600_put_gpios(struct fte3600 *f)
{
	unsigned int i;

	for (i = 0; i < FTE3600_LEASE_COUNT; i++)
		if (!IS_ERR_OR_NULL(f->lines[i]))
			gpiod_put(f->lines[i]);
	if (f->mapped)
		acpi_dev_remove_driver_gpios(f->adev);
}

static void fte3600_free(struct fte3600 *f)
{
	fte3600_put_gpios(f);
	put_device(&f->adev->dev);
	put_device(f->dev);
	kfree(f);
}

static void fte3600_cleanup(struct work_struct *work)
{
	struct fte3600 *f = container_of(work, struct fte3600, cleanup);
	unsigned int i;
	bool pending;
	char *label;

	/* Gpiolib clears FLAG_REQUESTED only after our .free callback returns. Wait
	 * for this final commit before removing the chip. No successful new
	 * requests are possible after the driver reference has been dropped.
	 * Allocation failure in the label query is conservatively still busy.
	 */
	do {
		pending = false;
		for (i = 0; i < FTE3600_NGPIO; i++) {
			label = gpiochip_dup_line_label(&f->chip, i);
			pending |= label != NULL;
			if (!IS_ERR(label))
				kfree(label);
		}
		if (pending)
			msleep(1);
	} while (pending);
	gpiochip_remove(&f->chip);
	fte3600_free(f);
}

static void fte3600_last_reference(struct kref *ref)
{
	struct fte3600 *f = container_of(ref, struct fte3600, ref);

	/* .free must return before cleanup; module exit drains this dedicated
	 * queue after the last GPIO request drops its gpiolib module reference.
	 */
	queue_work(fte3600_cleanup_queue, &f->cleanup);
}

static int fte3600_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *spi = dev->parent;
	struct fte3600_resources *r;
	struct fte3600 *f;
	unsigned int indices[FTE3600_LEASE_COUNT];
	unsigned int i, gpio_count;
	acpi_status status;
	int ret;

	if (!spi || spi->bus != &spi_bus_type || !ACPI_COMPANION(spi))
		return -ENODEV;
	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	f->dev = get_device(dev);
	f->adev = ACPI_COMPANION(spi);
	get_device(&f->adev->dev);
	mutex_init(&f->lock);
	kref_init(&f->ref);
	INIT_WORK(&f->cleanup, fte3600_cleanup);
	r = &f->resources;
	r->cs_control = fte3600_cs_control_supported(
		!!spi_get_csgpiod(to_spi_device(spi), 0),
		!!(to_spi_device(spi)->controller->mode_bits & SPI_CS_HIGH));
	status = acpi_walk_resources(f->adev->handle, METHOD_NAME__CRS,
				     fte3600_resource, r);
	if (ACPI_FAILURE(status) || !fte3600_resources_valid(r)) {
		ret = dev_err_probe(dev, -EINVAL,
			"Need one 8-bit SPI, one reset GpioIo and one edge GPIO or ACPI interrupt\n");
		goto free;
	}
	/* OutputOnly ACPI bias may override even GPIOD_ASIS. Reject a bias
	 * which could assert reset before the verified H/L/H protocol begins.
	 */
	if (!fte3600_reset_bias_valid(r->reset_bias)) {
		ret = dev_err_probe(dev, -EINVAL, "Reset bias conflicts with inactive-high reset\n");
		goto free;
	}
	indices[FTE3600_RESET] = r->reset_index;
	indices[FTE3600_IRQ] = r->irq_index;
	gpio_count = r->irq_is_gpio ? 2 : 1;
	if (!r->irq_is_gpio &&
	    (device_property_present(spi, "irq-gpios") ||
	     device_property_present(spi, "irq-gpio"))) {
		ret = dev_err_probe(dev, -EINVAL,
			"A named IRQ GPIO conflicts with the non-GPIO interrupt resource\n");
		goto free;
	}
	for (i = 0; i < gpio_count; i++) {
		ret = fte3600_check_property(spi, i, indices[i]);
		if (ret) {
			dev_err_probe(dev, ret, "Conflicting named GPIO property\n");
			goto free;
		}
		f->params[i].crs_entry_index = indices[i];
		f->params[i].active_low = i == FTE3600_RESET;
		f->mapping[i].name = i == FTE3600_RESET ? "reset-gpios" : "irq-gpios";
		f->mapping[i].data = &f->params[i];
		f->mapping[i].size = 1;
	}
	/* Do not replace another driver's mapping or share its resources. */
	if (f->adev->driver_gpios) {
		ret = -EBUSY;
		goto free;
	}
	ret = acpi_dev_add_driver_gpios(f->adev, f->mapping);
	if (ret)
		goto free;
	f->mapped = true;
	for (i = 0; i < gpio_count; i++) {
		f->lines[i] = fwnode_gpiod_get_index(acpi_fwnode_handle(f->adev),
			fte3600_line_names[i], 0, GPIOD_ASIS, dev_name(dev));
		if (IS_ERR(f->lines[i])) {
			ret = PTR_ERR(f->lines[i]);
			goto gpio;
		}
	}
	if (!gpiod_is_active_low(f->lines[FTE3600_RESET])) {
		ret = -EINVAL;
		goto gpio;
	}
	ret = fte3600_deassert(f);
	if (ret)
		goto gpio;
	if (r->irq_is_gpio) {
		ret = gpiod_direction_input(f->lines[FTE3600_IRQ]);
		if (ret)
			goto gpio;
		f->irq = gpiod_to_irq(f->lines[FTE3600_IRQ]);
	} else {
		/* SPI core already resolved the ordinary ACPI IRQ. Its number is
		 * neither a GPIO offset nor a raw ACPI GSI to remap ourselves.
		 */
		f->irq = to_spi_device(spi)->irq;
	}
	if (f->irq <= 0) {
		ret = f->irq < 0 ? f->irq : -ENXIO;
		goto gpio;
	}

	f->chip.label = dev_name(dev);
	f->chip.parent = dev;
	f->chip.owner = THIS_MODULE;
	f->chip.base = -1;
	f->chip.ngpio = FTE3600_NGPIO;
	f->chip.names = fte3600_line_names;
	f->chip.can_sleep = true;
	f->chip.request = fte3600_gpio_request;
	f->chip.free = fte3600_gpio_free;
	f->chip.get_direction = fte3600_gpio_get_direction;
	f->chip.direction_input = fte3600_gpio_direction_input;
	f->chip.direction_output = fte3600_gpio_direction_output;
	f->chip.get = fte3600_gpio_get;
	f->chip.set = fte3600_gpio_set;
	f->uio.name = "fte3600-irq";
	f->uio.version = "2";
	f->uio.irq = UIO_IRQ_CUSTOM;
	f->uio.open = fte3600_uio_open;
	f->uio.release = fte3600_uio_release;
	f->uio.priv = f;
	f->pm.notifier_call = fte3600_pm_notify;
	platform_set_drvdata(pdev, f);
	ret = register_pm_notifier(&f->pm);
	if (ret)
		goto gpio;
	ret = gpiochip_add_data(&f->chip, f);
	if (ret)
		goto notifier;
	ret = uio_register_device(dev, &f->uio);
	if (ret)
		goto chip;
	mutex_lock(&f->lock);
	f->uio_registered = true;
	mutex_unlock(&f->lock);
	ret = sysfs_create_group(&dev->kobj, &fte3600_attr_group);
	if (ret)
		goto uio;
	mutex_lock(&f->lock);
	f->lease.online = true;
	mutex_unlock(&f->lock);
	/* GPIO/UIO ADD may precede metadata publication. Retry this exact set
	 * after publication, without touching unrelated devices.
	 */
	kobject_uevent(&dev->kobj, KOBJ_CHANGE);
	device_for_each_child(dev, NULL, fte3600_change_uevent);
	device_for_each_child(spi, NULL, fte3600_change_uevent);
	dev_info(dev, "ACPI reset and %s IRQ ready; SPI remains with spidev\n",
		 r->irq_is_gpio ? "GPIO" : "ordinary");
	return 0;

uio:
	/* No open can succeed before lease.online was published. */
	mutex_lock(&f->lock);
	f->uio_registered = false;
	mutex_unlock(&f->lock);
	uio_unregister_device(&f->uio);
chip:
	gpiochip_remove(&f->chip);
notifier:
	unregister_pm_notifier(&f->pm);
gpio:
	dev_err_probe(dev, ret, "Could not create ACPI reset/IRQ interfaces\n");
free:
	platform_set_drvdata(pdev, NULL);
	fte3600_free(f);
	return ret;
}

static void fte3600_remove(struct platform_device *pdev)
{
	struct fte3600 *f = platform_get_drvdata(pdev);
	int ret = 0;

	/* Stop new userspace pairing before removing callbacks or descriptors. */
	sysfs_remove_group(&pdev->dev.kobj, &fte3600_attr_group);
	unregister_pm_notifier(&f->pm);
	mutex_lock(&f->lock);
	f->lease.online = false;
	fte3600_stop_irq(f);
	fte3600_lease_free(&f->lease, FTE3600_IRQ);
	f->uio_registered = false;
	if (!f->lease.suspended)
		ret = fte3600_deassert(f);
	mutex_unlock(&f->lock);
	/* UIO holds info_lock while calling open/release, which take f->lock.
	 * Unregister outside f->lock, before dropping the registration reference.
	 * It drains those callbacks and makes old UIO fds return EIO/HUP. Old fd
	 * release will NOT call our callback after unregister, so UIO opens must
	 * not own private krefs requiring that callback to relinquish them.
	 */
	uio_unregister_device(&f->uio);
	if (ret)
		dev_warn(f->dev, "Could not deassert reset on removal: %d\n", ret);
	platform_set_drvdata(pdev, NULL);
	/* Keep the chip and underlying descriptors alive until GPIO cdev has
	 * released the outstanding reset request. Old requests fail lease checks;
	 * userspace observes missing metadata and must close its line fds.
	 */
	kref_put(&f->ref, fte3600_last_reference);
}

static struct platform_driver fte3600_glue_driver = {
	.probe = fte3600_probe,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
	.remove = fte3600_remove,
#else
	.remove_new = fte3600_remove,
#endif
	.driver = {
		.name = FTE3600_GLUE_NAME,
	},
};

static bool fte3600_is_spidev(struct device *dev)
{
	return dev->driver && !strcmp(dev->driver->name, "spidev") &&
	       ACPI_COMPANION(dev) &&
	       !acpi_match_device_ids(ACPI_COMPANION(dev), fte3600_acpi_ids);
}

/* SPI driver core holds the physical device lock during BOUND/UNBIND. The
 * initial scan takes it explicitly. Never bind another driver to that device,
 * mutate its drvdata/mode, or create a second SPI device on its chip select.
 */
static int fte3600_attach(struct device *dev)
{
	struct fte3600_binding *binding, *iter;
	int ret = 0;

	if (!fte3600_is_spidev(dev))
		return 0;
	mutex_lock(&fte3600_bindings_lock);
	list_for_each_entry(iter, &fte3600_bindings, node)
		if (iter->spi == dev)
			goto unlock;
	binding = kzalloc(sizeof(*binding), GFP_KERNEL);
	if (!binding) {
		ret = -ENOMEM;
		goto unlock;
	}
	binding->pdev = platform_device_alloc(FTE3600_GLUE_NAME, PLATFORM_DEVID_AUTO);
	if (!binding->pdev) {
		ret = -ENOMEM;
		goto free;
	}
	binding->spi = dev;
	binding->pdev->dev.parent = dev;
	ret = platform_device_add(binding->pdev);
	if (ret) {
		platform_device_put(binding->pdev);
		goto free;
	}
	list_add_tail(&binding->node, &fte3600_bindings);
	goto unlock;
free:
	kfree(binding);
unlock:
	mutex_unlock(&fte3600_bindings_lock);
	return ret;
}

static void fte3600_detach(struct device *dev)
{
	struct fte3600_binding *binding, *next;

	mutex_lock(&fte3600_bindings_lock);
	list_for_each_entry_safe(binding, next, &fte3600_bindings, node) {
		if (dev && binding->spi != dev)
			continue;
		list_del(&binding->node);
		platform_device_unregister(binding->pdev);
		kfree(binding);
	}
	mutex_unlock(&fte3600_bindings_lock);
}

static int fte3600_spi_notify(struct notifier_block *nb, unsigned long event,
			      void *data)
{
	struct device *dev = data;
	int ret;

	switch (event) {
	case BUS_NOTIFY_BOUND_DRIVER:
		ret = fte3600_attach(dev);
		if (ret)
			dev_warn(dev, "Could not create FTE3600 glue device: %d\n", ret);
		break;
	case BUS_NOTIFY_UNBIND_DRIVER:
	case BUS_NOTIFY_DEL_DEVICE:
		fte3600_detach(dev);
		break;
	default:
		break;
	}
	/* Observing binding never changes another driver's probe outcome. */
	return NOTIFY_OK;
}

static struct notifier_block fte3600_spi_notifier = {
	.notifier_call = fte3600_spi_notify,
};

static int fte3600_scan(struct device *dev, void *unused)
{
	int ret;

	device_lock(dev);
	ret = fte3600_attach(dev);
	device_unlock(dev);
	if (ret)
		dev_warn(dev, "Could not create FTE3600 glue device: %d\n", ret);
	/* Once a subset has been published, module init must not fail while a
	 * concurrent GPIO client could already hold one of its requests.
	 */
	return 0;
}

static int __init fte3600_init(void)
{
	int ret;

	if (!IS_ENABLED(CONFIG_ACPI) || !IS_ENABLED(CONFIG_GPIO_CDEV) ||
	    !IS_ENABLED(CONFIG_UIO))
		return -ENODEV;
	fte3600_cleanup_queue = alloc_workqueue("fte3600-cleanup", WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	if (!fte3600_cleanup_queue)
		return -ENOMEM;
	ret = platform_driver_register(&fte3600_glue_driver);
	if (ret)
		goto queue;
	ret = bus_register_notifier(&spi_bus_type, &fte3600_spi_notifier);
	if (ret)
		goto driver;
	ret = bus_for_each_dev(&spi_bus_type, NULL, NULL, fte3600_scan);
	if (!ret)
		return 0;
	bus_unregister_notifier(&spi_bus_type, &fte3600_spi_notifier);
	fte3600_detach(NULL);
driver:
	platform_driver_unregister(&fte3600_glue_driver);
queue:
	destroy_workqueue(fte3600_cleanup_queue);
	return ret;
}
module_init(fte3600_init);

static void __exit fte3600_exit(void)
{
	bus_unregister_notifier(&spi_bus_type, &fte3600_spi_notifier);
	fte3600_detach(NULL);
	platform_driver_unregister(&fte3600_glue_driver);
	destroy_workqueue(fte3600_cleanup_queue);
}
module_exit(fte3600_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FTE3600 ACPI reset and IRQ glue for standard spidev");
