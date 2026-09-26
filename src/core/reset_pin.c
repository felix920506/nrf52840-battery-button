/*
 * SPDX-License-Identifier: MIT
 */

#include "reset_pin.h"
#include "status_led.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(reset_pin, CONFIG_LOG_DEFAULT_LEVEL);

#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user), factory_reset_gpios)

static const struct gpio_dt_spec pad = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), factory_reset_gpios);
static struct gpio_callback pad_cb;
static reset_pin_handler_t reset_handler;
static uint16_t generation;

static void hold_expiry(struct k_timer *timer)
{
	app_loop_post(APP_EVT_FACTORY_RESET_TIMER, 0, generation);
}

static K_TIMER_DEFINE(hold_timer, hold_expiry, NULL);

static void pad_isr(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins)
{
	/* Level interrupt: keeps firing until re-armed for the other level. */
	gpio_pin_interrupt_configure_dt(&pad, GPIO_INT_DISABLE);
	app_loop_post(APP_EVT_RESET_PIN, 0, 0);
}

static void on_pad_change(void)
{
	bool held = gpio_pin_get_dt(&pad) > 0;

	/* Any change restarts the count: the pad has to be held continuously. */
	k_timer_stop(&hold_timer);
	generation++;

	if (held) {
		LOG_INF("Reset pad held, factory reset in %u ms", CONFIG_APP_FACTORY_RESET_HOLD_MS);
		status_led_flash(1);
		k_timer_start(&hold_timer, K_MSEC(CONFIG_APP_FACTORY_RESET_HOLD_MS), K_NO_WAIT);
	}

	/* Level interrupts fire right away if the pad changed meanwhile. */
	gpio_pin_interrupt_configure_dt(&pad, held ? GPIO_INT_LEVEL_INACTIVE : GPIO_INT_LEVEL_ACTIVE);
}

void reset_pin_process(const struct app_evt *evt)
{
	switch (evt->type) {
	case APP_EVT_RESET_PIN:
		on_pad_change();
		break;
	case APP_EVT_FACTORY_RESET_TIMER:
		if (evt->arg == generation && gpio_pin_get_dt(&pad) > 0) {
			reset_handler();
		}
		break;
	default:
		break;
	}
}

int reset_pin_init(reset_pin_handler_t on_reset)
{
	int err;

	reset_handler = on_reset;

	if (!gpio_is_ready_dt(&pad)) {
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&pad, GPIO_INPUT);
	if (err) {
		return err;
	}

	gpio_init_callback(&pad_cb, pad_isr, BIT(pad.pin));
	err = gpio_add_callback_dt(&pad, &pad_cb);
	if (err) {
		return err;
	}

	/* A pad held at boot counts too: the device may be stuck in a reboot loop. */
	on_pad_change();

	return 0;
}

#else /* No reset pad on this board */

void reset_pin_process(const struct app_evt *evt)
{
}

int reset_pin_init(reset_pin_handler_t on_reset)
{
	return -ENOTSUP;
}

#endif
