/*
 * PHYLAX Blocks: "this one" on a key block.
 *
 * While the main block asks "Key block 2b69c617 is locked. Unlock it?" (or
 * "New key block found. Set it up?"), it tells that key block over the bus
 * (PHYLAX_MSG_ATTENTION, repeated every PHYLAX_ATTENTION_REPEAT_MS), and the
 * key block shows "Main block asks: unlock this block?" with its own
 * fingerprint, so the user knows which block in the stack is meant. Without
 * a repeat for PHYLAX_ATTENTION_TTL_MS (main block gone) it shows its home
 * screen again. Older key firmware ignores the message.
 */

#ifndef __PHYLAX_ATTENTION_H__
#define __PHYLAX_ATTENTION_H__

#include <stdbool.h>
#include <stdint.h>

// PHYLAX_MSG_ATTENTION payload: what the main block asks about the block.
typedef enum {
  PHYLAX_ATTENTION_OFF = 0,
  PHYLAX_ATTENTION_UNLOCK = 1,
  PHYLAX_ATTENTION_SETUP = 2,
} PhylaxAttention;

#define PHYLAX_ATTENTION_REPEAT_MS 2000
#define PHYLAX_ATTENTION_TTL_MS 6000

// Key block: a PHYLAX_MSG_ATTENTION frame arrived.
void phylax_attention_on_frame(const uint8_t *payload, uint16_t len);
// Key block: from the main loop (expiry).
void phylax_attention_poll(void);
// Key block: the home screen. True if the request was drawn instead.
bool phylax_attention_draw_home(void);
// Key block: the main block's request is shown. The block does not go to
// sleep (auto-lock screensaver) meanwhile: asleep, it would be woken by the
// next repeat and fall asleep again at once (a blinking screen).
bool phylax_attention_active(void);

// Main block: keep `kind` on the key block at `addr` (PHYLAX_ADDR_NONE or
// PHYLAX_ATTENTION_OFF: none). Call often; it sends only on a change and
// every PHYLAX_ATTENTION_REPEAT_MS.
void phylax_attention_target(uint8_t addr, PhylaxAttention kind);

#endif
