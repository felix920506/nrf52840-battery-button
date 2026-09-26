/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Factory reset through a dedicated pad that is not a switch input
 * (devicetree: `factory-reset-gpios` in the `zephyr,user` node). The reset
 * triggers only when the pad is held to GND continuously for
 * CONFIG_APP_FACTORY_RESET_HOLD_MS, so normal switch use can never cause it.
 */

#pragma once

#include "app_loop.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*reset_pin_handler_t)(void);

/* Returns -ENOTSUP if the board defines no reset pad. */
int reset_pin_init(reset_pin_handler_t on_reset);

/* Handles APP_EVT_RESET_PIN and APP_EVT_FACTORY_RESET_TIMER. */
void reset_pin_process(const struct app_evt *evt);

#ifdef __cplusplus
}
#endif
