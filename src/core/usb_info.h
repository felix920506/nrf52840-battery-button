/*
 * SPDX-License-Identifier: MIT
 *
 * USB serial port showing the pairing code, pairing diagnostics and a few
 * single-key commands (CONFIG_APP_USB_INFO). Use core/diag.h to write to it.
 */

#pragma once

#include <stdbool.h>

#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start USB; it is enabled whenever USB power is present. `factory_reset`
 * is called when the factory reset command is confirmed.
 */
int usb_info_init(void (*factory_reset)(void));

/* A terminal opened the port (on APP_EVT_USB_TERMINAL): print the pairing information. */
void usb_info_print(void);

/* Show new diagnostic events (on APP_EVT_USB_DIAG). */
void usb_info_flush(void);

/* Handle a key typed in the terminal (on APP_EVT_USB_KEY). */
void usb_info_key(void);

/*
 * Keep the diagnostic history for the next start: call right before a
 * restart the device chooses (crash, factory reset). A power cycle loses it.
 */
void usb_info_retain(void);

/* True if this start kept the history of the previous run. */
bool usb_info_history_restored(void);

/* See core/diag.h. */
void usb_info_event(const char *fmt, ...) __printf_like(1, 2);
void usb_info_printf(const char *fmt, ...) __printf_like(1, 2);

#ifdef __cplusplus
}
#endif
