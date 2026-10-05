/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FTE3600_POLICY_H
#define FTE3600_POLICY_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
#else
#include <errno.h>
#include <stdbool.h>
#endif
#include <linux/spi/spidev.h>

/* Only resource roles are retained. Controller names and pin numbers remain
 * in ACPI and are resolved by gpiolib, never by this policy.
 */
enum fte3600_resource_kind {
	FTE3600_RESOURCE_OTHER,
	FTE3600_RESOURCE_SPI,
	FTE3600_RESOURCE_RESET,
	FTE3600_RESOURCE_IRQ,
	FTE3600_RESOURCE_INVALID_GPIO,
};

enum fte3600_reset_bias {
	FTE3600_RESET_BIAS_DEFAULT,
	FTE3600_RESET_BIAS_PULL_UP,
	FTE3600_RESET_BIAS_DISABLED,
	FTE3600_RESET_BIAS_PULL_DOWN,
	FTE3600_RESET_BIAS_UNKNOWN,
};

/* ABI values are logical assertions, not the Windows raw pin values.
 * An active-low descriptor maps DEASSERTED to physical H, ASSERTED to L.
 */
enum fte3600_reset_state {
	FTE3600_RESET_DEASSERTED = 0,
	FTE3600_RESET_ASSERTED = 1,
};

/* The caller serializes access and supplies setup only while the controller
 * is available. A mode value alone cannot prove hardware state after an error.
 */
struct fte3600_cs_state {
	unsigned int original_cs;
	bool configuration_invalid;
};

/* Start a session only after any previous restoration has succeeded. */
static inline void
fte3600_cs_begin(struct fte3600_cs_state *cs, unsigned int mode)
{
	cs->original_cs = mode & SPI_CS_HIGH;
}

static inline int
fte3600_cs_set(struct fte3600_cs_state *cs, unsigned int *mode, bool high,
	       int (*setup)(void *), void *context)
{
	unsigned int old_mode = *mode;
	int ret;

	if (cs->configuration_invalid)
		return -EHOSTDOWN;
	*mode = (old_mode & ~SPI_CS_HIGH) | (high ? SPI_CS_HIGH : 0);
	ret = setup(context);
	if (ret) {
		*mode = old_mode;
		/* A failed rollback must block further sensor transactions. */
		cs->configuration_invalid = setup(context) != 0;
	}
	return ret;
}

static inline int
fte3600_cs_restore(struct fte3600_cs_state *cs, unsigned int *mode,
		   int (*setup)(void *), void *context)
{
	int ret;

	if ((*mode & SPI_CS_HIGH) == cs->original_cs &&
	    !cs->configuration_invalid)
		return 0;
	*mode = (*mode & ~SPI_CS_HIGH) | cs->original_cs;
	ret = setup(context);
	/* Keep the saved target on failure. Even with matching software bits,
	 * the next restoration must program the controller again.
	 */
	cs->configuration_invalid = ret != 0;
	return ret;
}

struct fte3600_resources {
	unsigned int gpio_index;
	unsigned int reset_index;
	unsigned int resets;
	unsigned int interrupts;
	unsigned int spi;
	enum fte3600_reset_bias reset_bias;
	bool invalid;
};

static inline bool
fte3600_reset_bias_valid(enum fte3600_reset_bias bias)
{
	return bias == FTE3600_RESET_BIAS_DEFAULT ||
	       bias == FTE3600_RESET_BIAS_PULL_UP ||
	       bias == FTE3600_RESET_BIAS_DISABLED;
}

static inline bool
fte3600_reset_reference_valid(bool same_device, unsigned int nargs,
			      unsigned long long resource,
			      unsigned long long pin,
			      unsigned long long active_low,
			      unsigned int reset_index)
{
	return same_device && nargs == 3 && resource == reset_index &&
	       pin == 0 && active_low == 1;
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
		r->interrupts++;
		break;
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

static inline int
fte3600_check_transfer(const struct spi_ioc_transfer *xfer,
		       unsigned int limit)
{
	if (!xfer->len || xfer->len > limit)
		return -EMSGSIZE;
	if ((!xfer->tx_buf && !xfer->rx_buf) || xfer->speed_hz ||
	    xfer->bits_per_word || xfer->delay_usecs || xfer->cs_change ||
	    xfer->tx_nbits || xfer->rx_nbits || xfer->word_delay_usecs ||
	    xfer->pad)
		return -EINVAL;
	return 0;
}

static inline int
fte3600_transfer_result(int status, unsigned int actual,
			unsigned int requested)
{
	if (status)
		return status;
	/* A truncated command or image is not a successful transaction. */
	if (actual != requested)
		return -EIO;
	return requested;
}

#endif
