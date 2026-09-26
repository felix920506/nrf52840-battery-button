/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Single application event loop. Interrupts and timers only post small
 * events here; all application logic runs in the main thread, so the core
 * modules need no locking.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum app_evt_type {
	/* switch_input: a GPIO level interrupt fired for input `index`. */
	APP_EVT_INPUT_IRQ,
	/* switch_input: debounce time of input `index` elapsed. */
	APP_EVT_INPUT_DEBOUNCE,
	/* switch_input: time to sample closed latching switches. */
	APP_EVT_INPUT_POLL,
	/* switch_gesture: gesture timer of switch `index` expired, `arg` is its generation. */
	APP_EVT_GESTURE_TIMER,
	/* Periodic battery measurement. */
	APP_EVT_BATTERY,
	/* Transport: change the type of switch `index` to `arg` (enum switch_type). */
	APP_EVT_SWITCH_TYPE,
	/* reset_pin: the factory reset pad changed level. */
	APP_EVT_RESET_PIN,
	/* reset_pin: hold time of the reset pad elapsed, `arg` is its generation. */
	APP_EVT_FACTORY_RESET_TIMER,
	/* usb_info: a terminal opened the USB serial port. */
	APP_EVT_USB_TERMINAL,
};

struct app_evt {
	uint8_t type;
	uint8_t index;
	uint16_t arg;
};

/* Post an event to the application loop. Safe to call from interrupts. */
int app_loop_post(uint8_t type, uint8_t index, uint16_t arg);

/* Blocks until the next event is available and returns it. */
void app_loop_wait(struct app_evt *evt);

#ifdef __cplusplus
}
#endif
