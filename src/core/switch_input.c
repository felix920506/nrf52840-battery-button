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
 *    every CONFIG_APP_LATCH_POLL_INTERVAL_MS. The pull-up is turned on
 *    CONFIG_APP_LATCH_POLL_SETTLE_US before the sample, long enough to charge
 *    a capacitor against interference (see the user guide); the CPU sleeps
 *    meanwhile.
 *
 * NO/NC switch types (switches with `nc-gpios` in devicetree): a changeover
 * contact closes one of two pins in each position. One pin is then always
 * tied to GND, which interference on the cable can hardly lift, so a change
 * is only accepted once both pins have changed, in opposite directions.
 * Interference on the open pin alone reads as "both closed" and is ignored.
 * The closed pin is disconnected and the open one waits for a level
 * interrupt, so neither position draws pull-up current and no polling is
 * needed. Both pins are only connected while a debounce sample reads them.
 * With a plain type, the nc-gpios pin stays disconnected.
 */

#include "switch_input.h"
#include "diag.h"

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
/* Second contact for the NO/NC types; .port is NULL if there is none. */
#define SWITCH_NC_SPEC(node)  GPIO_DT_SPEC_GET_OR(node, nc_gpios, {0}),
/* Devicetree enum: "momentary", "latching", "momentary-nonc", "latching-nonc". */
#define DT_TYPE(idx)                                                                               \
	((idx) == 0   ? SWITCH_TYPE_MOMENTARY                                                     \
	 : (idx) == 1 ? SWITCH_TYPE_LATCHING_AS_PRESS                                             \
	 : (idx) == 2 ? SWITCH_TYPE_MOMENTARY_NONC                                                \
		      : SWITCH_TYPE_LATCHING_NONC)
#define SWITCH_TYPE(node)     DT_TYPE(DT_ENUM_IDX(node, switch_type)),
#define SWITCH_NC_CHECK(node)                                                                      \
	BUILD_ASSERT(DT_ENUM_IDX(node, switch_type) < 2 || DT_NODE_HAS_PROP(node, nc_gpios),        \
		     DT_NODE_FULL_NAME(node) ": a NO/NC switch-type needs nc-gpios");
#define SWITCH_LABEL(node)    DT_PROP_OR(node, label, DT_NODE_FULL_NAME(node)),

DT_FOREACH_CHILD_STATUS_OKAY(SWITCHES_NODE, SWITCH_NC_CHECK)

static const struct gpio_dt_spec specs[] = {
	DT_FOREACH_CHILD_STATUS_OKAY(SWITCHES_NODE, SWITCH_SPEC)
};
static const struct gpio_dt_spec nc_specs[] = {
	DT_FOREACH_CHILD_STATUS_OKAY(SWITCHES_NODE, SWITCH_NC_SPEC)
};
/* Defaults from devicetree; changeable at runtime with switch_input_set_type(). */
static uint8_t types[] = {
	DT_FOREACH_CHILD_STATUS_OKAY(SWITCHES_NODE, SWITCH_TYPE)
};

static const char *const type_names[] = {
	[SWITCH_TYPE_MOMENTARY] = "momentary",
	[SWITCH_TYPE_LATCHING_AS_PRESS] = "latching",
	[SWITCH_TYPE_MOMENTARY_NONC] = "momentary NO/NC",
	[SWITCH_TYPE_LATCHING_NONC] = "latching NO/NC",
};

static const char *const labels[] = {
	DT_FOREACH_CHILD_STATUS_OKAY(SWITCHES_NODE, SWITCH_LABEL)
};

#define NUM_INPUTS ARRAY_SIZE(specs)

BUILD_ASSERT(NUM_INPUTS >= 1 && NUM_INPUTS <= SWITCH_INPUT_MAX,
	     "Between 1 and 6 switches must be defined in devicetree");

/* Debounce sample of a NO/NC switch, relative to its debounced state. */
enum pair_reading {
	/* The contact of the current state is closed, the other one open. */
	PAIR_UNCHANGED,
	/* The other contact closed and the one of the current state opened. */
	PAIR_CHANGED,
	/* Between positions (break-before-make), or a broken wire. */
	PAIR_BOTH_OPEN,
	/* Interference on the open pin, or a short / make-before-break switch. */
	PAIR_BOTH_CLOSED,
};

struct input_state {
	struct gpio_callback cb;
	/* NO/NC switch: callback of the nc-gpios pin. */
	struct gpio_callback nc_cb;
	struct k_timer debounce_timer;
	/* Debounced state, true = pressed/closed (`gpios` contact closed). */
	bool active;
	/* Waiting for the debounce timer. */
	bool debouncing;
	/*
	 * Latest debounce sample (pin level, or enum pair_reading for a NO/NC
	 * switch), and how many in a row agreed.
	 */
	uint8_t candidate;
	uint8_t stable_samples;
	uint8_t total_samples;
	/*
	 * Pins disconnected, sampled by the poll timer: a closed latching
	 * switch, or a NO/NC switch with both contacts closed.
	 */
	bool polling;
	/* NO/NC switch: both contacts read closed in the latest sample. */
	bool both_closed;
	/* NO/NC wiring problems already shown on the serial port (once per boot). */
	bool told_both_open;
	bool told_both_closed;
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

static void gpio_nc_isr(const struct device *port, struct gpio_callback *cb,
			gpio_port_pins_t pins)
{
	struct input_state *in = CONTAINER_OF(cb, struct input_state, nc_cb);
	uint8_t idx = in - inputs;

	gpio_pin_interrupt_configure_dt(&nc_specs[idx], GPIO_INT_DISABLE);
	app_loop_post(APP_EVT_INPUT_IRQ, idx, 0);
}

/* The switch has a second pin in devicetree, so the NO/NC types can be used. */
static bool nc_available(uint8_t idx)
{
	return nc_specs[idx].port != NULL;
}

/* The switch is read as a NO/NC pair. */
static bool paired(uint8_t idx)
{
	return nc_available(idx) && switch_type_is_nonc(types[idx]);
}

static int connect_spec(const struct gpio_dt_spec *spec)
{
	/* Input with the flags from devicetree (pull-up, active low). */
	return gpio_pin_configure_dt(spec, GPIO_INPUT);
}

static int disconnect_spec(const struct gpio_dt_spec *spec)
{
	/* No input buffer, no pull: a closed contact draws no current. */
	return gpio_pin_configure(spec->port, spec->pin, GPIO_DISCONNECTED);
}

static bool read_spec(const struct gpio_dt_spec *spec)
{
	return gpio_pin_get_dt(spec) > 0;
}

static int connect_pin(uint8_t idx)
{
	return connect_spec(&specs[idx]);
}

static int disconnect_pin(uint8_t idx)
{
	return disconnect_spec(&specs[idx]);
}

static bool read_pin(uint8_t idx)
{
	return read_spec(&specs[idx]);
}

/*
 * Wait for just-connected pull-ups to charge their pins, including a
 * capacitor of up to 10 nF from a pin to GND. Sleeping, not busy waiting:
 * this runs every poll interval while a latching switch is closed.
 */
static void settle(void)
{
	k_usleep(CONFIG_APP_LATCH_POLL_SETTLE_US);
}

/* NO/NC switch: the contact that is closed in the debounced state... */
static const struct gpio_dt_spec *held_contact(uint8_t idx)
{
	return inputs[idx].active ? &specs[idx] : &nc_specs[idx];
}

/* ...and the one that closes when the switch changes. */
static const struct gpio_dt_spec *watched_contact(uint8_t idx)
{
	return inputs[idx].active ? &nc_specs[idx] : &specs[idx];
}

/* Read both contacts of a NO/NC switch; leaves both pins disconnected. */
static void read_contacts(uint8_t idx, bool *no_closed, bool *nc_closed)
{
	connect_spec(&specs[idx]);
	connect_spec(&nc_specs[idx]);
	settle();
	*no_closed = read_spec(&specs[idx]);
	*nc_closed = read_spec(&nc_specs[idx]);
	disconnect_spec(&specs[idx]);
	disconnect_spec(&nc_specs[idx]);
}

static enum pair_reading read_pair(uint8_t idx)
{
	bool no_closed, nc_closed;

	read_contacts(idx, &no_closed, &nc_closed);

	bool held = inputs[idx].active ? no_closed : nc_closed;
	bool watched = inputs[idx].active ? nc_closed : no_closed;

	if (held) {
		return watched ? PAIR_BOTH_CLOSED : PAIR_UNCHANGED;
	}
	return watched ? PAIR_CHANGED : PAIR_BOTH_OPEN;
}

/*
 * Read the state of a switch without debouncing (boot, type change). For a
 * NO/NC switch, returns PAIR_BOTH_OPEN or PAIR_BOTH_CLOSED if the contacts
 * don't make sense, otherwise PAIR_UNCHANGED.
 */
static enum pair_reading read_initial_state(uint8_t idx)
{
	struct input_state *in = &inputs[idx];

	in->both_closed = false;

	if (!paired(idx)) {
		if (nc_available(idx)) {
			disconnect_spec(&nc_specs[idx]);
		}
		connect_pin(idx);
		settle();
		in->active = read_pin(idx);
		return PAIR_UNCHANGED;
	}

	bool no_closed, nc_closed;

	read_contacts(idx, &no_closed, &nc_closed);
	if (no_closed != nc_closed) {
		in->active = no_closed;
		return PAIR_UNCHANGED;
	}
	/* Both open or both closed: keep the current state. */
	in->both_closed = no_closed;
	return no_closed ? PAIR_BOTH_CLOSED : PAIR_BOTH_OPEN;
}

/* Tell the person on the serial port about NO/NC wiring problems, once. */
static void report_pair_problem(uint8_t idx, enum pair_reading reading)
{
	struct input_state *in = &inputs[idx];

	if (reading == PAIR_BOTH_CLOSED) {
		LOG_WRN("%s: both contacts closed", labels[idx]);
		if (!in->told_both_closed) {
			in->told_both_closed = true;
			diag_event("%s: both contacts of the NO/NC switch are closed.", labels[idx]);
			diag_event("  -> Check the wiring for a short between the two pins. The switch");
			diag_event("     must break one contact before making the other. Very strong");
			diag_event("     interference on the cable can also cause this.");
		}
	} else if (reading == PAIR_BOTH_OPEN) {
		LOG_WRN("%s: both contacts open", labels[idx]);
		if (!in->told_both_open) {
			in->told_both_open = true;
			diag_event("%s: neither contact of the NO/NC switch is closed.", labels[idx]);
			diag_event("  -> Unless the switch was held halfway, check for a broken wire or");
			diag_event("     a missing connection to one of its two pins or to GND. A switch");
			diag_event("     with only two wires needs a type without NO/NC.");
		}
	}
}

/* Give up on a pin that never settles, so noise can't keep the CPU awake. */
#define DEBOUNCE_MAX_SAMPLES (8 * CONFIG_APP_DEBOUNCE_SAMPLES)

BUILD_ASSERT(DEBOUNCE_MAX_SAMPLES <= UINT8_MAX);

/*
 * The debounce time is split into CONFIG_APP_DEBOUNCE_SAMPLES samples that
 * all have to agree. A single sample would turn one noise spike (e.g. picked
 * up from mains wiring next to the switch cable) into a change, which a
 * latching switch reports as a press.
 */
static void schedule_sample(uint8_t idx)
{
	uint32_t ms = switch_input_is_latching(idx) ? CONFIG_APP_DEBOUNCE_LATCHING_MS
						    : CONFIG_APP_DEBOUNCE_MS;
	uint32_t us = MAX(ms * 1000U / CONFIG_APP_DEBOUNCE_SAMPLES, 1U);

	k_timer_start(&inputs[idx].debounce_timer, K_USEC(us), K_NO_WAIT);
}

static void start_debounce(uint8_t idx)
{
	inputs[idx].debouncing = true;
	inputs[idx].stable_samples = 0;
	inputs[idx].total_samples = 0;
	schedule_sample(idx);
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

	if (paired(idx)) {
		const struct gpio_dt_spec *held = held_contact(idx);
		const struct gpio_dt_spec *watched = watched_contact(idx);

		/* The closed contact needs no pull-up: nothing happens until the other closes. */
		gpio_pin_interrupt_configure_dt(held, GPIO_INT_DISABLE);
		disconnect_spec(held);

		if (in->both_closed) {
			/* An interrupt on a closed pin would fire right away: poll instead. */
			gpio_pin_interrupt_configure_dt(watched, GPIO_INT_DISABLE);
			disconnect_spec(watched);
			in->polling = true;
			start_polling();
			return;
		}

		in->polling = false;
		connect_spec(watched);
		gpio_pin_interrupt_configure_dt(watched, GPIO_INT_LEVEL_ACTIVE);
		return;
	}

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

/* End of the debounce of a NO/NC switch. */
static void finish_pair_debounce(uint8_t idx, bool settled)
{
	struct input_state *in = &inputs[idx];
	enum pair_reading reading = in->candidate;

	/*
	 * Only a change that every sample agreed on counts. Anything else keeps
	 * the state; arm() catches a later real change.
	 */
	bool changed = settled && reading == PAIR_CHANGED;

	if (changed) {
		in->active = !in->active;
	}
	in->both_closed = reading == PAIR_BOTH_CLOSED;
	if (settled) {
		report_pair_problem(idx, reading);
	}
	arm(idx);

	if (changed) {
		LOG_DBG("%s: %s", labels[idx], in->active ? "closed" : "open");
		input_handler(idx, in->active);
	}
}

static void process_debounce(uint8_t idx)
{
	struct input_state *in = &inputs[idx];

	if (!in->debouncing) {
		return;
	}

	uint8_t sample = paired(idx) ? read_pair(idx) : read_pin(idx);

	if (in->total_samples == 0 || sample != in->candidate) {
		in->candidate = sample;
		in->stable_samples = 1;
	} else {
		in->stable_samples++;
	}
	in->total_samples++;

	bool settled = in->stable_samples >= CONFIG_APP_DEBOUNCE_SAMPLES;

	if (!settled && in->total_samples < DEBOUNCE_MAX_SAMPLES) {
		schedule_sample(idx);
		return;
	}

	in->debouncing = false;

	if (paired(idx)) {
		finish_pair_debounce(idx, settled);
		return;
	}

	bool active = sample;

	/* A pin that never settled keeps its state; arm() catches a real change. */
	bool changed = settled && active != in->active;

	if (changed) {
		in->active = active;
	}
	arm(idx);

	if (changed) {
		LOG_DBG("%s: %s", labels[idx], active ? "closed" : "open");
		input_handler(idx, active);
	}
}

static void process_poll(void)
{
	bool any_polling = false;

	/* One settle time for all closed latching switches. */
	for (uint8_t i = 0; i < NUM_INPUTS; i++) {
		if (inputs[i].polling && !paired(i)) {
			connect_pin(i);
		}
	}
	settle();

	for (uint8_t i = 0; i < NUM_INPUTS; i++) {
		struct input_state *in = &inputs[i];

		if (!in->polling) {
			continue;
		}

		if (paired(i)) {
			/* Both contacts were closed (see arm()). */
			if (read_pair(i) == PAIR_BOTH_CLOSED) {
				any_polling = true;
			} else {
				in->polling = false;
				start_debounce(i);
			}
			continue;
		}

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

		if (nc_available(i) && !gpio_is_ready_dt(&nc_specs[i])) {
			LOG_ERR("%s: NC GPIO not ready", labels[i]);
			return -ENODEV;
		}

		err = connect_pin(i);
		if (err) {
			LOG_ERR("%s: configure failed (%d)", labels[i], err);
			return err;
		}

		enum pair_reading wiring = read_initial_state(i);

		k_timer_init(&inputs[i].debounce_timer, debounce_expiry, NULL);

		gpio_init_callback(&inputs[i].cb, gpio_isr, BIT(specs[i].pin));
		err = gpio_add_callback_dt(&specs[i], &inputs[i].cb);
		if (!err && nc_available(i)) {
			gpio_init_callback(&inputs[i].nc_cb, gpio_nc_isr, BIT(nc_specs[i].pin));
			err = gpio_add_callback_dt(&nc_specs[i], &inputs[i].nc_cb);
		}
		if (err) {
			LOG_ERR("%s: add callback failed (%d)", labels[i], err);
			return err;
		}

		arm(i);
		report_pair_problem(i, wiring);

		LOG_INF("%s: %s switch%s, initially %s", labels[i], type_names[types[i]],
			nc_available(i) ? " (NO/NC possible)" : "",
			inputs[i].active ? "closed" : "open");
	}

	return 0;
}

uint8_t switch_input_count(void)
{
	return NUM_INPUTS;
}

void switch_input_set_type(uint8_t index, enum switch_type type)
{
	if (!switch_input_type_supported(index, type) || types[index] == type) {
		return;
	}

	struct input_state *in = &inputs[index];

	types[index] = type;

	/* Start over from a fresh sample, with the new type's power handling. */
	k_timer_stop(&in->debounce_timer);
	in->debouncing = false;
	gpio_pin_interrupt_configure_dt(&specs[index], GPIO_INT_DISABLE);
	if (nc_available(index)) {
		gpio_pin_interrupt_configure_dt(&nc_specs[index], GPIO_INT_DISABLE);
	}
	enum pair_reading wiring = read_initial_state(index);

	arm(index);

	LOG_INF("%s: now %s", labels[index], type_names[type]);
	report_pair_problem(index, wiring);
}

enum switch_type switch_input_get_type(uint8_t index)
{
	return index < NUM_INPUTS ? (enum switch_type)types[index] : SWITCH_TYPE_MOMENTARY;
}

bool switch_input_is_latching(uint8_t index)
{
	return switch_type_is_latching(switch_input_get_type(index));
}

bool switch_input_nc_available(uint8_t index)
{
	return index < NUM_INPUTS && nc_available(index);
}

bool switch_input_type_supported(uint8_t index, uint32_t type)
{
	return index < NUM_INPUTS && switch_type_is_valid(type) &&
	       (!switch_type_is_nonc(type) || nc_available(index));
}

bool switch_input_is_active(uint8_t index)
{
	return index < NUM_INPUTS && inputs[index].active;
}
