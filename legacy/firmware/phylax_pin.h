/*
 * PHYLAX Blocks - key block: PIN entry on its own two buttons, and the
 * button-timing entropy pool.
 *
 * When the main block sets a key block up with no PC (see phylax_setup.h),
 * it first sends LOCAL_UI(1). From then on, a PIN that the key block needs
 * for a bus conversation is entered on the key block itself instead of being
 * sent to the host as a PinMatrixRequest:
 *
 *     Choose a PIN
 *        * * * *
 *     <  4   [5]   6  >
 *     both = select
 *
 *   left / right   move through 1..9, OK, DEL (it wraps around)
 *   both together  select the item in the middle
 *
 * Only digits 1..9, the same as the PC's PIN matrix, so a PIN chosen here
 * can later be typed in Suite when signing.
 *
 * Every button change while entering the PIN is fed into an entropy pool
 * (when it happened, in ms and in loop turns since the last change). The new
 * seed's internal entropy becomes SHA256(TRNG || pool), so the presses can
 * only add to the hardware random number generator, never weaken it.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef __PHYLAX_PIN_H__
#define __PHYLAX_PIN_H__

#include <stdbool.h>
#include <stdint.h>

#include "messages-common.pb.h"

// Set by the main block's LOCAL_UI frame.
void phylax_set_local_ui(bool on);
// True when PINs for the current (bus) conversation are entered locally.
bool phylax_local_ui(void);

// The PIN entered on the buttons, or NULL if cancelled (Cancel/Initialize
// from the main block).
const char *phylax_pin_entry(PinMatrixRequestType type);

// Entropy pool: note an event, and mix the pool into 32 bytes of entropy
// (out = SHA256(out || pool)); the pool is emptied. No events: unchanged.
void phylax_entropy_note(uint32_t a, uint32_t b);
void phylax_entropy_mix(uint8_t entropy[32]);
uint32_t phylax_entropy_events(void);

#endif
