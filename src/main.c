#include "schedule.h"
#include "ble_svc.h"
#include "ui.h"
#include "battery.h"
#include "buttons.h"
#include "game.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/uart/cdc_acm.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/fatal.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_power.h>
#include <lvgl.h>
#include <string.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* ── Self-recovery ───────────────────────────────────────────────────────────
 * The Zephyr default fatal-error handler HALTS the CPU forever (interrupts off):
 * a fault — null deref, MPU/stack-overflow trap, kernel panic — would leave the
 * panel frozen on its last frame with the system workqueue dead, so even the
 * button-driven reset/bootloader stop responding (exactly the "blank screen the
 * next day, can't reset" symptom). Override it to cold-reboot instead, so any
 * fault self-heals: the board reboots and the companion auto-resyncs the clock.
 * NOTE: do NOT request the UF2 bootloader here (leave GPREGRET alone) — we want
 * to come straight back up into the app, not get stuck in DFU. */
void k_sys_fatal_error_handler(unsigned int reason, const z_arch_esf_t *esf)
{
	ARG_UNUSED(reason);
	ARG_UNUSED(esf);
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}

/* Hardware watchdog: catches true HANGS/deadlocks (a fault goes through the
 * handler above; a livelock/deadlock or CPU lockup does not). Fed from the main
 * loop every iteration (<=500ms apart even overnight), so if that loop ever
 * wedges the SoC resets within WDT_TIMEOUT_MS and the device comes back.
 * The timeout is deliberately generous: the Adafruit/Seeed UF2 bootloader does
 * NOT feed the watchdog and the nRF52 WDT keeps running across a soft reset, so
 * the window must comfortably exceed a normal drag-drop DFU. We also feed it
 * just before any intentional reboot-to-bootloader to hand the bootloader a full
 * window. (The WDT runs during WFI sleep, so an idle deadlock is still caught.) */
#define WDT_TIMEOUT_MS 30000
static const struct device *const wdt_dev = DEVICE_DT_GET(DT_NODELABEL(wdt0));
static int wdt_channel = -1;

static void watchdog_init(void)
{
	if (!device_is_ready(wdt_dev)) {
		LOG_ERR("watchdog not ready");
		return;
	}
	struct wdt_timeout_cfg cfg = {
		.window = { .min = 0U, .max = WDT_TIMEOUT_MS },
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};
	wdt_channel = wdt_install_timeout(wdt_dev, &cfg);
	if (wdt_channel < 0) {
		LOG_ERR("wdt_install_timeout: %d", wdt_channel);
		return;
	}
	int rc = wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (rc) {
		LOG_ERR("wdt_setup: %d", rc);
		wdt_channel = -1;
		return;
	}
	LOG_INF("watchdog armed (%d ms)", WDT_TIMEOUT_MS);
}

static inline void watchdog_feed(void)
{
	if (wdt_channel >= 0) {
		wdt_feed(wdt_dev, wdt_channel);
	}
}

/* System-workqueue liveness canary. The watchdog is fed from the main loop, so a
 * wedged main loop is caught — but the button-driven reset/bootloader runs on the
 * SYSTEM workqueue (debounce_work, btn3_poll_work), and a workqueue-only deadlock
 * would leave that path dead while the main loop keeps petting the dog forever.
 * So a self-resubmitting work item bumps a heartbeat, and the main loop only pets
 * the watchdog when the heartbeat has advanced — making the dog prove BOTH the
 * main loop AND the system workqueue are alive. */
static struct k_work_delayable wq_canary;
static atomic_t wq_heartbeat;

static void wq_canary_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	atomic_inc(&wq_heartbeat);
	k_work_reschedule(&wq_canary, K_MSEC(2000));   /* << WDT_TIMEOUT_MS */
}

/* Set to 1 to run ONLY a full-screen colour-cycle panel test (no LVGL/BLE). */
#define PANEL_TEST 0

/* Set to 1 to run ONLY the button wiring/debounce test: shows live press state
 * and a per-button counter on screen (no schedule UI/BLE). Flip back to 0 once
 * the three buttons are confirmed. */
#define BUTTON_TEST 0

/* Set to 1 to run ONLY the wiring scanner: sweeps every free header pin as a
 * driven-low common while reading the others, and prints which pins short
 * together when you hold each button — so it self-discovers the wiring. */
#define BUTTON_DISCOVER 0

static const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
/* Backlight: PWM-dimmed on P1.11 (PWM_OUT0 / pwm1 ch0) to save battery. Driven
 * via the PWM driver in backlight_set(); period/pins come from the overlay. */
static const struct device *const backlight_pwm = DEVICE_DT_GET(DT_NODELABEL(pwm1));
#define BACKLIGHT_PWM_CH     0
#define BACKLIGHT_PERIOD_NS  PWM_USEC(100)   /* 10 kHz: well above any flicker */
#define BACKLIGHT_PCT        60              /* brightness % when "on" (battery) */
#if BUTTON_DISCOVER
static const struct device *gpio0_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *gpio1_dev = DEVICE_DT_GET(DT_NODELABEL(gpio1));
#endif

/* Second CDC ACM port, dedicated to the host->board data push (separate from the
 * log console so its IRQ reader doesn't clobber the console's own handler). */
static const struct device *data_dev = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart1));
/* Serial protocol bytes (data port): host pings, board ACKs / confirms apply. */
#define SERIAL_ACK 0x06

/* ── Deferred apply of a received schedule (flash write off the BT context) ── */
static uint8_t  pending_buf[1100];
static volatile uint16_t pending_len;
static volatile bool     pending_from_serial;
static struct k_work apply_work;

static void apply_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	/* Snapshot the shared inbox under irq_lock so the CDC RX ISR can't tear the
	 * buffer (or clobber pending_len) out from under the multi-ms parse below.
	 * static: ~1.1 KB is too big for the system-workqueue stack. */
	static uint8_t local_buf[sizeof(pending_buf)];
	unsigned int key = irq_lock();
	uint16_t len = pending_len;
	bool from_serial = pending_from_serial;
	if (len) {
		memcpy(local_buf, pending_buf, len);
		pending_len = 0;
	}
	irq_unlock(key);
	if (len == 0) {
		return;
	}
	int rc = elron_schedule_apply_wire(local_buf, len);
	if (rc) {
		LOG_WRN("apply_wire failed: %d", rc);
	} else {
		LOG_INF("schedule applied + persisted");
		elron_ble_set_has_schedule(true);
		/* Confirm back to the host over the data port so the companion knows
		 * the push actually landed (not just that bytes were written). */
		if (from_serial && device_is_ready(data_dev)) {
			uart_poll_out(data_dev, SERIAL_ACK);
		}
	}
}

/* ── Auto-flash: reboot into the UF2 bootloader on a 1200-baud serial touch.
 * Lets the host trigger DFU without the physical double-tap (Arduino style).
 * The CDC DTE-rate callback fires when the host sets the baud; we reboot from a
 * thread (not the USB callback context). 0x57 = Adafruit nRF52 "enter UF2". ── */
#define ADAFRUIT_UF2_MAGIC 0x57
static const struct device *console_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static volatile bool dfu_requested;

static void cdc_dte_rate_cb(const struct device *dev, uint32_t rate)
{
	ARG_UNUSED(dev);
	if (rate == 1200) {
		dfu_requested = true;
	}
}

static void dfu_touch_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	while (1) {
		if (dfu_requested) {
			LOG_INF("1200-baud touch -> rebooting to UF2 bootloader");
			nrf_power_gpregret_set(NRF_POWER, 0, ADAFRUIT_UF2_MAGIC);
			watchdog_feed();   /* hand the bootloader a full WDT window */
			k_sleep(K_MSEC(50));
			sys_reboot(SYS_REBOOT_COLD);
		}
		k_sleep(K_MSEC(50));
	}
}
K_THREAD_DEFINE(dfu_touch_tid, 1024, dfu_touch_thread, NULL, NULL, NULL,
		7, 0, 1500);

static void submit_payload(const uint8_t *buf, size_t len, bool from_serial)
{
	if (len > sizeof(pending_buf)) {
		len = sizeof(pending_buf);
	}
	/* Publish all three fields atomically vs apply_work_fn's snapshot (this can
	 * run from the CDC RX ISR). */
	unsigned int key = irq_lock();
	memcpy(pending_buf, buf, len);
	pending_len = len;
	pending_from_serial = from_serial;
	irq_unlock(key);
	k_work_submit(&apply_work);
}

/* BLE write callback (registered with elron_ble_init). */
static void on_ble_rx(const uint8_t *buf, size_t len)
{
	submit_payload(buf, len, false);
}

/* ── USB serial sync: the host pushes the same wire payload over the dedicated
 * data CDC port (cdc_acm_uart1) — more reliable than BLE when plugged in. Frame:
 *   0x01 0x45 <u16 len LE> <payload>.  A bare ping (0x01 0x50) is answered with
 *   an ACK so the host can discover which of the board's two COM ports is this
 *   data channel. Read via the CDC interrupt API (poll_in doesn't drain CDC RX).
 *   The log console lives on a separate port, so taking this device's IRQ
 *   callback doesn't break logs. */
#define SERIAL_FRAME_DATA 0x45
#define SERIAL_FRAME_PING 0x50
static void serial_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);
	static uint8_t sbuf[sizeof(pending_buf)];
	static enum { S1, S2, L0, L1, DAT } st = S1;
	static uint16_t need, got;
	uint8_t c;

	if (!uart_irq_update(dev)) {
		return;
	}
	while (uart_irq_rx_ready(dev) && uart_fifo_read(dev, &c, 1) == 1) {
		switch (st) {
		case S1: st = (c == 0x01) ? S2 : S1; break;
		case S2:
			if (c == SERIAL_FRAME_DATA) {
				st = L0;
			} else if (c == SERIAL_FRAME_PING) {
				uart_poll_out(dev, SERIAL_ACK);   /* "this is the data port" */
				st = S1;
			} else {
				st = S1;
			}
			break;
		case L0: need = c; st = L1; break;
		case L1:
			need |= (uint16_t)c << 8;
			got = 0;
			st = (need && need <= sizeof(sbuf)) ? DAT : S1;
			break;
		case DAT:
			sbuf[got++] = c;
			if (got >= need) {
				submit_payload(sbuf, need, true);
				st = S1;
			}
			break;
		}
	}
}

/* ── Direct panel self-test: solid R/G/B bands via the display driver, no LVGL.
 * If these bands appear, SPI + DC/reset + the ST7789 driver all work and the
 * blank screen is an LVGL problem. If it stays white, it's the panel path. ── */
#if PANEL_TEST
/* Fill the whole 240x240 panel with one RGB565 colour. */
static int display_fill(const struct device *dev, uint16_t color)
{
	static uint16_t strip[240 * 16];
	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(strip),
		.width = 240,
		.height = 16,
		.pitch = 240,
	};
	for (size_t i = 0; i < ARRAY_SIZE(strip); i++) {
		strip[i] = color;
	}
	for (uint16_t y = 0; y < 240; y += 16) {
		int rc = display_write(dev, 0, y, &desc, strip);
		if (rc) {
			LOG_ERR("display_write y=%u rc=%d", y, rc);
			return rc;
		}
	}
	return 0;
}
#endif /* PANEL_TEST */

/* Backlight on between 09:00 and 23:00 local; off overnight to save battery.
 * On when the clock isn't set yet (so the screen is visible during setup). */
#define BL_ON_MIN   (9 * 60)
#define BL_OFF_MIN  (23 * 60)

static void backlight_set(bool on)
{
	if (!device_is_ready(backlight_pwm)) {
		return;
	}
	/* on -> BACKLIGHT_PCT duty; off -> 0 (the nRF PWM driver drops the pin to a
	 * static low and stops the peripheral at 0%/100%, so "off" draws nothing). */
	uint32_t pulse = on ? (BACKLIGHT_PERIOD_NS * BACKLIGHT_PCT / 100) : 0;
	pwm_set(backlight_pwm, BACKLIGHT_PWM_CH, BACKLIGHT_PERIOD_NS, pulse, 0);
}

static void backlight_init(void)
{
	/* Pin + PWM are set up via devicetree (pwm1 / pinctrl); just start it off. */
	backlight_set(false);
}

/* Turn the whole screen on/off on the daytime schedule. Off = backlight off +
 * panel DISP_OFF (reversible); on = DISP_ON re-enabled + a forced redraw, so it
 * always comes back cleanly. Returns whether the screen is currently on. */
static bool screen_on_state = true;

/* Set to 1 to test blank/re-init: screen off for the first 25 s, then on. */
#define SCREEN_TEST 0

static bool screen_update(void)
{
#if SCREEN_TEST
	bool on = k_uptime_get() > 25000;
#else
	int lm = elron_clock_local_min();
	bool on = (lm < 0) || (lm >= BL_ON_MIN && lm < BL_OFF_MIN);
#endif

	if (on != screen_on_state) {
		/* display_blanking_on/off issues a single command transaction on this
		 * thread, but the LVGL flush thread (CONFIG_LV_Z_FLUSH_THREAD) may be mid
		 * display_write — and the ST7789 driver toggles the shared DC line around
		 * each SPI write without locking, so a concurrent flush can clobber DC and
		 * make the panel latch our DISP_ON/DISP_OFF opcode as pixel data (lost).
		 * If DISP_ON is dropped the screen stays dark until the next day. Force a
		 * full render and let lv_refr_now() drive it to completion (it waits on the
		 * flush thread via wait_cb), so the bus is provably idle before we toggle. */
		if (on) {
			elron_ui_refresh(elron_ble_connected());  /* fresh content first */
			lv_obj_invalidate(lv_scr_act());
			lv_refr_now(NULL);            /* render it + drain the flush thread */
			display_blanking_off(display_dev);  /* reveal the ready frame, bus idle */
		} else {
			backlight_set(false);
			lv_obj_invalidate(lv_scr_act());
			lv_refr_now(NULL);            /* drain the flush thread before DISP_OFF */
			display_blanking_on(display_dev);   /* panel output off */
		}
		screen_on_state = on;
	}
	backlight_set(on);
	return on;
}

/* ── Buttons (D0/D1/D2, common D8) ──────────────────────────────────────────
 * For now the three buttons have no assigned function — this just confirms the
 * wiring + debounce work. Normal mode logs each press; BUTTON_TEST shows it on
 * screen. We'll wire real actions in once we decide what they should do. */
#if BUTTON_TEST
static volatile uint32_t btn_press_cnt[ELRON_NUM_BUTTONS];

static void btn_test_cb(uint8_t idx, bool pressed)
{
	if (pressed && idx < ELRON_NUM_BUTTONS) {
		btn_press_cnt[idx]++;
	}
	LOG_INF("BTN D%u %s", idx, pressed ? "DOWN" : "up");
}
#else
/* Physical buttons: #1 (D1) and #2 (D2) are unassigned for now. #3 (D0) is a
 * deliberately-staged hold (it's a sensitive button, so a tap does nothing):
 *   hold >= 3s, release < 6s  -> reset (clean reboot; also re-triggers a
 *                                companion resync since the clock is RAM-only)
 *   hold >= 6s, release < 10s -> enter the UF2 bootloader (GPREGRET 0x57)
 *   hold >= 10s               -> timer exits, nothing happens (escape hatch)
 *   release < 3s              -> nothing
 * A poll timer runs while it's held: it advances the on-screen hint and trips
 * the 10s cancel. The action itself is chosen from the hold duration on release. */
#define BTN3_RESET_MS   3000
#define BTN3_BOOT_MS    6000
#define BTN3_CANCEL_MS  10000

static struct k_work_delayable btn3_poll_work;
static volatile int64_t btn3_press_ms;
static volatile bool    btn3_held;
static volatile int     btn3_hint;   /* feeds elron_ui_button_hint() from main loop */

static void btn3_poll_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (!btn3_held) {
		return;
	}
	int64_t held = k_uptime_get() - btn3_press_ms;
	if (held >= BTN3_CANCEL_MS) {
		btn3_hint = 4;            /* "Cancelled" — release now does nothing */
		return;                  /* stop polling: escape hatch tripped */
	} else if (held >= BTN3_CANCEL_MS - 1000) {
		btn3_hint = 5;           /* last second: release NOW or it cancels */
	} else if (held >= BTN3_BOOT_MS) {
		btn3_hint = 3;           /* bootloader armed */
	} else if (held >= BTN3_RESET_MS) {
		btn3_hint = 2;           /* reset armed; keep holding for bootloader */
	} else {
		btn3_hint = 1;           /* "Hold 3s for reset" */
	}
	k_work_reschedule(&btn3_poll_work, K_MSEC(100));
}

static void btn_cb(uint8_t idx, bool pressed)
{
	if (idx == 2) {   /* #3 = staged hold: reset / bootloader / cancel */
		if (pressed) {
			btn3_press_ms = k_uptime_get();
			btn3_held = true;
			btn3_hint = 1;
			k_work_reschedule(&btn3_poll_work, K_NO_WAIT);
			return;
		}
		/* Released: stop polling, clear hint, act on how long it was held. */
		btn3_held = false;
		k_work_cancel_delayable(&btn3_poll_work);
		int64_t held = k_uptime_get() - btn3_press_ms;
		btn3_hint = 0;
		if (held >= BTN3_CANCEL_MS) {
			LOG_INF("button #3 held >=10s -> cancelled");
		} else if (held >= BTN3_BOOT_MS) {
			LOG_INF("button #3 held %lldms -> UF2 bootloader", held);
			nrf_power_gpregret_set(NRF_POWER, 0, ADAFRUIT_UF2_MAGIC);
			watchdog_feed();   /* hand the bootloader a full WDT window */
			k_sleep(K_MSEC(50));
			sys_reboot(SYS_REBOOT_COLD);
		} else if (held >= BTN3_RESET_MS) {
			LOG_INF("button #3 held %lldms -> reboot", held);
			k_sleep(K_MSEC(50));
			sys_reboot(SYS_REBOOT_COLD);
		} else {
			LOG_INF("button #3 tapped (%lldms) -> ignored", held);
		}
		return;
	}

	/* #1 / #2: in-game controls, or the entry chord from the schedule view. */
	if (elron_game_active()) {
		elron_game_button(idx, pressed);
		return;
	}
	if (pressed) {
		uint32_t st = elron_buttons_state();
		if ((st & BIT(0)) && (st & BIT(1))) {   /* #1 + #2 held -> launch */
			elron_game_enter();
		} else {
			LOG_INF("button #%u (%s) pressed", idx + 1, elron_button_name[idx]);
		}
	}
}
#endif

#if BUTTON_DISCOVER
/* ── Wiring scanner ─────────────────────────────────────────────────────────
 * Every free XIAO header pin is a candidate. Each cycle we drive one candidate
 * low (push-pull) and read the others (input + pull-up); a held button shows up
 * as a short — the read pin goes low. We also test a "nothing driven" baseline
 * to catch a button wired straight to GND. Confirmed shorts are tallied; the pin
 * that shorts to the most others is the shared common. Never returns. */
struct disc_cand { const struct device *port; uint8_t pin; const char *name; };

static void button_discover(void)
{
	const struct disc_cand c[] = {
		{ gpio0_dev,  2, "D0"  },   /* P0.02 */
		{ gpio0_dev,  3, "D1"  },   /* P0.03 */
		{ gpio0_dev, 28, "D2"  },   /* P0.28 */
		{ gpio1_dev, 13, "D8"  },   /* P1.13 */
		{ gpio1_dev, 15, "D10" },   /* P1.15 */
	};
	const int n = ARRAY_SIZE(c);
	static uint16_t pair[8][8];   /* pair[i][j] (i<j) = shorts seen between i,j */
	static uint16_t gnd[8];       /* gnd[i] = times i read low with nothing driven */
	const uint16_t HIT_OK = 3;

	for (int i = 0; i < n; i++) {
		gpio_pin_configure(c[i].port, c[i].pin, GPIO_INPUT | GPIO_PULL_UP);
	}

	lv_disp_t *disp = lv_disp_get_default();
	if (disp && disp->driver) {
		disp->driver->sw_rotate = 1;
		lv_disp_set_rotation(disp, LV_DISP_ROT_270);
	}
	lv_obj_t *scr = lv_scr_act();
	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_t *title = lv_label_create(scr);
	lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
	lv_obj_set_style_text_color(title, lv_color_hex(0xfb4f14), 0);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);
	lv_label_set_text(title, "WIRING SCAN");
	lv_obj_t *hint = lv_label_create(scr);
	lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(hint, lv_color_hex(0xb4bcc6), 0);
	lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 34);
	lv_label_set_text(hint, "hold each button (one at a time)");
	lv_obj_t *out = lv_label_create(scr);
	lv_obj_set_style_text_font(out, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(out, lv_color_white(), 0);
	lv_obj_set_width(out, 232);
	lv_obj_align(out, LV_ALIGN_TOP_LEFT, 8, 62);
	lv_label_set_text(out, "press a button...");

	char buf[256];
	for (;;) {
		/* Baseline: nothing driven — a low pin is shorted to GND. */
		for (int i = 0; i < n; i++) {
			gpio_pin_configure(c[i].port, c[i].pin, GPIO_INPUT | GPIO_PULL_UP);
		}
		k_busy_wait(60);
		for (int i = 0; i < n; i++) {
			if (gpio_pin_get_raw(c[i].port, c[i].pin) == 0 && gnd[i] < 0xffff) {
				gnd[i]++;
			}
		}

		/* Sweep: drive each candidate low, read the rest. */
		for (int d = 0; d < n; d++) {
			gpio_pin_configure(c[d].port, c[d].pin, GPIO_OUTPUT_LOW);
			k_busy_wait(60);
			for (int o = 0; o < n; o++) {
				if (o == d) {
					continue;
				}
				if (gpio_pin_get_raw(c[o].port, c[o].pin) == 0) {
					int a = MIN(d, o), b = MAX(d, o);
					if (pair[a][b] < 0xffff) {
						pair[a][b]++;
					}
				}
			}
			gpio_pin_configure(c[d].port, c[d].pin, GPIO_INPUT | GPIO_PULL_UP);
		}

		/* Degree of each pin among confirmed shorts -> common = highest. */
		int deg[8] = {0};
		for (int a = 0; a < n; a++) {
			for (int b = a + 1; b < n; b++) {
				if (pair[a][b] >= HIT_OK) {
					deg[a]++;
					deg[b]++;
				}
			}
		}
		int common = -1;
		for (int i = 0; i < n; i++) {
			if (deg[i] >= 1 && (common < 0 || deg[i] > deg[common])) {
				common = i;
			}
		}

		size_t off = 0;
		buf[0] = '\0';
		if (common >= 0) {
			off += snprintf(buf + off, sizeof(buf) - off,
					"common: %s\nbtns:", c[common].name);
			for (int i = 0; i < n && off < sizeof(buf); i++) {
				int a = MIN(common, i), b = MAX(common, i);
				if (i != common && pair[a][b] >= HIT_OK) {
					off += snprintf(buf + off, sizeof(buf) - off,
							" %s", c[i].name);
				}
			}
			if (off < sizeof(buf)) {
				off += snprintf(buf + off, sizeof(buf) - off, "\n");
			}
		}
		for (int i = 0; i < n && off < sizeof(buf); i++) {
			if (gnd[i] >= HIT_OK) {
				off += snprintf(buf + off, sizeof(buf) - off,
						"%s - GND\n", c[i].name);
			}
		}
		if (off == 0) {
			snprintf(buf, sizeof(buf), "press a button...");
		}
		lv_label_set_text(out, buf);

		lv_task_handler();
		k_sleep(K_MSEC(15));
	}
}
#endif /* BUTTON_DISCOVER */

int main(void)
{
	/* USB CDC-ACM console — don't block waiting for a terminal. */
	if (usb_enable(NULL)) {
		LOG_WRN("usb_enable failed");
	}
	if (device_is_ready(console_dev)) {
		cdc_acm_dte_rate_callback_set(console_dev, cdc_dte_rate_cb);
	}
	/* Dedicated data port: IRQ-driven reader for the host schedule push. */
	if (device_is_ready(data_dev)) {
		uart_irq_callback_set(data_dev, serial_isr);
		uart_irq_rx_enable(data_dev);
		LOG_INF("data port (cdc_acm_uart1) RX enabled");
	} else {
		LOG_ERR("data port (cdc_acm_uart1) NOT READY");
	}
	k_sleep(K_MSEC(500));

	LOG_INF("Elron train display booting");

	if (!device_is_ready(display_dev)) {
		/* A transient SPI/power glitch at boot would otherwise leave a dead board:
		 * main() returns before the watchdog is even armed, so nothing recovers it.
		 * Cold-reboot to retry init from scratch instead. */
		LOG_ERR("display device NOT READY -> rebooting");
		k_sleep(K_MSEC(200));   /* let the log drain to USB CDC first */
		sys_reboot(SYS_REBOOT_COLD);
	}
	LOG_INF("display device ready");

	struct display_capabilities cap;
	display_get_capabilities(display_dev, &cap);
	LOG_INF("panel %ux%u, pixfmt=0x%x", cap.x_resolution, cap.y_resolution,
		cap.current_pixel_format);

	backlight_init();
	battery_init();

	int rc = display_blanking_off(display_dev);
	LOG_INF("display_blanking_off rc=%d", rc);

#if PANEL_TEST
	/* Unmissable panel test: cycle full-screen colours forever, no LVGL. */
	const struct { uint16_t c; const char *n; } seq[] = {
		{ 0xF800, "RED" }, { 0x07E0, "GREEN" }, { 0x001F, "BLUE" },
		{ 0xFFFF, "WHITE" }, { 0x0000, "BLACK" },
	};
	LOG_INF("PANEL_TEST: cycling colours");
	for (int i = 0;; i = (i + 1) % (int)ARRAY_SIZE(seq)) {
		int frc = display_fill(display_dev, seq[i].c);
		LOG_INF("fill %s rc=%d", seq[i].n, frc);
		k_sleep(K_MSEC(1200));
	}
#endif

#if BUTTON_DISCOVER
	/* Self-discover the button wiring on screen, nothing else. Never returns. */
	button_discover();
#endif

#if BUTTON_TEST
	/* Confirm the three buttons + debounce on screen, nothing else. */
	if (elron_buttons_init(btn_test_cb)) {
		LOG_ERR("buttons init failed");
	}
	lv_disp_t *disp = lv_disp_get_default();
	if (disp && disp->driver) {
		disp->driver->sw_rotate = 1;
		lv_disp_set_rotation(disp, LV_DISP_ROT_270);
	}
	lv_obj_t *scr = lv_scr_act();
	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_t *title = lv_label_create(scr);
	lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
	lv_obj_set_style_text_color(title, lv_color_hex(0xfb4f14), 0);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
	lv_label_set_text(title, "BUTTON TEST");
	lv_obj_t *rows[ELRON_NUM_BUTTONS];
	for (int i = 0; i < ELRON_NUM_BUTTONS; i++) {
		rows[i] = lv_label_create(scr);
		lv_obj_set_style_text_font(rows[i], &lv_font_montserrat_24, 0);
		lv_obj_set_style_text_color(rows[i], lv_color_white(), 0);
		lv_obj_align(rows[i], LV_ALIGN_TOP_LEFT, 24, 56 + i * 40);
	}
	for (;;) {
		uint32_t st = elron_buttons_state();
		for (int i = 0; i < ELRON_NUM_BUTTONS; i++) {
			bool down = (st & BIT(i)) != 0;
			lv_obj_set_style_text_color(rows[i],
				down ? lv_color_hex(0x35c46a) : lv_color_hex(0x808080), 0);
			lv_label_set_text_fmt(rows[i], "#%d %s  %s  x%u", i + 1,
				elron_button_name[i], down ? "DOWN" : " -- ",
				btn_press_cnt[i]);
		}
		lv_task_handler();
		k_sleep(K_MSEC(20));
	}
#endif

	k_work_init(&apply_work, apply_work_fn);

	/* Persisted schedule first, so the screen has content before any BLE. */
	elron_schedule_init();
	/* Always advertise "needs sync" after a boot: the schedule may be persisted
	 * but the clock is RAM-only, so the companion must re-send to set the time.
	 * This is what makes the board auto-resync when plugged in / reflashed. */
	elron_ble_set_has_schedule(false);

	elron_ui_init();
	elron_ui_refresh(false);
	elron_game_init();

	if (elron_ble_init(on_ble_rx)) {
		LOG_ERR("ble init failed");
	}

#if !BUTTON_TEST
	k_work_init_delayable(&btn3_poll_work, btn3_poll_fn);
	if (elron_buttons_init(btn_cb)) {
		LOG_ERR("buttons init failed");
	}
#endif

	/* Start the workqueue canary, then arm the watchdog last, once init is done. */
	k_work_init_delayable(&wq_canary, wq_canary_fn);
	k_work_reschedule(&wq_canary, K_NO_WAIT);
	watchdog_init();

	int64_t last_tick = 0;
	atomic_val_t last_hb = atomic_get(&wq_heartbeat) - 1;   /* != hb -> feed once now */
	while (1) {
		/* Pet the watchdog — but only if the system workqueue is also making
		 * progress (heartbeat advanced). If EITHER this loop wedges (stops petting)
		 * or the workqueue wedges (heartbeat stalls), the SoC resets within
		 * WDT_TIMEOUT_MS and recovers. */
		atomic_val_t hb = atomic_get(&wq_heartbeat);
		if (hb != last_hb) {
			last_hb = hb;
			watchdog_feed();
		}
#if !BUTTON_TEST
		/* Live hold-to-reset/bootloader hint (cheap; only redraws on change). */
		elron_ui_button_hint(btn3_hint);
		if (btn3_held) {
			backlight_set(true);   /* make the hint visible even if it's dark */
		}

		/* Game mode takes over the loop: tick ~30 fps, keep the screen lit,
		 * and skip the schedule refresh until the player exits. */
		if (elron_game_active()) {
			if (!screen_on_state) {
				/* Drain the flush thread before toggling DC (see screen_update). */
				lv_obj_invalidate(lv_scr_act());
				lv_refr_now(NULL);
				display_blanking_off(display_dev);
				screen_on_state = true;
			}
			backlight_set(true);
			elron_game_tick();
			lv_task_handler();
			int64_t gnow = k_uptime_get();
			if (gnow - last_tick >= 1000) {
				last_tick = gnow;
				battery_charge_manage();
			}
			k_sleep(K_MSEC(33));
			continue;
		}
#endif
		/* Only drive LVGL while the panel is actually showing something. Overnight
		 * (screen blanked) this keeps the flush thread idle — saving power and
		 * guaranteeing no flush is in flight when screen_update() re-issues DISP_ON
		 * at 09:00, which is what closes the DC-line race. screen_update() itself
		 * does its own render at each on/off transition. */
		if (screen_on_state) {
			lv_task_handler();
		}

		/* Once a second: screen schedule + UI + charge cap (uptime-based so it
		 * works regardless of how long we idle below). */
		int64_t now = k_uptime_get();
		if (now - last_tick >= 1000) {
			last_tick = now;
			if (screen_update()) {
				elron_ui_refresh(elron_ble_connected());
			}
			battery_charge_manage();
		}

		/* Overnight (screen off) idle much longer so the CPU sits in WFI most
		 * of the time — low-power without losing the clock or BLE. While the
		 * screen is on, tick fast for smooth scrolling. */
		k_sleep(screen_on_state ? K_MSEC(15) : K_MSEC(500));
	}
	return 0;
}
