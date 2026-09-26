/*
 * SPDX-License-Identifier: MIT
 *
 * The battery is connected directly to the 3V3 rail, so the battery voltage
 * is the nRF52840 VDD, which the SAADC can measure internally without a
 * divider (and without the divider's leakage current).
 *
 * The curves below are for low drain loads (a few uA average with short
 * radio bursts) at room temperature. They are estimates; the voltage of
 * alkaline and coin cells also depends on temperature and recent load.
 */

#include "battery.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(battery, CONFIG_LOG_DEFAULT_LEVEL);

#if !DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#error "zephyr,user must have io-channels pointing to an ADC channel measuring VDD"
#endif

static const struct adc_dt_spec vdd_channel = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));

struct curve_point {
	uint16_t mv;
	uint8_t percent;
};

/* Points must be sorted by descending voltage and end with 0 %. */
#if defined(CONFIG_APP_BATTERY_CR2032)
static const struct curve_point curve[] = {
	{ 3000, 100 }, { 2900, 80 }, { 2800, 60 }, { 2700, 40 },
	{ 2600, 20 },  { 2500, 10 }, { 2400, 5 },  { 2000, 0 },
};
static const struct battery_info info = { BATTERY_TYPE_CR2032, 1, "CR2032" };
#elif defined(CONFIG_APP_BATTERY_2XAAA_ALKALINE)
static const struct curve_point curve[] = {
	{ 3100, 100 }, { 2900, 80 }, { 2700, 60 }, { 2560, 45 }, { 2440, 30 },
	{ 2300, 15 },  { 2200, 8 },  { 2000, 2 },  { 1800, 0 },
};
static const struct battery_info info = { BATTERY_TYPE_AAA_ALKALINE, 2, "2x AAA alkaline" };
#elif defined(CONFIG_APP_BATTERY_2XAAA_CARBON_ZINC)
static const struct curve_point curve[] = {
	{ 3000, 100 }, { 2800, 75 }, { 2600, 50 }, { 2400, 30 },
	{ 2200, 15 },  { 2000, 5 },  { 1800, 0 },
};
static const struct battery_info info = { BATTERY_TYPE_AAA_CARBON_ZINC, 2, "2x AAA carbon-zinc" };
#elif defined(CONFIG_APP_BATTERY_2XAAA_NIMH)
/* NiMH has a very flat curve, the estimate is coarse in the middle. */
static const struct curve_point curve[] = {
	{ 2800, 100 }, { 2600, 90 }, { 2500, 70 }, { 2440, 50 }, { 2400, 35 },
	{ 2340, 20 },  { 2240, 10 }, { 2100, 5 },  { 2000, 0 },
};
static const struct battery_info info = { BATTERY_TYPE_AAA_NIMH, 2, "2x AAA NiMH" };
#else
#error "No battery type selected"
#endif

static uint8_t voltage_to_percent(uint16_t mv)
{
	if (mv >= curve[0].mv) {
		return curve[0].percent;
	}

	for (size_t i = 1; i < ARRAY_SIZE(curve); i++) {
		const struct curve_point *hi = &curve[i - 1];
		const struct curve_point *lo = &curve[i];

		if (mv >= lo->mv) {
			/* Linear interpolation between the two points. */
			return lo->percent + ((mv - lo->mv) * (hi->percent - lo->percent)) /
						     (hi->mv - lo->mv);
		}
	}

	return 0;
}

int battery_init(void)
{
	if (!adc_is_ready_dt(&vdd_channel)) {
		LOG_ERR("ADC not ready");
		return -ENODEV;
	}

	return adc_channel_setup_dt(&vdd_channel);
}

int battery_measure(struct battery_state *state)
{
	int16_t sample = 0;
	struct adc_sequence sequence = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};
	int32_t mv;
	int err;

	err = adc_sequence_init_dt(&vdd_channel, &sequence);
	if (err) {
		return err;
	}

	err = adc_read_dt(&vdd_channel, &sequence);
	if (err) {
		LOG_ERR("ADC read failed (%d)", err);
		return err;
	}

	mv = MAX(sample, 0);
	err = adc_raw_to_millivolts_dt(&vdd_channel, &mv);
	if (err) {
		return err;
	}

	state->voltage_mv = (uint16_t)CLAMP(mv, 0, UINT16_MAX);
	state->percent = voltage_to_percent(state->voltage_mv);

	if (state->percent <= CONFIG_APP_BATTERY_CRITICAL_PERCENT) {
		state->level = BATTERY_LEVEL_CRITICAL;
	} else if (state->percent <= CONFIG_APP_BATTERY_WARNING_PERCENT) {
		state->level = BATTERY_LEVEL_WARNING;
	} else {
		state->level = BATTERY_LEVEL_OK;
	}

	LOG_INF("Battery: %u mV, %u %%", state->voltage_mv, state->percent);

	return 0;
}

const struct battery_info *battery_get_info(void)
{
	return &info;
}
