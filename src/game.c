#include "game.h"
#include "ui.h"

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <errno.h>

LOG_MODULE_REGISTER(game, LOG_LEVEL_INF);

/* ── Tunables (logical 240x240 space, same rotation as the rest of the UI).
 * Positions/velocities are 1/256-px fixed-point so motion is smooth without
 * floats. Tweak these on-device to taste. ─────────────────────────────────── */
#define FP            256
#define SCN_W         240
#define GROUND_Y      196          /* platform line (px) */
#define RUNNER_X      40
#define RUNNER_W      16
#define RUNNER_H      24
#define RUNNER_TOP    (GROUND_Y - RUNNER_H)

#define GRAVITY_FP    300          /* ~1.17 px/tick^2 */
#define JUMP_V_FP     (-2900)      /* ~-11.3 px/tick (single jump) */
#define SPEED0_FP     740          /* ~2.9 px/tick start */
#define SPEED_MAX_FP  1700         /* ~6.6 px/tick cap */
#define SPEED_RAMP_DIV 8           /* speed_fp = SPEED0 + dist_px/this */

#define N_OBST        4
#define OBST_W        16
#define OBST_MIN_H    14
#define OBST_MAX_H    34
#define OBST_MIN_GAP  150          /* px between obstacles (kept clearable) */
#define OBST_MAX_GAP  240

#define OVER_TIMEOUT_MS 20000      /* auto-exit from the game-over screen */

#define COL_BG     lv_color_hex(0x000000)
#define COL_ELRON  lv_color_hex(0xfb4f14)
#define COL_TEXT   lv_color_hex(0xffffff)
#define COL_GROUND lv_color_hex(0x4a5560)
#define COL_OBST   lv_color_hex(0xb4bcc6)

enum gstate { G_TITLE, G_PLAYING, G_OVER };

struct obstacle {
	bool   active;
	int32_t x_fp;   /* left edge */
	int     h;
};

static lv_obj_t *scr;
static lv_obj_t *ground, *train, *runner, *score_lbl;
static lv_obj_t *title_lbl, *best_lbl, *hint_lbl, *over_lbl;
static lv_obj_t *obst_obj[N_OBST];
static struct obstacle obst[N_OBST];

static bool        active;
static enum gstate state;
static int32_t     runner_y_fp, vy_fp;
static bool        grounded;
static int32_t     dist_fp, speed_fp;
static uint32_t    score, hi;
static int         next_gap;
static int         train_phase;
static int64_t     over_ms;
static uint32_t    rng;

/* Enter/press requests arrive on the debounce workqueue thread, but LVGL is not
 * thread-safe — driving it there races the main loop's lv_task_handler() and was
 * dropping the game's full-screen invalidate, leaving stale schedule pixels
 * behind the game. So elron_game_enter()/_button() only record intent here, and
 * elron_game_tick() (main-loop thread) applies it next to its own LVGL calls. */
static atomic_t enter_req;
K_MSGQ_DEFINE(btn_msgq, sizeof(uint8_t), 8, 1);

/* ── High score persistence (own "game" settings subtree) ─────────────────── */
static int game_set_cb(const char *name, size_t len,
		       settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "hi", NULL)) {
		if (len == sizeof(hi)) {
			read_cb(cb_arg, &hi, sizeof(hi));
		}
		return 0;
	}
	return -ENOENT;
}
static struct settings_handler gsh = { .name = "game", .h_set = game_set_cb };

static void save_hi(void)
{
	int rc = settings_save_one("game/hi", &hi, sizeof(hi));
	if (rc) {
		LOG_WRN("save hi: %d", rc);
	}
}

/* ── Tiny PRNG (xorshift32), seeded per run from the cycle counter ─────────── */
static uint32_t prng(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

/* ── State / overlays ─────────────────────────────────────────────────────── */
static void show(lv_obj_t *o, bool on)
{
	if (on) {
		lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
	}
}

static void set_state(enum gstate s)
{
	state = s;
	switch (s) {
	case G_TITLE:
		/* Clean "ready" scene: just the runner standing on the platform.
		 * The train and obstacles stay hidden until the chase begins, so the
		 * title text never collides with stray gameplay scenery. */
		show(score_lbl, false);
		show(train, false);
		show(over_lbl, false);
		for (int i = 0; i < N_OBST; i++) {
			show(obst_obj[i], false);
		}
		lv_obj_set_pos(runner, RUNNER_X, RUNNER_TOP);
		show(runner, true);
		lv_label_set_text_fmt(best_lbl, "best  %u", hi);
		show(title_lbl, true);
		show(best_lbl, true);
		show(hint_lbl, true);
		break;
	case G_PLAYING:
		show(title_lbl, false);
		show(best_lbl, false);
		show(hint_lbl, false);
		show(over_lbl, false);
		lv_obj_set_y(train, 24);
		show(train, true);
		show(runner, true);
		show(score_lbl, true);
		break;
	case G_OVER:
		over_ms = k_uptime_get();
		lv_label_set_text_fmt(over_lbl,
			"MISSED IT!\n\nscore  %u\nbest   %u\n\nGO  again    BACK  exit",
			score, hi);
		show(over_lbl, true);
		lv_obj_move_foreground(over_lbl);
		break;
	}
}

/* ── Gameplay ─────────────────────────────────────────────────────────────── */
static void spawn(int x_px)
{
	for (int i = 0; i < N_OBST; i++) {
		if (!obst[i].active) {
			obst[i].active = true;
			obst[i].x_fp = x_px * FP;
			obst[i].h = OBST_MIN_H + (prng() % (OBST_MAX_H - OBST_MIN_H + 1));
			next_gap = OBST_MIN_GAP + (prng() % (OBST_MAX_GAP - OBST_MIN_GAP + 1));
			return;
		}
	}
}

static void start_game(void)
{
	rng = k_cycle_get_32() | 1u;
	dist_fp = 0;
	speed_fp = SPEED0_FP;
	score = 0;
	runner_y_fp = RUNNER_TOP * FP;
	vy_fp = 0;
	grounded = true;
	for (int i = 0; i < N_OBST; i++) {
		obst[i].active = false;
	}
	next_gap = OBST_MIN_GAP;
	spawn(SCN_W + 80);        /* a little breathing room before the first one */
	set_state(G_PLAYING);
}

static void render(void)
{
	lv_obj_set_pos(runner, RUNNER_X, runner_y_fp >> 8);

	for (int i = 0; i < N_OBST; i++) {
		if (obst[i].active) {
			lv_obj_set_size(obst_obj[i], OBST_W, obst[i].h);
			lv_obj_set_pos(obst_obj[i], obst[i].x_fp >> 8,
				       GROUND_Y - obst[i].h);
			show(obst_obj[i], true);
		} else {
			show(obst_obj[i], false);
		}
	}

	/* Train bobs gently as it pulls away. */
	lv_obj_set_y(train, 24 + (((train_phase >> 3) & 3) - 1));
	lv_label_set_text_fmt(score_lbl, "%u", score);
}

static void update(void)
{
	train_phase++;

	/* Runner physics. */
	vy_fp += GRAVITY_FP;
	runner_y_fp += vy_fp;
	if (runner_y_fp >= RUNNER_TOP * FP) {
		runner_y_fp = RUNNER_TOP * FP;
		vy_fp = 0;
		grounded = true;
	} else {
		grounded = false;
	}

	/* Distance, score, speed ramp. */
	dist_fp += speed_fp;
	int dist_px = dist_fp >> 8;
	score = dist_px / 4;
	speed_fp = SPEED0_FP + dist_px / SPEED_RAMP_DIV;
	if (speed_fp > SPEED_MAX_FP) {
		speed_fp = SPEED_MAX_FP;
	}

	/* Scroll obstacles, recycle off-screen ones, track the rightmost. */
	int rightmost = -10000;
	for (int i = 0; i < N_OBST; i++) {
		if (!obst[i].active) {
			continue;
		}
		obst[i].x_fp -= speed_fp;
		int x_px = obst[i].x_fp >> 8;
		if (x_px + OBST_W < 0) {
			obst[i].active = false;
			continue;
		}
		if (x_px > rightmost) {
			rightmost = x_px;
		}
	}
	if (rightmost < SCN_W - next_gap) {
		spawn(SCN_W);
	}

	/* Collision (slightly forgiving box). */
	int ry = runner_y_fp >> 8;
	for (int i = 0; i < N_OBST; i++) {
		if (!obst[i].active) {
			continue;
		}
		int ox = obst[i].x_fp >> 8;
		int otop = GROUND_Y - obst[i].h;
		if (RUNNER_X + 2 < ox + OBST_W && RUNNER_X + RUNNER_W - 2 > ox &&
		    ry + RUNNER_H - 2 > otop) {
			if (score > hi) {
				hi = score;
				save_hi();
			}
			set_state(G_OVER);
			return;
		}
	}
}

/* ── Public API ───────────────────────────────────────────────────────────── */
bool elron_game_active(void)
{
	return active;
}

void elron_game_enter(void)
{
	/* Workqueue context: flag the request, do NOT touch LVGL. The main loop
	 * sees active and applies the screen switch in elron_game_tick(). */
	active = true;
	atomic_set(&enter_req, 1);
}

static void game_exit(void)
{
	active = false;
	k_msgq_purge(&btn_msgq);                 /* drop presses queued before exit */
	lv_scr_load(elron_ui_screen());
	lv_obj_invalidate(elron_ui_screen());    /* repaint every chunk (see tick) */
	elron_ui_refresh(false);
	LOG_INF("game: exit");
}

/* Apply one queued button press. Runs on the main-loop thread (drained by
 * elron_game_tick), so it's safe to call LVGL from here. */
static void handle_button(uint8_t idx)
{
	if (idx == 1) {            /* #2 = BACK, always exits */
		game_exit();
		return;
	}
	/* idx == 0: #1 = GO */
	switch (state) {
	case G_TITLE:
		start_game();
		break;
	case G_PLAYING:
		if (grounded) {
			vy_fp = JUMP_V_FP;
			grounded = false;
		}
		break;
	case G_OVER:
		start_game();
		break;
	}
}

void elron_game_button(uint8_t idx, bool pressed)
{
	/* Workqueue context: just queue the press; the main loop applies it. */
	if (!active || !pressed) {
		return;
	}
	(void)k_msgq_put(&btn_msgq, &idx, K_NO_WAIT);
}

void elron_game_tick(void)
{
	if (!active) {
		return;
	}

	/* Apply a pending enter here so the screen switch + full-screen invalidate
	 * happen on this (main-loop) thread, just before lv_task_handler() runs —
	 * not racing it from the button workqueue. The partial render buffer
	 * (LV_Z_VDB_SIZE=14%) repaints in chunks, so a dropped invalidate would
	 * leave stale schedule pixels in chunks the game never redraws over. */
	if (atomic_cas(&enter_req, 1, 0)) {
		set_state(G_TITLE);
		lv_scr_load(scr);
		lv_obj_invalidate(scr);
		LOG_INF("game: enter");
	}

	/* Drain queued presses (also this thread, so LVGL stays single-threaded). */
	uint8_t idx;
	while (active && k_msgq_get(&btn_msgq, &idx, K_NO_WAIT) == 0) {
		handle_button(idx);
	}

	if (state == G_PLAYING) {
		update();
		if (state == G_PLAYING) {   /* update() may have ended the run */
			render();
		}
	} else if (state == G_OVER) {
		if (k_uptime_get() - over_ms > OVER_TIMEOUT_MS) {
			game_exit();
		}
	}
}

void elron_game_init(void)
{
	settings_register(&gsh);
	settings_load_subtree("game");   /* subsys already init'd by schedule */

	scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr, COL_BG, 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	/* Platform line. */
	ground = lv_obj_create(scr);
	lv_obj_remove_style_all(ground);
	lv_obj_set_size(ground, SCN_W, 3);
	lv_obj_set_pos(ground, 0, GROUND_Y);
	lv_obj_set_style_bg_color(ground, COL_GROUND, 0);
	lv_obj_set_style_bg_opa(ground, LV_OPA_COVER, 0);

	/* The train you're chasing (flavor). */
	train = lv_obj_create(scr);
	lv_obj_remove_style_all(train);
	lv_obj_set_size(train, 56, 22);
	lv_obj_set_pos(train, 176, 24);
	lv_obj_set_style_bg_color(train, COL_ELRON, 0);
	lv_obj_set_style_bg_opa(train, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(train, 4, 0);

	/* Runner. */
	runner = lv_obj_create(scr);
	lv_obj_remove_style_all(runner);
	lv_obj_set_size(runner, RUNNER_W, RUNNER_H);
	lv_obj_set_style_bg_color(runner, COL_ELRON, 0);
	lv_obj_set_style_bg_opa(runner, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(runner, 3, 0);

	/* Obstacle pool. */
	for (int i = 0; i < N_OBST; i++) {
		obst_obj[i] = lv_obj_create(scr);
		lv_obj_remove_style_all(obst_obj[i]);
		lv_obj_set_style_bg_color(obst_obj[i], COL_OBST, 0);
		lv_obj_set_style_bg_opa(obst_obj[i], LV_OPA_COVER, 0);
		lv_obj_set_style_radius(obst_obj[i], 2, 0);
		lv_obj_add_flag(obst_obj[i], LV_OBJ_FLAG_HIDDEN);
	}

	/* Score. */
	score_lbl = lv_label_create(scr);
	lv_obj_set_style_text_font(score_lbl, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(score_lbl, COL_TEXT, 0);
	lv_obj_align(score_lbl, LV_ALIGN_TOP_RIGHT, -8, 8);
	lv_label_set_text(score_lbl, "0");

	/* Title screen: bold orange title up top, a best-score line, and footer
	 * hints below the platform. Separate labels so each sits cleanly around the
	 * standing runner instead of one text block piled over the scenery. */
	title_lbl = lv_label_create(scr);
	lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_24, 0);
	lv_obj_set_style_text_color(title_lbl, COL_ELRON, 0);
	lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 40);
	lv_label_set_text(title_lbl, "CATCH THE TRAIN");

	best_lbl = lv_label_create(scr);
	lv_obj_set_style_text_font(best_lbl, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(best_lbl, COL_TEXT, 0);
	lv_obj_align(best_lbl, LV_ALIGN_TOP_MID, 0, 80);
	lv_label_set_text(best_lbl, "best  0");

	hint_lbl = lv_label_create(scr);
	lv_obj_set_style_text_font(hint_lbl, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(hint_lbl, COL_OBST, 0);
	lv_obj_align(hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -12);
	lv_label_set_text(hint_lbl, "GO  run        BACK  exit");

	/* Game-over card: a bordered, opaque panel centred over the frozen scene. */
	over_lbl = lv_label_create(scr);
	lv_obj_set_style_text_font(over_lbl, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(over_lbl, COL_TEXT, 0);
	lv_obj_set_style_bg_color(over_lbl, COL_BG, 0);
	lv_obj_set_style_bg_opa(over_lbl, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(over_lbl, 16, 0);
	lv_obj_set_style_radius(over_lbl, 10, 0);
	lv_obj_set_style_border_color(over_lbl, COL_ELRON, 0);
	lv_obj_set_style_border_width(over_lbl, 2, 0);
	lv_obj_set_width(over_lbl, 204);
	lv_obj_set_style_text_align(over_lbl, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(over_lbl, LV_ALIGN_CENTER, 0, 0);
	lv_label_set_text(over_lbl, "");
	lv_obj_add_flag(over_lbl, LV_OBJ_FLAG_HIDDEN);

	LOG_INF("game ready (best %u)", hi);
}
