// SPDX-License-Identifier: GPL-2.0-only
/* Host tests for the bridge's pure resource and transaction policy.
 * These do not emulate gpiolib, interrupt delivery or kernel power management.
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "fte3600-policy.h"
#include "fte3600-bridge.h"

/* Adding capabilities consumes a previously zeroed reserved slot without
 * changing ABI 1's ioctl number or the 32/64-bit wire layout. */
_Static_assert(sizeof(struct fte3600_bridge_info) == 32, "ABI 1 info size");
_Static_assert(offsetof(struct fte3600_bridge_info, capabilities) == 20, "ABI 1 capability offset");
_Static_assert(_IOC_SIZE(FTE3600_IOC_GET_INFO) == 32, "ABI 1 ioctl size");

static void test_resource_order(void)
{
	const struct {
		enum fte3600_resource_kind order[3];
		unsigned int reset_index;
	} cases[] = {
		{ { FTE3600_RESOURCE_SPI, FTE3600_RESOURCE_RESET,
		    FTE3600_RESOURCE_IRQ }, 0 },
		{ { FTE3600_RESOURCE_SPI, FTE3600_RESOURCE_IRQ,
		    FTE3600_RESOURCE_RESET }, 1 },
		{ { FTE3600_RESOURCE_RESET, FTE3600_RESOURCE_SPI,
		    FTE3600_RESOURCE_IRQ }, 0 },
		{ { FTE3600_RESOURCE_RESET, FTE3600_RESOURCE_IRQ,
		    FTE3600_RESOURCE_SPI }, 0 },
		{ { FTE3600_RESOURCE_IRQ, FTE3600_RESOURCE_SPI,
		    FTE3600_RESOURCE_RESET }, 1 },
		{ { FTE3600_RESOURCE_IRQ, FTE3600_RESOURCE_RESET,
		    FTE3600_RESOURCE_SPI }, 1 },
	};
	unsigned int i, j;

	/* Firmware may place SPI, reset and interrupt in any resource order. */
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct fte3600_resources r = { 0 };

		for (j = 0; j < 3; j++) {
			fte3600_resource_add(&r, FTE3600_RESOURCE_OTHER, true);
			fte3600_resource_add(&r, cases[i].order[j], true);
		}
		assert(fte3600_resources_valid(&r));
		assert(r.reset_index == cases[i].reset_index);
		assert(r.gpio_index == 2);
	}
}

static void test_resource_ambiguity(void)
{
	const enum fte3600_resource_kind required[] = {
		FTE3600_RESOURCE_SPI, FTE3600_RESOURCE_RESET,
		FTE3600_RESOURCE_IRQ,
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

		fte3600_resource_add(&r, FTE3600_RESOURCE_INVALID_GPIO, true);
		for (i = 0; i < 3; i++)
			fte3600_resource_add(&r, required[i], true);
		assert(!fte3600_resources_valid(&r));
		assert(r.reset_index == 1);
	}
}

static void test_transfer_bounds(void)
{
	struct spi_ioc_transfer xfer = { .tx_buf = 1, .len = 32768 };

	assert(fte3600_check_transfer(&xfer, 32768) == 0);
	xfer.rx_buf = 2;
	assert(fte3600_check_transfer(&xfer, 32768) == 0);
	xfer.tx_buf = 0;
	assert(fte3600_check_transfer(&xfer, 32768) == 0);
	xfer.rx_buf = 0;
	assert(fte3600_check_transfer(&xfer, 32768) == -EINVAL);
	xfer.tx_buf = 1;
	xfer.len = 0;
	assert(fte3600_check_transfer(&xfer, 32768) == -EMSGSIZE);
	xfer.len = 32769;
	assert(fte3600_check_transfer(&xfer, 32768) == -EMSGSIZE);
	xfer.len = 1;
	assert(fte3600_check_transfer(&xfer, 0) == -EMSGSIZE);
	xfer.len = 10403;
	assert(fte3600_check_transfer(&xfer, 8192) == -EMSGSIZE);
	assert(fte3600_check_transfer(&xfer, 32768) == 0);

#define REJECT_OVERRIDE(member) do { \
	xfer.member = 1; \
	assert(fte3600_check_transfer(&xfer, 32768) == -EINVAL); \
	xfer.member = 0; \
} while (0)
	REJECT_OVERRIDE(speed_hz);
	REJECT_OVERRIDE(bits_per_word);
	REJECT_OVERRIDE(delay_usecs);
	REJECT_OVERRIDE(cs_change);
	REJECT_OVERRIDE(tx_nbits);
	REJECT_OVERRIDE(rx_nbits);
	REJECT_OVERRIDE(word_delay_usecs);
	REJECT_OVERRIDE(pad);
#undef REJECT_OVERRIDE
}

static void test_transfer_completion(void)
{
	assert(fte3600_transfer_result(0, 5128, 5128) == 5128);
	assert(fte3600_transfer_result(-ETIMEDOUT, 0, 5128) == -ETIMEDOUT);
	assert(fte3600_transfer_result(-EIO, 5128, 5128) == -EIO);
	assert(fte3600_transfer_result(0, 0, 5128) == -EIO);
	assert(fte3600_transfer_result(0, 5127, 5128) == -EIO);
	assert(fte3600_transfer_result(0, 5129, 5128) == -EIO);
}

static void test_reset_metadata(void)
{
	/* The bridge contract is logical deassert/assert. On the required
	 * active-low descriptor these correspond to physical high/low.
	 */
	assert(FTE3600_RESET_DEASSERTED == 0);
	assert(FTE3600_RESET_ASSERTED == 1);
	assert(fte3600_reset_bias_valid(FTE3600_RESET_BIAS_DEFAULT));
	assert(fte3600_reset_bias_valid(FTE3600_RESET_BIAS_PULL_UP));
	assert(fte3600_reset_bias_valid(FTE3600_RESET_BIAS_DISABLED));
	assert(!fte3600_reset_bias_valid(FTE3600_RESET_BIAS_PULL_DOWN));
	assert(!fte3600_reset_bias_valid(FTE3600_RESET_BIAS_UNKNOWN));
	assert(fte3600_reset_reference_valid(true, 3, 1, 0, 1, 1));
	assert(fte3600_reset_reference_valid(true, 3, 0, 0, 1, 0));
	assert(!fte3600_reset_reference_valid(true, 3, 1, 0, 0, 1));
	assert(!fte3600_reset_reference_valid(true, 3, 1, 0, 2, 1));
	assert(!fte3600_reset_reference_valid(false, 3, 1, 0, 1, 1));
	assert(!fte3600_reset_reference_valid(true, 2, 1, 0, 1, 1));
	assert(!fte3600_reset_reference_valid(true, 4, 1, 0, 1, 1));
	assert(!fte3600_reset_reference_valid(true, 3, 0, 0, 1, 1));
	assert(!fte3600_reset_reference_valid(true, 3, 1, 1, 1, 1));
	assert(!fte3600_reset_reference_valid(true, 3, 0x100000001ULL, 0, 1, 1));
}

struct setup_call {
	unsigned int mode;
	int result;
};

struct setup_mock {
	unsigned int *mode;
	const struct setup_call *calls;
	unsigned int count;
	unsigned int next;
	unsigned int applied_mode;
};

/* The callback observes the mode presented to spi_setup(), not a separately
 * computed expected state. Failed setup leaves controller state unspecified;
 * applied_mode is consulted only after a successful setup or rollback.
 */
static int mock_setup(void *context)
{
	struct setup_mock *mock = context;
	const struct setup_call *call;

	assert(mock->next < mock->count);
	call = &mock->calls[mock->next++];
	assert(*mock->mode == call->mode);
	if (!call->result)
		mock->applied_mode = *mock->mode;
	return call->result;
}

static void test_cs_restore_unchanged(void)
{
	unsigned int clock, high;

	for (clock = 0; clock < 4; clock++) {
		for (high = 0; high < 2; high++) {
			unsigned int mode = clock | SPI_LSB_FIRST |
				(high ? SPI_CS_HIGH : 0);
			struct fte3600_cs_state state = { 0 };
			struct setup_mock mock = { .mode = &mode };

			fte3600_cs_begin(&state, mode);
			assert(state.original_cs == (high ? SPI_CS_HIGH : 0));
			assert(!state.configuration_invalid);
			assert(fte3600_cs_restore(&state, &mode, mock_setup,
						  &mock) == 0);
			assert(mode == (clock | SPI_LSB_FIRST |
					(high ? SPI_CS_HIGH : 0)));
			assert(mock.next == 0);
		}
	}
}

static void test_cs_repeated_toggles(void)
{
	unsigned int clock, high;

	for (clock = 0; clock < 4; clock++) {
		for (high = 0; high < 2; high++) {
			const unsigned int original = clock | SPI_LSB_FIRST |
				(high ? SPI_CS_HIGH : 0);
			const unsigned int trial = original ^ SPI_CS_HIGH;
			const struct setup_call calls[] = {
				{ trial, 0 }, { original, 0 },
				{ trial, 0 }, { original, 0 },
			};
			unsigned int mode = original;
			struct fte3600_cs_state state = { 0 };
			struct setup_mock mock = {
				.mode = &mode, .calls = calls, .count = 4,
				.applied_mode = original,
			};

			fte3600_cs_begin(&state, mode);
			assert(fte3600_cs_set(&state, &mode, !high,
					     mock_setup, &mock) == 0);
			assert(mode == trial && mock.applied_mode == trial);
			assert(state.original_cs == (original & SPI_CS_HIGH));
			assert(fte3600_cs_set(&state, &mode, high,
					     mock_setup, &mock) == 0);
			assert(mode == original && mock.applied_mode == original);
			assert(fte3600_cs_restore(&state, &mode, mock_setup,
						  &mock) == 0);
			assert(mock.next == 2);
			assert(fte3600_cs_set(&state, &mode, !high,
					     mock_setup, &mock) == 0);
			assert(fte3600_cs_restore(&state, &mode, mock_setup,
						  &mock) == 0);
			assert(mode == original && mock.applied_mode == original);
			assert(state.original_cs == (original & SPI_CS_HIGH));
			assert(!state.configuration_invalid);
			assert(fte3600_cs_restore(&state, &mode, mock_setup,
						  &mock) == 0);
			assert(mock.next == mock.count);
		}
	}
}

static void test_cs_change_failure_rollback(void)
{
	unsigned int clock, high;

	for (clock = 0; clock < 4; clock++) {
		for (high = 0; high < 2; high++) {
			const unsigned int original = clock | SPI_LSB_FIRST |
				(high ? SPI_CS_HIGH : 0);
			const struct setup_call calls[] = {
				{ original ^ SPI_CS_HIGH, -ETIMEDOUT },
				{ original, 0 },
			};
			unsigned int mode = original;
			struct fte3600_cs_state state = { 0 };
			struct setup_mock mock = {
				.mode = &mode, .calls = calls, .count = 2,
			};

			fte3600_cs_begin(&state, mode);
			assert(fte3600_cs_set(&state, &mode, !high,
					     mock_setup, &mock) == -ETIMEDOUT);
			assert(mode == original && mock.applied_mode == original);
			assert(state.original_cs == (original & SPI_CS_HIGH));
			assert(!state.configuration_invalid);
			assert(fte3600_cs_restore(&state, &mode, mock_setup,
						  &mock) == 0);
			assert(mock.next == mock.count);
		}
	}
}

static void test_cs_failed_rollback_recovery(void)
{
	unsigned int clock, high;

	for (clock = 0; clock < 4; clock++) {
		for (high = 0; high < 2; high++) {
			const unsigned int original = clock | SPI_LSB_FIRST |
				(high ? SPI_CS_HIGH : 0);
			const struct setup_call calls[] = {
				{ original ^ SPI_CS_HIGH, -EINVAL },
				{ original, -EIO },
				{ original, 0 },
			};
			unsigned int mode = original;
			struct fte3600_cs_state state = { 0 };
			struct setup_mock mock = {
				.mode = &mode, .calls = calls, .count = 3,
			};

			fte3600_cs_begin(&state, mode);
			/* Report the attempted change's error, not rollback's error. */
			assert(fte3600_cs_set(&state, &mode, !high,
					     mock_setup, &mock) == -EINVAL);
			assert(mode == original);
			assert(state.original_cs == (original & SPI_CS_HIGH));
			assert(state.configuration_invalid);
			assert(mock.next == 2);
			/* Neither another trial nor an apparent no-op may use an
			 * unverified controller configuration. */
			assert(fte3600_cs_set(&state, &mode, !high,
					     mock_setup, &mock) == -EHOSTDOWN);
			assert(fte3600_cs_set(&state, &mode, high,
					     mock_setup, &mock) == -EHOSTDOWN);
			assert(mock.next == 2);
			assert(state.original_cs == (original & SPI_CS_HIGH));
			/* The software bit already equals original, but rollback
			 * failed: restore must still call setup and prove recovery. */
			assert(fte3600_cs_restore(&state, &mode, mock_setup,
						  &mock) == 0);
			assert(mock.applied_mode == original);
			assert(!state.configuration_invalid);
			assert(fte3600_cs_restore(&state, &mode, mock_setup,
						  &mock) == 0);
			assert(mock.next == mock.count);
		}
	}
}

static void test_cs_failed_return_rolls_back_trial(void)
{
	unsigned int clock, high;

	for (clock = 0; clock < 4; clock++) {
		for (high = 0; high < 2; high++) {
			const unsigned int original = clock | SPI_LSB_FIRST |
				(high ? SPI_CS_HIGH : 0);
			const unsigned int trial = original ^ SPI_CS_HIGH;
			const struct setup_call calls[] = {
				{ trial, 0 }, { original, -EIO },
				{ trial, 0 }, { original, 0 },
			};
			unsigned int mode = original;
			struct fte3600_cs_state state = { 0 };
			struct setup_mock mock = {
				.mode = &mode, .calls = calls, .count = 4,
			};

			fte3600_cs_begin(&state, mode);
			assert(fte3600_cs_set(&state, &mode, !high,
					     mock_setup, &mock) == 0);
			/* A failed set rolls back the immediately preceding mode,
			 * which need not be the session's eventual restore target. */
			assert(fte3600_cs_set(&state, &mode, high,
					     mock_setup, &mock) == -EIO);
			assert(mode == trial && mock.applied_mode == trial);
			assert(!state.configuration_invalid);
			assert(state.original_cs == (original & SPI_CS_HIGH));
			assert(mock.next == 3);
			assert(fte3600_cs_restore(&state, &mode, mock_setup,
						  &mock) == 0);
			assert(mode == original && mock.applied_mode == original);
			assert(!state.configuration_invalid);
			assert(mock.next == mock.count);
		}
	}
}

static void test_cs_restore_failure_retry(void)
{
	unsigned int clock, high;

	for (clock = 0; clock < 4; clock++) {
		for (high = 0; high < 2; high++) {
			const unsigned int original = clock | SPI_LSB_FIRST |
				(high ? SPI_CS_HIGH : 0);
			const struct setup_call calls[] = {
				{ original ^ SPI_CS_HIGH, 0 },
				{ original, -EIO },
				{ original, -ETIMEDOUT },
				{ original, 0 },
			};
			unsigned int mode = original;
			struct fte3600_cs_state state = { 0 };
			struct setup_mock mock = {
				.mode = &mode, .calls = calls, .count = 4,
			};

			fte3600_cs_begin(&state, mode);
			assert(fte3600_cs_set(&state, &mode, !high,
					     mock_setup, &mock) == 0);
			assert(fte3600_cs_restore(&state, &mode, mock_setup,
						  &mock) == -EIO);
			assert(mode == original);
			assert(state.configuration_invalid);
			assert(state.original_cs == (original & SPI_CS_HIGH));
			assert(fte3600_cs_set(&state, &mode, !high,
					     mock_setup, &mock) == -EHOSTDOWN);
			assert(mock.next == 2);
			assert(fte3600_cs_restore(&state, &mode, mock_setup,
						  &mock) == -ETIMEDOUT);
			assert(mode == original);
			assert(state.configuration_invalid);
			assert(state.original_cs == (original & SPI_CS_HIGH));
			assert(mock.next == 3);
			assert(fte3600_cs_restore(&state, &mode, mock_setup,
						  &mock) == 0);
			assert(mode == original && mock.applied_mode == original);
			assert(!state.configuration_invalid);
			assert(state.original_cs == (original & SPI_CS_HIGH));
			/* Begin a new session only after successful recovery. */
			fte3600_cs_begin(&state, mode);
			assert(state.original_cs == (original & SPI_CS_HIGH));
			assert(fte3600_cs_restore(&state, &mode, mock_setup,
						  &mock) == 0);
			assert(mock.next == mock.count);
		}
	}
}

static void test_cs_restore_preserves_current_mode(void)
{
	const struct setup_call calls[] = {
		{ SPI_CPOL | SPI_3WIRE | SPI_CS_HIGH, 0 },
	};
	unsigned int mode = SPI_CPHA | SPI_LSB_FIRST | SPI_CS_HIGH;
	struct fte3600_cs_state state = { 0 };
	struct setup_mock mock = { .mode = &mode, .calls = calls, .count = 1 };

	fte3600_cs_begin(&state, mode);
	mode = SPI_CPOL | SPI_3WIRE;
	assert(fte3600_cs_restore(&state, &mode, mock_setup, &mock) == 0);
	assert(mode == (SPI_CPOL | SPI_3WIRE | SPI_CS_HIGH));
	assert(mock.applied_mode == mode);
	assert(state.original_cs == SPI_CS_HIGH);
	assert(!state.configuration_invalid);
	assert(mock.next == mock.count);
}

int main(void)
{
	test_resource_order();
	test_resource_ambiguity();
	test_transfer_bounds();
	test_transfer_completion();
	test_reset_metadata();
	test_cs_restore_unchanged();
	test_cs_repeated_toggles();
	test_cs_change_failure_rollback();
	test_cs_failed_rollback_recovery();
	test_cs_failed_return_rolls_back_trial();
	test_cs_restore_failure_retry();
	test_cs_restore_preserves_current_mode();
	puts("FTE3600 bridge policy: resources, reset metadata, SPI boundaries and CS recovery passed");
	return 0;
}
