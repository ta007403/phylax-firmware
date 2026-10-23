/*
 * PHYLAX Blocks - your own randomness: coin flips or dice rolls entered on
 * the block's two buttons, mixed into a new seed.
 *
 *     Add your own randomness?               [Skip] [Add]
 *     Coin flips or dice?                    [Coin] [Dice]
 *
 *   coin  256 flips of a real coin: left = heads, right = tails,
 *         both = undo the last flip (before the first: back to the question)
 *   dice  99 rolls of a real 6-sided die (99 x log2 6 = 255.9 bits): the
 *         carousel 1..6, DEL like the PIN entry, both buttons select
 *         (DEL before the first roll: back to the question)
 *
 * Either is full strength on its own. Only SHA-256("PHYLAX coin flips v1" or
 * "PHYLAX dice rolls v1" || flips) is kept; it is mixed into the block's own
 * random bytes (out = SHA256(out || digest)), never replacing them.
 *
 * Used in two places:
 *  - the main block setting up a new key block (phylax_setup.c): the result
 *    goes into the EntropyAck the main block sends;
 *  - a PHYLAX block creating its own wallet (ResetDevice from the PC, or
 *    from Suite through the main block): reset.c asks after the PIN and mixes
 *    the result into every entropy round (the entropy check still works,
 *    since the mixed bytes are what the block commits to and reveals).
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef __PHYLAX_USERENT_H__
#define __PHYLAX_USERENT_H__

#include <stdbool.h>
#include <stdint.h>

#include "phylax_pin.h"

typedef enum {
  PHYLAX_USERENT_CONTINUE,  // still choosing / entering
  PHYLAX_USERENT_SKIPPED,   // "Skip": the block's own random numbers only
  PHYLAX_USERENT_DONE,      // all flips or rolls entered
} PhylaxUserentResult;

// Non-blocking use (main loop): show "Add your own randomness?", feed button
// events, redraw when something else covered the screen.
void phylax_userent_begin(void);
PhylaxUserentResult phylax_userent_event(PhylaxButtonEvent e);
void phylax_userent_redraw(void);  // also marks layoutLast while showing

// After DONE: out = SHA256(out || digest). No-op without a result.
bool phylax_userent_have(void);
void phylax_userent_mix(uint8_t entropy[32]);
void phylax_userent_forget(void);

// Blocking, for creating this block's own wallet (reset.c): asks only on
// PHYLAX blocks and not when the main block is setting this key block up
// (it asked already). Keeps the host waiting with a ButtonRequest. Returns
// false if the host cancelled (Cancel / Initialize).
bool phylax_userent_collect_for_reset(void);

#endif
