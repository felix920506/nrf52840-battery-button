/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Battery powered switch: up to 6 external momentary or latching switches.
 *
 * Everything the application does happens in this loop: interrupts and
 * timers only post events. Between events the CPU sleeps.
 */

#include "core/app_loop.h"
#include "core/battery.h"
#include "core/reset_pin.h"
#include "core/switch_config.h"
#include "core/status_led.h"
#include "core/switch_gesture.h"
#include "core/switch_input.h"
#include "transport/transport.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

static void battery_timer_expiry(struct k_timer *timer)
{
	app_loop_post(APP_EVT_BATTERY, 0, 0);
}

static K_TIMER_DEFINE(battery_timer, battery_timer_expiry, NULL);

/* Called when the reset pad has been held to GND for CONFIG_APP_FACTORY_RESET_HOLD_MS. */
static void factory_reset(void)
{
	LOG_WRN("Factory reset");
	status_led_flash(5);
	transport_factory_reset();
}

static void on_switch_event(uint8_t index, const struct switch_event *evt)
{
	transport_switch_event(index, evt);
}

static void on_input(uint8_t index, bool active)
{
	/* Any switch activity makes an unprovisioned device discoverable again. */
	if (!transport_is_provisioned()) {
		transport_start_pairing();
	}

	switch_gesture_input(index, active);
}

/* Change requested through the transport (e.g. the smart home app). */
static void change_switch_type(uint8_t index, uint16_t type)
{
	if (index >= switch_input_count() || !switch_type_is_valid(type) ||
	    switch_input_get_type(index) == type) {
		return;
	}

	switch_gesture_reset(index);
	switch_input_set_type(index, type);

	int err = switch_config_save(index, type);

	if (err) {
		LOG_ERR("Saving switch %u type failed (%d)", index + 1, err);
	}

	transport_switch_type_changed(index, type, switch_input_is_active(index));
}

static void measure_battery(void)
{
	struct battery_state state;

	if (battery_measure(&state) == 0) {
		transport_battery_update(&state);
	}
}

static void suspend_external_flash(void)
{
	/*
	 * The XIAO has a 2 MB QSPI flash that is not used. Put it into deep
	 * power-down, otherwise it idles in standby and wastes current.
	 */
#if defined(CONFIG_NORDIC_QSPI_NOR) && defined(CONFIG_PM_DEVICE) &&                                \
	DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(p25q16h))
	const struct device *flash = DEVICE_DT_GET(DT_NODELABEL(p25q16h));

	if (device_is_ready(flash)) {
		(void)pm_device_action_run(flash, PM_DEVICE_ACTION_SUSPEND);
	}
#endif
}

int main(void)
{
	struct transport_switch_config switches = { 0 };
	int err;

	suspend_external_flash();

	err = status_led_init();
	if (err) {
		LOG_WRN("Status LED init failed (%d)", err);
	}

	err = battery_init();
	if (err) {
		LOG_WRN("Battery measurement init failed (%d)", err);
	}

	err = switch_input_init(on_input);
	if (err) {
		LOG_ERR("Switch input init failed (%d)", err);
		return err;
	}

	err = switch_config_load();
	if (err) {
		LOG_WRN("Loading switch types failed (%d), using defaults", err);
	}

	switches.count = switch_input_count();
	for (uint8_t i = 0; i < switches.count; i++) {
		switches.type[i] = switch_input_get_type(i);
		switches.active[i] = switch_input_is_active(i);
	}

	switch_gesture_init(switches.count, on_switch_event);

	err = transport_init(&switches, battery_get_info());
	if (err) {
		LOG_ERR("Transport init failed (%d)", err);
		return err;
	}

	err = reset_pin_init(factory_reset);
	if (err) {
		LOG_WRN("No factory reset pad (%d)", err);
	}

	measure_battery();
	k_timer_start(&battery_timer, K_SECONDS(CONFIG_APP_BATTERY_MEASURE_INTERVAL_S),
		      K_SECONDS(CONFIG_APP_BATTERY_MEASURE_INTERVAL_S));

	for (;;) {
		struct app_evt evt;

		app_loop_wait(&evt);

		switch (evt.type) {
		case APP_EVT_INPUT_IRQ:
		case APP_EVT_INPUT_DEBOUNCE:
		case APP_EVT_INPUT_POLL:
			switch_input_process(&evt);
			break;
		case APP_EVT_GESTURE_TIMER:
			switch_gesture_process(&evt);
			break;
		case APP_EVT_SWITCH_TYPE:
			change_switch_type(evt.index, evt.arg);
			break;
		case APP_EVT_BATTERY:
			measure_battery();
			break;
		case APP_EVT_RESET_PIN:
		case APP_EVT_FACTORY_RESET_TIMER:
			reset_pin_process(&evt);
			break;
		default:
			break;
		}
	}

	return 0;
}
