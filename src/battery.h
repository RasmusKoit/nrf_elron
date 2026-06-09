/* Rough LiPo battery indicator for the XIAO nRF52840.
 * VBAT is divided onto AIN7 (P0.31); P0.14 low enables the divider. */
#ifndef ELRON_BATTERY_H_
#define ELRON_BATTERY_H_

void battery_init(void);

/* Manage the charge cap: stop charging near ~90% and resume if it sags.
 * Call periodically (e.g. once a second). */
void battery_charge_manage(void);

/* An LVGL battery/charge symbol reflecting a rough level (cached ~30s).
 * Returns "" if the ADC isn't ready. */
const char *battery_symbol(void);

#endif /* ELRON_BATTERY_H_ */
