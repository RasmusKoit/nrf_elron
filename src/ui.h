/* LVGL screen for the Elron departures. */
#ifndef ELRON_UI_H_
#define ELRON_UI_H_

#include <stdbool.h>
#include <lvgl.h>

/* Build the static widget tree on the active LVGL screen. */
void elron_ui_init(void);

/* The schedule LVGL screen, so other screens (e.g. the game) can return to it. */
lv_obj_t *elron_ui_screen(void);

/* Recompute "next" departures from the schedule + clock and update the labels.
 * Safe to call periodically (e.g. once per second) and after a BLE sync. */
void elron_ui_refresh(bool ble_connected);

/* Transient full-width hint shown while button #1 is held (hold-to-reset /
 * hold-longer-for-bootloader). Cheap to call every loop; only redraws on change.
 *   0 = hide, 1 = "Hold 3s for reset", 2 = "Keep holding for bootloader",
 *   3 = "Release for bootloader" (armed), 4 = "Cancelled",
 *   5 = "Release NOW or it cancels" (last second before the cancel). */
void elron_ui_button_hint(int state);

#endif /* ELRON_UI_H_ */
