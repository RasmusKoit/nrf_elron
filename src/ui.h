/* LVGL screen for the Elron departures. */
#ifndef ELRON_UI_H_
#define ELRON_UI_H_

#include <stdbool.h>

/* Build the static widget tree on the active LVGL screen. */
void elron_ui_init(void);

/* Recompute "next" departures from the schedule + clock and update the labels.
 * Safe to call periodically (e.g. once per second) and after a BLE sync. */
void elron_ui_refresh(bool ble_connected);

#endif /* ELRON_UI_H_ */
