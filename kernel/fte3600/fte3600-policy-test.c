// SPDX-License-Identifier: GPL-2.0-only
/* Execute the same pure resource and lease policy used by GPIO/UIO callbacks.
 * These tests do not emulate IRQ delivery, driver-core removal, or suspend.
 */
#include <assert.h>
#include <stdio.h>

#include "fte3600-policy.h"

static void test_resource_order(void)
{
	const enum fte3600_resource_kind orders[][3] = {
		{ FTE3600_RESOURCE_SPI, FTE3600_RESOURCE_RESET, FTE3600_RESOURCE_IRQ },
		{ FTE3600_RESOURCE_SPI, FTE3600_RESOURCE_IRQ, FTE3600_RESOURCE_RESET },
		{ FTE3600_RESOURCE_RESET, FTE3600_RESOURCE_SPI, FTE3600_RESOURCE_IRQ },
		{ FTE3600_RESOURCE_RESET, FTE3600_RESOURCE_IRQ, FTE3600_RESOURCE_SPI },
		{ FTE3600_RESOURCE_IRQ, FTE3600_RESOURCE_SPI, FTE3600_RESOURCE_RESET },
		{ FTE3600_RESOURCE_IRQ, FTE3600_RESOURCE_RESET, FTE3600_RESOURCE_SPI },
	};
	unsigned int i, j;

	for (i = 0; i < sizeof(orders) / sizeof(orders[0]); i++) {
		struct fte3600_resources r = { 0 };

		for (j = 0; j < 3; j++) {
			fte3600_resource_add(&r, FTE3600_RESOURCE_OTHER, true);
			fte3600_resource_add(&r, orders[i][j], true);
		}
		assert(fte3600_resources_valid(&r));
		assert(r.gpio_index == 2);
		assert(r.reset_index + r.irq_index == 1);
		assert(r.reset_index != r.irq_index);
		assert(r.reset_index == (i == 1 || i >= 4));
		assert(r.irq_is_gpio);
	}
}

static void test_ordinary_irq_order(void)
{
	const enum fte3600_resource_kind orders[][3] = {
		{ FTE3600_RESOURCE_SPI, FTE3600_RESOURCE_RESET, FTE3600_RESOURCE_IRQ_ACPI },
		{ FTE3600_RESOURCE_SPI, FTE3600_RESOURCE_IRQ_ACPI, FTE3600_RESOURCE_RESET },
		{ FTE3600_RESOURCE_RESET, FTE3600_RESOURCE_SPI, FTE3600_RESOURCE_IRQ_ACPI },
		{ FTE3600_RESOURCE_RESET, FTE3600_RESOURCE_IRQ_ACPI, FTE3600_RESOURCE_SPI },
		{ FTE3600_RESOURCE_IRQ_ACPI, FTE3600_RESOURCE_SPI, FTE3600_RESOURCE_RESET },
		{ FTE3600_RESOURCE_IRQ_ACPI, FTE3600_RESOURCE_RESET, FTE3600_RESOURCE_SPI },
	};
	unsigned int i, j;

	for (i = 0; i < sizeof(orders) / sizeof(orders[0]); i++) {
		struct fte3600_resources r = { 0 };
		struct fte3600_resources invalid = { 0 };

		for (j = 0; j < 3; j++) {
			fte3600_resource_add(&r, orders[i][j], true);
			fte3600_resource_add(&invalid, orders[i][j],
				orders[i][j] != FTE3600_RESOURCE_IRQ_ACPI);
		}
		assert(fte3600_resources_valid(&r));
		assert(!fte3600_resources_valid(&invalid));
		/* Normal IRQs never shift reset's ACPI GPIO resource index. */
		assert(r.gpio_index == 1 && r.reset_index == 0);
		assert(!r.irq_is_gpio);
		fte3600_resource_add(&r, FTE3600_RESOURCE_IRQ, true);
		assert(!fte3600_resources_valid(&r));
	}
	{
		struct fte3600_resources r = { 0 };

		fte3600_resource_add(&r, FTE3600_RESOURCE_SPI, true);
		fte3600_resource_add(&r, FTE3600_RESOURCE_RESET, true);
		fte3600_resource_add(&r, FTE3600_RESOURCE_IRQ, true);
		fte3600_resource_add(&r, FTE3600_RESOURCE_IRQ_ACPI, true);
		assert(!fte3600_resources_valid(&r));
		/* Replacing GpioInt with two ordinary descriptors is ambiguous too. */
		r = (struct fte3600_resources) { 0 };
		fte3600_resource_add(&r, FTE3600_RESOURCE_SPI, true);
		fte3600_resource_add(&r, FTE3600_RESOURCE_RESET, true);
		fte3600_resource_add(&r, FTE3600_RESOURCE_IRQ_ACPI, true);
		fte3600_resource_add(&r, FTE3600_RESOURCE_IRQ_ACPI, true);
		assert(!fte3600_resources_valid(&r));
	}
}

static void test_resource_rejection(void)
{
	const enum fte3600_resource_kind required[] = {
		FTE3600_RESOURCE_SPI, FTE3600_RESOURCE_RESET, FTE3600_RESOURCE_IRQ,
	};
	unsigned int i, j;

	for (i = 0; i < 3; i++) {
		struct fte3600_resources missing = { 0 };
		struct fte3600_resources duplicate = { 0 };
		struct fte3600_resources invalid = { 0 };

		for (j = 0; j < 3; j++) {
			if (i != j)
				fte3600_resource_add(&missing, required[j], true);
			fte3600_resource_add(&duplicate, required[j], true);
			fte3600_resource_add(&invalid, required[j], i != j);
		}
		fte3600_resource_add(&duplicate, required[i], true);
		assert(!fte3600_resources_valid(&missing));
		assert(!fte3600_resources_valid(&duplicate));
		assert(!fte3600_resources_valid(&invalid));
	}
	{
		struct fte3600_resources r = { 0 };

		for (j = 0; j < 3; j++)
			fte3600_resource_add(&r, required[j], true);
		fte3600_resource_add(&r, FTE3600_RESOURCE_INVALID_GPIO, true);
		assert(!fte3600_resources_valid(&r));
	}
}

static void test_polarity_and_reference(void)
{
	unsigned int index, low;

	assert(fte3600_reset_bias_valid(FTE3600_RESET_BIAS_DEFAULT));
	assert(fte3600_reset_bias_valid(FTE3600_RESET_BIAS_PULL_UP));
	assert(fte3600_reset_bias_valid(FTE3600_RESET_BIAS_DISABLED));
	assert(!fte3600_reset_bias_valid(FTE3600_RESET_BIAS_PULL_DOWN));
	assert(!fte3600_reset_bias_valid(FTE3600_RESET_BIAS_UNKNOWN));
	for (index = 0; index < 2; index++) {
		for (low = 0; low < 2; low++) {
			assert(fte3600_gpio_reference_valid(true, 3, index, 0, low,
							  index, low));
			assert(!fte3600_gpio_reference_valid(false, 3, index, 0, low,
							   index, low));
			assert(!fte3600_gpio_reference_valid(true, 2, index, 0, low,
							   index, low));
			assert(!fte3600_gpio_reference_valid(true, 3, index ^ 1, 0, low,
							   index, low));
			assert(!fte3600_gpio_reference_valid(true, 3, index, 1, low,
							   index, low));
			assert(!fte3600_gpio_reference_valid(true, 3, index, 0, low ^ 1,
							   index, low));
			assert(!fte3600_gpio_reference_valid(true, 3, index, 0, 2,
							   index, low));
		}
	}
}

static void test_exclusive_session(void)
{
	struct fte3600_lease s = { .online = true };

	assert(fte3600_lease_request(&s, FTE3600_IRQ) == -EHOSTDOWN);
	assert(!s.requested);
	assert(fte3600_lease_request(&s, FTE3600_RESET) == 0);
	assert(fte3600_lease_check(&s, FTE3600_RESET) == 0);
	assert(fte3600_lease_request(&s, FTE3600_RESET) == -EBUSY);
	assert(fte3600_lease_request(&s, FTE3600_IRQ) == 0);
	assert(fte3600_lease_check(&s, FTE3600_IRQ) == 0);
	assert(fte3600_lease_request(&s, FTE3600_IRQ) == -EBUSY);
	/* Process fd-close ordering is unspecified: reset can close first. */
	fte3600_lease_free(&s, FTE3600_RESET);
	assert(s.generation == 1);
	assert(fte3600_lease_check(&s, FTE3600_IRQ) == -EHOSTDOWN);
	assert(fte3600_lease_request(&s, FTE3600_RESET) == -EBUSY);
	fte3600_lease_free(&s, FTE3600_IRQ);
	assert(s.requested == 0);
	assert(fte3600_lease_request(&s, FTE3600_RESET) == 0);
	assert(fte3600_lease_request(&s, FTE3600_IRQ) == 0);
	/* Normal shutdown closes IRQ first, then reset. */
	fte3600_lease_free(&s, FTE3600_IRQ);
	assert(fte3600_lease_check(&s, FTE3600_RESET) == 0);
	fte3600_lease_free(&s, FTE3600_RESET);
	assert(s.requested == 0 && s.generation == 1);
}

static void test_irq_setup_rollback(void)
{
	struct fte3600_lease s = { .online = true, .generation = 18 };

	assert(fte3600_lease_request(&s, FTE3600_RESET) == 0);
	assert(fte3600_lease_request(&s, FTE3600_IRQ) == 0);
	/* request_irq failure rolls back only the IRQ claim; reset remains
	 * usable, and another open can retry without changing the generation.
	 */
	fte3600_lease_free(&s, FTE3600_IRQ);
	assert(fte3600_lease_check(&s, FTE3600_RESET) == 0);
	assert(s.generation == 18);
	assert(fte3600_lease_request(&s, FTE3600_IRQ) == 0);
	assert(fte3600_lease_request(&s, FTE3600_IRQ) == -EBUSY);
	s.online = false;
	fte3600_lease_free(&s, FTE3600_IRQ);
	assert(fte3600_lease_check(&s, FTE3600_RESET) == -ENODEV);
	assert(fte3600_lease_request(&s, FTE3600_IRQ) == -ENODEV);
	fte3600_lease_free(&s, FTE3600_RESET);
	assert(!s.requested && s.generation == 18);
}

static void test_power_generation(void)
{
	struct fte3600_lease s = { .online = true, .generation = 73 };

	assert(fte3600_lease_request(&s, FTE3600_RESET) == 0);
	assert(fte3600_lease_request(&s, FTE3600_IRQ) == 0);
	fte3600_lease_suspend(&s);
	assert(s.generation == 74 && s.suspended);
	assert(fte3600_lease_check(&s, FTE3600_RESET) == -EHOSTDOWN);
	assert(fte3600_lease_check(&s, FTE3600_IRQ) == -EHOSTDOWN);
	assert(fte3600_lease_request(&s, FTE3600_RESET) == -EHOSTDOWN);
	/* Duplicate prepare does not issue a second epoch for one transition. */
	fte3600_lease_suspend(&s);
	assert(s.generation == 74);
	s.suspended = false;
	/* Resuming suppliers never makes an old line request valid again. */
	assert(fte3600_lease_check(&s, FTE3600_RESET) == -EHOSTDOWN);
	assert(fte3600_lease_check(&s, FTE3600_IRQ) == -EHOSTDOWN);
	fte3600_lease_free(&s, FTE3600_IRQ);
	assert(fte3600_lease_request(&s, FTE3600_IRQ) == -EHOSTDOWN);
	fte3600_lease_free(&s, FTE3600_RESET);
	assert(fte3600_lease_request(&s, FTE3600_RESET) == 0);
	assert(fte3600_lease_request(&s, FTE3600_IRQ) == 0);
	assert(fte3600_lease_check(&s, FTE3600_RESET) == 0);
	assert(fte3600_lease_check(&s, FTE3600_IRQ) == 0);
	fte3600_lease_suspend(&s);
	/* Closing while suspended still relinquishes both claims. */
	fte3600_lease_free(&s, FTE3600_RESET);
	fte3600_lease_free(&s, FTE3600_IRQ);
	assert(s.requested == 0 && s.generation == 75);
	s.suspended = false;
	assert(fte3600_lease_request(&s, FTE3600_RESET) == 0);
}

static void test_removed_and_invalid_lines(void)
{
	struct fte3600_lease s = { .online = true };

	assert(fte3600_lease_request(&s, 2) == -EINVAL);
	assert(fte3600_lease_check(&s, 32) == -EINVAL);
	fte3600_lease_free(&s, 32);
	assert(!s.requested);
	assert(fte3600_lease_request(&s, FTE3600_RESET) == 0);
	s.online = false;
	assert(fte3600_lease_check(&s, FTE3600_RESET) == -ENODEV);
	assert(fte3600_lease_request(&s, FTE3600_IRQ) == -ENODEV);
	fte3600_lease_free(&s, FTE3600_RESET);
	assert(!s.requested);
	assert(fte3600_lease_request(&s, FTE3600_RESET) == -ENODEV);
}

static void test_cs_control(void)
{
	assert(fte3600_cs_control_supported(false, true));
	assert(!fte3600_cs_control_supported(false, false));
	assert(!fte3600_cs_control_supported(true, true));
	assert(!fte3600_cs_control_supported(true, false));
}

int main(void)
{
	test_cs_control();
	test_resource_order();
	test_ordinary_irq_order();
	test_resource_rejection();
	test_polarity_and_reference();
	test_exclusive_session();
	test_irq_setup_rollback();
	test_power_generation();
	test_removed_and_invalid_lines();
	puts("FTE3600 glue policy: 9 groups passed (GPIO/ACPI IRQ, CS capability, polarity, lease, PM, removal)");
	return 0;
}
