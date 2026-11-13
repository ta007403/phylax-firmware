/*
 * PHYLAX Blocks: "this one" on a key block. See phylax_attention.h.
 */

#include "phylax_attention.h"

#include "config.h"
#include "gettext.h"
#include "layout2.h"
#include "phylax_bus.h"
#include "phylax_pin.h"
#include "timer.h"

// ------------------------------------------------------------ key block

static PhylaxAttention shown_kind = PHYLAX_ATTENTION_OFF;
static uint32_t expires_ms = 0;

// Its home screen (or the screensaver) is on: the request may take its place.
static bool home_on_screen(void) {
  return layoutLast == layoutHomescreen || layoutLast == layoutScreensaver;
}

static void set_kind(PhylaxAttention kind) {
  if (kind == shown_kind) return;
  shown_kind = kind;
  // Anything else on screen (a PC request) stays; the home screen shows the
  // change. The PIN entry of an unlock does not set layoutLast: never redraw
  // while the main block's conversation (its PIN, its setup) is running
  // (found on hardware: the logo over the PIN entry until a button press).
  if (home_on_screen() && !phylax_local_ui()) layoutHomescreen();
}

void phylax_attention_on_frame(const uint8_t *payload, uint16_t len) {
  if (len < 1) return;
  PhylaxAttention kind = (PhylaxAttention)payload[0];
  if (kind != PHYLAX_ATTENTION_UNLOCK && kind != PHYLAX_ATTENTION_SETUP) {
    kind = PHYLAX_ATTENTION_OFF;
  }
  expires_ms = timer_ms() + PHYLAX_ATTENTION_TTL_MS;
  // The block went to sleep (screensaver) while the request was shown: a
  // repeat shows it again.
  if (kind == shown_kind && kind != PHYLAX_ATTENTION_OFF &&
      layoutLast == layoutScreensaver && !phylax_local_ui()) {
    layoutHomescreen();
    return;
  }
  set_kind(kind);
}

void phylax_attention_poll(void) {
  if (shown_kind != PHYLAX_ATTENTION_OFF &&
      (int32_t)(timer_ms() - expires_ms) >= 0) {
    set_kind(PHYLAX_ATTENTION_OFF);
  }
}

bool phylax_attention_draw_home(void) {
  if (shown_kind == PHYLAX_ATTENTION_OFF) return false;
  if (shown_kind == PHYLAX_ATTENTION_UNLOCK) {
    static const char hexd[] = "0123456789abcdef";
    static char fp_line[24];
    uint32_t fp = 0;
    if (config_getPhylaxFingerprint(&fp)) {
      static const char prefix[] = "Key block ";
      size_t n = sizeof(prefix) - 1;
      for (size_t i = 0; i < n; i++) fp_line[i] = prefix[i];
      for (int i = 0; i < 8; i++)
        fp_line[n + i] = hexd[(fp >> (28 - 4 * i)) & 0xF];
      fp_line[n + 8] = '\0';
    } else {
      fp_line[0] = '\0';
    }
    layoutDialog(&bmp_icon_question, NULL, NULL, NULL, _("Main block asks:"),
                 _("Unlock this block?"), NULL, fp_line[0] ? fp_line : NULL,
                 _("Answer on the"), _("main block."));
  } else {
    layoutDialog(&bmp_icon_question, NULL, NULL, NULL, _("Main block asks:"),
                 _("Set up this block?"), NULL, _("Answer on the"),
                 _("main block."), NULL);
  }
  layoutLast = layoutHomescreen;
  return true;
}

// ------------------------------------------------------------ main block

static uint8_t target_addr = PHYLAX_ADDR_NONE;
static PhylaxAttention target_kind = PHYLAX_ATTENTION_OFF;
static uint32_t sent_ms = 0;

static void send(uint8_t addr, PhylaxAttention kind) {
  uint8_t payload = (uint8_t)kind;
  phylax_bus_send(addr, PHYLAX_MSG_ATTENTION, &payload, 1);
  sent_ms = timer_ms();
}

void phylax_attention_target(uint8_t addr, PhylaxAttention kind) {
  if (addr == PHYLAX_ADDR_NONE) kind = PHYLAX_ATTENTION_OFF;
  if (kind == PHYLAX_ATTENTION_OFF) addr = PHYLAX_ADDR_NONE;
  if (addr != target_addr || kind != target_kind) {
    // the block asked before: back to its home screen
    if (target_addr != PHYLAX_ADDR_NONE)
      send(target_addr, PHYLAX_ATTENTION_OFF);
    target_addr = addr;
    target_kind = kind;
    if (addr != PHYLAX_ADDR_NONE) send(addr, kind);
    return;
  }
  if (addr != PHYLAX_ADDR_NONE &&
      (uint32_t)(timer_ms() - sent_ms) >= PHYLAX_ATTENTION_REPEAT_MS) {
    send(addr, kind);
  }
}
