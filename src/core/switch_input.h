/*
 * SPDX-License-Identifier: Apache-2.0
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

/* Called from the application loop with the debounced state of a switch. */
typedef void (*switch_input_handler_t)(uint8_t index, bool active);

int switch_input_init(switch_input_handler_t handler);

/* Number of switches defined in devicetree. */
uint8_t switch_input_count(void);

bool switch_input_is_latching(uint8_t index);

/* Debounced state: true while the switch is pressed/closed. */
bool switch_input_is_active(uint8_t index);

/* Handles APP_EVT_INPUT_* events. */
void switch_input_process(const struct app_evt *evt);

#ifdef __cplusplus
}
#endif
