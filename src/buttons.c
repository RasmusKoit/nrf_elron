#include "buttons.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(buttons, LOG_LEVEL_INF);

/* nRF52840 GPIO ports. The three buttons all live on gpio0; the common is on
 * gpio1 (D8 = P1.13). */
static const struct device *const gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *const gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));

#define COMMON_PIN 13   /* on gpio1: D8 = P1.13, driven low */

/* Button input pins on gpio0 (active-low: pressed -> shorted to driven-low common).
 * Ordered by PHYSICAL position left->right, which is D1, D2, D0 on this build —
 * so index 0/1/2 = the 1st/2nd/3rd button as mounted, not the D-number. */
static const gpio_pin_t btn_pin[ELRON_NUM_BUTTONS]  = { 3, 28, 2 };       /* D1, D2, D0 */
const char *const elron_button_name[ELRON_NUM_BUTTONS] = { "D1", "D2", "D0" };

#define DEBOUNCE_MS 25

static struct gpio_callback   gpio0_cb;
static struct k_work_delayable debounce_work;
static elron_button_cb        user_cb;
static uint32_t               stable_state;   /* bit i = button i pressed */

/* Sample all three inputs: physical low (== 0) means pressed. */
static uint32_t sample_raw(void)
{
	uint32_t m = 0;
	for (int i = 0; i < ELRON_NUM_BUTTONS; i++) {
		if (gpio_pin_get_raw(gpio0, btn_pin[i]) == 0) {
			m |= BIT(i);
		}
	}
	return m;
}

/* Runs DEBOUNCE_MS after the line last went quiet: latch the stable state and
 * report any buttons whose level changed. */
static void debounce_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	uint32_t now = sample_raw();
	uint32_t changed = now ^ stable_state;

	stable_state = now;
	if (!changed || !user_cb) {
		return;
	}
	for (int i = 0; i < ELRON_NUM_BUTTONS; i++) {
		if (changed & BIT(i)) {
			user_cb((uint8_t)i, (now & BIT(i)) != 0);
		}
	}
}

/* GPIO edge interrupt: every edge (incl. bounce) just re-arms the sampler, so
 * we only latch once the input has been quiet for DEBOUNCE_MS. */
static void edge_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	k_work_reschedule(&debounce_work, K_MSEC(DEBOUNCE_MS));
}

int elron_buttons_init(elron_button_cb cb)
{
	user_cb = cb;

	if (!device_is_ready(gpio0) || !device_is_ready(gpio1)) {
		LOG_ERR("gpio not ready");
		return -ENODEV;
	}

	/* Shared common: drive D8 (P1.13) low so a press pulls its input low. */
	int rc = gpio_pin_configure(gpio1, COMMON_PIN, GPIO_OUTPUT_LOW);
	if (rc) {
		LOG_ERR("common (D8) cfg: %d", rc);
		return rc;
	}

	k_work_init_delayable(&debounce_work, debounce_fn);

	uint32_t mask = 0;
	for (int i = 0; i < ELRON_NUM_BUTTONS; i++) {
		rc = gpio_pin_configure(gpio0, btn_pin[i], GPIO_INPUT | GPIO_PULL_UP);
		if (rc) {
			LOG_ERR("btn%d (P0.%02d) cfg: %d", i, btn_pin[i], rc);
			return rc;
		}
		rc = gpio_pin_interrupt_configure(gpio0, btn_pin[i], GPIO_INT_EDGE_BOTH);
		if (rc) {
			LOG_ERR("btn%d (P0.%02d) int: %d", i, btn_pin[i], rc);
			return rc;
		}
		mask |= BIT(btn_pin[i]);
	}

	/* One callback object covers all three pins (all on gpio0). */
	gpio_init_callback(&gpio0_cb, edge_isr, mask);
	gpio_add_callback(gpio0, &gpio0_cb);

	/* Latch the initial level (in case a button is held at boot). */
	stable_state = sample_raw();
	LOG_INF("buttons ready: D0/D1/D2, common D8; state=0x%x", stable_state);
	return 0;
}

uint32_t elron_buttons_state(void)
{
	return stable_state;
}
