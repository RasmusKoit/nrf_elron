#include "schedule.h"
#include "ble_svc.h"
#include "ui.h"
#include "battery.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/uart/cdc_acm.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_power.h>
#include <lvgl.h>
#include <string.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* Set to 1 to run ONLY a full-screen colour-cycle panel test (no LVGL/BLE). */
#define PANEL_TEST 0

static const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
/* Backlight: active-high GPIO P1.11 (gpio1 pin 11). */
static const struct device *gpio1_dev = DEVICE_DT_GET(DT_NODELABEL(gpio1));
#define BACKLIGHT_PIN 11

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
	uint16_t len = pending_len;
	if (len == 0) {
		return;
	}
	bool from_serial = pending_from_serial;
	int rc = elron_schedule_apply_wire(pending_buf, len);
	pending_len = 0;
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
	memcpy(pending_buf, buf, len);
	pending_len = len;
	pending_from_serial = from_serial;
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
	if (device_is_ready(gpio1_dev)) {
		gpio_pin_set(gpio1_dev, BACKLIGHT_PIN, on ? 1 : 0);
	}
}

static void backlight_init(void)
{
	if (device_is_ready(gpio1_dev)) {
		gpio_pin_configure(gpio1_dev, BACKLIGHT_PIN, GPIO_OUTPUT_ACTIVE);
	}
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
		if (on) {
			display_blanking_off(display_dev);   /* re-init panel output */
			elron_ui_refresh(elron_ble_connected());
			lv_obj_invalidate(lv_scr_act());     /* force a full redraw */
		} else {
			backlight_set(false);
			display_blanking_on(display_dev);    /* panel output off */
		}
		screen_on_state = on;
	}
	backlight_set(on);
	return on;
}

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
		LOG_ERR("display device NOT READY");
		return 0;
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

	k_work_init(&apply_work, apply_work_fn);

	/* Persisted schedule first, so the screen has content before any BLE. */
	elron_schedule_init();
	/* Always advertise "needs sync" after a boot: the schedule may be persisted
	 * but the clock is RAM-only, so the companion must re-send to set the time.
	 * This is what makes the board auto-resync when plugged in / reflashed. */
	elron_ble_set_has_schedule(false);

	elron_ui_init();
	elron_ui_refresh(false);

	if (elron_ble_init(on_ble_rx)) {
		LOG_ERR("ble init failed");
	}

	int64_t last_tick = 0;
	while (1) {
		lv_task_handler();

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
