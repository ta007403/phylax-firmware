/*
 * PHYLAX Blocks - key block: PIN entry on its own two buttons, and the
 * button-timing entropy pool. See phylax_pin.h.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "phylax_pin.h"

#include <string.h>
#if EMULATOR
#include <stdlib.h>
#endif

#include "buttons.h"
#include "config.h"
#include "fsm.h"
#include "gettext.h"
#include "layout.h"
#include "memzero.h"
#include "messages.h"
#include "oled.h"
#include "protect.h"
#include "phylax_bus.h"
#include "rand.h"
#include "rng.h"
#include "sha2.h"
#include "timer.h"
#include "usb.h"

// ------------------------------------------------------------ local UI flag

static bool local_ui = false;

void phylax_set_local_ui(bool on) { local_ui = on; }

bool phylax_local_ui(void) {
  // Only for conversations with the main block: a PC talking to this block
  // over its own USB still gets the normal PIN matrix.
  return local_ui && phylax_role() == PHYLAX_ROLE_KEY && phylax_reply_via_bus();
}

// ------------------------------------------------------------ entropy pool

static SHA256_CTX pool;
static uint32_t pool_events = 0;

void phylax_entropy_note(uint32_t a, uint32_t b) {
  if (pool_events == 0) sha256_Init(&pool);
  uint32_t v[3] = {a, b, timer_ms()};
  sha256_Update(&pool, (const uint8_t *)v, sizeof(v));
  pool_events++;
  memzero(v, sizeof(v));
}

uint32_t phylax_entropy_events(void) { return pool_events; }

void phylax_entropy_mix(uint8_t entropy[32]) {
  if (pool_events == 0) return;
  uint8_t digest[SHA256_DIGEST_LENGTH];
  sha256_Final(&pool, digest);
  SHA256_CTX ctx;
  sha256_Init(&ctx);
  sha256_Update(&ctx, entropy, 32);
  sha256_Update(&ctx, digest, sizeof(digest));
  sha256_Final(&ctx, entropy);
  memzero(digest, sizeof(digest));
  memzero(&ctx, sizeof(ctx));
  memzero(&pool, sizeof(pool));
  pool_events = 0;
}

// ------------------------------------------------------------ buttons

typedef PhylaxButtonEvent ButtonEvent;
#define EV_NONE PHYLAX_BUTTON_NONE
#define EV_LEFT PHYLAX_BUTTON_LEFT
#define EV_RIGHT PHYLAX_BUTTON_RIGHT
#define EV_BOTH PHYLAX_BUTTON_BOTH

#define BOTH_MASK (BTN_PIN_YES | BTN_PIN_NO)
#define SAMPLE_MS 10  // sampling every 10 ms also debounces the contacts

static uint32_t spin = 0;  // loop turns: timing jitter below 1 ms

// A press is reported when every button is released again: left alone,
// right alone, or both (they were down together at some point).
static ButtonEvent read_buttons(void) {
  static uint32_t last_sample_ms = 0;
  static uint16_t last_down = 0;
  static uint16_t seen = 0;
  static uint32_t last_change_spin = 0;

  uint32_t now = timer_ms();
  if (now - last_sample_ms < SAMPLE_MS) return EV_NONE;
  last_sample_ms = now;

  uint16_t down = (uint16_t)(~buttonRead()) & BOTH_MASK;  // 1 = pressed
  if (down != last_down) {
    phylax_entropy_note(spin - last_change_spin, down);
    last_change_spin = spin;
    last_down = down;
  }
  if (down) {
    seen |= down;
    return EV_NONE;
  }
  if (!seen) return EV_NONE;
  ButtonEvent e = seen == BOTH_MASK            ? EV_BOTH
                  : (seen & BTN_PIN_YES) != 0 ? EV_RIGHT
                                              : EV_LEFT;
  seen = 0;
  return e;
}

#if EMULATOR
// Tests: PHYLAX_EMU_PIN_KEYS="RRB..." (L, R, B = both) replaces the buttons,
// read one key at a time across every PIN entry of the run.
static const char *emu_keys(void) {
  static const char *keys = NULL;
  static bool read = false;
  if (!read) {
    keys = getenv("PHYLAX_EMU_PIN_KEYS");
    read = true;
  }
  return phylax_emu_autoconfirm() ? keys : NULL;
}

static ButtonEvent emu_next(void) {
  static size_t pos = 0;
  static uint32_t last_ms = 0;
  const char *keys = emu_keys();
  if (keys == NULL || keys[pos] == '\0') return EV_NONE;
  if (timer_ms() - last_ms < 20) return EV_NONE;
  last_ms = timer_ms();
  phylax_entropy_note(spin, (uint32_t)pos);
  switch (keys[pos++]) {
    case 'L':
      return EV_LEFT;
    case 'R':
      return EV_RIGHT;
    case 'B':
      return EV_BOTH;
    default:
      return EV_NONE;
  }
}
#endif

static ButtonEvent next_event(void) {
#if EMULATOR
  if (emu_keys() != NULL) return emu_next();
#endif
  return read_buttons();
}

PhylaxButtonEvent phylax_buttons_next(void) {
  spin++;
  return next_event();
}

bool phylax_buttons_scripted(void) {
#if EMULATOR
  return emu_keys() != NULL;
#else
  return false;
#endif
}

// ------------------------------------------------------------ PIN entry

// The carousel: 1..9, OK, DEL
#define ITEM_OK 9
#define ITEM_DEL 10
#define ITEMS 11

static const char *item_label(int i) {
  static const char *const labels[ITEMS] = {"1", "2", "3", "4",  "5",  "6",
                                            "7", "8", "9", "OK", "DEL"};
  return labels[(i + ITEMS) % ITEMS];
}

static const char *title_for(PinMatrixRequestType type) {
  switch (type) {
    case PinMatrixRequestType_PinMatrixRequestType_NewFirst:
      return _("Choose a PIN");
    case PinMatrixRequestType_PinMatrixRequestType_NewSecond:
      return _("Enter it again");
    default:
      return _("Enter PIN");
  }
}

static void draw(PinMatrixRequestType type, size_t len, int pos) {
  oledClear();
  oledDrawStringCenter(OLED_WIDTH / 2, 0, title_for(type), FONT_STANDARD);

  // entered digits as stars (the count only, never the digits)
  char stars[2 * 10 + 1];
  size_t shown = len > 10 ? 10 : len;
  for (size_t i = 0; i < shown; i++) {
    stars[2 * i] = '*';
    stars[2 * i + 1] = ' ';
  }
  stars[shown ? 2 * shown - 1 : 0] = '\0';
  oledDrawStringCenter(OLED_WIDTH / 2, 13, len ? stars : "-", FONT_STANDARD);

  // < prev [ current ] next >
  const int y = 30;
  oledDrawString(0, y + 4, "<", FONT_STANDARD);
  oledDrawStringRight(OLED_WIDTH - 1, y + 4, ">", FONT_STANDARD);
  oledDrawString(12, y + 4, item_label(pos - 1), FONT_STANDARD);
  oledDrawStringRight(OLED_WIDTH - 13, y + 4, item_label(pos + 1),
                      FONT_STANDARD);
  oledDrawStringCenter(OLED_WIDTH / 2, y, item_label(pos),
                       FONT_STANDARD | FONT_DOUBLE);
  oledInvert(OLED_WIDTH / 2 - 20, y - 2, OLED_WIDTH / 2 + 19, y + 16);

  oledDrawStringCenter(OLED_WIDTH / 2, OLED_HEIGHT - 8, _("both = select"),
                       FONT_STANDARD);
  oledRefresh();
}

const char *phylax_pin_entry(PinMatrixRequestType type) {
  static CONFIDENTIAL char pin[MAX_PIN_LEN + 1];
  memzero(pin, sizeof(pin));
  size_t len = 0;

  // Start on a random digit, like the Trezor Safe 3 (tests: always on "1").
  int pos = (int)random_uniform(9);
#if EMULATOR
  if (emu_keys() != NULL) pos = 0;
#endif

  usbTiny(1);
  buttonUpdate();
  draw(type, len, pos);
  for (;;) {
    usbPoll();  // keeps the bus alive: PING, DISCOVER, Cancel
    spin++;

    // check for Cancel / Initialize (from the main block)
    protectAbortedByCancel = (msg_tiny_id == MessageType_MessageType_Cancel);
    protectAbortedByInitialize =
        (msg_tiny_id == MessageType_MessageType_Initialize);
    if (protectAbortedByCancel || protectAbortedByInitialize) {
      msg_tiny_id = 0xFFFF;
      memzero(pin, sizeof(pin));
      usbTiny(0);
      return NULL;
    }
#if DEBUG_LINK
    if (msg_tiny_id == MessageType_MessageType_DebugLinkGetState) {
      msg_tiny_id = 0xFFFF;
      fsm_msgDebugLinkGetState((DebugLinkGetState *)msg_tiny);
    }
#endif

    ButtonEvent e = next_event();
    if (e == EV_NONE) continue;
    if (e == EV_LEFT) {
      pos = (pos + ITEMS - 1) % ITEMS;
    } else if (e == EV_RIGHT) {
      pos = (pos + 1) % ITEMS;
    } else if (pos == ITEM_OK) {
      if (len > 0) {
        usbTiny(0);
        return pin;
      }
    } else if (pos == ITEM_DEL) {
      if (len > 0) pin[--len] = '\0';
    } else if (len < MAX_PIN_LEN) {
      pin[len++] = (char)('1' + pos);
      pin[len] = '\0';
    }
    draw(type, len, pos);
  }
}
