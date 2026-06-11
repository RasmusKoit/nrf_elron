/* Three front-panel push-buttons (Seeed XIAO nRF52840 header).
 *
 * Wiring: each button bridges one input pin to a shared common pin.
 *   button 0 -> D0 (P0.02)
 *   button 1 -> D1 (P0.03)
 *   button 2 -> D2 (P0.28)
 *   common   -> D8 (P1.13), driven low in firmware
 * (D9/P1.14 is the display CS, so the common can only be D8.)
 *
 * Inputs use internal pull-ups, so a press pulls the line low. Debounce is
 * interrupt-driven: edges (re)arm a delayed sampler, so the buttons cost zero
 * wakeups while idle — important for the overnight low-power loop.
 */
#ifndef ELRON_BUTTONS_H_
#define ELRON_BUTTONS_H_

#include <stdbool.h>
#include <stdint.h>

#define ELRON_NUM_BUTTONS 3

/* Human-readable pin label per button index, in physical left->right order. */
extern const char *const elron_button_name[ELRON_NUM_BUTTONS];

/* Fired from a workqueue context on each debounced edge.
 *   idx     = 0..ELRON_NUM_BUTTONS-1 (0 = D0, 1 = D1, 2 = D2)
 *   pressed = true on press, false on release
 */
typedef void (*elron_button_cb)(uint8_t idx, bool pressed);

/* Configure the common + the three inputs and start debounced reporting.
 * Returns 0 on success, negative errno otherwise. */
int elron_buttons_init(elron_button_cb cb);

/* Current debounced state bitmask: bit i set = button i is held. */
uint32_t elron_buttons_state(void);

#endif /* ELRON_BUTTONS_H_ */
