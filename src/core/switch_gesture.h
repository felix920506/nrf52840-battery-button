/*
 * SPDX-License-Identifier: MIT
 *
 * Turns debounced switch states into switch events. The events follow the
 * Matter Switch cluster semantics (they map 1:1 to its events), but nothing
 * here depends on Matter.
 *
 * Momentary switch (features MS, MSR, MSL, MSM), e.g. a double press:
 *   InitialPress, ShortRelease, InitialPress, MultiPressOngoing(2),
 *   ShortRelease, ... window elapses ..., MultiPressComplete(2)
 * A long press (only possible as the first press of a sequence):
 *   InitialPress, LongPress, LongRelease
 *
 * Latching switch (features MS, MSR, MSM): every change is a short press,
 * i.e. InitialPress, ShortRelease, then MultiPressComplete.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_loop.h"

#ifdef __cplusplus
extern "C" {
#endif

enum switch_event_type {
	/* Same values as the Matter Switch cluster event IDs (0 is SwitchLatched, unused). */
	SWITCH_EVENT_INITIAL_PRESS = 1,
	SWITCH_EVENT_LONG_PRESS,
	SWITCH_EVENT_SHORT_RELEASE,
	SWITCH_EVENT_LONG_RELEASE,
	SWITCH_EVENT_MULTI_PRESS_ONGOING,
	SWITCH_EVENT_MULTI_PRESS_COMPLETE,
};

/* Switch positions: 0 = released/open, 1 = pressed/closed. */
#define SWITCH_POSITION_OPEN   0
#define SWITCH_POSITION_CLOSED 1

struct switch_event {
	enum switch_event_type type;
	/* New position (latched, press events) or previous position (release, complete). */
	uint8_t position;
	/* Press count for multi press events; 0 in MultiPressComplete means
	 * more than CONFIG_APP_MULTI_PRESS_MAX presses.
	 */
	uint8_t count;
};

typedef void (*switch_event_handler_t)(uint8_t index, const struct switch_event *evt);

void switch_gesture_init(uint8_t count, switch_event_handler_t handler);

/* Feed a debounced state change of switch `index`. */
void switch_gesture_input(uint8_t index, bool active);

/* Abandon any sequence in progress, e.g. when the switch type changes. */
void switch_gesture_reset(uint8_t index);

/* Handles APP_EVT_GESTURE_TIMER. */
void switch_gesture_process(const struct app_evt *evt);

#ifdef __cplusplus
}
#endif
