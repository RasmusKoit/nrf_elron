/* LVGL screen for the Elron departures. */
#ifndef ELRON_UI_H_
#define ELRON_UI_H_

#include <stdbool.h>

/* Build the static widget tree on the active LVGL screen. */
void elron_ui_init(void);

/* Recompute "next" departures from the schedule + clock and update the labels.
 * Safe to call periodically (e.g. once per second) and after a BLE sync. */
void elron_ui_refresh(bool ble_connected);

/* Transient full-width hint shown while button #3 is held (hold-to-reset /
 * hold-longer-for-bootloader). Cheap to call every loop; only redraws on change.
 *   0 = hide, 1 = "Hold 3s for reset", 2 = "Keep holding for bootloader",
 *   3 = "Release for bootloader" (armed), 4 = "Cancelled". */
void elron_ui_button_hint(int state);

#endif /* ELRON_UI_H_ */
