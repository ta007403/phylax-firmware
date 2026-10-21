/*
 * PHYLAX Blocks - main block: saved wallets. See phylax_wallet.h.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "phylax_wallet.h"

#include <string.h>

#include "bip32.h"
#include "bip39.h"
#include "buttons.h"
#include "config.h"
#include "curves.h"
#include "gettext.h"
#include "layout2.h"
#include "memzero.h"
#include "messages.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "phylax_relay.h"
#include "phylax_stack.h"
#include "timer.h"

#define REQUEST_TIMEOUT_MS 800
#define REQUEST_RETRIES 3
#define XPUB_TIMEOUT_MS 10000
#define RETRY_PAUSE_MS 5000  // after a failed fetch or save

// ------------------------------------------------------------ state

static PhylaxWalletRec wallets[PHYLAX_WALLET_SLOTS];
static uint8_t storage_addr = PHYLAX_ADDR_NONE;
static bool loaded = false;

typedef enum {
  W_IDLE,
  W_LOADING,      // reading records from the storage block
  W_FETCHING,     // asking key blocks for their xpubs
  W_PROMPT,       // "New n-of-n wallet?" on screen
  W_SAVING,       // writing a record to the storage block
} WalletState;
static WalletState wstate = W_IDLE;

// Request in flight to the storage block (loading or saving).
static struct {
  int slot;
  uint16_t offset;
  uint16_t total;  // saving: record length
  uint8_t retries;
  uint32_t sent_ms;
  uint8_t data[PHYLAX_WALLET_RECORD_SIZE];
} io;

// The wallet being created.
static PhylaxWalletRec candidate;
static uint8_t candidate_addr[PHYLAX_WALLET_MAX_KEYS];  // 0 = main block
static int fetch_key;      // index into candidate.keys
static int fetch_net;      // 0 mainnet, 1 testnet
static uint32_t fetch_ms;

// Fingerprint set the user said "no" to (sorted); asked again if it changes.
static uint32_t declined[PHYLAX_WALLET_MAX_KEYS];
static uint8_t declined_n = 0;

// Earliest time to try creating a wallet again after a failure.
static uint32_t retry_after_ms = 0;
static bool retry_pause = false;

static void pause_retries(void) {
  retry_after_ms = timer_ms() + RETRY_PAUSE_MS;
  retry_pause = true;
}

// What the screen shows, so it is redrawn only on change.
static char shown[64];

// ------------------------------------------------------------ helpers

bool phylax_wallet_storage_present(void) {
  return storage_addr != PHYLAX_ADDR_NONE;
}

static void hex32(char *out, uint32_t v) {
  static const char hexd[] = "0123456789abcdef";
  for (int i = 0; i < 8; i++) out[i] = hexd[(v >> (28 - 4 * i)) & 0xF];
  out[8] = '\0';
}

// The seed fingerprints in the stack now: the main block's and every key
// block's that has a seed and is unlocked. Returns how many (sorted), with
// the bus address of each (0 for the main block).
static int present_fps(uint32_t fps[PHYLAX_STACK_MAX + 1],
                       uint8_t addrs[PHYLAX_STACK_MAX + 1]) {
  int n = 0;
  PhylaxHello me;
  phylax_my_hello(&me);
  if (me.flags & PHYLAX_HELLO_HAS_FP) {
    fps[n] = me.fingerprint;
    addrs[n++] = 0;
  }
  const PhylaxStackEntry *e;
  for (size_t i = 0; (e = phylax_stack_get(i)) != NULL; i++) {
    if (e->hello.kind == PHYLAX_KIND_KEY &&
        (e->hello.flags & PHYLAX_HELLO_HAS_FP)) {
      fps[n] = e->hello.fingerprint;
      addrs[n++] = e->addr;
    }
  }
  // sort both by fingerprint
  for (int i = 1; i < n; i++) {
    for (int j = i; j > 0 && fps[j - 1] > fps[j]; j--) {
      uint32_t t = fps[j];
      fps[j] = fps[j - 1];
      fps[j - 1] = t;
      uint8_t a = addrs[j];
      addrs[j] = addrs[j - 1];
      addrs[j - 1] = a;
    }
  }
  return n;
}

static bool fp_in(uint32_t fp, const uint32_t *set, int n) {
  for (int i = 0; i < n; i++) {
    if (set[i] == fp) return true;
  }
  return false;
}

const PhylaxWalletRec *phylax_wallet_get(int slot, bool *complete) {
  if (slot < 0 || slot >= PHYLAX_WALLET_SLOTS || !wallets[slot].used) {
    return NULL;
  }
  if (complete) {
    uint32_t fps[PHYLAX_STACK_MAX + 1];
    uint8_t addrs[PHYLAX_STACK_MAX + 1];
    int n = present_fps(fps, addrs);
    *complete = true;
    for (int k = 0; k < wallets[slot].n; k++) {
      if (!fp_in(wallets[slot].keys[k].fingerprint, fps, n)) *complete = false;
    }
  }
  return &wallets[slot];
}

// ------------------------------------------------------------ storage I/O

static void send_get(void) {
  uint8_t p[4] = {(uint8_t)io.slot, io.offset & 0xFF, io.offset >> 8,
                  PHYLAX_STORE_CHUNK};
  io.sent_ms = timer_ms();
  phylax_bus_send(storage_addr, PHYLAX_MSG_STORE_GET, p, sizeof(p));
}

static void send_put(void) {
  uint8_t p[5 + PHYLAX_STORE_CHUNK];
  uint16_t n = io.total - io.offset;
  if (n > PHYLAX_STORE_CHUNK) n = PHYLAX_STORE_CHUNK;
  p[0] = io.slot;
  p[1] = io.offset & 0xFF;
  p[2] = io.offset >> 8;
  p[3] = io.total & 0xFF;
  p[4] = io.total >> 8;
  memcpy(p + 5, io.data + io.offset, n);
  io.sent_ms = timer_ms();
  phylax_bus_send(storage_addr, PHYLAX_MSG_STORE_PUT, p, 5 + n);
}

static void start_loading(void) {
  memzero(wallets, sizeof(wallets));
  loaded = false;
  wstate = W_LOADING;
  io.slot = 0;
  io.offset = 0;
  io.retries = 0;
  send_get();
}

static void load_next_slot(void) {
  io.slot++;
  io.offset = 0;
  io.retries = 0;
  if (io.slot >= PHYLAX_WALLET_SLOTS) {
    loaded = true;
    wstate = W_IDLE;
    shown[0] = '\0';  // redraw the status
    return;
  }
  send_get();
}

static void start_saving(int slot, const PhylaxWalletRec *w) {
  io.slot = slot;
  io.offset = 0;
  io.retries = 0;
  io.total = phylax_wallet_encode(w, io.data);
  wstate = W_SAVING;
  send_put();
}

void phylax_wallet_on_store_reply(const PhylaxFrame *f) {
  if (f->src != storage_addr || f->len < 4) return;
  uint8_t slot = f->payload[0];
  uint16_t offset = f->payload[1] | (f->payload[2] << 8);
  uint8_t status = f->payload[3];
  if (slot != io.slot || offset != io.offset) return;  // stale

  if (wstate == W_LOADING && f->type == PHYLAX_MSG_STORE_DATA) {
    uint16_t n = f->len - 4;
    if (status != PHYLAX_STORE_OK) {  // empty slot (or error): skip it
      load_next_slot();
      return;
    }
    if (n > 0 && io.offset + n <= sizeof(io.data)) {
      memcpy(io.data + io.offset, f->payload + 4, n);
      io.offset += n;
    }
    if (n == PHYLAX_STORE_CHUNK && io.offset < sizeof(io.data)) {
      io.retries = 0;
      send_get();  // more to read
      return;
    }
    phylax_wallet_decode(io.data, io.offset, &wallets[io.slot]);
    load_next_slot();
    return;
  }

  if (wstate == W_SAVING && f->type == PHYLAX_MSG_STORE_ACK) {
    if (status != PHYLAX_STORE_OK) {
      wstate = W_IDLE;  // failed: the stack is checked again a bit later
      pause_retries();
      return;
    }
    uint16_t n = io.total - io.offset;
    io.offset += n > PHYLAX_STORE_CHUNK ? PHYLAX_STORE_CHUNK : n;
    io.retries = 0;
    if (io.offset < io.total) {
      send_put();
      return;
    }
    wallets[io.slot] = candidate;  // saved
    wallets[io.slot].used = true;
    wstate = W_IDLE;
    shown[0] = '\0';
  }
}

// ------------------------------------------------------------ xpubs

// The main block's own account xpub, derived from its mnemonic (no
// passphrase, never prompts). Slow the first time on real hardware.
static bool own_xpub(uint32_t coin_type, PhylaxXpub *x) {
  static char mnemonic[MAX_MNEMONIC_LEN + 1];
  static uint8_t seed[64];
  static HDNode node;
  bool ok = false;
  if (config_getMnemonic(mnemonic, sizeof(mnemonic))) {
    mnemonic_to_seed(mnemonic, "", seed, NULL);
    if (hdnode_from_seed(seed, sizeof(seed), SECP256K1_NAME, &node) == 1 &&
        hdnode_private_ckd_prime(&node, 48) == 1 &&
        hdnode_private_ckd_prime(&node, coin_type) == 1 &&
        hdnode_private_ckd_prime(&node, 0) == 1) {
      hdnode_fill_public_key(&node);
      x->parent_fingerprint = hdnode_fingerprint(&node);
      if (hdnode_private_ckd_prime(&node, 2) == 1) {
        hdnode_fill_public_key(&node);
        x->child_num = node.child_num;
        memcpy(x->chain_code, node.chain_code, 32);
        memcpy(x->public_key, node.public_key, 33);
        ok = true;
      }
    }
  }
  memzero(mnemonic, sizeof(mnemonic));
  memzero(seed, sizeof(seed));
  memzero(&node, sizeof(node));
  return ok;
}

static void fetch_failed(void) {
  phylax_bus_set_trezor_peer(PHYLAX_ADDR_NONE, NULL);
  wstate = W_IDLE;
  pause_retries();
}

static void fetch_next(void);

static void on_key_message(uint16_t msg_id, const uint8_t *data,
                           uint32_t len) {
  if (wstate != W_FETCHING) return;
  if (msg_id != MessageType_MessageType_PublicKey) {
    fetch_failed();  // Failure (locked, wrong path, ...): try again later
    return;
  }
  static PublicKey pk;
  memzero(&pk, sizeof(pk));
  pb_istream_t stream = pb_istream_from_buffer(data, len);
  if (!pb_decode(&stream, PublicKey_fields, &pk) ||
      pk.node.public_key.size != 33 || pk.node.chain_code.size != 32) {
    fetch_failed();
    return;
  }
  PhylaxKeyRec *k = &candidate.keys[fetch_key];
  PhylaxXpub *x = fetch_net == 0 ? &k->mainnet : &k->testnet;
  x->parent_fingerprint = pk.node.fingerprint;
  x->child_num = pk.node.child_num;
  memcpy(x->chain_code, pk.node.chain_code.bytes, 32);
  memcpy(x->public_key, pk.node.public_key.bytes, 33);
  fetch_net++;
  fetch_next();
}

static void prompt_show(void);

// Get the next missing xpub: the main block's own locally, a key block's
// with a GetPublicKey over the bus.
static void fetch_next(void) {
  for (;;) {
    if (fetch_net > 1) {
      fetch_net = 0;
      fetch_key++;
    }
    if (fetch_key >= candidate.n) {
      phylax_bus_set_trezor_peer(PHYLAX_ADDR_NONE, NULL);
      wstate = W_PROMPT;
      prompt_show();
      return;
    }
    PhylaxKeyRec *k = &candidate.keys[fetch_key];
    uint32_t coin_type = fetch_net == 0 ? 0 : 1;
    if (candidate_addr[fetch_key] == 0) {  // the main block itself
      if (!own_xpub(coin_type, fetch_net == 0 ? &k->mainnet : &k->testnet)) {
        fetch_failed();
        return;
      }
      fetch_net++;
      continue;
    }
    static GetPublicKey req;
    static uint8_t buf[GetPublicKey_size];
    memzero(&req, sizeof(req));
    req.address_n_count = 4;
    req.address_n[0] = 0x80000000 | 48;
    req.address_n[1] = 0x80000000 | coin_type;
    req.address_n[2] = 0x80000000 | 0;
    req.address_n[3] = 0x80000000 | 2;
    req.has_coin_name = true;
    strlcpy(req.coin_name, coin_type == 0 ? "Bitcoin" : "Testnet",
            sizeof(req.coin_name));
    req.has_script_type = true;
    req.script_type = InputScriptType_SPENDMULTISIG;
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&stream, GetPublicKey_fields, &req)) {
      fetch_failed();
      return;
    }
    fetch_ms = timer_ms();
    phylax_bus_set_trezor_peer(candidate_addr[fetch_key], on_key_message);
    phylax_bus_send_trezor_msg(candidate_addr[fetch_key],
                               MessageType_MessageType_GetPublicKey, buf,
                               stream.bytes_written);
    return;  // wait for PublicKey
  }
}

static int count_n_of_n(uint8_t n) {
  int c = 0;
  for (int i = 0; i < PHYLAX_WALLET_SLOTS; i++) {
    if (wallets[i].used && wallets[i].n == n) c++;
  }
  return c;
}

static void start_new_wallet(const uint32_t *fps, const uint8_t *addrs,
                             int n) {
  memzero(&candidate, sizeof(candidate));
  candidate.n = n;
  for (int i = 0; i < n; i++) {
    candidate.keys[i].fingerprint = fps[i];
    candidate_addr[i] = addrs[i];
  }
  // Auto name, e.g. "2-of-2 #1"; the PC can rename it later.
  int k = count_n_of_n(n) + 1;
  char *p = candidate.name;
  *p++ = '0' + n;
  memcpy(p, "-of-", 4);
  p += 4;
  *p++ = '0' + n;
  memcpy(p, " #", 2);
  p += 2;
  if (k >= 10) *p++ = '0' + k / 10;
  *p++ = '0' + k % 10;
  *p = '\0';
  fetch_key = 0;
  fetch_net = 0;
  wstate = W_FETCHING;
  fetch_next();
}

// ------------------------------------------------------------ screens

static void show_status(const char *line1, const char *line2,
                        const char *line3) {
  char key[64];
  strlcpy(key, line1, sizeof(key));
  strlcat(key, line2 ? line2 : "", sizeof(key));
  strlcat(key, line3 ? line3 : "", sizeof(key));
  if (strcmp(key, shown) == 0 && layoutLast == layoutDialog) return;
  strlcpy(shown, key, sizeof(shown));
  layoutDialog(&bmp_icon_info, NULL, NULL, NULL, _("PHYLAX"), line1, line2,
               line3, NULL, NULL);
}

static void prompt_show(void) {
  static char title[24];
  static char fp_lines[3][24];
  strlcpy(title, "New ", sizeof(title));
  strlcat(title, candidate.name, sizeof(title));
  // drop the " #k" suffix from the title
  char *hash = strchr(title, '#');
  if (hash && hash > title) hash[-1] = '\0';
  strlcat(title, " wallet?", sizeof(title));
  for (int i = 0; i < 3; i++) fp_lines[i][0] = '\0';
  for (int i = 0; i < candidate.n && i < 3; i++) {
    hex32(fp_lines[i], candidate.keys[i].fingerprint);
  }
  shown[0] = '\0';
  layoutDialog(&bmp_icon_question, _("No"), _("Create"), NULL, title,
               _("Blocks:"), fp_lines[0], fp_lines[1], fp_lines[2], NULL);
}

static int free_slot(void) {
  for (int i = 0; i < PHYLAX_WALLET_SLOTS; i++) {
    if (!wallets[i].used) return i;
  }
  return -1;
}

static void prompt_answer(bool yes) {
  if (!yes) {
    declined_n = candidate.n;
    for (int i = 0; i < candidate.n; i++) {
      declined[i] = candidate.keys[i].fingerprint;
    }
    wstate = W_IDLE;
    shown[0] = '\0';
    return;
  }
  int slot = free_slot();
  if (slot < 0) {
    wstate = W_IDLE;
    show_status(_("Storage is full"), _("(8 wallets)"), NULL);
    return;
  }
  candidate.used = true;
  layoutDialog(&bmp_icon_info, NULL, NULL, NULL, _("PHYLAX"),
               _("Saving wallet"), candidate.name, NULL, NULL, NULL);
  start_saving(slot, &candidate);
}

void phylax_wallet_ui_poll(void) {
  if (phylax_role() != PHYLAX_ROLE_MAIN || wstate != W_PROMPT) return;
#if EMULATOR
  if (phylax_emu_autoconfirm()) {  // tests: nobody presses the buttons
    prompt_answer(true);
    return;
  }
#endif
  if (button.YesUp) prompt_answer(true);
  else if (button.NoUp) prompt_answer(false);
}

// What the stack means for the saved wallets: ready, missing a block, or a
// new combination to offer.
static void check_stack(void) {
  uint32_t fps[PHYLAX_STACK_MAX + 1];
  uint8_t addrs[PHYLAX_STACK_MAX + 1];
  int n = present_fps(fps, addrs);

  // 1. A saved wallet with exactly the blocks present.
  int exact = -1;
  for (int i = 0; i < PHYLAX_WALLET_SLOTS && exact < 0; i++) {
    const PhylaxWalletRec *w = &wallets[i];
    if (!w->used || w->n != n) continue;
    bool same = true;
    for (int k = 0; k < w->n; k++) {
      if (!fp_in(w->keys[k].fingerprint, fps, n)) same = false;
    }
    if (same) exact = i;
  }
  if (exact >= 0) {
    show_status(wallets[exact].name, _("is ready."), NULL);
    return;
  }

  // 2. A new combination of 2..3 blocks: offer to create a wallet.
  bool was_declined = declined_n == n;
  for (int i = 0; was_declined && i < n; i++) {
    if (declined[i] != fps[i]) was_declined = false;
  }
  bool may_try = !retry_pause || (int32_t)(timer_ms() - retry_after_ms) >= 0;
  if (n >= 2 && n <= PHYLAX_WALLET_MAX_KEYS && !was_declined &&
      free_slot() >= 0 && may_try) {
    retry_pause = false;
    start_new_wallet(fps, addrs, n);
    return;
  }

  // 3. A saved wallet missing exactly one block: say which one to attach.
  for (int i = 0; i < PHYLAX_WALLET_SLOTS; i++) {
    const PhylaxWalletRec *w = &wallets[i];
    if (!w->used) continue;
    int missing = 0;
    uint32_t missing_fp = 0;
    for (int k = 0; k < w->n; k++) {
      if (!fp_in(w->keys[k].fingerprint, fps, n)) {
        missing++;
        missing_fp = w->keys[k].fingerprint;
      }
    }
    if (missing == 1) {
      static char line[24];
      strlcpy(line, _("Attach key block "), sizeof(line));
      hex32(line + strlen(line), missing_fp);
      show_status(line, _("to use wallet"), w->name);
      return;
    }
  }
}

// ------------------------------------------------------------ poll

static uint8_t find_storage(void) {
  const PhylaxStackEntry *e;
  for (size_t i = 0; (e = phylax_stack_get(i)) != NULL; i++) {
    if (e->hello.kind == PHYLAX_KIND_STORAGE) return e->addr;
  }
  return PHYLAX_ADDR_NONE;
}

void phylax_wallet_poll(void) {
  // Signing has priority: drop whatever we were doing with a key block.
  if (phylax_relay_state() != PHYLAX_RELAY_IDLE) {
    if (wstate == W_FETCHING || wstate == W_PROMPT) {
      if (wstate == W_FETCHING) phylax_bus_set_trezor_peer(PHYLAX_ADDR_NONE,
                                                           NULL);
      wstate = W_IDLE;
    }
    return;
  }

  uint8_t s = find_storage();
  if (s != storage_addr) {
    storage_addr = s;
    if (s == PHYLAX_ADDR_NONE) {  // storage block removed
      memzero(wallets, sizeof(wallets));
      loaded = false;
      if (wstate != W_FETCHING) wstate = W_IDLE;
    } else {
      start_loading();
    }
    return;
  }

  uint32_t now = timer_ms();
  switch (wstate) {
    case W_LOADING:
    case W_SAVING:
      if (now - io.sent_ms >= REQUEST_TIMEOUT_MS) {
        if (++io.retries > REQUEST_RETRIES) {
          wstate = W_IDLE;  // give up; reloaded when the storage block returns
          pause_retries();
          return;
        }
        if (wstate == W_LOADING) send_get();
        else send_put();
      }
      return;
    case W_FETCHING:
      if (now - fetch_ms >= XPUB_TIMEOUT_MS) fetch_failed();
      return;
    case W_PROMPT: {
      // Cancel the prompt if the blocks changed meanwhile.
      uint32_t fps[PHYLAX_STACK_MAX + 1];
      uint8_t addrs[PHYLAX_STACK_MAX + 1];
      int n = present_fps(fps, addrs);
      bool same = n == candidate.n;
      for (int i = 0; same && i < n; i++) {
        if (fps[i] != candidate.keys[i].fingerprint) same = false;
      }
      if (!same) {
        wstate = W_IDLE;
        shown[0] = '\0';
      }
      return;
    }
    case W_IDLE:
      break;
  }

  if (!loaded || !config_isInitialized()) return;
  // Only change the screen while nothing else is shown.
  if (layoutLast != layoutHomescreen && layoutLast != layoutDialog) return;
  check_stack();
}
