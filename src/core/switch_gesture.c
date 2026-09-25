/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "switch_gesture.h"
#include "switch_input.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(switch_gesture, CONFIG_LOG_DEFAULT_LEVEL);

enum gesture_state {
	GESTURE_IDLE,
	/* Pressed, long press timer running (first press only). */
	GESTURE_PRESSED,
	/* Held past the long press time. */
	GESTURE_LONG_PRESSED,
	/* Released, waiting for another press within the multi press window. */
	GESTURE_RELEASED,
};

struct gesture {
	struct k_timer timer;
	enum gesture_state state;
	uint8_t count;
	/*
	 * Incremented whenever the timer is (re)started or stopped. An expiry
	 * that was already queued before that carries an old value and is ignored.
	 */
	uint16_t generation;
};

static struct gesture gestures[SWITCH_INPUT_MAX];
static uint8_t gesture_count;
static switch_event_handler_t event_handler;

static void timer_expiry(struct k_timer *timer)
{
	struct gesture *g = CONTAINER_OF(timer, struct gesture, timer);

	app_loop_post(APP_EVT_GESTURE_TIMER, g - gestures, g->generation);
}

static void timer_stop(struct gesture *g)
{
	/* Stop first: an expiry racing with this still carries the old generation. */
	k_timer_stop(&g->timer);
	g->generation++;
}

static void timer_start(struct gesture *g, uint32_t ms)
{
	timer_stop(g);
	k_timer_start(&g->timer, K_MSEC(ms), K_NO_WAIT);
}

static void emit(uint8_t index, enum switch_event_type type, uint8_t position, uint8_t count)
{
	struct switch_event evt = {
		.type = type,
		.position = position,
		.count = count,
	};

	event_handler(index, &evt);
}

static void on_press(uint8_t index, struct gesture *g)
{
	switch (g->state) {
	case GESTURE_IDLE:
		g->count = 1;
		g->state = GESTURE_PRESSED;
		timer_start(g, CONFIG_APP_LONG_PRESS_MS);
		emit(index, SWITCH_EVENT_INITIAL_PRESS, SWITCH_POSITION_CLOSED, 0);
		break;

	case GESTURE_RELEASED:
		/* Next press of a multi press sequence; these are never long presses. */
		timer_stop(g);
		if (g->count < UINT8_MAX) {
			g->count++;
		}
		g->state = GESTURE_PRESSED;
		emit(index, SWITCH_EVENT_INITIAL_PRESS, SWITCH_POSITION_CLOSED, 0);
		if (g->count <= CONFIG_APP_MULTI_PRESS_MAX) {
			emit(index, SWITCH_EVENT_MULTI_PRESS_ONGOING, SWITCH_POSITION_CLOSED, g->count);
		}
		break;

	default:
		/* Already pressed, a press was lost. Nothing sensible to report. */
		break;
	}
}

static void on_release(uint8_t index, struct gesture *g)
{
	switch (g->state) {
	case GESTURE_PRESSED:
		g->state = GESTURE_RELEASED;
		timer_start(g, CONFIG_APP_MULTI_PRESS_WINDOW_MS);
		emit(index, SWITCH_EVENT_SHORT_RELEASE, SWITCH_POSITION_CLOSED, 0);
		break;

	case GESTURE_LONG_PRESSED:
		/* A long press ends the sequence without MultiPressComplete. */
		g->state = GESTURE_IDLE;
		g->count = 0;
		emit(index, SWITCH_EVENT_LONG_RELEASE, SWITCH_POSITION_CLOSED, 0);
		break;

	default:
		/* E.g. the switch was already held down at boot. */
		break;
	}
}

static void on_timer(uint8_t index, struct gesture *g)
{
	switch (g->state) {
	case GESTURE_PRESSED:
		g->state = GESTURE_LONG_PRESSED;
		emit(index, SWITCH_EVENT_LONG_PRESS, SWITCH_POSITION_CLOSED, 0);
		break;

	case GESTURE_RELEASED: {
		uint8_t total = g->count <= CONFIG_APP_MULTI_PRESS_MAX ? g->count : 0;

		g->state = GESTURE_IDLE;
		g->count = 0;
		emit(index, SWITCH_EVENT_MULTI_PRESS_COMPLETE, SWITCH_POSITION_CLOSED, total);
		break;
	}

	default:
		break;
	}
}

void switch_gesture_input(uint8_t index, bool active)
{
	if (index >= gesture_count) {
		return;
	}

	switch (switch_input_get_type(index)) {
	case SWITCH_TYPE_LATCHING:
		emit(index, SWITCH_EVENT_LATCHED,
		     active ? SWITCH_POSITION_CLOSED : SWITCH_POSITION_OPEN, 0);
		break;

	case SWITCH_TYPE_LATCHING_AS_PRESS:
		/*
		 * The position means nothing, only the change does: report every
		 * change as a complete short press. Changes within the multi press
		 * window count as a multi press, like quick presses of a button.
		 */
		on_press(index, &gestures[index]);
		on_release(index, &gestures[index]);
		break;

	case SWITCH_TYPE_MOMENTARY:
	default:
		if (active) {
			on_press(index, &gestures[index]);
		} else {
			on_release(index, &gestures[index]);
		}
		break;
	}
}

void switch_gesture_process(const struct app_evt *evt)
{
	if (evt->type != APP_EVT_GESTURE_TIMER || evt->index >= gesture_count) {
		return;
	}

	struct gesture *g = &gestures[evt->index];

	if (evt->arg != g->generation) {
		return;
	}

	on_timer(evt->index, g);
}

void switch_gesture_init(uint8_t count, switch_event_handler_t handler)
{
	gesture_count = MIN(count, SWITCH_INPUT_MAX);
	event_handler = handler;

	for (uint8_t i = 0; i < gesture_count; i++) {
		k_timer_init(&gestures[i].timer, timer_expiry, NULL);
		gestures[i].state = GESTURE_IDLE;
	}
}
