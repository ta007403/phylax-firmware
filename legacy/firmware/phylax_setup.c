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
#include "rand.h"
#include "rng.h"
#include "sha2.h"
#include "timer.h"

#define RESULT_SHOW_MS 3000
#define RETRY_PAUSE_MS 3000  // after a mistyped PIN: ask again

typedef enum {
  S_IDLE,
  S_PROMPT,     // "New key block found. Set it up?"
  S_ENT_ASK,    // "Add your own randomness?"  Skip / Add
  S_ENT_KIND,   // "Coin flips or dice?"       Coin / Dice
  S_ENT_COIN,   // 256 coin flips
  S_ENT_DICE,   // 99 dice rolls
  S_RUNNING,    // the user follows the key block's screen
  S_RESULT,     // "Key block is ready." / why it stopped
} SetupState;

// Your own randomness: 256 coin flips (256 bits) or 99 dice rolls
// (99 x log2(6) = 255.9 bits), so it is full strength on its own. It is
// hashed and mixed into the main block's random bytes, never replacing them.
#define COIN_FLIPS 256
#define DICE_ROLLS 99
#define DICE_UNDO 6  // carousel: 1..6, UNDO
#define DICE_ITEMS 7

static SetupState state = S_IDLE;
static uint8_t target_uid[PHYLAX_UID_SIZE];
static uint8_t target_addr = PHYLAX_ADDR_NONE;
static uint32_t result_until_ms = 0;
static uint32_t retry_after_ms = 0;

// The flips or rolls being entered (H/T or '1'..'6'), and once complete their
// SHA-256, mixed into the EntropyAck.
static CONFIDENTIAL char user_inputs[COIN_FLIPS + 1];
static size_t user_count = 0;
static int dice_pos = 0;
static CONFIDENTIAL uint8_t user_digest[SHA256_DIGEST_LENGTH];
static bool have_user_entropy = false;

static void forget_user_entropy(void) {
  memzero(user_inputs, sizeof(user_inputs));
  memzero(user_digest, sizeof(user_digest));
  user_count = 0;
  have_user_entropy = false;
}

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

static void show_ent_ask(void) {
  layoutDialog(&bmp_icon_question, _("Skip"), _("Add"), NULL,
               _("Add your own"), _("randomness?"), NULL,
               _("Coin flips or dice,"), _("mixed with the block's"),
               _("random numbers."));
}

static void show_ent_kind(void) {
  layoutDialog(&bmp_icon_question, _("Coin"), _("Dice"), NULL,
               _("Coin flips or dice?"), NULL, _("256 flips of a real coin"),
               _("or 99 rolls of a"), _("real 6-sided die."), NULL);
}

// The last inputs, newest on the right, as many as fit.
static void draw_history(int y) {
  const size_t fit = 20;
  size_t from = user_count > fit ? user_count - fit : 0;
  char line[21];
  size_t n = user_count - from;
  memcpy(line, user_inputs + from, n);
  line[n] = '\0';
  oledDrawStringCenter(OLED_WIDTH / 2, y, n ? line : "-", FONT_STANDARD);
  memzero(line, sizeof(line));
}

static void draw_count(const char *what, size_t total) {
  char title[24];
  // "Flip 37 of 256" / "Roll 12 of 99"
  size_t i = 0;
  for (const char *c = what; *c && i < sizeof(title) - 1; c++) title[i++] = *c;
  char num[12];
  int k = 0;
  size_t v = user_count + 1;
  do {
    num[k++] = (char)('0' + v % 10);
    v /= 10;
  } while (v && k < 11);
  while (k && i < sizeof(title) - 1) title[i++] = num[--k];
  const char *of = " of ";
  for (const char *c = of; *c && i < sizeof(title) - 1; c++) title[i++] = *c;
  v = total;
  k = 0;
  do {
    num[k++] = (char)('0' + v % 10);
    v /= 10;
  } while (v && k < 11);
  while (k && i < sizeof(title) - 1) title[i++] = num[--k];
  title[i] = '\0';
  oledDrawStringCenter(OLED_WIDTH / 2, 0, title, FONT_STANDARD);
}

static void show_coin(void) {
  oledClear();
  draw_count(_("Flip "), COIN_FLIPS);
  draw_history(12);
  oledDrawStringCenter(OLED_WIDTH / 2, 26, _("Flip a real coin."),
                       FONT_STANDARD);
  oledDrawString(0, 40, _("< heads"), FONT_STANDARD);
  oledDrawStringRight(OLED_WIDTH - 1, 40, _("tails >"), FONT_STANDARD);
  oledDrawStringCenter(OLED_WIDTH / 2, OLED_HEIGHT - 8,
                       user_count ? _("both = undo") : _("both = cancel"),
                       FONT_STANDARD);
  oledRefresh();
}

static const char *dice_label(int i) {
  static const char *const labels[DICE_ITEMS] = {"1", "2", "3", "4",
                                                 "5", "6", "UNDO"};
  return labels[(i + DICE_ITEMS) % DICE_ITEMS];
}

static void show_dice(void) {
  oledClear();
  draw_count(_("Roll "), DICE_ROLLS);
  draw_history(12);
  const int y = 28;
  oledDrawString(0, y + 4, "<", FONT_STANDARD);
  oledDrawStringRight(OLED_WIDTH - 1, y + 4, ">", FONT_STANDARD);
  oledDrawString(12, y + 4, dice_label(dice_pos - 1), FONT_STANDARD);
  oledDrawStringRight(OLED_WIDTH - 13, y + 4, dice_label(dice_pos + 1),
                      FONT_STANDARD);
  const char *cur = dice_pos == DICE_UNDO && user_count == 0
                        ? _("BACK")
                        : dice_label(dice_pos);
  oledDrawStringCenter(OLED_WIDTH / 2, y, cur, FONT_STANDARD | FONT_DOUBLE);
  oledInvert(OLED_WIDTH / 2 - 26, y - 2, OLED_WIDTH / 2 + 25, y + 16);
  oledDrawStringCenter(OLED_WIDTH / 2, OLED_HEIGHT - 8, _("both = select"),
                       FONT_STANDARD);
  oledRefresh();
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
  forget_user_entropy();
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
      // Button timings while the user was choosing, then the user's own
      // coin flips or dice rolls: SHA256(random || flips). Both can only add.
      phylax_entropy_mix(ack.entropy.bytes);
      if (have_user_entropy) {
        SHA256_CTX ctx;
        sha256_Init(&ctx);
        sha256_Update(&ctx, ack.entropy.bytes, 32);
        sha256_Update(&ctx, user_digest, sizeof(user_digest));
        sha256_Final(&ctx, ack.entropy.bytes);
        memzero(&ctx, sizeof(ctx));
        forget_user_entropy();
      }
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

static bool choosing(void) {
  return state == S_ENT_ASK || state == S_ENT_KIND || state == S_ENT_COIN ||
         state == S_ENT_DICE;
}

bool phylax_setup_busy(void) {
  return state == S_PROMPT || choosing() || state == S_RUNNING;
}

void phylax_setup_abort(void) {
  if (state == S_RUNNING) end_conversation(true);
  forget_user_entropy();
  if (state != S_IDLE) state = S_IDLE;
}

static void ask_entropy(void) {
  forget_user_entropy();
  state = S_ENT_ASK;
  show_ent_ask();
}

static void answer(bool yes) {
  if (yes) {
    ask_entropy();
  } else {
    decline(target_uid);
    state = S_IDLE;
  }
}

// The flips or rolls are complete: keep only their hash.
static void finish_entropy(void) {
  static const char tag_coin[] = "PHYLAX coin flips v1";
  static const char tag_dice[] = "PHYLAX dice rolls v1";
  SHA256_CTX ctx;
  sha256_Init(&ctx);
  if (state == S_ENT_COIN) {
    sha256_Update(&ctx, (const uint8_t *)tag_coin, sizeof(tag_coin) - 1);
  } else {
    sha256_Update(&ctx, (const uint8_t *)tag_dice, sizeof(tag_dice) - 1);
  }
  sha256_Update(&ctx, (const uint8_t *)user_inputs, user_count);
  sha256_Final(&ctx, user_digest);
  memzero(&ctx, sizeof(ctx));
  memzero(user_inputs, sizeof(user_inputs));
  user_count = 0;
  have_user_entropy = true;
  start_setup();
}

static void coin_event(PhylaxButtonEvent e) {
  if (e == PHYLAX_BUTTON_BOTH) {
    if (user_count == 0) {
      ask_entropy();
      return;
    }
    user_inputs[--user_count] = '\0';
  } else {
    user_inputs[user_count++] = e == PHYLAX_BUTTON_LEFT ? 'H' : 'T';
    if (user_count == COIN_FLIPS) {
      finish_entropy();
      return;
    }
  }
  show_coin();
}

static void dice_event(PhylaxButtonEvent e) {
  if (e == PHYLAX_BUTTON_LEFT) {
    dice_pos = (dice_pos + DICE_ITEMS - 1) % DICE_ITEMS;
  } else if (e == PHYLAX_BUTTON_RIGHT) {
    dice_pos = (dice_pos + 1) % DICE_ITEMS;
  } else if (dice_pos == DICE_UNDO) {
    if (user_count == 0) {
      ask_entropy();
      return;
    }
    user_inputs[--user_count] = '\0';
  } else {
    user_inputs[user_count++] = (char)('1' + dice_pos);
    if (user_count == DICE_ROLLS) {
      finish_entropy();
      return;
    }
  }
  show_dice();
}

static void choosing_event(PhylaxButtonEvent e) {
  switch (state) {
    case S_ENT_ASK:
      if (e == PHYLAX_BUTTON_LEFT) {
        start_setup();  // skip: the block's own random numbers only
      } else if (e == PHYLAX_BUTTON_RIGHT) {
        state = S_ENT_KIND;
        show_ent_kind();
      }
      break;
    case S_ENT_KIND:
      if (e == PHYLAX_BUTTON_LEFT) {
        state = S_ENT_COIN;
        show_coin();
      } else if (e == PHYLAX_BUTTON_RIGHT) {
        state = S_ENT_DICE;
        dice_pos = 0;
        show_dice();
      } else {
        ask_entropy();
      }
      break;
    case S_ENT_COIN:
      coin_event(e);
      break;
    case S_ENT_DICE:
      dice_event(e);
      break;
    default:
      break;
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
    if (e != PHYLAX_BUTTON_NONE) choosing_event(e);
    return;
  }
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
    case S_PROMPT:
    case S_ENT_ASK:
    case S_ENT_KIND:
    case S_ENT_COIN:
    case S_ENT_DICE: {
      const PhylaxStackEntry *e = find_uid(target_uid);
      if (e == NULL || (e->hello.flags & PHYLAX_HELLO_INITIALIZED)) {
        forget_user_entropy();
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
