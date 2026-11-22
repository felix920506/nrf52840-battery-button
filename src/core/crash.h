/*
 * SPDX-License-Identifier: MIT
 *
 * Crash record (CONFIG_APP_CRASH_RESTART): a fatal error is recorded in RAM
 * that survives the restart that follows, so the next start can report it
 * on the USB serial port. A crash while pairing would otherwise look like
 * a pairing attempt that just stopped.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct crash_info {
	/* Zephyr fatal error reason (K_ERR_*). */
	uint32_t reason;
	/* Program counter of the faulting code, 0 if unknown. */
	uint32_t pc;
};

#ifdef CONFIG_APP_CRASH_RESTART

/* The crash that ended the previous run, if it ended in one. */
bool crash_get_last(struct crash_info *info);

/* Short description of a K_ERR_* reason. */
const char *crash_reason_name(uint32_t reason);

#else

static inline bool crash_get_last(struct crash_info *info)
{
	(void)info;
	return false;
}

static inline const char *crash_reason_name(uint32_t reason)
{
	(void)reason;
	return "";
}

#endif

#ifdef __cplusplus
}
#endif
