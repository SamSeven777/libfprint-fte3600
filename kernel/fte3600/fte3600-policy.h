/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FTE3600_POLICY_H
#define FTE3600_POLICY_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
#else
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t u64;
#endif

#define FTE3600_GLUE_ABI 2
#define FTE3600_NGPIO 1
#define FTE3600_LEASE_COUNT 2
#define FTE3600_RESET 0
#define FTE3600_IRQ 1

enum fte3600_resource_kind {
	FTE3600_RESOURCE_OTHER,
	FTE3600_RESOURCE_SPI,
	FTE3600_RESOURCE_RESET,
	FTE3600_RESOURCE_IRQ,
	FTE3600_RESOURCE_IRQ_ACPI,
	FTE3600_RESOURCE_INVALID_GPIO,
};

enum fte3600_reset_bias {
	FTE3600_RESET_BIAS_DEFAULT,
	FTE3600_RESET_BIAS_PULL_UP,
	FTE3600_RESET_BIAS_DISABLED,
	FTE3600_RESET_BIAS_PULL_DOWN,
	FTE3600_RESET_BIAS_UNKNOWN,
};

/* Only resource roles are retained; controller names and pin numbers are
 * resolved by ACPI/gpiolib, never by a board or pin-number table.
 */
struct fte3600_resources {
	unsigned int gpio_index;
	unsigned int reset_index;
	unsigned int irq_index;
	unsigned int resets;
	unsigned int interrupts;
	unsigned int spi;
	unsigned int mode;
	unsigned int speed_hz;
	enum fte3600_reset_bias reset_bias;
	bool irq_active_low;
	bool irq_is_gpio;
	bool cs_control;
	bool invalid;
};

/* GPIO CS is controller-owned: stock spidev masks/forces its CS_HIGH bit. */
static inline bool
fte3600_cs_control_supported(bool gpio_cs, bool controller_cs_high)
{
	return !gpio_cs && controller_cs_high;
}

static inline bool
fte3600_reset_bias_valid(enum fte3600_reset_bias bias)
{
	return bias == FTE3600_RESET_BIAS_DEFAULT ||
	       bias == FTE3600_RESET_BIAS_PULL_UP ||
	       bias == FTE3600_RESET_BIAS_DISABLED;
}

static inline bool
fte3600_gpio_reference_valid(bool same_device, unsigned int nargs,
		unsigned long long resource, unsigned long long pin,
		unsigned long long active_low, unsigned int expected_resource,
		bool expected_active_low)
{
	return same_device && nargs == 3 && resource == expected_resource &&
	       pin == 0 && active_low == expected_active_low;
}

static inline void
fte3600_resource_add(struct fte3600_resources *r,
		    enum fte3600_resource_kind kind, bool valid)
{
	if (!valid)
		r->invalid = true;
	switch (kind) {
	case FTE3600_RESOURCE_OTHER:
		return;
	case FTE3600_RESOURCE_SPI:
		r->spi++;
		return;
	case FTE3600_RESOURCE_RESET:
		r->reset_index = r->gpio_index;
		r->resets++;
		break;
	case FTE3600_RESOURCE_IRQ:
		r->irq_index = r->gpio_index;
		r->interrupts++;
		r->irq_is_gpio = true;
		break;
	case FTE3600_RESOURCE_IRQ_ACPI:
		r->interrupts++;
		return; /* An Interrupt resource is not a GPIO lookup index. */
	case FTE3600_RESOURCE_INVALID_GPIO:
		r->invalid = true;
		break;
	}
	r->gpio_index++;
}

static inline bool
fte3600_resources_valid(const struct fte3600_resources *r)
{
	return !r->invalid && r->spi == 1 && r->resets == 1 &&
	       r->interrupts == 1;
}

/* The reset GPIO request owns a cooperative userspace session; the second
 * lease belongs to the UIO IRQ open, not to a second GPIO. Stock spidev does
 * not consult this state: clients must check generation before AND after I/O.
 * Every callback using the state is serialized by the driver's mutex.
 */
struct fte3600_lease {
	u64 generation;
	u64 acquired[FTE3600_LEASE_COUNT];
	unsigned int requested;
	bool suspended;
	bool online;
};

static inline int fte3600_lease_check(const struct fte3600_lease *s,
				    unsigned int line)
{
	if (line >= FTE3600_LEASE_COUNT)
		return -EINVAL;
	if (!s->online)
		return -ENODEV;
	if (s->suspended || !(s->requested & (1U << line)) ||
	    s->acquired[line] != s->generation)
		return -EHOSTDOWN;
	if (line == FTE3600_IRQ &&
	    (!(s->requested & (1U << FTE3600_RESET)) ||
	     s->acquired[FTE3600_RESET] != s->generation))
		return -EHOSTDOWN;
	return 0;
}

static inline int fte3600_lease_request(struct fte3600_lease *s,
				      unsigned int line)
{
	if (line >= FTE3600_LEASE_COUNT)
		return -EINVAL;
	if (!s->online)
		return -ENODEV;
	if (s->suspended)
		return -EHOSTDOWN;
	if ((s->requested & (1U << line)) ||
	    (line == FTE3600_RESET && s->requested))
		return -EBUSY;
	if (line == FTE3600_IRQ && fte3600_lease_check(s, FTE3600_RESET))
		return -EHOSTDOWN;
	s->requested |= 1U << line;
	s->acquired[line] = s->generation;
	return 0;
}

static inline void fte3600_lease_free(struct fte3600_lease *s,
				    unsigned int line)
{
	/* A surviving UIO fd must observe reset-first shutdown as session loss.
	 * Do not advance an already stale generation again during PM cleanup.
	 */
	if (line == FTE3600_RESET &&
	    (s->requested & (1U << FTE3600_RESET)) &&
	    (s->requested & (1U << FTE3600_IRQ)) &&
	    s->acquired[FTE3600_RESET] == s->generation)
		s->generation++;
	if (line < FTE3600_LEASE_COUNT)
		s->requested &= ~(1U << line);
}

static inline void fte3600_lease_suspend(struct fte3600_lease *s)
{
	if (!s->suspended) {
		s->suspended = true;
		s->generation++;
	}
}

#endif
