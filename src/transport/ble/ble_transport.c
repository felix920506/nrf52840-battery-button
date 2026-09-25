/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bluetooth LE transport: a connectable peripheral with
 *
 *   Battery Service (0x180F)  battery level in percent (read, notify)
 *   Switch Service            5a1d0001-6b8f-4c1e-9a52-0c4f0b8e0a11
 *     Switch Event            ...0002  (read, notify), 4 bytes:
 *                             [0] switch number, 1..6
 *                             [1] event, same IDs as the Matter Switch cluster:
 *                                 0 SwitchLatched, 1 InitialPress, 2 LongPress,
 *                                 3 ShortRelease, 4 LongRelease,
 *                                 5 MultiPressOngoing, 6 MultiPressComplete
 *                             [2] position (new or previous, see switch_gesture.h)
 *                             [3] press count (multi press events)
 *     Switch Info             ...0003  (read): [0] number of switches,
 *                             [1..n] switch type (0 momentary, 1 latching,
 *                             2 latching-as-press)
 *     Battery Voltage         ...0004  (read, notify): uint16 little endian, mV
 *
 * There is no pairing or provisioning; any central can connect. Events that
 * happen while nothing is connected are dropped.
 */

#include "transport/transport.h"

#include "core/status_led.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(ble_transport, CONFIG_LOG_DEFAULT_LEVEL);

/* Matter Switch cluster event IDs, which switch_event_type follows. */
BUILD_ASSERT(SWITCH_EVENT_LATCHED == 0 && SWITCH_EVENT_INITIAL_PRESS == 1 &&
	     SWITCH_EVENT_LONG_PRESS == 2 && SWITCH_EVENT_SHORT_RELEASE == 3 &&
	     SWITCH_EVENT_LONG_RELEASE == 4 && SWITCH_EVENT_MULTI_PRESS_ONGOING == 5 &&
	     SWITCH_EVENT_MULTI_PRESS_COMPLETE == 6);

#define SWITCH_UUID(n) BT_UUID_128_ENCODE(0x5a1d0000 + (n), 0x6b8f, 0x4c1e, 0x9a52, 0x0c4f0b8e0a11)

static const struct bt_uuid_128 switch_svc_uuid = BT_UUID_INIT_128(SWITCH_UUID(1));
static const struct bt_uuid_128 switch_event_uuid = BT_UUID_INIT_128(SWITCH_UUID(2));
static const struct bt_uuid_128 switch_info_uuid = BT_UUID_INIT_128(SWITCH_UUID(3));
static const struct bt_uuid_128 battery_voltage_uuid = BT_UUID_INIT_128(SWITCH_UUID(4));

static uint8_t switch_info[1 + SWITCH_INPUT_MAX];
static uint8_t switch_info_len;
static uint8_t last_event[4];
static uint8_t voltage_le[2];

static ssize_t read_value(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			  uint16_t len, uint16_t offset)
{
	const uint8_t *value = attr->user_data;
	uint16_t size = sizeof(last_event);

	if (value == switch_info) {
		size = switch_info_len;
	} else if (value == voltage_le) {
		size = sizeof(voltage_le);
	}

	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, size);
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	LOG_INF("Notifications %s", value == BT_GATT_CCC_NOTIFY ? "enabled" : "disabled");
}

BT_GATT_SERVICE_DEFINE(switch_svc,
	BT_GATT_PRIMARY_SERVICE(&switch_svc_uuid),
	BT_GATT_CHARACTERISTIC(&switch_event_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_value, NULL, last_event),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&switch_info_uuid.uuid, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, read_value, NULL, switch_info),
	BT_GATT_CHARACTERISTIC(&battery_voltage_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_value, NULL, voltage_le),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* Value attributes within switch_svc.attrs. */
#define ATTR_SWITCH_EVENT    2
#define ATTR_BATTERY_VOLTAGE 7

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, SWITCH_UUID(1)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void start_advertising(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN,
						  CONFIG_APP_BLE_ADV_INTERVAL_MS * 8 / 5,
						  CONFIG_APP_BLE_ADV_INTERVAL_MS * 8 / 5 + 32, NULL),
				  ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err && err != -EALREADY) {
		LOG_ERR("Advertising failed to start (%d)", err);
		return;
	}

	LOG_INF("Advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
}

static void adv_work_handler(struct k_work *work)
{
	start_advertising();
}

static K_WORK_DEFINE(adv_work, adv_work_handler);

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_WRN("Connection failed (0x%02x)", err);
		return;
	}

	LOG_INF("Connected");
	status_led_set_pairing(false);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected (0x%02x)", reason);
	status_led_set_pairing(true);
}

/* The connection object is free again: advertising can be restarted. */
static void recycled(void)
{
	k_work_submit(&adv_work);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
};

int transport_init(const struct transport_switch_config *switches,
		   const struct battery_info *battery)
{
	int err;

	switch_info[0] = switches->count;
	for (uint8_t i = 0; i < switches->count; i++) {
		switch_info[1 + i] = switches->type[i];
	}
	switch_info_len = 1 + switches->count;

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (%d)", err);
		return err;
	}

	LOG_INF("Battery: %s", battery->description);

	start_advertising();
	status_led_set_pairing(true);

	return 0;
}

void transport_switch_event(uint8_t index, const struct switch_event *evt)
{
	last_event[0] = index + 1;
	last_event[1] = evt->type;
	last_event[2] = evt->position;
	last_event[3] = evt->count;

	LOG_INF("Switch %u: event %u, position %u, count %u", last_event[0], last_event[1],
		last_event[2], last_event[3]);

	/* -ENOTCONN / -EINVAL just mean nobody is listening. */
	(void)bt_gatt_notify(NULL, &switch_svc.attrs[ATTR_SWITCH_EVENT], last_event,
			     sizeof(last_event));
}

void transport_battery_update(const struct battery_state *state)
{
	sys_put_le16(state->voltage_mv, voltage_le);

	(void)bt_bas_set_battery_level(state->percent);
	(void)bt_gatt_notify(NULL, &switch_svc.attrs[ATTR_BATTERY_VOLTAGE], voltage_le,
			     sizeof(voltage_le));
}

bool transport_is_provisioned(void)
{
	/* Nothing to provision: the device is always connectable. */
	return true;
}

void transport_start_pairing(void)
{
}

void transport_factory_reset(void)
{
	/* Nothing is stored; a reboot restores the initial state. */
	sys_reboot(SYS_REBOOT_COLD);
}
