/*
 * Schedule model + persistence (v3).
 *
 * Holds up to a week of Elron departures as ABSOLUTE unix timestamps, so the
 * device shows the right "next train" across days with no re-sync. Each carries
 * its travel time, final destination, and an express flag. Persisted to flash
 * (settings/NVS) so it survives power-cycles offline.
 *
 * The companion also sends a walk time (minutes to reach the station); the
 * device shows when to *start walking* to catch the soonest reachable train.
 */
#ifndef ELRON_SCHEDULE_H_
#define ELRON_SCHEDULE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ELRON_MAX_DEPARTURES 120
#define ELRON_NAME_LEN       16
#define ELRON_MAX_DESTS      12
#define ELRON_DESTNAME_LEN   12
#define ELRON_MSG_LEN        40

#define ELRON_FLAG_EXPRESS   0x01

struct elron_departure {
	uint32_t dep_epoch;   /* UTC unix seconds of departure */
	uint8_t  dur_min;     /* travel time to destination, minutes */
	uint8_t  dest_idx;    /* index into elron_schedule.dests */
	uint8_t  flags;       /* ELRON_FLAG_* */
};

struct elron_schedule {
	char     origin[ELRON_NAME_LEN];
	char     dest[ELRON_NAME_LEN];
	uint32_t synced_epoch;
	int16_t  tz_off_min;          /* local offset from UTC, for HH:MM display */
	uint16_t walk_min;            /* minutes to walk to the station */
	char     msg[ELRON_MSG_LEN];  /* disruption banner, "" if none */
	uint8_t  ndest;
	char     dests[ELRON_MAX_DESTS][ELRON_DESTNAME_LEN];
	uint16_t count;
	struct elron_departure dep[ELRON_MAX_DEPARTURES];
};

/* Wire format v3 (little-endian, packed), written by the companion over BLE:
 *
 *   u8   version (=3)
 *   u32  epoch_now        (sets the device clock)
 *   i16  tz_offset_min
 *   u16  walk_min
 *   u8   origin_len, origin bytes
 *   u8   dest_len,   dest bytes
 *   u8   msg_len,    msg bytes
 *   u8   ndest, ndest * { u8 len, bytes }   (final-destination table)
 *   u8   count
 *   count * { u32 dep_epoch, u8 dur_min, u8 dest_idx, u8 flags }
 */
#define ELRON_WIRE_VERSION 3

int elron_schedule_init(void);
int elron_schedule_apply_wire(const uint8_t *buf, size_t len);
const struct elron_schedule *elron_schedule_get(void);
bool elron_schedule_valid(void);
const char *elron_dep_dest(const struct elron_departure *d);

/* ── Clock (companion epoch anchored to k_uptime, free-runs between syncs) ── */
void     elron_clock_set(uint32_t epoch_now, int16_t tz_offset_min);
uint32_t elron_clock_now_utc(void);   /* 0 if never set */
bool     elron_clock_is_set(void);
int      elron_clock_local_min(void); /* local minute-of-day 0..1439, -1 if unset */

#endif /* ELRON_SCHEDULE_H_ */
