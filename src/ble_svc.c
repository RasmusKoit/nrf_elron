#include "ble_svc.h"
#include "schedule.h"

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_power.h>
#include <string.h>

LOG_MODULE_REGISTER(ble_svc, LOG_LEVEL_INF);

/* 6e656c45-726f-6e21-0000-0000000000xx  ("elEron!" themed) */
#define ELRON_UUID_SVC \
	BT_UUID_128_ENCODE(0x6e656c45, 0x726f, 0x6e21, 0x0000, 0x000000000001)
#define ELRON_UUID_SCHED \
	BT_UUID_128_ENCODE(0x6e656c45, 0x726f, 0x6e21, 0x0000, 0x000000000002)
#define ELRON_UUID_CTRL \
	BT_UUID_128_ENCODE(0x6e656c45, 0x726f, 0x6e21, 0x0000, 0x000000000003)

static struct bt_uuid_128 svc_uuid   = BT_UUID_INIT_128(ELRON_UUID_SVC);
static struct bt_uuid_128 sched_uuid = BT_UUID_INIT_128(ELRON_UUID_SCHED);
static struct bt_uuid_128 ctrl_uuid  = BT_UUID_INIT_128(ELRON_UUID_CTRL);

/* Control char command bytes. */
#define ELRON_CMD_BOOTLOADER 0xB0   /* reboot into the UF2 bootloader */

static elron_ble_rx_cb user_cb;
static bool connected;
static bool adv_active;

/* Drop a connection that goes idle, so a phantom/half-open link (Windows leaves
 * these) can't stop us advertising. Rescheduled on every write. */
static struct bt_conn *cur_conn;
#define IDLE_DISCONNECT K_SECONDS(20)

static void idle_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (cur_conn) {
		LOG_INF("idle connection -> disconnecting");
		bt_conn_disconnect(cur_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}
static K_WORK_DELAYABLE_DEFINE(idle_work, idle_work_fn);

/* Reassembly buffer for chunked schedule writes (a week of departures). */
static uint8_t  rx_buf[1100];
static uint16_t rx_len;

/* Total expected v3 payload length given the bytes seen so far, or 0 if the
 * header isn't complete enough yet. Skips the u8-length-prefixed origin/dest/msg
 * + destination table, then adds count*7. */
static uint16_t wire_total_len(const uint8_t *b, uint16_t have)
{
	uint16_t p = 9;                 /* ver(1)+epoch(4)+tz(2)+walk(2) */
	for (int i = 0; i < 3; i++) {   /* origin, dest, msg */
		if (have < p + 1) return 0;
		p += 1 + b[p];
	}
	if (have < p + 1) return 0;     /* ndest */
	uint8_t nd = b[p++];
	for (uint8_t i = 0; i < nd; i++) {
		if (have < p + 1) return 0;
		p += 1 + b[p];
	}
	if (have < p + 1) return 0;     /* count */
	return p + 1 + b[p] * 7;
}

/* Chunked protocol: the companion sends the wire payload as small frames, each
 * [seq:u8][<=CHUNK data], so every GATT write fits a single ATT PDU (no MTU
 * negotiation, no long/prepared writes — which Windows' stack rejected). The
 * firmware places each chunk at seq*CHUNK and fires once the payload is whole. */
#define ELRON_CHUNK 18

static ssize_t sched_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	if (len < 1) {
		return len;
	}
	k_work_reschedule(&idle_work, IDLE_DISCONNECT);   /* activity: stay alive */
	const uint8_t *b = buf;
	uint16_t seq = b[0];
	uint16_t dlen = len - 1;
	uint16_t dest = seq * ELRON_CHUNK;

	if (dest + dlen > sizeof(rx_buf)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	/* seq 0 begins a fresh transfer: clear the high-water mark so a re-push or a
	 * dropped/gapped chunk can't complete against stale bytes from a prior one. */
	if (seq == 0) {
		rx_len = 0;
	}
	memcpy(&rx_buf[dest], &b[1], dlen);
	if (dest + dlen > rx_len) {
		rx_len = dest + dlen;
	}

	uint16_t total = wire_total_len(rx_buf, rx_len);
	if (user_cb && total != 0 && rx_len >= total) {
		LOG_INF("schedule complete: %u bytes", total);
		user_cb(rx_buf, total);
		rx_len = 0;
	}
	return len;
}

/* Reboot to the UF2 bootloader, deferred so the ATT write response goes out
 * first (otherwise the central reports the write as failed). */
static void reboot_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	nrf_power_gpregret_set(NRF_POWER, 0, 0x57);   /* Adafruit/Seeed "enter UF2" */
	sys_reboot(SYS_REBOOT_COLD);
}
static K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_work_fn);

static ssize_t ctrl_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(offset); ARG_UNUSED(flags);
	const uint8_t *b = buf;
	if (len >= 1 && b[0] == ELRON_CMD_BOOTLOADER) {
		LOG_INF("ctrl: reboot to bootloader requested");
		k_work_schedule(&reboot_work, K_MSEC(250));
	}
	return len;
}

/* GATT service: schedule (write) + control (write) characteristics. */
BT_GATT_SERVICE_DEFINE(elron_svc,
	BT_GATT_PRIMARY_SERVICE(&svc_uuid),
	BT_GATT_CHARACTERISTIC(&sched_uuid.uuid,
		BT_GATT_CHRC_WRITE,
		BT_GATT_PERM_WRITE,
		NULL, sched_write, NULL),
	BT_GATT_CHARACTERISTIC(&ctrl_uuid.uuid,
		BT_GATT_CHRC_WRITE,
		BT_GATT_PERM_WRITE,
		NULL, ctrl_write, NULL),
);

/* Manufacturer data: [company 0xFFFF][has_schedule]. The companion reads the
 * flag while scanning so it knows to (re)send after a reflash wipes the device. */
static uint8_t mfg_data[3] = { 0xFF, 0xFF, 0x00 };

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, sizeof(mfg_data)),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, ELRON_UUID_SVC),
};

void elron_ble_set_has_schedule(bool has)
{
	mfg_data[2] = has ? 1 : 0;
	if (adv_active && !connected) {
		(void)bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	}
}

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_WRN("connection failed (0x%02x)", err);
		return;
	}
	connected = true;
	rx_len = 0;
	cur_conn = bt_conn_ref(conn);
	k_work_reschedule(&idle_work, IDLE_DISCONNECT);
	LOG_INF("central connected");
}

static void start_adv(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad),
				  sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("adv start failed: %d", err);
		return;
	}
	adv_active = true;
	LOG_INF("advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	connected = false;
	rx_len = 0;
	k_work_cancel_delayable(&idle_work);
	if (cur_conn) {
		bt_conn_unref(cur_conn);
		cur_conn = NULL;
	}
	LOG_INF("central disconnected (0x%02x)", reason);
	start_adv();   /* otherwise the device goes silent after one connection */
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
};

static void bt_ready(int err)
{
	if (err) {
		LOG_ERR("bt enable failed: %d", err);
		return;
	}
	start_adv();
}

int elron_ble_init(elron_ble_rx_cb cb)
{
	user_cb = cb;
	return bt_enable(bt_ready);
}

bool elron_ble_connected(void)
{
	return connected;
}
