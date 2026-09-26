/*
 * SPDX-License-Identifier: MIT
 *
 * GPIO handling for the external switches: interrupt wake-up, debouncing
 * and low-power polling of closed latching switches.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_loop.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_INPUT_MAX 6

/*
 * Switch types. The values are stored in settings and used as Matter Mode
 * Select modes, so they must not change. Value 1 was a latching type that
 * reported its position; it was removed (controllers such as Home Assistant
 * can't follow a switch changing to it), and stored 1s are migrated to
 * SWITCH_TYPE_LATCHING_AS_PRESS.
 */
enum switch_type {
	/* Push button: closed only while held. */
	SWITCH_TYPE_MOMENTARY = 0,
	/* Latching (rocker, toggle, ...): every change is reported as a short press. */
	SWITCH_TYPE_LATCHING_AS_PRESS = 2,
};

static inline bool switch_type_is_valid(uint32_t type)
{
	return type == SWITCH_TYPE_MOMENTARY || type == SWITCH_TYPE_LATCHING_AS_PRESS;
}

/* Called from the application loop with the debounced state of a switch. */
typedef void (*switch_input_handler_t)(uint8_t index, bool active);

int switch_input_init(switch_input_handler_t handler);

/* Number of switches defined in devicetree. */
uint8_t switch_input_count(void);

enum switch_type switch_input_get_type(uint8_t index);

/*
 * Change the type of a switch at runtime. The pin is sampled again and its
 * state becomes the new debounced state, without reporting a change.
 */
void switch_input_set_type(uint8_t index, enum switch_type type);

/* True if the switch stays closed on its own (SWITCH_TYPE_LATCHING_AS_PRESS). */
bool switch_input_is_latching(uint8_t index);

/* Debounced state: true while the switch is pressed/closed. */
bool switch_input_is_active(uint8_t index);

/* Handles APP_EVT_INPUT_* events. */
void switch_input_process(const struct app_evt *evt);

#ifdef __cplusplus
}
#endif
