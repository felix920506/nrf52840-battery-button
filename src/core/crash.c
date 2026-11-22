/*
 * SPDX-License-Identifier: MIT
 *
 * Replaces the nRF Connect SDK's reset-on-fatal-error handler
 * (CONFIG_RESET_ON_FATAL_ERROR, which is off for this): it also restarts
 * the device, but first records why, and keeps the pairing diagnostics
 * history for the next start.
 */

#include "crash.h"

#ifdef CONFIG_APP_USB_INFO
#include "usb_info.h"
#endif

#include <zephyr/arch/cpu.h>
#include <zephyr/fatal.h>
#include <zephyr/init.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/toolchain.h>

#define CRASH_MAGIC 0x43525348 /* "CRSH" */

/* Not initialized at boot, so it survives the restart. */
static struct {
	uint32_t magic;
	uint32_t reason;
	uint32_t pc;
	uint32_t check;
} record __noinit;

static struct crash_info last;
static bool have_last;

extern void sys_arch_reboot(int type);

bool crash_get_last(struct crash_info *info)
{
	if (have_last) {
		*info = last;
	}
	return have_last;
}

const char *crash_reason_name(uint32_t reason)
{
	switch (reason) {
	case K_ERR_CPU_EXCEPTION:
		return "CPU exception";
	case K_ERR_SPURIOUS_IRQ:
		return "unhandled interrupt";
	case K_ERR_STACK_CHK_FAIL:
		return "stack overflow";
	case K_ERR_KERNEL_OOPS:
		return "kernel oops";
	case K_ERR_KERNEL_PANIC:
		return "kernel panic or failed assertion";
	default:
		return "fatal error";
	}
}

/* Before main() and the Matter thread: take the record left by the previous run. */
static int crash_restore(void)
{
	/* After power-on, RAM is random: the check makes a false match unlikely. */
	if (record.magic == CRASH_MAGIC && record.check == ~(record.reason ^ record.pc)) {
		last.reason = record.reason;
		last.pc = record.pc;
		have_last = true;
	}
	record.magic = 0;
	return 0;
}

SYS_INIT(crash_restore, POST_KERNEL, 0);

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	record.reason = reason;
	record.pc = esf ? esf->basic.pc : 0;
	record.check = ~(record.reason ^ record.pc);
	record.magic = CRASH_MAGIC;

#ifdef CONFIG_APP_USB_INFO
	usb_info_retain();
#endif

	LOG_PANIC();
	sys_arch_reboot(0);

	CODE_UNREACHABLE;
}
