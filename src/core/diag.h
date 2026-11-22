/*
 * SPDX-License-Identifier: MIT
 *
 * Pairing diagnostics for the person setting up the device. Smart home apps
 * usually only say "pairing failed"; these messages say which step failed
 * and what to check.
 *
 *   diag_event()  Something happened (pairing step, error). Callable from
 *                 any thread except interrupts. Kept in a short history and
 *                 shown live while a terminal has the USB serial port open.
 *   diag_print()  Answer to a console command. Application loop only.
 *
 * Both take printf arguments; the text has no trailing newline. Builds
 * without the pairing console (usb-logging.conf, debug.conf) print them
 * with printk.
 */

#pragma once

#ifdef CONFIG_APP_USB_INFO

#include "core/usb_info.h"

#define diag_event(...) usb_info_event(__VA_ARGS__)
#define diag_print(...) usb_info_printf(__VA_ARGS__)

#else

#include <zephyr/sys/printk.h>

#define diag_event(fmt, ...) printk(fmt "\n", ##__VA_ARGS__)
#define diag_print(fmt, ...) printk(fmt "\n", ##__VA_ARGS__)

#endif
