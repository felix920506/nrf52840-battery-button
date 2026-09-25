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
	bool latching[SWITCH_INPUT_MAX];
	/* Position at boot, for latching switches. */
	bool active[SWITCH_INPUT_MAX];
};

/* Start the stack. Returns once the device is ready to report events. */
int transport_init(const struct transport_switch_config *switches,
		   const struct battery_info *battery);

void transport_switch_event(uint8_t index, const struct switch_event *evt);

void transport_battery_update(const struct battery_state *state);

/* True once the device has been added to a network/controller. */
bool transport_is_provisioned(void);

/* Make the device discoverable for commissioning/pairing, if it is not already. */
void transport_start_pairing(void);

/* Erase all network credentials and settings, then reboot. */
void transport_factory_reset(void);

#ifdef __cplusplus
}
#endif
