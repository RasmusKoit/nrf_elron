/* Rough LiPo battery indicator for the XIAO nRF52840.
 * VBAT is divided onto AIN7 (P0.31); P0.14 low enables the divider. */
#ifndef ELRON_BATTERY_H_
#define ELRON_BATTERY_H_

#include <stdbool.h>

void battery_init(void);

/* Manage the charge cap: stop charging near ~90%, resume if it sags, and top
 * up again whenever USB power returns after a spell on battery.
 * Call periodically (e.g. once a second) — on battery too, so it can see USB
 * come and go. */
void battery_charge_manage(void);

/* True while USB (VBUS) power is present. */
bool battery_usb_present(void);

/* True once the cell is nearly empty while running on battery. Latches until
 * USB power returns. Samples the ADC at most every ~30 s, so it is cheap to
 * call every second. */
bool battery_low(void);

/* An LVGL battery/charge symbol reflecting a rough level (cached ~30s).
 * Returns "" if the ADC isn't ready. */
const char *battery_symbol(void);

#endif /* ELRON_BATTERY_H_ */
