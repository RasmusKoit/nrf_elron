#include "battery.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_power.h>
#include <lvgl.h>

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

/* XIAO nRF52840 P0.13 sets the BQ25101 charge current:
 *   low = 100 mA, input/high-Z = 50 mA, high = 0 mA (off).
 * We use 100 mA for the 750 mAh cell. */
#define CHARGE_CURRENT_MA 100
#define CHG_CTRL_PIN      13

/* Stop charging at ~90% (4.11 V) and don't resume until ~50% (3.75 V): a wide
 * band that spares the LiPo and barely cycles on continuous USB. Thresholds are
 * the standard LiPo voltage->SoC curve (4110=90%, 3750=50%). */
#define CHG_STOP_MV       4110
#define CHG_RESUME_MV     3750

/* Nearly-empty cutoff while on battery. Below this the caller keeps the screen
 * (the backlight is ~90% of the power budget) off, so the last few percent keep
 * the clock and BLE alive instead of running the cell into brown-out — which
 * also deep-discharged the LiPo every time it happened. The ADC reading is
 * noisy (+-50 mV), so require a few consecutive low samples (one every ~30 s);
 * the very first sample after a boot counts on its own, so a brown-out reboot
 * can't relight the backlight and loop. */
#define LOW_BATT_MV       3400
#define LOW_BATT_SAMPLES  3

/* XIAO nRF52840 divider is R1=1M / R2=510k, so real Vbat = ADC mV * (1000+510)/510
 * = *2.961. This is the documented value — no per-board calibration needed (the
 * ~2-3% resistor tolerance is negligible for a rough icon + wide-band cap). */
#define VBAT_MUL_X1000    2961

static const struct adc_dt_spec adc = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));
static const struct device *const gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
#define VBAT_ENABLE_PIN 14   /* P0.14: low = connect divider */

static bool ready;
static bool charging_on = true;
static int64_t last_read;
static int cached_mv = -1;

static void chg_set(bool on)
{
	if (!device_is_ready(gpio0)) {
		return;
	}
	if (!on) {
		gpio_pin_configure(gpio0, CHG_CTRL_PIN, GPIO_OUTPUT_HIGH);   /* 0 mA */
	} else if (CHARGE_CURRENT_MA >= 100) {
		gpio_pin_configure(gpio0, CHG_CTRL_PIN, GPIO_OUTPUT_LOW);    /* 100 mA */
	} else {
		gpio_pin_configure(gpio0, CHG_CTRL_PIN, GPIO_INPUT);         /* 50 mA */
	}
	charging_on = on;
}

void battery_init(void)
{
	if (!adc_is_ready_dt(&adc) || adc_channel_setup_dt(&adc) != 0) {
		LOG_WRN("battery ADC not ready");
		return;
	}
	if (device_is_ready(gpio0)) {
		gpio_pin_configure(gpio0, VBAT_ENABLE_PIN, GPIO_OUTPUT_INACTIVE);
	}
	chg_set(true);
	ready = true;
}

static int read_vbat_mv(void)
{
	int16_t raw = 0;
	struct adc_sequence seq = { .buffer = &raw, .buffer_size = sizeof(raw) };

	gpio_pin_set(gpio0, VBAT_ENABLE_PIN, 0);   /* enable divider */
	k_msleep(2);
	adc_sequence_init_dt(&adc, &seq);
	int rc = adc_read_dt(&adc, &seq);
	gpio_pin_set(gpio0, VBAT_ENABLE_PIN, 1);   /* disable to save power */
	if (rc) {
		return -1;
	}
	int32_t mv = raw;
	if (adc_raw_to_millivolts_dt(&adc, &mv) != 0) {
		return -1;
	}
	return (int)((int64_t)mv * VBAT_MUL_X1000 / 1000);
}

bool battery_usb_present(void)
{
	return nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
}

static bool low_latched;
static uint8_t low_cnt;
static bool low_first_sample = true;

/* Feed one fresh ADC sample into the low-battery latch. */
static void low_eval(int mv)
{
	bool first = low_first_sample;

	low_first_sample = false;
	if (mv < 0 || mv >= LOW_BATT_MV || battery_usb_present()) {
		low_cnt = 0;
		return;
	}
	if (first) {
		/* Booted straight into a low reading: confirm it with one immediate
		 * re-read rather than waiting ~60 s with the backlight on, but don't
		 * latch on a single (possibly glitched) first conversion either. */
		int again = read_vbat_mv();

		if (again < 0 || again >= LOW_BATT_MV) {
			low_cnt = 1;
			return;
		}
	} else if (++low_cnt < LOW_BATT_SAMPLES) {
		return;
	}
	if (!low_latched) {
		LOG_WRN("battery low (%d mV) -> screen stays off until USB", mv);
	}
	low_latched = true;
}

static int vbat_cached(void)
{
	int64_t now = k_uptime_get();
	if (cached_mv < 0 || now - last_read > 30000) {
		cached_mv = read_vbat_mv();
		last_read = now;
		LOG_INF("vbat ~%d mV (chg=%d)", cached_mv, charging_on);
		low_eval(cached_mv);
	}
	return cached_mv;
}

bool battery_low(void)
{
	if (!ready) {
		return false;
	}
	if (battery_usb_present()) {
		low_latched = false;
		low_cnt = 0;
		return false;
	}
	(void)vbat_cached();   /* takes (and evaluates) a fresh sample every ~30 s */
	return low_latched;
}

void battery_charge_manage(void)
{
	static bool usb_prev;
	bool usb = battery_usb_present();

	/* USB just came back after a spell on battery: top up straight away. The
	 * wide 4.11 -> 3.75 V hold band below is right for a board that sits on USB
	 * (it barely cycles the cell), but on its own it meant a night on battery
	 * was never recharged the next day — the charge stayed paused until the cell
	 * had sagged to ~50%, so the board routinely went into a weekend half empty. */
	if (ready && usb && !usb_prev && !charging_on) {
		LOG_INF("USB power back -> top up");
		chg_set(true);
	}
	usb_prev = usb;

	if (!ready || !usb) {
		return;   /* can only charge on USB */
	}
	int mv = vbat_cached();
	if (mv < 0) {
		return;
	}
	if (charging_on && mv >= CHG_STOP_MV) {
		LOG_INF("charge cap reached (%d mV) -> pause", mv);
		chg_set(false);
	} else if (!charging_on && mv <= CHG_RESUME_MV) {
		LOG_INF("battery sagged (%d mV) -> resume charge", mv);
		chg_set(true);
	}
}

const char *battery_symbol(void)
{
	if (!ready) {
		return "";
	}
	/* Plugged in -> always show the charge bolt. We still pause the actual charge
	 * current at the cap for LiPo longevity (charging_on toggles), but to the user
	 * "on USB" should read as charging — otherwise a topped-off battery sitting in
	 * the 4.11->3.75 V hold band looks like charging silently stopped. */
	if (battery_usb_present()) {
		return LV_SYMBOL_CHARGE;
	}
	int mv = vbat_cached();
	if (mv < 0) {
		return "";
	}
	if (mv >= 4100) {
		return LV_SYMBOL_BATTERY_FULL;
	} else if (mv >= 3900) {
		return LV_SYMBOL_BATTERY_3;
	} else if (mv >= 3750) {
		return LV_SYMBOL_BATTERY_2;
	} else if (mv >= 3600) {
		return LV_SYMBOL_BATTERY_1;
	}
	return LV_SYMBOL_BATTERY_EMPTY;
}
