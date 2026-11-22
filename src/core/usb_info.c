/*
 * SPDX-License-Identifier: MIT
 *
 * USB serial port for setting the device up. When a terminal opens the
 * port, the pairing code and a QR code (drawn with text characters) are
 * printed. Opening the port again prints it again.
 *
 * While the port is open, pairing diagnostics from the transport
 * (core/diag.h) are shown as they happen, and single keys run commands:
 * show the status, start or stop pairing mode, factory reset. Smart home
 * apps often report only "pairing failed"; this shows which step failed.
 *
 * USB is only enabled while USB power (VBUS) is present, so on battery this
 * costs no current.
 */

#include "usb_info.h"

#include "app_loop.h"
#include "transport/transport.h"
#include "third_party/qrcodegen/qrcodegen.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usbd.h>

#include <hal/nrf_power.h>

LOG_MODULE_REGISTER(usb_info, CONFIG_LOG_DEFAULT_LEVEL);

/* Zephyr project VID with the PID the boards' CDC ACM console uses. */
#define USB_VID 0x2fe3
#define USB_PID 0x0004

/* A Matter payload (22 alphanumeric characters) fits into version 1 (21x21 modules). */
#define QR_MAX_VERSION 3
#define QR_QUIET_ZONE  2

/* Recent diagnostic events, shown with the L command. */
#define DIAG_LINES    16
#define DIAG_LINE_LEN 112

/* Pause after this many bytes so the 2 KB USB transmit buffer can drain. */
#define TX_CHUNK 512

/* How long the factory reset command waits for its confirmation. */
#define RESET_CONFIRM_MS 10000

static const struct device *const uart = DEVICE_DT_GET_ONE(zephyr_cdc_acm_uart);

USBD_DEVICE_DEFINE(usb_info_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), USB_VID, USB_PID);
USBD_DESC_LANG_DEFINE(usb_info_lang);
USBD_DESC_PRODUCT_DEFINE(usb_info_product, CONFIG_APP_PRODUCT_NAME);
USBD_DESC_CONFIG_DEFINE(usb_info_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(usb_info_config, 0, 100, &usb_info_cfg_desc);

/* Set while a terminal has the port open (DTR set). */
static atomic_t terminal_open;
/* Last key typed and not handled yet, 0 if none. */
static atomic_t pending_key;
/* APP_EVT_USB_DIAG has been posted and not handled yet. */
static atomic_t flush_pending;

static void (*factory_reset_cb)(void);
static int64_t reset_requested_at = -1;

/*
 * Diagnostic history: diag_head counts the lines ever written, diag_shown
 * the ones already printed live. Written from any thread, read in the
 * application loop.
 */
static char diag_ring[DIAG_LINES][DIAG_LINE_LEN];
static uint32_t diag_head;
static uint32_t diag_shown;
static struct k_spinlock diag_lock;

static size_t tx_count;

/*
 * Without hardware flow control the CDC ACM driver drops what doesn't fit
 * into its transmit buffer. Long outputs pause briefly so the host can
 * read it; a terminal reads far faster than this.
 */
static void put(const char *s)
{
	while (*s) {
		uart_poll_out(uart, *s++);
		if (++tx_count >= TX_CHUNK) {
			tx_count = 0;
			k_msleep(5);
		}
	}
}

/*
 * Two rows of modules per text line with half block characters. Colours
 * are set explicitly (black on white) so the code scans on dark terminals.
 */
static void put_qr(const char *payload)
{
	static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
	static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];

	if (!qrcodegen_encodeText(payload, tmp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
				  QR_MAX_VERSION, qrcodegen_Mask_AUTO, true)) {
		return;
	}

	int size = qrcodegen_getSize(qr);

	for (int y = -QR_QUIET_ZONE; y < size + QR_QUIET_ZONE; y += 2) {
		put("  \x1b[30;107m");
		for (int x = -QR_QUIET_ZONE; x < size + QR_QUIET_ZONE; x++) {
			bool top = qrcodegen_getModule(qr, x, y);
			bool bottom = qrcodegen_getModule(qr, x, y + 1);

			put(top ? (bottom ? "█" : "▀") : (bottom ? "▄" : " "));
		}
		put("\x1b[0m\r\n");
	}
}

static void put_pairing_info(void)
{
	struct transport_pairing_info info;

	memset(&info, 0, sizeof(info));
	transport_get_pairing_info(&info);

	put("\r\n" CONFIG_APP_PRODUCT_NAME "\r\n");
	if (info.serial[0]) {
		put("Serial number: ");
		put(info.serial);
		put("\r\n");
	}
	put(info.provisioned ? "Status: paired\r\n" : "Status: not paired yet\r\n");

	if (info.qr[0]) {
		put("\r\nScan this QR code in your smart home app, or enter the code below.\r\n\r\n");
		put_qr(info.qr);
		put("\r\n  QR code: ");
		put(info.qr);
		put("\r\n");
	}
	if (info.code[0]) {
		put("  Pairing code: ");
		put(info.code);
		put("\r\n");
	} else if (info.hint[0]) {
		put(info.hint);
		put("\r\n");
	}
	put("\r\n");
}

static void put_help(void)
{
	put("Commands (press the key):\r\n"
	    "  S  status: pairing, network, controllers, last pairing attempt\r\n"
	    "  L  recent pairing events\r\n"
	    "  P  start pairing mode (Bluetooth advertising)\r\n"
	    "  X  stop pairing mode\r\n"
	    "  I  show the pairing code and QR code again\r\n"
	    "  F  factory reset (asks for confirmation)\r\n"
	    "  H  this help\r\n\r\n");
}

void usb_info_print(void)
{
	k_spinlock_key_t key = k_spin_lock(&diag_lock);
	uint32_t earlier = MIN(diag_head, DIAG_LINES);

	/* Live output starts now; earlier events are one key away. */
	diag_shown = diag_head;
	k_spin_unlock(&diag_lock, key);

	put_pairing_info();
	put("Pairing events are shown here as they happen. Press H for commands");
	if (earlier) {
		char buf[48];

		snprintf(buf, sizeof(buf), ", L for the last %u events", (unsigned int)earlier);
		put(buf);
	}
	put(".\r\n\r\n");
}

void usb_info_event(const char *fmt, ...)
{
	char line[DIAG_LINE_LEN];
	uint32_t ms = k_uptime_get_32();
	int len;
	va_list ap;

	len = snprintf(line, sizeof(line), "[%4u.%u s] ", ms / 1000, (ms % 1000) / 100);
	va_start(ap, fmt);
	vsnprintf(line + len, sizeof(line) - len, fmt, ap);
	va_end(ap);

	k_spinlock_key_t key = k_spin_lock(&diag_lock);

	memcpy(diag_ring[diag_head % DIAG_LINES], line, sizeof(line));
	diag_head++;
	k_spin_unlock(&diag_lock, key);

	if (atomic_get(&terminal_open) && atomic_cas(&flush_pending, 0, 1)) {
		if (app_loop_post(APP_EVT_USB_DIAG, 0, 0)) {
			atomic_clear(&flush_pending);
		}
	}
}

void usb_info_printf(const char *fmt, ...)
{
	char line[DIAG_LINE_LEN + 16];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	put(line);
	put("\r\n");
}

/* Prints history lines `from` up to the newest one. */
static void put_diag_lines(uint32_t from)
{
	char line[DIAG_LINE_LEN];

	for (;;) {
		k_spinlock_key_t key = k_spin_lock(&diag_lock);

		if (from >= diag_head) {
			k_spin_unlock(&diag_lock, key);
			return;
		}
		if (diag_head - from > DIAG_LINES) {
			from = diag_head - DIAG_LINES;
		}
		memcpy(line, diag_ring[from % DIAG_LINES], sizeof(line));
		from++;
		k_spin_unlock(&diag_lock, key);

		put(line);
		put("\r\n");
	}
}

void usb_info_flush(void)
{
	atomic_clear(&flush_pending);

	if (!atomic_get(&terminal_open)) {
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&diag_lock);
	uint32_t from = diag_shown;

	if (diag_head - from > DIAG_LINES) {
		k_spin_unlock(&diag_lock, key);
		put("(some events were not shown, press L for the latest)\r\n");
		key = k_spin_lock(&diag_lock);
		from = diag_head - DIAG_LINES;
	}
	diag_shown = diag_head;
	k_spin_unlock(&diag_lock, key);

	put_diag_lines(from);
}

static void show_log(void)
{
	uint32_t head = diag_head;

	if (head == 0) {
		put("No pairing events since the device started.\r\n\r\n");
		return;
	}
	put("Recent pairing events (time since start):\r\n");
	put_diag_lines(head > DIAG_LINES ? head - DIAG_LINES : 0);
	put("\r\n");
}

static void start_pairing(void)
{
	if (transport_is_provisioned()) {
		put("The device is already paired. To pair it with a different controller,\r\n"
		    "remove it in the app it is paired with, or factory reset it (F).\r\n\r\n");
		return;
	}
	if (transport_is_pairing()) {
		put("Pairing mode is already on.\r\n\r\n");
		return;
	}
	put("Starting pairing mode...\r\n");
	transport_start_pairing();
}

static void stop_pairing(void)
{
	if (!transport_is_pairing()) {
		put("Pairing mode is not on.\r\n\r\n");
		return;
	}
	put("Stopping pairing mode...\r\n");
	transport_stop_pairing();
}

void usb_info_key(void)
{
	int c = toupper((int)atomic_set(&pending_key, 0));
	bool confirm = reset_requested_at >= 0 &&
		       k_uptime_get() - reset_requested_at < RESET_CONFIRM_MS;

	reset_requested_at = -1;
	if (c == 0 || !atomic_get(&terminal_open)) {
		return;
	}

	if (confirm) {
		if (c == 'Y') {
			put("Factory reset: erasing pairing and network settings, then restarting.\r\n"
			    "The pairing code stays the same.\r\n");
			k_msleep(50);
			factory_reset_cb();
		} else {
			put("Factory reset cancelled.\r\n\r\n");
		}
		return;
	}

	switch (c) {
	case 'S':
		put("\r\n");
		transport_print_status();
		put("\r\n");
		break;
	case 'L':
		put("\r\n");
		show_log();
		break;
	case 'P':
		start_pairing();
		break;
	case 'X':
		stop_pairing();
		break;
	case 'I':
		put_pairing_info();
		break;
	case 'F':
		put("Factory reset removes the device from all controllers and networks.\r\n"
		    "Press Y within 10 seconds to confirm, any other key to cancel.\r\n");
		reset_requested_at = k_uptime_get();
		break;
	case 'H':
	case '?':
		put("\r\n");
		put_help();
		break;
	default:
		put("Unknown command. Press H for help.\r\n");
		break;
	}
}

/* Runs in the CDC ACM driver's work queue: keep the newest key for the application loop. */
static void uart_rx(const struct device *dev, void *user_data)
{
	uint8_t buf[16];
	int len;

	if (!uart_irq_update(dev)) {
		return;
	}

	while (uart_irq_rx_ready(dev) && (len = uart_fifo_read(dev, buf, sizeof(buf))) > 0) {
		for (int i = 0; i < len; i++) {
			/* Ignore line endings and other control characters. */
			if (buf[i] <= ' ' || buf[i] >= 0x7f) {
				continue;
			}
			/* Keys typed faster than they are handled are dropped. */
			if (atomic_cas(&pending_key, 0, buf[i]) &&
			    app_loop_post(APP_EVT_USB_KEY, 0, 0)) {
				atomic_clear(&pending_key);
			}
		}
	}
}

/* Runs in the USB stack's context: only post to the application loop. */
static void usb_msg(struct usbd_context *const ctx, const struct usbd_msg *msg)
{
	uint32_t dtr = 0;

	switch (msg->type) {
	case USBD_MSG_VBUS_READY:
		(void)usbd_enable(ctx);
		break;
	case USBD_MSG_VBUS_REMOVED:
		atomic_clear(&terminal_open);
		(void)usbd_disable(ctx);
		break;
	case USBD_MSG_CDC_ACM_CONTROL_LINE_STATE:
		/* A terminal sets DTR when it opens the port and clears it when it closes it. */
		if (uart_line_ctrl_get(msg->dev, UART_LINE_CTRL_DTR, &dtr) != 0) {
			break;
		}
		if (dtr && !atomic_set(&terminal_open, 1)) {
			app_loop_post(APP_EVT_USB_TERMINAL, 0, 0);
		} else if (!dtr) {
			atomic_clear(&terminal_open);
		}
		break;
	default:
		break;
	}
}

int usb_info_init(void (*factory_reset)(void))
{
	int err;

	if (!device_is_ready(uart)) {
		return -ENODEV;
	}

	factory_reset_cb = factory_reset;
	uart_irq_callback_set(uart, uart_rx);
	uart_irq_rx_enable(uart);

	err = usbd_add_descriptor(&usb_info_usbd, &usb_info_lang);
	if (!err) {
		err = usbd_add_descriptor(&usb_info_usbd, &usb_info_product);
	}
	if (!err) {
		err = usbd_add_configuration(&usb_info_usbd, USBD_SPEED_FS, &usb_info_config);
	}
	if (!err) {
		err = usbd_register_all_classes(&usb_info_usbd, USBD_SPEED_FS, 1, NULL);
	}
	if (!err) {
		/* CDC ACM has two interfaces: use the Interface Association Descriptor. */
		err = usbd_device_set_code_triple(&usb_info_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS,
						  0x02, 0x01);
	}
	if (!err) {
		err = usbd_msg_register_cb(&usb_info_usbd, usb_msg);
	}
	if (!err) {
		err = usbd_init(&usb_info_usbd);
	}
	/*
	 * The driver reports VBUS changes only. After a soft reset (e.g. by
	 * the bootloader after flashing) USB power is already there, so check.
	 */
	if (!err && (!usbd_can_detect_vbus(&usb_info_usbd) ||
		     nrf_power_usbregstatus_vbusdet_get(NRF_POWER))) {
		err = usbd_enable(&usb_info_usbd);
	}

	if (err) {
		LOG_ERR("USB init failed (%d)", err);
	}
	return err;
}
