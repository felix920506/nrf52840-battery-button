/*
 * SPDX-License-Identifier: MIT
 */

#include "status_led.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>

#if defined(CONFIG_APP_STATUS_LED) && DT_NODE_EXISTS(DT_ALIAS(status_led))

struct pattern {
	uint16_t on_ms;
	uint16_t off_ms;
};

static const struct pattern flash_pattern = { 100, 150 };
static const struct pattern identify_pattern = { 250, 250 };
static const struct pattern pairing_pattern = { 20, 1980 };

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(status_led), gpios);
static struct k_spinlock lock;
static bool pairing;
static bool identify;
static uint8_t flashes_left;
static bool led_on;

static void timer_expiry(struct k_timer *timer);
static K_TIMER_DEFINE(led_timer, timer_expiry, NULL);

/* Highest priority pattern that is currently requested, or NULL. */
static const struct pattern *current_pattern(void)
{
	if (flashes_left) {
		return &flash_pattern;
	}
	if (identify) {
		return &identify_pattern;
	}
	if (pairing) {
		return &pairing_pattern;
	}
	return NULL;
}

static void led_set(bool on)
{
	led_on = on;
	gpio_pin_set_dt(&led, on);
}

/* Restart the blinking with the current pattern. Call with `lock` held. */
static void restart(void)
{
	const struct pattern *p = current_pattern();

	k_timer_stop(&led_timer);

	if (p == NULL) {
		led_set(false);
		return;
	}

	led_set(true);
	k_timer_start(&led_timer, K_MSEC(p->on_ms), K_NO_WAIT);
}

static void timer_expiry(struct k_timer *timer)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	const struct pattern *p = current_pattern();

	if (p == NULL) {
		led_set(false);
	} else if (led_on) {
		led_set(false);
		if (p == &flash_pattern) {
			flashes_left--;
		}
		k_timer_start(&led_timer, K_MSEC(p->off_ms), K_NO_WAIT);
	} else {
		led_set(true);
		k_timer_start(&led_timer, K_MSEC(p->on_ms), K_NO_WAIT);
	}

	k_spin_unlock(&lock, key);
}

int status_led_init(void)
{
	if (!gpio_is_ready_dt(&led)) {
		return -ENODEV;
	}

	return gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
}

void status_led_set_pairing(bool on)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (pairing != on) {
		pairing = on;
		restart();
	}

	k_spin_unlock(&lock, key);
}

void status_led_set_identify(bool on)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (identify != on) {
		identify = on;
		restart();
	}

	k_spin_unlock(&lock, key);
}

void status_led_flash(uint8_t times)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	flashes_left = times;
	restart();

	k_spin_unlock(&lock, key);
}

#else /* No status LED */

int status_led_init(void)
{
	return 0;
}

void status_led_set_pairing(bool on)
{
}

void status_led_set_identify(bool on)
{
}

void status_led_flash(uint8_t times)
{
}

#endif
