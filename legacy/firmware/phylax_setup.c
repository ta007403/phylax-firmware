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
#include "pb_decode.h"
#include "pb_encode.h"
#include "phylax_bus.h"
#include "phylax_stack.h"
#include "rand.h"
#include "rng.h"
#include "timer.h"

#define RESULT_SHOW_MS 3000
#define RETRY_PAUSE_MS 3000  // after a mistyped PIN: ask again

typedef enum {
  S_IDLE,
  S_PROMPT,   // "New key block found. Set it up?"
  S_RUNNING,  // the user follows the key block's screen
  S_RESULT,   // "Key block is ready." / why it stopped
} SetupState;

static SetupState state = S_IDLE;
static uint8_t target_uid[PHYLAX_UID_SIZE];
static uint8_t target_addr = PHYLAX_ADDR_NONE;
static uint32_t result_until_ms = 0;
static uint32_t retry_after_ms = 0;

// Blocks the user said "no" to, until they leave the stack.
static uint8_t declined[PHYLAX_STACK_MAX][PHYLAX_UID_SIZE];
static uint8_t declined_n = 0;

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

static void show_prompt(void) {
  layoutDialog(&bmp_icon_question, _("No"), _("Set up"), NULL,
               _("New key block found."), _("Set it up?"), NULL,
               _("You will choose a PIN"), _("and write down 24"),
               _("words on the block."));
}

static void show_running(void) {
  layoutDialog(&bmp_icon_info, NULL, NULL, NULL, _("Setting up the"),
               _("key block."), NULL, _("Follow the steps on"),
               _("its own screen."), NULL);
}

static void show_result(const char *line1, const char *line2) {
  layoutDialog(&bmp_icon_info, NULL, NULL, NULL, _("PHYLAX"), line1, line2,
               NULL, NULL, NULL);
  state = S_RESULT;
  result_until_ms = timer_ms() + RESULT_SHOW_MS;
}

// ------------------------------------------------------------ the conversation

static void end_conversation(bool cancel_key) {
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
  FailureType code = decoded && fail.has_code
                         ? fail.code
                         : FailureType_Failure_ProcessError;
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

static void on_key_message(uint16_t msg_id, const uint8_t *data,
                           uint32_t len) {
  if (state != S_RUNNING) return;
  switch (msg_id) {
    case MessageType_MessageType_ButtonRequest:
      // The user confirms on the key block itself.
      phylax_bus_send_trezor_msg(target_addr,
                                 MessageType_MessageType_ButtonAck, NULL, 0);
      break;
    case MessageType_MessageType_EntropyRequest: {
      // The main block's TRNG plays the part of the PC's entropy.
      static EntropyAck ack;
      static uint8_t buf[EntropyAck_size];
      memzero(&ack, sizeof(ack));
      random_buffer(ack.entropy.bytes, 32);
      ack.entropy.size = 32;
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

static void start_setup(void) {
  const PhylaxStackEntry *e = find_uid(target_uid);
  if (e == NULL || e->addr == PHYLAX_ADDR_NONE) {
    state = S_IDLE;
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

bool phylax_setup_busy(void) {
  return state == S_PROMPT || state == S_RUNNING;
}

void phylax_setup_abort(void) {
  if (state == S_RUNNING) end_conversation(true);
  if (state != S_IDLE) state = S_IDLE;
}

static void answer(bool yes) {
  if (yes) {
    start_setup();
  } else {
    decline(target_uid);
    state = S_IDLE;
  }
}

void phylax_setup_ui_poll(void) {
  if (state != S_PROMPT) return;
#if EMULATOR
  if (phylax_emu_autoconfirm()) {  // tests: nobody presses the buttons
    answer(true);
    return;
  }
#endif
  if (button.YesUp) answer(true);
  else if (button.NoUp) answer(false);
}

bool phylax_setup_poll(void) {
  prune_declined();
  switch (state) {
    case S_IDLE: {
      if ((int32_t)(timer_ms() - retry_after_ms) < 0) return false;
      const PhylaxStackEntry *e = find_blank_key_block();
      if (e == NULL) return false;
      memcpy(target_uid, e->hello.uid, PHYLAX_UID_SIZE);
      state = S_PROMPT;
      show_prompt();
      return true;
    }
    case S_PROMPT: {
      const PhylaxStackEntry *e = find_uid(target_uid);
      if (e == NULL || (e->hello.flags & PHYLAX_HELLO_INITIALIZED)) {
        state = S_IDLE;  // removed, or set up some other way meanwhile
        return false;
      }
      return true;
    }
    case S_RUNNING:
      if (find_uid(target_uid) == NULL) {
        // Removed half-way: it starts again from scratch when re-attached.
        phylax_bus_set_trezor_peer(PHYLAX_ADDR_NONE, NULL);
        target_addr = PHYLAX_ADDR_NONE;
        show_result(_("Key block removed."), _("Setup stopped."));
      }
      return true;
    case S_RESULT:
      if ((int32_t)(timer_ms() - result_until_ms) < 0) return true;
      state = S_IDLE;
      return false;
  }
  return false;
}
