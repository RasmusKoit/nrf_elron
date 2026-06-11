/* "Catch the Train" — a one-button endless runner hidden behind the departures
 * screen. Enter from the schedule view with the #1+#2 chord; #1 = GO (start /
 * jump / replay), #2 = BACK (quit to schedule). See README "Buttons". */
#ifndef ELRON_GAME_H_
#define ELRON_GAME_H_

#include <stdbool.h>
#include <stdint.h>

/* Build the (hidden) game screen once and load the persisted high score.
 * Call after elron_ui_init(). */
void elron_game_init(void);

/* True while the game screen is the active LVGL screen. */
bool elron_game_active(void);

/* Switch from the schedule view into the game (title screen). */
void elron_game_enter(void);

/* Feed a button edge to the game. idx 0 = #1 (GO), idx 1 = #2 (BACK). */
void elron_game_button(uint8_t idx, bool pressed);

/* Advance one frame; call ~30x/s from the main loop while active. */
void elron_game_tick(void);

#endif /* ELRON_GAME_H_ */
