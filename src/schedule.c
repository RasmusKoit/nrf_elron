#include "schedule.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(schedule, LOG_LEVEL_INF);

#define SETTINGS_KEY      "elron/sched"
#define SETTINGS_SUBTREE  "elron"

static struct elron_schedule sched;
static bool sched_valid;

/* ── Clock: anchor companion-supplied epoch to k_uptime, free-run between ── */
static uint32_t clock_epoch_base;
static int64_t  clock_uptime_base;
static bool     clock_set;

void elron_clock_set(uint32_t epoch_now, int16_t tz_offset_min)
{
	clock_epoch_base = epoch_now;
	clock_uptime_base = k_uptime_get();
	sched.tz_off_min = tz_offset_min;
	clock_set = true;
}

uint32_t elron_clock_now_utc(void)
{
	if (!clock_set) {
		return 0;
	}
	int64_t elapsed_s = (k_uptime_get() - clock_uptime_base) / 1000;
	return clock_epoch_base + (uint32_t)elapsed_s;
}

bool elron_clock_is_set(void)
{
	return clock_set;
}

int elron_clock_local_min(void)
{
	if (!clock_set) {
		return -1;
	}
	int64_t local = (int64_t)elron_clock_now_utc() + sched.tz_off_min * 60;
	int64_t mod = local % 86400;
	if (mod < 0) {
		mod += 86400;
	}
	return (int)(mod / 60);
}

/* ── Persistence ──────────────────────────────────────────────────────── */
static int settings_set_cb(const char *name, size_t len,
			   settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "sched", NULL)) {
		if (len != sizeof(sched)) {
			LOG_WRN("stored schedule size %u != %u, ignoring",
				(unsigned)len, (unsigned)sizeof(sched));
			return 0;
		}
		ssize_t rc = read_cb(cb_arg, &sched, sizeof(sched));
		if (rc == sizeof(sched)) {
			sched_valid = true;
			LOG_INF("loaded %u persisted departures (%s -> %s)",
				sched.count, sched.origin, sched.dest);
		}
		return 0;
	}
	return -ENOENT;
}

static struct settings_handler sh = {
	.name = SETTINGS_SUBTREE,
	.h_set = settings_set_cb,
};

int elron_schedule_init(void)
{
	int rc = settings_subsys_init();
	if (rc) {
		LOG_ERR("settings_subsys_init: %d", rc);
		return rc;
	}
	rc = settings_register(&sh);
	if (rc) {
		LOG_ERR("settings_register: %d", rc);
		return rc;
	}
	rc = settings_load_subtree(SETTINGS_SUBTREE);
	if (rc) {
		LOG_WRN("settings_load: %d", rc);
	}
	return 0;
}

static int schedule_save(void)
{
	int rc = settings_save_one(SETTINGS_KEY, &sched, sizeof(sched));
	if (rc) {
		LOG_ERR("settings_save_one: %d", rc);
	}
	return rc;
}

/* ── Wire decode (v3) ─────────────────────────────────────────────────── */
static int rd_str(const uint8_t *buf, size_t len, size_t *p, char *dst, size_t cap)
{
	if (cap == 0) return -EINVAL;   /* else 'cap - 1' (size_t) underflows to SIZE_MAX */
	if (*p >= len) return -EINVAL;
	uint8_t n = buf[(*p)++];
	if (*p + n > len) return -EINVAL;
	size_t copy = n < cap - 1 ? n : cap - 1;
	memcpy(dst, &buf[*p], copy);
	dst[copy] = '\0';
	*p += n;
	return 0;
}

int elron_schedule_apply_wire(const uint8_t *buf, size_t len)
{
	size_t p = 0;
	/* static: the struct is ~1 KB — too big for the workqueue stack, and this
	 * runs serially from one context so there's no reentrancy concern. */
	static struct elron_schedule s;

	memset(&s, 0, sizeof(s));

	if (len < 9 || buf[p++] != ELRON_WIRE_VERSION) {
		LOG_WRN("bad wire header");
		return -EINVAL;
	}

	uint32_t epoch = sys_get_le32(&buf[p]); p += 4;
	int16_t  tz    = (int16_t)sys_get_le16(&buf[p]); p += 2;
	s.walk_min     = sys_get_le16(&buf[p]); p += 2;
	s.tz_off_min   = tz;

	if (rd_str(buf, len, &p, s.origin, sizeof(s.origin))) return -EINVAL;
	if (rd_str(buf, len, &p, s.dest, sizeof(s.dest))) return -EINVAL;
	if (rd_str(buf, len, &p, s.msg, sizeof(s.msg))) return -EINVAL;

	if (p >= len) return -EINVAL;
	uint8_t nd = buf[p++];
	s.ndest = nd < ELRON_MAX_DESTS ? nd : ELRON_MAX_DESTS;
	for (uint8_t i = 0; i < nd; i++) {
		char tmp[ELRON_DESTNAME_LEN];
		if (rd_str(buf, len, &p, tmp, sizeof(tmp))) return -EINVAL;
		if (i < ELRON_MAX_DESTS) {
			memcpy(s.dests[i], tmp, sizeof(tmp));
		}
	}

	if (p >= len) return -EINVAL;
	uint16_t count = buf[p++];
	if (count > ELRON_MAX_DEPARTURES) {
		count = ELRON_MAX_DEPARTURES;
	}
	for (uint16_t i = 0; i < count; i++) {
		if (p + 7 > len) return -EINVAL;
		s.dep[i].dep_epoch = sys_get_le32(&buf[p]); p += 4;
		s.dep[i].dur_min   = buf[p++];
		s.dep[i].dest_idx  = buf[p++];
		s.dep[i].flags     = buf[p++];
	}
	s.count = count;
	s.synced_epoch = epoch;

	/* Persist only when the timetable content actually changed. Every push carries
	 * a fresh synced_epoch, so comparing with that one field neutralised avoids
	 * rewriting ~1.2 KB to NVS (and the periodic GC sector-erase that costs) on
	 * identical re-pushes. The RAM clock is always re-anchored regardless. */
	uint32_t new_synced = s.synced_epoch;
	s.synced_epoch = sched.synced_epoch;
	bool changed = !sched_valid || memcmp(&s, &sched, sizeof(s)) != 0;
	s.synced_epoch = new_synced;

	memcpy(&sched, &s, sizeof(sched));
	sched_valid = true;
	elron_clock_set(epoch, tz);
	LOG_INF("applied %u departures (%u dests, walk %um)%s",
		count, s.ndest, s.walk_min, changed ? "" : " [unchanged]");

	return changed ? schedule_save() : 0;
}

const struct elron_schedule *elron_schedule_get(void)
{
	return &sched;
}

bool elron_schedule_valid(void)
{
	return sched_valid;
}

const char *elron_dep_dest(const struct elron_departure *d)
{
	if (d->dest_idx < sched.ndest) {
		return sched.dests[d->dest_idx];
	}
	return "";
}
