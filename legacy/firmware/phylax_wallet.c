/*
 * PHYLAX Blocks - main block: saved wallets, a copy on every block.
 * See phylax_wallet.h.
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
#include "phylax_setup.h"
#include "phylax_stack.h"
#include "timer.h"

#define REQUEST_TIMEOUT_MS 800
#define REQUEST_RETRIES 3
#define XPUB_TIMEOUT_MS 10000
#define RETRY_PAUSE_MS 5000  // after a failed fetch, or a block not answering

// ------------------------------------------------------------ state

// The merged wallet list: what every block's copy should become.
static PhylaxWalletRec wallets[PHYLAX_WALLET_SLOTS];

// What one copy holds in one slot: the wallet's identity and generation.
typedef struct {
  uint32_t id;  // 0 = empty
  uint32_t generation;
} SlotState;

// The main block's own copy.
static bool own_loaded = false;
static SlotState own_slots[PHYLAX_WALLET_SLOTS];

// The other blocks' copies, known by block id.
typedef struct {
  bool used;
  uint8_t uid[PHYLAX_UID_SIZE];
  uint8_t addr;
  bool loaded;
  uint32_t retry_after_ms;  // a block that did not answer is left alone
  SlotState slots[PHYLAX_WALLET_SLOTS];
} Replica;
static Replica reps[PHYLAX_STACK_MAX];

typedef enum {
  W_IDLE,
  W_LOADING,   // reading a block's copy
  W_WRITING,   // writing one slot of a block's copy
  W_FETCHING,  // asking key blocks for their xpubs
  W_PROMPT,    // "New n-of-n wallet?" on screen
} WalletState;
static WalletState wstate = W_IDLE;

// Request in flight to a block (loading or writing one slot).
static struct {
  int rep;
  int slot;
  uint16_t offset;
  uint16_t total;  // writing: record length (0 = delete)
  uint8_t retries;
  uint32_t sent_ms;
  SlotState writing;  // what the block's copy holds once the write is done
  uint8_t data[PHYLAX_WALLET_RECORD_SIZE];
} io;

// The wallet being created.
static PhylaxWalletRec candidate;
static uint8_t candidate_addr[PHYLAX_WALLET_MAX_KEYS];  // 0 = main block
static int fetch_key;      // index into candidate.keys
static int fetch_net;      // 0 mainnet, 1 testnet
static uint32_t fetch_ms;

// Fingerprint set the user said "no" to, or whose wallet the PC deleted
// (sorted); asked again once the blocks change.
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

static void hex32(char *out, uint32_t v) {
  static const char hexd[] = "0123456789abcdef";
  for (int i = 0; i < 8; i++) out[i] = hexd[(v >> (28 - 4 * i)) & 0xF];
  out[8] = '\0';
}

// A wallet the user can see and use (not deleted).
static bool live(const PhylaxWalletRec *w) { return w->used && !w->deleted; }

// A wallet's identity: its n and set of fingerprints (stored sorted).
static uint32_t wallet_id(const PhylaxWalletRec *w) {
  if (!w->used) return 0;
  uint32_t h = 2166136261u ^ w->n;  // FNV-1a
  for (int k = 0; k < w->n; k++) {
    for (int b = 0; b < 4; b++) {
      h ^= (w->keys[k].fingerprint >> (8 * b)) & 0xFF;
      h *= 16777619u;
    }
  }
  return h ? h : 1;
}

static SlotState slot_state(const PhylaxWalletRec *w) {
  SlotState st = {wallet_id(w), w->used ? w->generation : 0};
  return st;
}

static bool same_state(SlotState a, SlotState b) {
  return a.id == b.id && a.generation == b.generation;
}

// Fold one copy's record into the merged list: a wallet already known is
// replaced only by a newer generation; an unknown one takes the same slot if
// free, else the first free slot.
static void merge(int slot, const PhylaxWalletRec *w) {
  if (!w->used) return;
  uint32_t id = wallet_id(w);
  for (int i = 0; i < PHYLAX_WALLET_SLOTS; i++) {
    if (wallets[i].used && wallet_id(&wallets[i]) == id) {
      if (w->generation > wallets[i].generation) wallets[i] = *w;
      return;
    }
  }
  if (!wallets[slot].used) {
    wallets[slot] = *w;
    return;
  }
  for (int i = 0; i < PHYLAX_WALLET_SLOTS; i++) {
    if (!wallets[i].used) {
      wallets[i] = *w;
      return;
    }
  }
  // all 8 slots taken: the extra wallet is dropped
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
  if (slot < 0 || slot >= PHYLAX_WALLET_SLOTS || !live(&wallets[slot])) {
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

// ------------------------------------------------------------ copies

static void load_own_copy(void) {
  static uint8_t buf[PHYLAX_WALLET_RECORD_SIZE];
  static PhylaxWalletRec w;
  for (int s = 0; s < PHYLAX_WALLET_SLOTS; s++) {
    uint16_t len = 0;
    own_slots[s].id = 0;
    own_slots[s].generation = 0;
    if (phylax_store_get(s, buf, &len) && phylax_wallet_decode(buf, len, &w)) {
      merge(s, &w);
      own_slots[s] = slot_state(&w);
    }
  }
  own_loaded = true;
}

// Keep the replica list in step with the key blocks in the stack.
static void refresh_replicas(void) {
  const PhylaxStackEntry *e;
  // forget blocks that left
  for (int r = 0; r < PHYLAX_STACK_MAX; r++) {
    if (!reps[r].used) continue;
    bool present = false;
    for (size_t i = 0; (e = phylax_stack_get(i)) != NULL; i++) {
      if (memcmp(e->hello.uid, reps[r].uid, PHYLAX_UID_SIZE) == 0) {
        present = true;
        reps[r].addr = e->addr;
      }
    }
    if (!present) memzero(&reps[r], sizeof(reps[r]));
  }
  // add blocks that joined
  for (size_t i = 0; (e = phylax_stack_get(i)) != NULL; i++) {
    if (e->hello.kind != PHYLAX_KIND_KEY) continue;
    bool known = false;
    for (int r = 0; r < PHYLAX_STACK_MAX; r++) {
      if (reps[r].used &&
          memcmp(e->hello.uid, reps[r].uid, PHYLAX_UID_SIZE) == 0) {
        known = true;
      }
    }
    for (int r = 0; !known && r < PHYLAX_STACK_MAX; r++) {
      if (!reps[r].used) {
        reps[r].used = true;
        memcpy(reps[r].uid, e->hello.uid, PHYLAX_UID_SIZE);
        reps[r].addr = e->addr;
        known = true;
      }
    }
  }
}

static bool rep_available(int r) {
  return reps[r].used && (int32_t)(timer_ms() - reps[r].retry_after_ms) >= 0;
}

static void send_get(void) {
  uint8_t p[4] = {(uint8_t)io.slot, io.offset & 0xFF, io.offset >> 8,
                  PHYLAX_STORE_CHUNK};
  io.sent_ms = timer_ms();
  phylax_bus_send(reps[io.rep].addr, PHYLAX_MSG_STORE_GET, p, sizeof(p));
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
  if (n) memcpy(p + 5, io.data + io.offset, n);
  io.sent_ms = timer_ms();
  phylax_bus_send(reps[io.rep].addr, PHYLAX_MSG_STORE_PUT, p, 5 + n);
}

static void start_load(int r) {
  wstate = W_LOADING;
  io.rep = r;
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
    reps[io.rep].loaded = true;
    wstate = W_IDLE;
    shown[0] = '\0';
    return;
  }
  send_get();
}

static void start_write(int r, int slot) {
  wstate = W_WRITING;
  io.rep = r;
  io.slot = slot;
  io.offset = 0;
  io.retries = 0;
  io.total = wallets[slot].used ? phylax_wallet_encode(&wallets[slot], io.data)
                                : 0;  // 0: delete the slot
  // Snapshot: the PC may rename the wallet while the chunks are on the way.
  io.writing = slot_state(&wallets[slot]);
  send_put();
}

void phylax_wallet_on_store_reply(const PhylaxFrame *f) {
  if (wstate != W_LOADING && wstate != W_WRITING) return;
  if (f->src != reps[io.rep].addr || f->len < 4) return;
  uint8_t slot = f->payload[0];
  uint16_t offset = f->payload[1] | (f->payload[2] << 8);
  uint8_t status = f->payload[3];
  if (slot != io.slot || offset != io.offset) return;  // stale

  if (wstate == W_LOADING && f->type == PHYLAX_MSG_STORE_DATA) {
    uint16_t n = f->len - 4;
    SlotState *st = &reps[io.rep].slots[io.slot];
    if (status != PHYLAX_STORE_OK) {  // empty slot (or unreadable)
      st->id = 0;
      st->generation = 0;
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
    static PhylaxWalletRec w;
    if (phylax_wallet_decode(io.data, io.offset, &w)) {
      merge(io.slot, &w);
      *st = slot_state(&w);
    } else {
      st->id = 0;
      st->generation = 0;
    }
    load_next_slot();
    return;
  }

  if (wstate == W_WRITING && f->type == PHYLAX_MSG_STORE_ACK) {
    if (status != PHYLAX_STORE_OK) {
      reps[io.rep].retry_after_ms = timer_ms() + RETRY_PAUSE_MS;
      wstate = W_IDLE;
      return;
    }
    uint16_t n = io.total - io.offset;
    io.offset += n > PHYLAX_STORE_CHUNK ? PHYLAX_STORE_CHUNK : n;
    io.retries = 0;
    if (io.offset < io.total) {
      send_put();
      return;
    }
    reps[io.rep].slots[io.slot] = io.writing;
    wstate = W_IDLE;
  }
}

// One step of keeping every copy equal to the merged list. Returns true if
// it started (or did) some work, false when everything is in step.
static bool sync_step(void) {
  if (!own_loaded) {
    load_own_copy();
    return true;
  }
  for (int r = 0; r < PHYLAX_STACK_MAX; r++) {
    if (rep_available(r) && !reps[r].loaded) {
      start_load(r);
      return true;
    }
  }
  // own copy first (direct flash access), then the other blocks
  for (int s = 0; s < PHYLAX_WALLET_SLOTS; s++) {
    SlotState want = slot_state(&wallets[s]);
    if (!same_state(own_slots[s], want)) {
      static uint8_t buf[PHYLAX_WALLET_RECORD_SIZE];
      uint16_t len = wallets[s].used ? phylax_wallet_encode(&wallets[s], buf)
                                     : 0;
      if (phylax_store_put(s, buf, len)) own_slots[s] = want;
      return true;
    }
  }
  for (int r = 0; r < PHYLAX_STACK_MAX; r++) {
    if (!rep_available(r) || !reps[r].loaded) continue;
    for (int s = 0; s < PHYLAX_WALLET_SLOTS; s++) {
      if (!same_state(reps[r].slots[s], slot_state(&wallets[s]))) {
        start_write(r, s);
        return true;
      }
    }
  }
  return false;
}

bool phylax_wallet_ready(void) {
  if (!own_loaded) return false;
  for (int r = 0; r < PHYLAX_STACK_MAX; r++) {
    if (reps[r].used && !reps[r].loaded) return false;
  }
  return true;
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
  // If the key block is waiting for something (a PIN because it was locked
  // meanwhile, a button, ...), nobody will answer it: cancel it, so it is
  // free for the user's next request.
  uint8_t peer = phylax_bus_trezor_peer();
  if (peer != PHYLAX_ADDR_NONE) {
    phylax_bus_send_trezor_msg(peer, MessageType_MessageType_Cancel, NULL, 0);
  }
  phylax_bus_set_trezor_peer(PHYLAX_ADDR_NONE, NULL);
  wstate = W_IDLE;
  pause_retries();
}

static void fetch_next(void);

static void on_key_message(uint16_t msg_id, const uint8_t *data,
                           uint32_t len) {
  if (wstate != W_FETCHING) return;
  if (msg_id != MessageType_MessageType_PublicKey) {
    // PinMatrixRequest (locked), ButtonRequest, Failure, ...: never prompt
    // the user from the background. Cancelled; tried again later.
    fetch_failed();
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

// Case-insensitive: "Savings" and "savings" are the same name.
static bool name_taken(const char *name, int except_slot) {
  for (int i = 0; i < PHYLAX_WALLET_SLOTS; i++) {
    if (i == except_slot || !live(&wallets[i])) continue;
    const char *a = wallets[i].name, *b = name;
    while (*a && *b) {
      char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
      char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
      if (ca != cb) break;
      a++;
      b++;
    }
    if (*a == '\0' && *b == '\0') return true;
  }
  return false;
}

static void start_new_wallet(const uint32_t *fps, const uint8_t *addrs,
                             int n) {
  memzero(&candidate, sizeof(candidate));
  candidate.n = n;
  for (int i = 0; i < n; i++) {
    candidate.keys[i].fingerprint = fps[i];
    candidate_addr[i] = addrs[i];
  }
  // Auto name, e.g. "2-of-2 #1", the first number not taken (the PC may
  // have renamed or deleted some); the PC can rename it later.
  for (int k = 1; k < 100; k++) {
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
    if (!name_taken(candidate.name, -1)) break;
  }
  fetch_key = 0;
  fetch_net = 0;
  wstate = W_FETCHING;
  fetch_next();
}

// ------------------------------------------------------------ screens
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

// An empty slot first; else the oldest deleted wallet's hidden record (a
// block that missed that delete could then bring the wallet back).
static int free_slot(void) {
  for (int i = 0; i < PHYLAX_WALLET_SLOTS; i++) {
    if (!wallets[i].used) return i;
  }
  int best = -1;
  for (int i = 0; i < PHYLAX_WALLET_SLOTS; i++) {
    if (wallets[i].deleted &&
        (best < 0 || wallets[i].generation < wallets[best].generation)) {
      best = i;
    }
  }
  return best;
}

// The slot of the wallet with this identity (live or deleted), or -1.
static int slot_of(uint32_t id) {
  for (int i = 0; i < PHYLAX_WALLET_SLOTS; i++) {
    if (wallets[i].used && wallet_id(&wallets[i]) == id) return i;
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
  // The same blocks as a deleted wallet: it comes back in its own slot,
  // newer than its hidden record.
  candidate.used = true;
  candidate.generation = 1;
  int slot = slot_of(wallet_id(&candidate));
  if (slot >= 0) {
    candidate.generation = wallets[slot].generation + 1;
  } else {
    slot = free_slot();
  }
  if (slot < 0) {
    wstate = W_IDLE;
    show_status(_("Wallet list is full"), _("(8 wallets)"), NULL);
    return;
  }
  // Becomes the newest copy; the sync writes it to every block.
  wallets[slot] = candidate;
  wstate = W_IDLE;
  shown[0] = '\0';
  layoutDialog(&bmp_icon_info, NULL, NULL, NULL, _("PHYLAX"),
               _("Saving wallet"), candidate.name, NULL, NULL, NULL);
}

void phylax_wallet_ui_poll(void) {
  if (phylax_role() != PHYLAX_ROLE_MAIN) return;
  phylax_setup_ui_poll();
  if (wstate != W_PROMPT) return;
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
    if (!live(w) || w->n != n) continue;
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
  // The blocks changed since that "no" (or delete): forget it, so putting
  // the same blocks back asks again.
  if (!was_declined) declined_n = 0;
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
    if (!live(w)) continue;
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

void phylax_wallet_poll(void) {
  // Signing has priority: drop whatever we were doing with a key block.
  if (phylax_relay_state() != PHYLAX_RELAY_IDLE ||
      phylax_selected_block() != PHYLAX_ADDR_NONE) {
    // The relay (or the PC, in select mode) has taken over the conversation with the key blocks (it set
    // its own peer); just step aside. A late PublicKey is ignored by both.
    if (wstate == W_FETCHING || wstate == W_PROMPT) wstate = W_IDLE;
    phylax_setup_abort();
    return;  // a copy being read or written just times out and is retried
  }
  // Setting up a new key block: it has the screen and the bus conversation.
  if (phylax_setup_busy()) {
    phylax_setup_poll();
    shown[0] = '\0';
    return;
  }
  if (!config_isInitialized()) {
    // No wallets without the main block's own seed, but a key block can
    // still be set up.
    if (layoutLast == layoutHomescreen || layoutLast == layoutDialog) {
      phylax_setup_poll();
    }
    return;
  }

  uint32_t now = timer_ms();
  switch (wstate) {
    case W_LOADING:
    case W_WRITING:
      if (now - io.sent_ms >= REQUEST_TIMEOUT_MS) {
        if (++io.retries > REQUEST_RETRIES) {
          // The block does not answer: leave it alone for a while.
          reps[io.rep].retry_after_ms = now + RETRY_PAUSE_MS;
          reps[io.rep].loaded = false;
          wstate = W_IDLE;
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

  refresh_replicas();
  if (sync_step()) return;

  // Every copy is in step: tell the user what the stack means.
  // Only change the screen while nothing else is shown.
  if (layoutLast != layoutHomescreen && layoutLast != layoutDialog) return;
  // A key block without a seed first: "New key block found. Set it up?"
  if (phylax_setup_poll()) {
    shown[0] = '\0';
    return;
  }
  check_stack();
}

// ------------------------------------------------------------ PC changes

static const char *check_slot(int slot) {
  if (!phylax_wallet_ready()) {
    return _("Wallets still loading, try again");
  }
  if (slot < 0 || slot >= PHYLAX_WALLET_SLOTS || !live(&wallets[slot])) {
    return _("No wallet in that slot");
  }
  return NULL;
}

const char *phylax_wallet_check_rename(int slot, const char *name) {
  const char *err = check_slot(slot);
  if (err) return err;
  size_t len = strlen(name);
  if (len < 1 || len > PHYLAX_WALLET_NAME_MAX) {
    return _("Name must be 1 to 15 characters");
  }
  bool blank = true;
  for (size_t i = 0; i < len; i++) {
    // what the block's font can draw
    unsigned char c = (unsigned char)name[i];
    if (c < 0x20 || c > 0x7E) {
      return _("Name: letters, digits, symbols only");
    }
    if (c != ' ') blank = false;
  }
  if (blank) return _("Name must not be blank");
  if (strcmp(wallets[slot].name, name) == 0) {
    return _("The wallet already has that name");
  }
  if (name_taken(name, slot)) {
    return _("Another wallet has that name");
  }
  return NULL;
}

const char *phylax_wallet_check_delete(int slot) { return check_slot(slot); }

// A prompt on screen was just covered by the PC's confirmation: drop it,
// it comes back by itself.
static void drop_prompt(void) {
  if (wstate == W_PROMPT) wstate = W_IDLE;
  shown[0] = '\0';
}

void phylax_wallet_rename(int slot, const char *name) {
  if (phylax_wallet_check_rename(slot, name) != NULL) return;
  strlcpy(wallets[slot].name, name, sizeof(wallets[slot].name));
  wallets[slot].generation++;
  drop_prompt();
}

void phylax_wallet_delete(int slot) {
  if (phylax_wallet_check_delete(slot) != NULL) return;
  PhylaxWalletRec *w = &wallets[slot];
  w->deleted = true;
  w->generation++;
  // Not offered again at once while its blocks are still attached.
  declined_n = w->n;
  for (int i = 0; i < w->n; i++) declined[i] = w->keys[i].fingerprint;
  drop_prompt();
}
