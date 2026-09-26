/*
 * SPDX-License-Identifier: MIT
 *
 * Status LED patterns. Safe to call from any thread.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int status_led_init(void);

/* Short blink every 2 s while the device can be commissioned/paired. */
void status_led_set_pairing(bool on);

/* Steady 2 Hz blink while identify is active. */
void status_led_set_identify(bool on);

/* Flash `times` times quickly, then return to the current pattern. */
void status_led_flash(uint8_t times);

#ifdef __cplusplus
}
#endif
