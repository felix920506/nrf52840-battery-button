/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Persistent switch types. Devicetree provides the defaults; a type changed at
 * runtime (e.g. from the smart home app) is stored in settings and restored
 * at boot. Stored under "bsw/type/<index>".
 */

#pragma once

#include <stdint.h>

#include "switch_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Restore stored types (applied with switch_input_set_type()). */
int switch_config_load(void);

int switch_config_save(uint8_t index, enum switch_type type);

#ifdef __cplusplus
}
#endif
