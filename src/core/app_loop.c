/*
 * SPDX-License-Identifier: MIT
 */

#include "app_loop.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_loop, CONFIG_LOG_DEFAULT_LEVEL);

K_MSGQ_DEFINE(app_msgq, sizeof(struct app_evt), CONFIG_APP_EVENT_QUEUE_SIZE, 4);

int app_loop_post(uint8_t type, uint8_t index, uint16_t arg)
{
	struct app_evt evt = {
		.type = type,
		.index = index,
		.arg = arg,
	};
	int err = k_msgq_put(&app_msgq, &evt, K_NO_WAIT);

	if (err) {
		LOG_ERR("Event queue full, dropped event %u", type);
	}

	return err;
}

void app_loop_wait(struct app_evt *evt)
{
	k_msgq_get(&app_msgq, evt, K_FOREVER);
}
