/*
 * PHYLAX Blocks - your own randomness (coin flips / dice). See
 * phylax_userent.h.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "phylax_userent.h"

#include <string.h>

#include "buttons.h"
#include "fsm.h"
#include "gettext.h"
#include "layout2.h"
#include "memzero.h"
#include "messages.h"
#include "messages.pb.h"
#include "oled.h"
#include "phylax_bus.h"
#include "phylax_setup.h"
#include "protect.h"
#include "sha2.h"
#include "usb.h"

#define COIN_FLIPS 256
#define DICE_ROLLS 99
#define DICE_DEL 6  // carousel: 1..6, DEL
#define DICE_ITEMS 7

typedef enum { U_OFF, U_ASK, U_KIND, U_COIN, U_DICE } UserentState;

static UserentState ustate = U_OFF;
// The flips or rolls being entered (H/T or '1'..'6'), and once complete
// only their SHA-256.
static CONFIDENTIAL char inputs[COIN_FLIPS + 1];
static size_t count = 0;
static int dice_pos = 0;
static CONFIDENTIAL uint8_t digest[SHA256_DIGEST_LENGTH];
static bool have_digest = false;

static void clear_inputs(void) {
  memzero(inputs, sizeof(inputs));
  count = 0;
}

void phylax_userent_forget(void) {
  clear_inputs();
  memzero(digest, sizeof(digest));
  have_digest = false;
  ustate = U_OFF;
}

bool phylax_userent_have(void) { return have_digest; }

void phylax_userent_mix(uint8_t entropy[32]) {
  if (!have_digest) return;
  SHA256_CTX ctx;
  sha256_Init(&ctx);
  sha256_Update(&ctx, entropy, 32);
  sha256_Update(&ctx, digest, sizeof(digest));
  sha256_Final(&ctx, entropy);
  memzero(&ctx, sizeof(ctx));
}

// ------------------------------------------------------------ screens

static void show_ask(void) {
  layoutDialog(&bmp_icon_question, _("Skip"), _("Add"), NULL, _("Add your own"),
               _("randomness?"), NULL, _("Coin flips or dice,"),
               _("added to the seed."), NULL);
}

static void show_kind(void) {
  layoutDialog(&bmp_icon_question, _("Coin"), _("Dice"), NULL,
               _("Coin flips or dice?"), NULL, _("256 flips of a real coin"),
               _("or 99 rolls of a"), _("real 6-sided die."), NULL);
}

// The last inputs, newest on the right, as many as fit.
static void draw_history(int y) {
  const size_t fit = 20;
  size_t from = count > fit ? count - fit : 0;
  char line[21];
  size_t n = count - from;
  memcpy(line, inputs + from, n);
  line[n] = '\0';
  oledDrawStringCenter(OLED_WIDTH / 2, y, n ? line : "-", FONT_STANDARD);
  memzero(line, sizeof(line));
}

static void append_number(char *buf, size_t size, size_t *i, size_t v) {
  char num[12];
  int k = 0;
  do {
    num[k++] = (char)('0' + v % 10);
    v /= 10;
  } while (v && k < 11);
  while (k && *i < size - 1) buf[(*i)++] = num[--k];
}

// "Flip 37 of 256" / "Roll 12 of 99"
static void draw_count(const char *what, size_t total) {
  char title[24];
  size_t i = 0;
  for (const char *c = what; *c && i < sizeof(title) - 1; c++) title[i++] = *c;
  append_number(title, sizeof(title), &i, count + 1);
  for (const char *c = " of "; *c && i < sizeof(title) - 1; c++) {
    title[i++] = *c;
  }
  append_number(title, sizeof(title), &i, total);
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
                       count ? _("both = undo") : _("both = cancel"),
                       FONT_STANDARD);
  oledRefresh();
}

static const char *dice_label(int i) {
  static const char *const labels[DICE_ITEMS] = {"1", "2", "3",  "4",
                                                 "5", "6", "DEL"};
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
  const char *cur =
      dice_pos == DICE_DEL && count == 0 ? _("BACK") : dice_label(dice_pos);
  oledDrawStringCenter(OLED_WIDTH / 2, y, cur, FONT_STANDARD | FONT_DOUBLE);
  oledInvert(OLED_WIDTH / 2 - 26, y - 2, OLED_WIDTH / 2 + 25, y + 16);
  oledDrawStringCenter(OLED_WIDTH / 2, OLED_HEIGHT - 8, _("both = select"),
                       FONT_STANDARD);
  oledRefresh();
}

void phylax_userent_redraw(void) {
  switch (ustate) {
    case U_ASK:
      show_ask();
      break;
    case U_KIND:
      show_kind();
      break;
    case U_COIN:
      show_coin();
      break;
    case U_DICE:
      show_dice();
      break;
    case U_OFF:
      return;
  }
  // Plain dialogs never set layoutLast: mark the screen as ours, so a home
  // screen drawn over it (after a PC message) can be noticed and redrawn.
  layoutLast = phylax_userent_redraw;
}

// ------------------------------------------------------------ events

void phylax_userent_begin(void) {
  phylax_userent_forget();
  ustate = U_ASK;
  phylax_userent_redraw();
}

static void ask_again(void) {
  clear_inputs();
  ustate = U_ASK;
}

// The flips or rolls are complete: keep only their hash.
static PhylaxUserentResult finish(void) {
  static const char tag_coin[] = "PHYLAX coin flips v1";
  static const char tag_dice[] = "PHYLAX dice rolls v1";
  SHA256_CTX ctx;
  sha256_Init(&ctx);
  if (ustate == U_COIN) {
    sha256_Update(&ctx, (const uint8_t *)tag_coin, sizeof(tag_coin) - 1);
  } else {
    sha256_Update(&ctx, (const uint8_t *)tag_dice, sizeof(tag_dice) - 1);
  }
  sha256_Update(&ctx, (const uint8_t *)inputs, count);
  sha256_Final(&ctx, digest);
  memzero(&ctx, sizeof(ctx));
  clear_inputs();
  have_digest = true;
  ustate = U_OFF;
  return PHYLAX_USERENT_DONE;
}

static PhylaxUserentResult coin_event(PhylaxButtonEvent e) {
  if (e == PHYLAX_BUTTON_BOTH) {
    if (count == 0) {
      ask_again();
      return PHYLAX_USERENT_CONTINUE;
    }
    inputs[--count] = '\0';
  } else {
    inputs[count++] = e == PHYLAX_BUTTON_LEFT ? 'H' : 'T';
    if (count == COIN_FLIPS) return finish();
  }
  return PHYLAX_USERENT_CONTINUE;
}

static PhylaxUserentResult dice_event(PhylaxButtonEvent e) {
  if (e == PHYLAX_BUTTON_LEFT) {
    dice_pos = (dice_pos + DICE_ITEMS - 1) % DICE_ITEMS;
  } else if (e == PHYLAX_BUTTON_RIGHT) {
    dice_pos = (dice_pos + 1) % DICE_ITEMS;
  } else if (dice_pos == DICE_DEL) {
    if (count == 0) {
      ask_again();
      return PHYLAX_USERENT_CONTINUE;
    }
    inputs[--count] = '\0';
  } else {
    inputs[count++] = (char)('1' + dice_pos);
    if (count == DICE_ROLLS) return finish();
  }
  return PHYLAX_USERENT_CONTINUE;
}

PhylaxUserentResult phylax_userent_event(PhylaxButtonEvent e) {
  PhylaxUserentResult r = PHYLAX_USERENT_CONTINUE;
  switch (ustate) {
    case U_ASK:
      if (e == PHYLAX_BUTTON_LEFT) {
        ustate = U_OFF;
        return PHYLAX_USERENT_SKIPPED;
      }
      if (e == PHYLAX_BUTTON_RIGHT) ustate = U_KIND;
      break;
    case U_KIND:
      if (e == PHYLAX_BUTTON_LEFT) {
        ustate = U_COIN;
      } else if (e == PHYLAX_BUTTON_RIGHT) {
        ustate = U_DICE;
        dice_pos = 0;
      } else {
        ask_again();
      }
      break;
    case U_COIN:
      r = coin_event(e);
      break;
    case U_DICE:
      r = dice_event(e);
      break;
    case U_OFF:
      return PHYLAX_USERENT_SKIPPED;
  }
  if (r == PHYLAX_USERENT_CONTINUE) phylax_userent_redraw();
  return r;
}

// ------------------------------------------------------------ own wallet

static bool wants_user_entropy(void) {
  if (phylax_role() == PHYLAX_ROLE_STOCK) return false;
  // A key block set up by the main block: the main block asked already.
  if (phylax_local_ui()) return false;
#if EMULATOR
  // Tests: with no button script nobody can answer, so skip the question.
  if (phylax_emu_autoconfirm() && !phylax_buttons_scripted()) return false;
#endif
  return true;
}

bool phylax_userent_collect_for_reset(void) {
  // The PC creating this block's own wallet comes first: stop a key-block
  // setup that is half-way (it shares these screens and starts again later).
  phylax_setup_abort();
  phylax_userent_forget();
  if (!wants_user_entropy()) return true;

  // The host (Suite) waits while the user answers on the block, like any
  // confirmation on the device.
  ButtonRequest req = {0};
  req.has_code = true;
  req.code = ButtonRequestType_ButtonRequest_Other;
  usbTiny(1);
  buttonUpdate();  // clear the button state
  msg_write(MessageType_MessageType_ButtonRequest, &req);

  phylax_userent_begin();
  bool ok = true;
  for (;;) {
    usbPoll();

    if (msg_tiny_id == MessageType_MessageType_ButtonAck) {
      msg_tiny_id = 0xFFFF;
    }
    protectAbortedByCancel = (msg_tiny_id == MessageType_MessageType_Cancel);
    protectAbortedByInitialize =
        (msg_tiny_id == MessageType_MessageType_Initialize);
    if (protectAbortedByCancel || protectAbortedByInitialize) {
      msg_tiny_id = 0xFFFF;
      ok = false;
      break;
    }
#if DEBUG_LINK
    if (msg_tiny_id == MessageType_MessageType_DebugLinkDecision) {
      msg_tiny_id = 0xFFFF;  // the buttons (or the test script) decide here
    }
    if (msg_tiny_id == MessageType_MessageType_DebugLinkGetState) {
      msg_tiny_id = 0xFFFF;
      fsm_msgDebugLinkGetState((DebugLinkGetState *)msg_tiny);
    }
#endif

    PhylaxButtonEvent e = phylax_buttons_next();
    if (e == PHYLAX_BUTTON_NONE) continue;
    PhylaxUserentResult r = phylax_userent_event(e);
    if (r != PHYLAX_USERENT_CONTINUE) break;
  }
  usbTiny(0);
  if (!ok) {
    phylax_userent_forget();
    return false;
  }
  layoutDialog(&bmp_icon_info, NULL, NULL, NULL, _("Creating your"),
               _("wallet..."), NULL, NULL, NULL, NULL);
  layoutLast = layoutDialog;  // no longer the randomness screens
  return true;
}
