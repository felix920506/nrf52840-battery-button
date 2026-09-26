/*
 * SPDX-License-Identifier: MIT
 *
 * Power strategy:
 *  - An open switch draws no current. Its pin has the pull-up enabled and a
 *    level interrupt armed for the "closed" level. On nRF52 level interrupts
 *    use the GPIO SENSE/PORT mechanism, which costs nothing while idle
 *    (unlike GPIOTE IN channels).
 *  - A pressed momentary switch keeps its pull-up (and draws current through
 *    it) until released. Presses are short, so this is fine.
 *  - A closed latching switch would draw current through the pull-up for as
 *    long as it stays closed. Instead, its pin is disconnected and sampled
 *    every CONFIG_APP_LATCH_POLL_INTERVAL_MS for a few microseconds.
 */

#include "switch_input.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(switch_input, CONFIG_LOG_DEFAULT_LEVEL);

#if !DT_HAS_COMPAT_STATUS_OKAY(battery_switch_inputs)
#error "No 'battery-switch,inputs' node in devicetree, see boards/xiao_ble.overlay"
#endif

#define SWITCHES_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(battery_switch_inputs)

#define SWITCH_SPEC(node)     GPIO_DT_SPEC_GET(node, gpios),
/* Devicetree enum: 0 "momentary", 1 "latching". */
#define SWITCH_TYPE(node)                                                                          \
	(DT_ENUM_IDX(node, switch_type) ? SWITCH_TYPE_LATCHING_AS_PRESS : SWITCH_TYPE_MOMENTARY),
#define SWITCH_LABEL(node)    DT_PROP_OR(node, label, DT_NODE_FULL_NAME(node)),

static const struct gpio_dt_spec specs[] = {
	DT_FOREACH_CHILD_STATUS_OKAY(SWITCHES_NODE, SWITCH_SPEC)
};
/* Defaults from devicetree; changeable at runtime with switch_input_set_type(). */
static uint8_t types[] = {
	DT_FOREACH_CHILD_STATUS_OKAY(SWITCHES_NODE, SWITCH_TYPE)
};

static const char *const type_names[] = {
	[SWITCH_TYPE_MOMENTARY] = "momentary",
	[SWITCH_TYPE_LATCHING_AS_PRESS] = "latching",
};

static const char *const labels[] = {
	DT_FOREACH_CHILD_STATUS_OKAY(SWITCHES_NODE, SWITCH_LABEL)
};

#define NUM_INPUTS ARRAY_SIZE(specs)

BUILD_ASSERT(NUM_INPUTS >= 1 && NUM_INPUTS <= SWITCH_INPUT_MAX,
	     "Between 1 and 6 switches must be defined in devicetree");

struct input_state {
	struct gpio_callback cb;
	struct k_timer debounce_timer;
	/* Debounced state, true = pressed/closed. */
	bool active;
	/* Waiting for the debounce timer. */
	bool debouncing;
	/* Closed latching switch: pin disconnected, sampled by the poll timer. */
	bool polling;
};

static struct input_state inputs[NUM_INPUTS];
static switch_input_handler_t input_handler;

static void debounce_expiry(struct k_timer *timer)
{
	struct input_state *in = CONTAINER_OF(timer, struct input_state, debounce_timer);

	app_loop_post(APP_EVT_INPUT_DEBOUNCE, in - inputs, 0);
}

static void poll_expiry(struct k_timer *timer)
{
	app_loop_post(APP_EVT_INPUT_POLL, 0, 0);
}

static K_TIMER_DEFINE(poll_timer, poll_expiry, NULL);
static bool poll_running;

static void gpio_isr(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins)
{
	struct input_state *in = CONTAINER_OF(cb, struct input_state, cb);
	uint8_t idx = in - inputs;

	/* Level interrupts keep firing until the level changes; the loop re-arms it. */
	gpio_pin_interrupt_configure_dt(&specs[idx], GPIO_INT_DISABLE);
	app_loop_post(APP_EVT_INPUT_IRQ, idx, 0);
}

static int connect_pin(uint8_t idx)
{
	/* Input with the flags from devicetree (pull-up, active low). */
	return gpio_pin_configure_dt(&specs[idx], GPIO_INPUT);
}

static int disconnect_pin(uint8_t idx)
{
	/* No input buffer, no pull: the closed switch draws no current. */
	return gpio_pin_configure(specs[idx].port, specs[idx].pin, GPIO_DISCONNECTED);
}

static bool read_pin(uint8_t idx)
{
	return gpio_pin_get_dt(&specs[idx]) > 0;
}

static void start_debounce(uint8_t idx)
{
	uint32_t ms = switch_input_is_latching(idx) ? CONFIG_APP_DEBOUNCE_LATCHING_MS
						    : CONFIG_APP_DEBOUNCE_MS;

	inputs[idx].debouncing = true;
	k_timer_start(&inputs[idx].debounce_timer, K_MSEC(ms), K_NO_WAIT);
}

static void start_polling(void)
{
	if (!poll_running) {
		poll_running = true;
		k_timer_start(&poll_timer, K_MSEC(CONFIG_APP_LATCH_POLL_INTERVAL_MS),
			      K_MSEC(CONFIG_APP_LATCH_POLL_INTERVAL_MS));
	}
}

/* Wait for the next change of a switch, in the cheapest way for its state. */
static void arm(uint8_t idx)
{
	struct input_state *in = &inputs[idx];

	if (switch_input_is_latching(idx) && in->active) {
		gpio_pin_interrupt_configure_dt(&specs[idx], GPIO_INT_DISABLE);
		disconnect_pin(idx);
		in->polling = true;
		start_polling();
		return;
	}

	in->polling = false;
	connect_pin(idx);
	/*
	 * A level interrupt (unlike an edge interrupt) fires right away if the
	 * pin already changed after it was sampled, so no change is missed.
	 */
	gpio_pin_interrupt_configure_dt(&specs[idx],
					in->active ? GPIO_INT_LEVEL_INACTIVE : GPIO_INT_LEVEL_ACTIVE);
}

static void process_debounce(uint8_t idx)
{
	struct input_state *in = &inputs[idx];

	if (!in->debouncing) {
		return;
	}

	in->debouncing = false;

	bool active = read_pin(idx);
	bool changed = active != in->active;

	in->active = active;
	arm(idx);

	if (changed) {
		LOG_DBG("%s: %s", labels[idx], active ? "closed" : "open");
		input_handler(idx, active);
	}
}

static void process_poll(void)
{
	bool any_polling = false;

	for (uint8_t i = 0; i < NUM_INPUTS; i++) {
		struct input_state *in = &inputs[i];

		if (!in->polling) {
			continue;
		}

		connect_pin(i);
		k_busy_wait(CONFIG_APP_LATCH_POLL_SETTLE_US);

		if (read_pin(i)) {
			/* Still closed. */
			disconnect_pin(i);
			any_polling = true;
		} else {
			/* Opened (or bouncing): keep the pull-up and debounce it. */
			in->polling = false;
			start_debounce(i);
		}
	}

	if (!any_polling) {
		k_timer_stop(&poll_timer);
		poll_running = false;
	}
}

void switch_input_process(const struct app_evt *evt)
{
	switch (evt->type) {
	case APP_EVT_INPUT_IRQ:
		if (evt->index < NUM_INPUTS) {
			start_debounce(evt->index);
		}
		break;
	case APP_EVT_INPUT_DEBOUNCE:
		if (evt->index < NUM_INPUTS) {
			process_debounce(evt->index);
		}
		break;
	case APP_EVT_INPUT_POLL:
		process_poll();
		break;
	default:
		break;
	}
}

int switch_input_init(switch_input_handler_t handler)
{
	input_handler = handler;

	for (uint8_t i = 0; i < NUM_INPUTS; i++) {
		int err;

		if (!gpio_is_ready_dt(&specs[i])) {
			LOG_ERR("%s: GPIO not ready", labels[i]);
			return -ENODEV;
		}

		err = connect_pin(i);
		if (err) {
			LOG_ERR("%s: configure failed (%d)", labels[i], err);
			return err;
		}

		k_busy_wait(CONFIG_APP_LATCH_POLL_SETTLE_US);
		inputs[i].active = read_pin(i);
		k_timer_init(&inputs[i].debounce_timer, debounce_expiry, NULL);

		gpio_init_callback(&inputs[i].cb, gpio_isr, BIT(specs[i].pin));
		err = gpio_add_callback_dt(&specs[i], &inputs[i].cb);
		if (err) {
			LOG_ERR("%s: add callback failed (%d)", labels[i], err);
			return err;
		}

		arm(i);

		LOG_INF("%s: %s switch, initially %s", labels[i],
			type_names[types[i]], inputs[i].active ? "closed" : "open");
	}

	return 0;
}

uint8_t switch_input_count(void)
{
	return NUM_INPUTS;
}

void switch_input_set_type(uint8_t index, enum switch_type type)
{
	if (index >= NUM_INPUTS || types[index] == type) {
		return;
	}

	struct input_state *in = &inputs[index];

	types[index] = type;

	/* Start over from a fresh sample, with the new type's power handling. */
	k_timer_stop(&in->debounce_timer);
	in->debouncing = false;
	gpio_pin_interrupt_configure_dt(&specs[index], GPIO_INT_DISABLE);
	connect_pin(index);
	k_busy_wait(CONFIG_APP_LATCH_POLL_SETTLE_US);
	in->active = read_pin(index);
	arm(index);

	LOG_INF("%s: now %s", labels[index], type_names[type]);
}

enum switch_type switch_input_get_type(uint8_t index)
{
	return index < NUM_INPUTS ? (enum switch_type)types[index] : SWITCH_TYPE_MOMENTARY;
}

bool switch_input_is_latching(uint8_t index)
{
	return switch_input_get_type(index) != SWITCH_TYPE_MOMENTARY;
}

bool switch_input_is_active(uint8_t index)
{
	return index < NUM_INPUTS && inputs[index].active;
}
