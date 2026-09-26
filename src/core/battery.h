/*
 * SPDX-License-Identifier: MIT
 *
 * Battery voltage measurement and state of charge estimation.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum battery_type {
	BATTERY_TYPE_CR2032,
	BATTERY_TYPE_AAA_ALKALINE,
	BATTERY_TYPE_AAA_CARBON_ZINC,
	BATTERY_TYPE_AAA_NIMH,
};

enum battery_level {
	BATTERY_LEVEL_OK,
	BATTERY_LEVEL_WARNING,
	BATTERY_LEVEL_CRITICAL,
};

struct battery_info {
	enum battery_type type;
	/* Number of cells. */
	uint8_t quantity;
	/* Short human readable description, e.g. "2x AAA alkaline". */
	const char *description;
};

struct battery_state {
	uint16_t voltage_mv;
	/* Estimated remaining charge, 0..100. */
	uint8_t percent;
	enum battery_level level;
};

int battery_init(void);

/* Measures the supply voltage. Takes well below a millisecond. */
int battery_measure(struct battery_state *state);

/* The battery selected with CONFIG_APP_BATTERY_TYPE. */
const struct battery_info *battery_get_info(void);

#ifdef __cplusplus
}
#endif
