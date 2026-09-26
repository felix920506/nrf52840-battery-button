/*
 * SPDX-License-Identifier: MIT
 *
 * USB serial port showing the pairing code (CONFIG_APP_USB_INFO).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Start USB; it is enabled whenever USB power is present. */
int usb_info_init(void);

/* Print the pairing information (on APP_EVT_USB_TERMINAL). */
void usb_info_print(void);

#ifdef __cplusplus
}
#endif
