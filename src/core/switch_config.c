/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "switch_config.h"

#include <stdio.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(switch_config, CONFIG_LOG_DEFAULT_LEVEL);

#define SUBTREE "bsw/type"

static int load_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg, void *param)
{
	uint8_t type;
	long index = strtol(key, NULL, 10);

	if (len != sizeof(type) || read_cb(cb_arg, &type, sizeof(type)) != sizeof(type)) {
		return 0;
	}

	if (index < 0 || index >= switch_input_count() || type > SWITCH_TYPE_LATCHING_AS_PRESS) {
		return 0;
	}

	switch_input_set_type((uint8_t)index, (enum switch_type)type);
	return 0;
}

int switch_config_load(void)
{
	int err = settings_subsys_init();

	if (err) {
		LOG_ERR("Settings init failed (%d)", err);
		return err;
	}

	return settings_load_subtree_direct(SUBTREE, load_cb, NULL);
}

int switch_config_save(uint8_t index, enum switch_type type)
{
	char key[sizeof(SUBTREE) + 4];
	uint8_t value = type;

	snprintf(key, sizeof(key), SUBTREE "/%u", index);
	return settings_save_one(key, &value, sizeof(value));
}
