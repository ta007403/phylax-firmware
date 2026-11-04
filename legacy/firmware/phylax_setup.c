/*
 * PHYLAX Blocks - main block: set up a new key block with no PC.
 * See phylax_setup.h.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "phylax_setup.h"

#include <string.h>

#include "buttons.h"
#include "gettext.h"
#include "layout2.h"
#include "memzero.h"
#include "messages.pb.h"
#include "oled.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "phylax_bus.h"
#include "phylax_pin.h"
#include "phylax_stack.h"
#include "phylax_userent.h"
#include "phylax_wallet.h"
#include "rand.h"
#include "rng.h"
#include "sha2.h"
#include "timer.h"

#define RESULT_SHOW_MS 3000
#define RETRY_PAUSE_MS 3000  // after a mistyped PIN: ask again

typedef enum {
  S_IDLE,
  S_PROMPT,   // "New key block found. Set it up?"
  S_ENT,      // your own randomness: coin flips or dice (phylax_userent.c)
  S_RUNNING,  // the user follows the key block's screen
  S_RESULT,   // "Key block is ready." / why it stopped
} SetupState;

// What the conversation with the key block is for.
typedef enum {
  JOB_SETUP,   // a key block without a seed: create one on it
  JOB_UNLOCK,  // a locked key block of a saved wallet: its PIN on its buttons
} SetupJob;

static SetupState state = S_IDLE;
static SetupJob job = JOB_SETUP;
static uint32_t target_fp = 0;  // JOB_UNLOCK: the locked block's fingerprint
static uint8_t target_uid[PHYLAX_UID_SIZE];
static uint8_t target_addr = PHYLAX_ADDR_NONE;
static uint32_t result_until_ms = 0;
static uint32_t retry_after_ms = 0;

// Blocks the user said "no" to, until they leave the stack.
static uint8_t declined[PHYLAX_STACK_MAX][PHYLAX_UID_SIZE];
static uint8_t declined_n = 0;

// Locked blocks the user said "Later" to, until they leave the stack or are
// unlocked (so the next time they lock, they are asked about again).
static uint8_t later[PHYLAX_STACK_MAX][PHYLAX_UID_SIZE];
static uint8_t later_n = 0;

// ------------------------------------------------------------ stack

static const PhylaxStackEntry *find_uid(const uint8_t *uid) {
  const PhylaxStackEntry *e;
  for (size_t i = 0; (e = phylax_stack_get(i)) != NULL; i++) {
    if (memcmp(e->hello.uid, uid, PHYLAX_UID_SIZE) == 0) return e;
  }
  return NULL;
}

static bool is_declined(const uint8_t *uid) {
  for (int i = 0; i < declined_n; i++) {
    if (memcmp(declined[i], uid, PHYLAX_UID_SIZE) == 0) return true;
  }
  return false;
}

static void decline(const uint8_t *uid) {
  if (is_declined(uid) || declined_n >= PHYLAX_STACK_MAX) return;
  memcpy(declined[declined_n++], uid, PHYLAX_UID_SIZE);
}

static bool is_later(const uint8_t *uid) {
  for (int i = 0; i < later_n; i++) {
    if (memcmp(later[i], uid, PHYLAX_UID_SIZE) == 0) return true;
  }
  return false;
}

static void say_later(const uint8_t *uid) {
  if (is_later(uid) || later_n >= PHYLAX_STACK_MAX) return;
  memcpy(later[later_n++], uid, PHYLAX_UID_SIZE);
}

static bool still_locked(const uint8_t *uid);

static void prune_later(void) {
  for (int i = 0; i < later_n;) {
    if (!still_locked(later[i])) {
      memmove(later[i], later[i + 1],
              (size_t)(later_n - i - 1) * PHYLAX_UID_SIZE);
      later_n--;
    } else {
      i++;
    }
  }
}

static void prune_declined(void) {
  for (int i = 0; i < declined_n;) {
    if (find_uid(declined[i]) == NULL) {
      memmove(declined[i], declined[i + 1],
              (size_t)(declined_n - i - 1) * PHYLAX_UID_SIZE);
      declined_n--;
    } else {
      i++;
    }
  }
}

// A locked key block with a seed. It reports its fingerprint only if it was
// unlocked at least once since its firmware was installed.
static bool locked_key_block(const PhylaxStackEntry *e) {
  return e->hello.kind == PHYLAX_KIND_KEY && e->addr != PHYLAX_ADDR_NONE &&
         (e->hello.flags & PHYLAX_HELLO_INITIALIZED) &&
         (e->hello.flags & PHYLAX_HELLO_LOCKED);
}

static bool still_locked(const uint8_t *uid) {
  const PhylaxStackEntry *e = find_uid(uid);
  return e != NULL && locked_key_block(e);
}

// A locked key block that a saved wallet needs, not put off with "Later".
static const PhylaxStackEntry *find_locked_wallet_block(void) {
  const PhylaxStackEntry *e;
  for (size_t i = 0; (e = phylax_stack_get(i)) != NULL; i++) {
    // Without a fingerprint it cannot say which wallet it belongs to; ask
    // anyway, or it could never be unlocked from here.
    if (locked_key_block(e) && !is_later(e->hello.uid) &&
        (!(e->hello.flags & PHYLAX_HELLO_HAS_FP) ||
         phylax_wallet_uses_fingerprint(e->hello.fingerprint))) {
      return e;
    }
  }
  return NULL;
}

// A key block in the stack with an address and no seed, not declined.
static const PhylaxStackEntry *find_blank_key_block(void) {
  const PhylaxStackEntry *e;
  for (size_t i = 0; (e = phylax_stack_get(i)) != NULL; i++) {
    if (e->hello.kind == PHYLAX_KIND_KEY && e->addr != PHYLAX_ADDR_NONE &&
        !(e->hello.flags & PHYLAX_HELLO_INITIALIZED) &&
        !is_declined(e->hello.uid)) {
      return e;
    }
  }
  return NULL;
}

// ------------------------------------------------------------ screens

static char fp_text[9];
static char fp_line[24];  // "Key block 73c5da0a"

// The names on the unlock screens: its fingerprint, or "0x10" (its bus
// address) if it does not report one yet.
static void name_unlock_target(const PhylaxStackEntry *e) {
  static const char hexd[] = "0123456789abcdef";
  if (e->hello.flags & PHYLAX_HELLO_HAS_FP) {
    target_fp = e->hello.fingerprint;
    for (int i = 0; i < 8; i++) {
      fp_text[i] = hexd[(target_fp >> (28 - 4 * i)) & 0xF];
    }
    fp_text[8] = '\0';
  } else {
    target_fp = 0;
    fp_text[0] = '0';
    fp_text[1] = 'x';
    fp_text[2] = hexd[(e->addr >> 4) & 0xF];
    fp_text[3] = hexd[e->addr & 0xF];
    fp_text[4] = '\0';
  }
  strlcpy(fp_line, _("Key block "), sizeof(fp_line));
  strlcat(fp_line, fp_text, sizeof(fp_line));
}

static void show_state(void);

// Setup no longer owns the screen: what is shown counts as a plain dialog,
// so the wallet logic may replace it as before.
static void release_screen(void) {
  if (layoutLast == show_state || layoutLast == phylax_userent_redraw) {
    layoutLast = layoutDialog;
  }
}

// "Set it up?" takes only a press made while it is on screen.
static PhylaxPressGuard prompt_guard;

static void show_prompt(void) {
  layoutDialog(&bmp_icon_question, _("No"), _("Set up"), NULL,
               _("New key block found."), _("Set it up?"), NULL,
               _("You choose a PIN and"), _("write down 24 words."), NULL);
  layoutLast = show_state;
}

// With buttons, five lines fit; a sixth runs into them.

static void show_unlock_prompt(void) {
  layoutDialog(&bmp_icon_question, _("Later"), _("Unlock"), NULL, fp_line,
               _("is locked."), NULL, _("Unlock it with its PIN"),
               _("on its own buttons?"), NULL);
  layoutLast = show_state;
}

// While the key block asks for its PIN: Cancel stops it.
static PhylaxPressGuard cancel_guard;

static void show_running(void) {
  if (job == JOB_UNLOCK) {
    layoutDialog(&bmp_icon_info, _("Cancel"), NULL, NULL,
                 _("Unlocking key block"), fp_text, NULL, _("Enter its PIN on"),
                 _("its own buttons."), NULL);
  } else {
    layoutDialog(&bmp_icon_info, NULL, NULL, NULL, _("Setting up the"),
                 _("key block."), NULL, _("Follow the steps on"),
                 _("its own screen."), NULL);
  }
  layoutLast = show_state;
}

// The screen of the current step. Messages from the PC end with the home
// screen (fsm.c), so the poll draws the step again when that happened.
static void show_state(void) {
  layoutLast = show_state;  // this step's screen (not the home screen)
  switch (state) {
    case S_PROMPT:
      if (job == JOB_UNLOCK) {
        show_unlock_prompt();
      } else {
        show_prompt();
      }
      break;
    case S_ENT:
      phylax_userent_redraw();
      break;
    case S_RUNNING:
      show_running();
      break;
    default:
      break;
  }
}

static void show_result(const char *line1, const char *line2) {
  layoutDialog(&bmp_icon_info, NULL, NULL, NULL, _("PHYLAX"), line1, line2,
               NULL, NULL, NULL);
  state = S_RESULT;
  release_screen();
  result_until_ms = timer_ms() + RESULT_SHOW_MS;
}

// ------------------------------------------------------------ the conversation

static void end_conversation(bool cancel_key) {
  phylax_userent_forget();
  if (target_addr != PHYLAX_ADDR_NONE) {
    if (cancel_key) {
      phylax_bus_send_trezor_msg(target_addr, MessageType_MessageType_Cancel,
                                 NULL, 0);
    }
    uint8_t off = 0;
    phylax_bus_send(target_addr, PHYLAX_MSG_LOCAL_UI, &off, 1);
  }
  if (phylax_bus_trezor_peer() == target_addr) {
    phylax_bus_set_trezor_peer(PHYLAX_ADDR_NONE, NULL);
  }
  target_addr = PHYLAX_ADDR_NONE;
}

static void on_failure(const uint8_t *data, uint32_t len) {
  static Failure fail;
  memzero(&fail, sizeof(fail));
  pb_istream_t stream = pb_istream_from_buffer(data, len);
  bool decoded = pb_decode(&stream, Failure_fields, &fail);
  FailureType code =
      decoded && fail.has_code ? fail.code : FailureType_Failure_ProcessError;
  end_conversation(false);
  switch (code) {
    case FailureType_Failure_PinMismatch:
      retry_after_ms = timer_ms() + RESULT_SHOW_MS + RETRY_PAUSE_MS;
      show_result(_("The PINs did not match."), _("Let's try again."));
      break;
    case FailureType_Failure_ActionCancelled:
    case FailureType_Failure_PinCancelled:
      decline(target_uid);
      show_result(_("Setup cancelled."), NULL);
      break;
    default:
      retry_after_ms = timer_ms() + RESULT_SHOW_MS + RETRY_PAUSE_MS;
      show_result(_("Setup failed:"),
                  decoded && fail.has_message ? fail.message : NULL);
      break;
  }
}

static void on_key_message(uint16_t msg_id, const uint8_t *data, uint32_t len) {
  if (state != S_RUNNING) return;
  switch (msg_id) {
    case MessageType_MessageType_ButtonRequest:
      // The user confirms on the key block itself.
      phylax_bus_send_trezor_msg(target_addr, MessageType_MessageType_ButtonAck,
                                 NULL, 0);
      break;
    case MessageType_MessageType_EntropyRequest: {
      // The main block's TRNG plays the part of the PC's entropy.
      static EntropyAck ack;
      static uint8_t buf[EntropyAck_size];
      memzero(&ack, sizeof(ack));
      random_buffer(ack.entropy.bytes, 32);
      ack.entropy.size = 32;
      // Button timings while the user was choosing, then the user's own
      // coin flips or dice rolls: SHA256(random || flips). Both can only add.
      phylax_entropy_mix(ack.entropy.bytes);
      phylax_userent_mix(ack.entropy.bytes);
      phylax_userent_forget();
      pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
      bool ok = pb_encode(&stream, EntropyAck_fields, &ack);
      memzero(&ack, sizeof(ack));
      if (!ok) {
        end_conversation(true);
        show_result(_("Setup failed."), NULL);
        break;
      }
      phylax_bus_send_trezor_msg(target_addr,
                                 MessageType_MessageType_EntropyAck, buf,
                                 stream.bytes_written);
      memzero(buf, sizeof(buf));
      break;
    }
    case MessageType_MessageType_Success:
      end_conversation(false);
      show_result(_("Key block is ready."), NULL);
      break;
    case MessageType_MessageType_Failure:
      on_failure(data, len);
      break;
    case MessageType_MessageType_PinMatrixRequest:
      // The key block should take its PIN on its own buttons; nobody can
      // answer a PIN matrix here.
      end_conversation(true);
      retry_after_ms = timer_ms() + RESULT_SHOW_MS + RETRY_PAUSE_MS;
      show_result(_("Setup failed:"), _("PIN not on the block"));
      break;
    default:
      break;  // e.g. EntropyCheckReady is never asked for
  }
}

// ------------------------------------------------------------ unlock

static void on_unlock_failure(const uint8_t *data, uint32_t len) {
  static Failure fail;
  memzero(&fail, sizeof(fail));
  pb_istream_t stream = pb_istream_from_buffer(data, len);
  bool decoded = pb_decode(&stream, Failure_fields, &fail);
  FailureType code =
      decoded && fail.has_code ? fail.code : FailureType_Failure_ProcessError;
  end_conversation(false);
  switch (code) {
    case FailureType_Failure_PinInvalid:
      retry_after_ms = timer_ms() + RESULT_SHOW_MS + RETRY_PAUSE_MS;
      show_result(_("Wrong PIN."), _("Let's try again."));
      break;
    case FailureType_Failure_ActionCancelled:
    case FailureType_Failure_PinCancelled:
      say_later(target_uid);
      show_result(_("Unlock cancelled."), NULL);
      break;
    default:
      retry_after_ms = timer_ms() + RESULT_SHOW_MS + RETRY_PAUSE_MS;
      show_result(_("Unlock failed:"),
                  decoded && fail.has_message ? fail.message : NULL);
      break;
  }
}

static void on_unlock_message(uint16_t msg_id, const uint8_t *data,
                              uint32_t len) {
  if (state != S_RUNNING || job != JOB_UNLOCK) return;
  switch (msg_id) {
    case MessageType_MessageType_PublicKey:
      // The PIN was right; the key itself is not needed.
      end_conversation(false);
      show_result(_("Key block unlocked."), NULL);
      break;
    case MessageType_MessageType_Failure:
      on_unlock_failure(data, len);
      break;
    case MessageType_MessageType_ButtonRequest:
      phylax_bus_send_trezor_msg(target_addr, MessageType_MessageType_ButtonAck,
                                 NULL, 0);
      break;
    case MessageType_MessageType_PinMatrixRequest:
    case MessageType_MessageType_PassphraseRequest:
      // Nobody can answer those here: the PIN goes on the key block.
      end_conversation(true);
      retry_after_ms = timer_ms() + RESULT_SHOW_MS + RETRY_PAUSE_MS;
      show_result(_("Unlock failed:"), _("PIN not on the block"));
      break;
    default:
      break;
  }
}

// Any request that needs the PIN unlocks the block. GetPublicKey of m/0'
// (Bitcoin): it reveals nothing the main block does not already have, and
// it also warms the key block's seed cache.
static void start_unlock(void) {
  const PhylaxStackEntry *e = find_uid(target_uid);
  if (e == NULL || !locked_key_block(e)) {
    state = S_IDLE;
    release_screen();
    return;
  }
  target_addr = e->addr;

  static GetPublicKey req;
  static uint8_t buf[GetPublicKey_size];
  memzero(&req, sizeof(req));
  req.address_n_count = 1;
  req.address_n[0] = 0x80000000;
  req.has_coin_name = true;
  strlcpy(req.coin_name, "Bitcoin", sizeof(req.coin_name));
  pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
  if (!pb_encode(&stream, GetPublicKey_fields, &req)) {
    target_addr = PHYLAX_ADDR_NONE;
    show_result(_("Unlock failed."), NULL);
    return;
  }

  uint8_t on = 1;
  phylax_bus_send(target_addr, PHYLAX_MSG_LOCAL_UI, &on, 1);
  phylax_bus_set_trezor_peer(target_addr, on_unlock_message);
  phylax_bus_send_trezor_msg(target_addr, MessageType_MessageType_GetPublicKey,
                             buf, stream.bytes_written);
  state = S_RUNNING;
  phylax_press_guard_reset(&cancel_guard);
  show_running();
}

static void start_setup(void) {
  const PhylaxStackEntry *e = find_uid(target_uid);
  if (e == NULL || e->addr == PHYLAX_ADDR_NONE) {
    state = S_IDLE;
    release_screen();
    return;
  }
  target_addr = e->addr;

  static ResetDevice req;
  static uint8_t buf[ResetDevice_size];
  memzero(&req, sizeof(req));
  req.has_strength = true;
  req.strength = 256;  // 24 words
  req.has_pin_protection = true;
  req.pin_protection = true;
  req.has_passphrase_protection = true;
  req.passphrase_protection = false;
  req.has_skip_backup = true;
  req.skip_backup = false;  // the words are shown on the key block now
  pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
  if (!pb_encode(&stream, ResetDevice_fields, &req)) {
    target_addr = PHYLAX_ADDR_NONE;
    show_result(_("Setup failed."), NULL);
    return;
  }

  uint8_t on = 1;
  phylax_bus_send(target_addr, PHYLAX_MSG_LOCAL_UI, &on, 1);
  phylax_bus_set_trezor_peer(target_addr, on_key_message);
  phylax_bus_send_trezor_msg(target_addr, MessageType_MessageType_ResetDevice,
                             buf, stream.bytes_written);
  state = S_RUNNING;
  show_running();
}

// ------------------------------------------------------------ API

bool phylax_setup_running(void) { return state == S_RUNNING; }

static bool choosing(void) { return state == S_ENT; }

bool phylax_setup_busy(void) {
  return state == S_PROMPT || choosing() || state == S_RUNNING;
}

void phylax_setup_abort(void) {
  if (state == S_RUNNING) end_conversation(true);
  phylax_userent_forget();
  if (state != S_IDLE) state = S_IDLE;
  release_screen();
}

static void answer(bool yes) {
  if (job == JOB_UNLOCK) {
    if (yes) {
      start_unlock();
    } else {
      say_later(target_uid);
      state = S_IDLE;
      release_screen();
    }
    return;
  }
  if (yes) {
    state = S_ENT;
    phylax_userent_begin();
  } else {
    decline(target_uid);
    state = S_IDLE;
    release_screen();
  }
}

void phylax_setup_ui_poll(void) {
  if (choosing()) {
#if EMULATOR
    // Tests: with no button script the main block skips this step.
    if (phylax_emu_autoconfirm() && !phylax_buttons_scripted()) {
      start_setup();
      return;
    }
#endif
    PhylaxButtonEvent e = phylax_buttons_next();
    if (e == PHYLAX_BUTTON_NONE) return;
    // Skipped or complete: the result (if any) goes into the EntropyAck.
    if (phylax_userent_event(e) != PHYLAX_USERENT_CONTINUE) start_setup();
    return;
  }
  if (state == S_RUNNING && job == JOB_UNLOCK) {
    if (layoutLast == show_state &&
        phylax_press_guard_poll(&cancel_guard) < 0) {
      end_conversation(true);  // the key block's PIN entry stops too
      say_later(target_uid);
      show_result(_("Unlock cancelled."), NULL);
    }
    return;
  }
  if (state != S_PROMPT) return;
#if EMULATOR
  if (phylax_emu_autoconfirm()) {  // tests: nobody presses the buttons
    answer(true);
    return;
  }
#endif
  // Covered (e.g. by the home screen after a PC message): no answer until
  // the question is drawn again.
  if (layoutLast != show_state) return;
  int pressed = phylax_press_guard_poll(&prompt_guard);
  if (pressed > 0)
    answer(true);
  else if (pressed < 0)
    answer(false);
}

const char *phylax_setup_request_unlock(uint8_t addr) {
  // A prompt on the main block ("Unlock it?", "Set it up?") gives way: it
  // comes back later if still needed. A setup or unlock in progress does not.
  if (state != S_IDLE && state != S_RESULT && state != S_PROMPT) {
    return _("The main block is busy with a key block");
  }
  const PhylaxStackEntry *e;
  for (size_t i = 0; (e = phylax_stack_get(i)) != NULL; i++) {
    if (e->addr == addr) break;
  }
  if (e == NULL) return _("No key block at that address");
  if (!locked_key_block(e)) return _("That key block is not locked");
  job = JOB_UNLOCK;
  name_unlock_target(e);
  memcpy(target_uid, e->hello.uid, PHYLAX_UID_SIZE);
  // Asked for on the PC: no "Unlock it?" on the main block, straight to the
  // PIN on the key block (Cancel on the main block still stops it).
  start_unlock();
  return state == S_RUNNING ? NULL : _("Could not start the unlock");
}

bool phylax_setup_draw_home(void) {
  if (state != S_PROMPT && state != S_ENT && state != S_RUNNING) return false;
  show_state();
  return true;
}

bool phylax_setup_poll(void) {
  prune_declined();
  prune_later();
  switch (state) {
    case S_IDLE: {
      if ((int32_t)(timer_ms() - retry_after_ms) < 0) return false;
      const PhylaxStackEntry *e = find_blank_key_block();
      job = JOB_SETUP;
      if (e == NULL) {
        e = find_locked_wallet_block();
        if (e == NULL) return false;
        job = JOB_UNLOCK;
        name_unlock_target(e);
      }
      memcpy(target_uid, e->hello.uid, PHYLAX_UID_SIZE);
      state = S_PROMPT;
      phylax_press_guard_reset(&prompt_guard);
      show_state();
      return true;
    }
    case S_PROMPT:
    case S_ENT: {
      const PhylaxStackEntry *e = find_uid(target_uid);
      bool gone =
          e == NULL ||
          (job == JOB_SETUP ? (e->hello.flags & PHYLAX_HELLO_INITIALIZED) != 0
                            : !locked_key_block(e));
      if (gone) {
        phylax_userent_forget();
        state = S_IDLE;  // removed, or set up some other way meanwhile
        release_screen();
        return false;
      }
      if (layoutLast == layoutHomescreen) show_state();
      return true;
    }
    case S_RUNNING:
      if (layoutLast == layoutHomescreen) show_state();
      if (find_uid(target_uid) == NULL) {
        // Removed half-way: it starts again from scratch when re-attached.
        phylax_bus_set_trezor_peer(PHYLAX_ADDR_NONE, NULL);
        target_addr = PHYLAX_ADDR_NONE;
        show_result(_("Key block removed."), job == JOB_UNLOCK
                                                 ? _("Unlock stopped.")
                                                 : _("Setup stopped."));
      }
      return true;
    case S_RESULT:
      if ((int32_t)(timer_ms() - result_until_ms) < 0) return true;
      state = S_IDLE;
      release_screen();
      return false;
  }
  return false;
}
