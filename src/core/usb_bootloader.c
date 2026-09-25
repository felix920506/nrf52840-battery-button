/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Development helper for builds with the USB console (usb-logging.conf):
 * reboot into the Adafruit bootloader when the host opens the USB serial
 * port at 1200 baud and closes it again (the "1200 baud touch" used by the
 * Arduino tools). This allows reflashing without pressing reset:
 *
 *   adafruit-nrfutil dfu serial --touch 1200 --singlebank -b 115200 \
 *                    -p /dev/cu.usbmodemXXXX -pkg pkg.zip
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <hal/nrf_power.h>

/* GPREGRET values understood by the Adafruit nRF52 bootloader. */
#define DFU_MAGIC_SERIAL_ONLY_RESET 0x4e
#define DFU_MAGIC_UF2_RESET         0x57

#ifdef CONFIG_APP_USB_BOOTLOADER_MODE_UF2
#define DFU_MAGIC DFU_MAGIC_UF2_RESET
#else
#define DFU_MAGIC DFU_MAGIC_SERIAL_ONLY_RESET
#endif

static const struct device *const console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static void check_touch(struct k_timer *timer)
{
	uint32_t baud = 0;
	uint32_t dtr = 1;

	if (uart_line_ctrl_get(console, UART_LINE_CTRL_BAUD_RATE, &baud) ||
	    uart_line_ctrl_get(console, UART_LINE_CTRL_DTR, &dtr)) {
		return;
	}

	if (baud == 1200 && !dtr) {
		nrf_power_gpregret_set(NRF_POWER, 0, DFU_MAGIC);
		NVIC_SystemReset();
	}
}

static K_TIMER_DEFINE(touch_timer, check_touch, NULL);

static int usb_bootloader_init(void)
{
	if (device_is_ready(console)) {
		k_timer_start(&touch_timer, K_MSEC(250), K_MSEC(250));
	}

	return 0;
}

SYS_INIT(usb_bootloader_init, APPLICATION, 99);
