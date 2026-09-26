/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Interface between the transport independent core (switches, battery) and
 * the radio protocol. Exactly one implementation is built, selected with
 * the APP_TRANSPORT Kconfig choice:
 *
 *   src/transport/matter/  Matter over Thread (CONFIG_APP_TRANSPORT_MATTER)
 *
 * To add Zigbee or BLE, implement these functions in a new directory, add a
 * Kconfig choice entry and add its sources in CMakeLists.txt.
 *
 * All functions are called from the application loop (main thread).
 *
 * A transport that lets the user change a switch's type (e.g. from the smart
 * home app) posts APP_EVT_SWITCH_TYPE with the switch index and the new
 * enum switch_type; the core applies it, stores it and then calls
 * transport_switch_type_changed().
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/battery.h"
#include "core/switch_gesture.h"
#include "core/switch_input.h"

#ifdef __cplusplus
extern "C" {
#endif

struct transport_switch_config {
	/* Number of switches, 1..SWITCH_INPUT_MAX. */
	uint8_t count;
	enum switch_type type[SWITCH_INPUT_MAX];
	/* Position at boot, for SWITCH_TYPE_LATCHING. */
	bool active[SWITCH_INPUT_MAX];
};

/* Start the stack. Returns once the device is ready to report events. */
int transport_init(const struct transport_switch_config *switches,
		   const struct battery_info *battery);

void transport_switch_event(uint8_t index, const struct switch_event *evt);

void transport_battery_update(const struct battery_state *state);

/* The type of switch `index` changed; `active` is its current state. */
void transport_switch_type_changed(uint8_t index, enum switch_type type, bool active);

/* True once the device has been added to a network/controller. */
bool transport_is_provisioned(void);

/* Make the device discoverable for commissioning/pairing, if it is not already. */
void transport_start_pairing(void);

/* Erase all network credentials and settings, then reboot. */
void transport_factory_reset(void);

/* How to pair the device, shown on the USB serial port. Empty strings are not shown. */
struct transport_pairing_info {
	/* Serial number or other identification. */
	char serial[33];
	/* Code to enter in the smart home app. */
	char code[24];
	/* Content of the pairing QR code. */
	char qr[32];
	/* Shown instead of the code if the device doesn't know it. */
	char hint[96];
	/* True once added to a network/controller. */
	bool provisioned;
};

/* Fills in `info`, which the caller has zeroed. */
void transport_get_pairing_info(struct transport_pairing_info *info);

#ifdef __cplusplus
}
#endif
