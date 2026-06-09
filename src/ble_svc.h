/*
 * Custom GATT service for receiving an Elron schedule push from the companion.
 *
 * Service UUID:        6e656c45-726f-6e21-0000-000000000001
 * Schedule char (W):   6e656c45-726f-6e21-0000-000000000002  (write / write-long)
 *
 * The companion writes the wire payload described in schedule.h to the schedule
 * characteristic. A registered callback is invoked once the full value lands.
 */
#ifndef ELRON_BLE_SVC_H_
#define ELRON_BLE_SVC_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Called (from the BT RX context) when a complete schedule payload is written. */
typedef void (*elron_ble_rx_cb)(const uint8_t *buf, size_t len);

/* Start advertising and register the schedule write callback. */
int elron_ble_init(elron_ble_rx_cb cb);

/* True while a central is connected. */
bool elron_ble_connected(void);

/* Advertise whether the device currently holds a schedule (so the companion
 * can detect a freshly-reflashed/empty device and resend). */
void elron_ble_set_has_schedule(bool has);

#endif /* ELRON_BLE_SVC_H_ */
