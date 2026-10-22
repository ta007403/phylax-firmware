/*
 * PHYLAX Blocks - main block: set up a new key block with no PC.
 *
 * When a key block without a seed joins the stack, the main block asks
 *
 *     New key block found. Set it up?        [No]  [Set up]
 *
 * on its own screen. On "Set up" it talks to the key block over the bus the
 * way Trezor Suite would talk to a Trezor over USB:
 *
 *   main                                  key block
 *   LOCAL_UI(1)  ------------------------>  PINs on its own buttons
 *   ResetDevice(24 words, PIN)  --------->  "Create a new wallet?"   (buttons)
 *                                           Choose a PIN, enter it again
 *   <------------------------------------  EntropyRequest
 *   EntropyAck(32 bytes from the main
 *              block's TRNG)  ------------>  seed = SHA256(
 *                                              SHA256(TRNG || button timings)
 *                                              || main block's bytes)
 *                                           24 words shown, twice   (buttons)
 *   <------------------------------------  Success
 *   LOCAL_UI(0)  ------------------------>
 *
 * Every ButtonRequest is answered with a ButtonAck at once: the user presses
 * the key block's own buttons. The seed never leaves the key block.
 *
 * Afterwards the key block's HELLO carries its fingerprint, and the saved-
 * wallet logic offers "New 2-of-2 wallet?" as usual.
 *
 * "No" (or cancelling on the key block) is remembered until that block
 * leaves the stack. A mistyped second PIN asks again after a short pause.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef __PHYLAX_SETUP_H__
#define __PHYLAX_SETUP_H__

#include <stdbool.h>

// Prompting or running a setup: nothing else may talk to key blocks or use
// the screen.
bool phylax_setup_busy(void);
// The key block is being set up (past the prompt).
bool phylax_setup_running(void);
// One step. Called by the wallet poll when it is idle and the screen is
// free. Returns true if setup uses the screen (prompt, progress, result).
bool phylax_setup_poll(void);
// Buttons on the main block (prompt only).
void phylax_setup_ui_poll(void);
// Something with priority (signing, the PC in select mode) took over.
void phylax_setup_abort(void);

#endif
