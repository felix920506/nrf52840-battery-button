/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB serial port that shows how to pair the device: when a terminal opens
 * the port, the pairing code and a QR code (drawn with text characters) are
 * printed. Opening the port again prints it again.
 *
 * USB is only enabled while USB power (VBUS) is present, so on battery this
 * costs no current.
 */

#include "usb_info.h"

#include "app_loop.h"
#include "transport/transport.h"
#include "third_party/qrcodegen/qrcodegen.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>

#include <hal/nrf_power.h>

LOG_MODULE_REGISTER(usb_info, CONFIG_LOG_DEFAULT_LEVEL);

/* Zephyr project VID with the PID the boards' CDC ACM console uses. */
#define USB_VID 0x2fe3
#define USB_PID 0x0004

/* A Matter payload (22 alphanumeric characters) fits into version 1 (21x21 modules). */
#define QR_MAX_VERSION 3
#define QR_QUIET_ZONE  2

static const struct device *const uart = DEVICE_DT_GET_ONE(zephyr_cdc_acm_uart);

USBD_DEVICE_DEFINE(usb_info_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), USB_VID, USB_PID);
USBD_DESC_LANG_DEFINE(usb_info_lang);
USBD_DESC_PRODUCT_DEFINE(usb_info_product, CONFIG_APP_PRODUCT_NAME);
USBD_DESC_CONFIG_DEFINE(usb_info_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(usb_info_config, 0, 100, &usb_info_cfg_desc);

static void put(const char *s)
{
	while (*s) {
		uart_poll_out(uart, *s++);
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

void usb_info_print(void)
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

/* Runs in the USB stack's context: only post to the application loop. */
static void usb_msg(struct usbd_context *const ctx, const struct usbd_msg *msg)
{
	uint32_t dtr = 0;

	switch (msg->type) {
	case USBD_MSG_VBUS_READY:
		(void)usbd_enable(ctx);
		break;
	case USBD_MSG_VBUS_REMOVED:
		(void)usbd_disable(ctx);
		break;
	case USBD_MSG_CDC_ACM_CONTROL_LINE_STATE:
		/* A terminal sets DTR when it opens the port. */
		if (uart_line_ctrl_get(msg->dev, UART_LINE_CTRL_DTR, &dtr) == 0 && dtr) {
			app_loop_post(APP_EVT_USB_TERMINAL, 0, 0);
		}
		break;
	default:
		break;
	}
}

int usb_info_init(void)
{
	int err;

	if (!device_is_ready(uart)) {
		return -ENODEV;
	}

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
