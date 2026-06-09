#include "ui.h"
#include "schedule.h"
#include "battery.h"

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

/* Palette — Elron brand: orange #fb4f14 on a departure-board black. */
#define COL_BG     lv_color_hex(0x000000)
#define COL_ELRON  lv_color_hex(0xfb4f14)
#define COL_TEXT   lv_color_hex(0xffffff)
#define COL_DIM    lv_color_hex(0xb4bcc6)
#define COL_URGENT lv_color_hex(0xff3b30)  /* red: leave very soon */
#define COL_OK     lv_color_hex(0x35c46a)  /* green: plenty of time */

static lv_obj_t *lbl_route;
static lv_obj_t *lbl_leadlbl;   /* "leave in" */
static lv_obj_t *lbl_lead;      /* big leave countdown / GO NOW */
static lv_obj_t *lbl_train;     /* which train: HH:MM -> dest */
static lv_obj_t *lbl_then;      /* following departures */
static lv_obj_t *lbl_status;

static void style_label(lv_obj_t *l, const lv_font_t *font, lv_color_t color)
{
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, color, 0);
}

static lv_obj_t *centered(lv_obj_t *scr, const lv_font_t *f, lv_color_t c, int y)
{
	lv_obj_t *l = lv_label_create(scr);
	style_label(l, f, c);
	lv_obj_set_width(l, 240);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
	lv_label_set_text(l, "");
	return l;
}

void elron_ui_init(void)
{
	lv_disp_t *disp = lv_disp_get_default();
	if (disp && disp->driver) {
		disp->driver->sw_rotate = 1;
		lv_disp_set_rotation(disp, LV_DISP_ROT_270);
	}

	lv_obj_t *scr = lv_scr_act();
	lv_obj_set_style_bg_color(scr, COL_BG, 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	lbl_route = lv_label_create(scr);
	style_label(lbl_route, &lv_font_montserrat_24, COL_TEXT);
	lv_obj_set_style_bg_color(lbl_route, COL_ELRON, 0);
	lv_obj_set_style_bg_opa(lbl_route, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_top(lbl_route, 5, 0);
	lv_obj_set_style_pad_bottom(lbl_route, 5, 0);
	lv_label_set_long_mode(lbl_route, LV_LABEL_LONG_SCROLL_CIRCULAR);
	lv_obj_set_width(lbl_route, 240);
	lv_obj_set_style_text_align(lbl_route, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(lbl_route, LV_ALIGN_TOP_MID, 0, 0);
	lv_label_set_text(lbl_route, "Elron");

	lbl_leadlbl = centered(scr, &lv_font_montserrat_20, COL_ELRON, 44);
	lbl_lead    = centered(scr, &lv_font_montserrat_48, COL_TEXT, 66);
	lbl_train   = centered(scr, &lv_font_montserrat_24, COL_ELRON, 128);
	lbl_then    = centered(scr, &lv_font_montserrat_20, COL_DIM, 164);

	lbl_status = lv_label_create(scr);
	style_label(lbl_status, &lv_font_montserrat_20, COL_DIM);
	lv_obj_set_width(lbl_status, 240);
	lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_MID, 0, -4);
	lv_label_set_text(lbl_status, "starting...");
}

/* "1h 05m" / "12 min" */
static void fmt_dur(int mins, char *out, size_t cap)
{
	if (mins >= 60) {
		snprintf(out, cap, "%dh %02dm", mins / 60, mins % 60);
	} else {
		snprintf(out, cap, "%d min", mins);
	}
}

static const char *const WDAY[7] = {"Mon", "Tue", "Wed", "Thu",
				    "Fri", "Sat", "Sun"};

static int local_day(uint32_t epoch, int tz)
{
	return (int)(((int64_t)epoch + tz * 60) / 86400);
}

/* "15:32" or "Tue 15:32" if not the same local day as `now`. */
static void fmt_when(uint32_t epoch, int tz, uint32_t now, char *out, size_t cap)
{
	int64_t loc = (int64_t)epoch + tz * 60;
	int hm = (int)((loc % 86400) / 60);
	int dd = local_day(epoch, tz) - local_day(now, tz);
	if (dd == 0) {
		snprintf(out, cap, "%02d:%02d", hm / 60, hm % 60);
	} else if (dd == 1) {
		snprintf(out, cap, "tmrw %02d:%02d", hm / 60, hm % 60);
	} else {
		int wd = (local_day(epoch, tz) + 3) % 7;   /* 0 = Monday */
		snprintf(out, cap, "%s %02d:%02d", WDAY[wd], hm / 60, hm % 60);
	}
}

void elron_ui_refresh(bool ble_connected)
{
	const struct elron_schedule *s = elron_schedule_get();
	char buf[96], tmp[48];

	if (elron_schedule_valid() && s->origin[0]) {
		snprintf(buf, sizeof(buf), "%s %s %s", s->origin, LV_SYMBOL_RIGHT, s->dest);
		lv_label_set_text(lbl_route, buf);
	}

	/* Status: disruption (if any) else sync age (no current clock). */
	uint32_t now = elron_clock_now_utc();
	if (s->msg[0]) {
		lv_obj_set_style_text_color(lbl_status, COL_ELRON, 0);
		lv_label_set_text(lbl_status, s->msg);
	} else {
		lv_obj_set_style_text_color(lbl_status, COL_DIM, 0);
		const char *ico = ble_connected ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_SD_CARD;
		const char *bat = battery_symbol();
		char age[24];
		if (now == 0) {
			/* Clock is RAM-only and hasn't been set this boot. */
			snprintf(age, sizeof(age), "not synced");
		} else {
			long am = (s->synced_epoch && now > s->synced_epoch)
					  ? (now - s->synced_epoch) / 60 : 0;
			if (am < 60) {
				snprintf(age, sizeof(age), "synced %ldm ago", am);
			} else if (am < 60 * 24) {
				snprintf(age, sizeof(age), "synced %ldh ago", am / 60);
			} else {
				snprintf(age, sizeof(age), "synced %ldd ago", am / (60 * 24));
			}
		}
		snprintf(buf, sizeof(buf), "%s%s%s  %s",
			 bat, bat[0] ? " " : "", ico, age);
		lv_label_set_text(lbl_status, buf);
	}

	/* No time since boot: the clock is RAM-only, so every power-up needs a
	 * fresh sync from the companion before we can show any countdown — even if
	 * a schedule is still persisted from before. Make that state unmistakable. */
	if (now == 0) {
		lv_obj_set_style_text_color(lbl_lead, COL_ELRON, 0);
		lv_label_set_text(lbl_leadlbl, "");
		lv_label_set_text(lbl_lead, LV_SYMBOL_REFRESH);
		lv_label_set_text(lbl_train, "Waiting for sync");
		lv_label_set_text(lbl_then,
			ble_connected ? "syncing..." : "open companion");
		return;
	}

	if (!elron_schedule_valid() || s->count == 0) {
		lv_label_set_text(lbl_leadlbl, "");
		lv_label_set_text(lbl_lead, "--:--");
		lv_label_set_text(lbl_train, "no departures");
		lv_label_set_text(lbl_then,
			ble_connected ? "syncing..." : "open companion");
		return;
	}

	/* Soonest train we can still catch: leave time = (dep-now) - walk.
	 * Allow a small grace so "GO NOW" shows briefly before rolling on. */
	const int walk_s = s->walk_min * 60;
	const int grace = 180;
	int target = -1;
	for (uint16_t i = 0; i < s->count; i++) {
		if (s->dep[i].dep_epoch + 60 < now) {
			continue;   /* already departed */
		}
		int lead = (int)(s->dep[i].dep_epoch - now) - walk_s;
		if (lead >= -grace) {
			target = i;
			break;
		}
	}

	if (target < 0) {
		lv_label_set_text(lbl_leadlbl, "");
		lv_label_set_text(lbl_lead, "--:--");
		lv_label_set_text(lbl_train, "no more trains");
		lv_label_set_text(lbl_then, "");
		return;
	}

	const struct elron_departure *d = &s->dep[target];
	int lead = (int)(d->dep_epoch - now) - walk_s;

	/* Hero: when to start walking. */
	if (lead <= 0) {
		lv_obj_set_style_text_color(lbl_lead, COL_URGENT, 0);
		lv_label_set_text(lbl_leadlbl, "time to");
		lv_label_set_text(lbl_lead, "GO NOW");
	} else {
		lv_color_t c = lead <= 5 * 60 ? COL_URGENT
			     : lead <= 15 * 60 ? COL_ELRON : COL_OK;
		lv_obj_set_style_text_color(lbl_lead, c, 0);
		lv_label_set_text(lbl_leadlbl, "leave in");
		fmt_dur((lead + 59) / 60, tmp, sizeof(tmp));   /* round up */
		lv_label_set_text(lbl_lead, tmp);
	}

	/* Which train. */
	{
		char when[24];
		fmt_when(d->dep_epoch, s->tz_off_min, now, when, sizeof(when));
		snprintf(buf, sizeof(buf), "%s  %s%s",
			 when,
			 (d->flags & ELRON_FLAG_EXPRESS) ? LV_SYMBOL_CHARGE " " : "",
			 elron_dep_dest(d));
		lv_label_set_text(lbl_train, buf);
	}

	/* Following departures. */
	{
		size_t off = 0; buf[0] = '\0';
		int shown = 0;
		for (uint16_t i = target + 1; i < s->count && shown < 2; i++) {
			if (s->dep[i].dep_epoch + 60 < now) {
				continue;
			}
			char when[24];
			fmt_when(s->dep[i].dep_epoch, s->tz_off_min, now, when, sizeof(when));
			off += snprintf(buf + off, sizeof(buf) - off, "%s  %s%s\n",
					when,
					(s->dep[i].flags & ELRON_FLAG_EXPRESS) ? LV_SYMBOL_CHARGE " " : "",
					elron_dep_dest(&s->dep[i]));
			shown++;
		}
		lv_label_set_text(lbl_then, buf);
	}
}
