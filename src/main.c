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
#include "core/status_led.h"
#include "core/switch_gesture.h"
#include "core/switch_input.h"
#include "transport/transport.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

#include <string.h>

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

/* Switch 1 doubles as the factory reset switch. */
#define RESET_SWITCH 0

static void battery_timer_expiry(struct k_timer *timer)
{
	app_loop_post(APP_EVT_BATTERY, 0, 0);
}

static void factory_reset_timer_expiry(struct k_timer *timer);

static K_TIMER_DEFINE(battery_timer, battery_timer_expiry, NULL);
static K_TIMER_DEFINE(factory_reset_timer, factory_reset_timer_expiry, NULL);
static uint16_t factory_reset_generation;

/* Latching switch 1: times of the last CONFIG_APP_FACTORY_RESET_TOGGLES changes. */
static int64_t toggle_times[CONFIG_APP_FACTORY_RESET_TOGGLES];
static uint8_t toggle_next;

static void factory_reset_timer_expiry(struct k_timer *timer)
{
	app_loop_post(APP_EVT_FACTORY_RESET_TIMER, 0, factory_reset_generation);
}

static void factory_reset(void)
{
	LOG_WRN("Factory reset");
	status_led_flash(5);
	transport_factory_reset();
}

/*
 * Momentary switch 1: hold for CONFIG_APP_FACTORY_RESET_HOLD_MS.
 * Latching switch 1: toggle CONFIG_APP_FACTORY_RESET_TOGGLES times within
 * CONFIG_APP_FACTORY_RESET_TOGGLE_WINDOW_MS.
 */
static void factory_reset_check(uint8_t index, bool active)
{
	if (index != RESET_SWITCH) {
		return;
	}

	if (switch_input_is_latching(index)) {
		int64_t now = k_uptime_get();
		/* The oldest of the recorded toggles is the one about to be overwritten. */
		int64_t oldest = toggle_times[toggle_next];

		toggle_times[toggle_next] = now;
		toggle_next = (toggle_next + 1) % ARRAY_SIZE(toggle_times);

		if (oldest != 0 && now - oldest <= CONFIG_APP_FACTORY_RESET_TOGGLE_WINDOW_MS) {
			memset(toggle_times, 0, sizeof(toggle_times));
			factory_reset();
		}
		return;
	}

	/* Stop first so an expiry that races with this carries the old generation. */
	k_timer_stop(&factory_reset_timer);
	factory_reset_generation++;

	if (active) {
		k_timer_start(&factory_reset_timer, K_MSEC(CONFIG_APP_FACTORY_RESET_HOLD_MS),
			      K_NO_WAIT);
	}
}

static void on_switch_event(uint8_t index, const struct switch_event *evt)
{
	transport_switch_event(index, evt);
}

static void on_input(uint8_t index, bool active)
{
	factory_reset_check(index, active);

	/* Any switch activity makes an unprovisioned device discoverable again. */
	if (!transport_is_provisioned()) {
		transport_start_pairing();
	}

	switch_gesture_input(index, active);
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

	switches.count = switch_input_count();
	for (uint8_t i = 0; i < switches.count; i++) {
		switches.latching[i] = switch_input_is_latching(i);
		switches.active[i] = switch_input_is_active(i);
	}

	switch_gesture_init(switches.count, on_switch_event);

	err = transport_init(&switches, battery_get_info());
	if (err) {
		LOG_ERR("Transport init failed (%d)", err);
		return err;
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
		case APP_EVT_BATTERY:
			measure_battery();
			break;
		case APP_EVT_FACTORY_RESET_TIMER:
			if (evt.arg == factory_reset_generation &&
			    switch_input_is_active(RESET_SWITCH)) {
				factory_reset();
			}
			break;
		default:
			break;
		}
	}

	return 0;
}
