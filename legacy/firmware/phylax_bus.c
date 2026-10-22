/*
 * PHYLAX Blocks - block-to-block bus. See phylax_bus.h.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "phylax_bus.h"
#include "phylax_pin.h"

#include <string.h>

#include "bip32.h"
#include "bip39.h"
#include "config.h"
#include "curves.h"
#include "memzero.h"
#include "phylax_relay.h"
#include "phylax_stack.h"
#include "phylax_wallet.h"
#include "timer.h"
#include "usb.h"
#include "version.h"

#if EMULATOR
#include <stdlib.h>
#endif

#ifndef PHYLAX_ROLE
#define PHYLAX_ROLE PHYLAX_ROLE_STOCK
#endif

// ------------------------------------------------------------ frame layer

uint16_t phylax_crc16(const uint8_t *data, size_t len, uint16_t crc) {
  // CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection.
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                           : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

size_t phylax_frame_encode(const PhylaxFrame *f, uint8_t *out,
                           size_t out_size) {
  if (f->len > PHYLAX_MAX_PAYLOAD) return 0;
  size_t total = PHYLAX_HEADER_SIZE + f->len + 2;
  if (out_size < total) return 0;
  out[0] = PHYLAX_SYNC1;
  out[1] = PHYLAX_SYNC2;
  out[2] = f->dst;
  out[3] = f->src;
  out[4] = f->type;
  out[5] = f->seq;
  out[6] = f->flags;
  out[7] = f->len & 0xFF;
  out[8] = f->len >> 8;
  memcpy(out + PHYLAX_HEADER_SIZE, f->payload, f->len);
  uint16_t crc = phylax_crc16(out + 2, PHYLAX_HEADER_SIZE - 2 + f->len, 0xFFFF);
  out[PHYLAX_HEADER_SIZE + f->len] = crc & 0xFF;
  out[PHYLAX_HEADER_SIZE + f->len + 1] = crc >> 8;
  return total;
}

enum {
  DEC_SYNC1,
  DEC_SYNC2,
  DEC_HEADER,
  DEC_PAYLOAD,
  DEC_CRC1,
  DEC_CRC2,
};

void phylax_decoder_reset(PhylaxDecoder *dec) {
  dec->state = DEC_SYNC1;
  dec->pos = 0;
  dec->crc = 0xFFFF;
}

bool phylax_decoder_feed(PhylaxDecoder *dec, uint8_t byte) {
  PhylaxFrame *f = &dec->frame;
  switch (dec->state) {
    case DEC_SYNC1:
      if (byte == PHYLAX_SYNC1) dec->state = DEC_SYNC2;
      return false;
    case DEC_SYNC2:
      if (byte == PHYLAX_SYNC2) {
        dec->state = DEC_HEADER;
        dec->pos = 0;
        dec->crc = 0xFFFF;
      } else if (byte != PHYLAX_SYNC1) {
        dec->state = DEC_SYNC1;
      }
      return false;
    case DEC_HEADER:
      dec->crc = phylax_crc16(&byte, 1, dec->crc);
      switch (dec->pos++) {
        case 0: f->dst = byte; break;
        case 1: f->src = byte; break;
        case 2: f->type = byte; break;
        case 3: f->seq = byte; break;
        case 4: f->flags = byte; break;
        case 5: f->len = byte; break;
        case 6:
          f->len |= (uint16_t)byte << 8;
          if (f->len > PHYLAX_MAX_PAYLOAD) {
            phylax_decoder_reset(dec);  // garbage: resync
            return false;
          }
          dec->pos = 0;
          dec->state = f->len ? DEC_PAYLOAD : DEC_CRC1;
          break;
      }
      return false;
    case DEC_PAYLOAD:
      dec->crc = phylax_crc16(&byte, 1, dec->crc);
      f->payload[dec->pos++] = byte;
      if (dec->pos == f->len) dec->state = DEC_CRC1;
      return false;
    case DEC_CRC1:
      dec->pos = byte;  // low byte of the received CRC
      dec->state = DEC_CRC2;
      return false;
    case DEC_CRC2: {
      uint16_t rx = (uint16_t)dec->pos | ((uint16_t)byte << 8);
      bool ok = (rx == dec->crc);
      phylax_decoder_reset(dec);
      return ok;
    }
  }
  phylax_decoder_reset(dec);
  return false;
}

size_t phylax_hello_encode(const PhylaxHello *h, uint8_t *out) {
  memcpy(out, h->uid, PHYLAX_UID_SIZE);
  out[12] = h->kind;
  out[13] = h->flags;
  out[14] = h->fingerprint & 0xFF;
  out[15] = (h->fingerprint >> 8) & 0xFF;
  out[16] = (h->fingerprint >> 16) & 0xFF;
  out[17] = (h->fingerprint >> 24) & 0xFF;
  memcpy(out + 18, h->version, 3);
  return PHYLAX_HELLO_SIZE;
}

bool phylax_hello_decode(const uint8_t *in, size_t len, PhylaxHello *h) {
  if (len < PHYLAX_HELLO_SIZE) return false;
  memcpy(h->uid, in, PHYLAX_UID_SIZE);
  h->kind = in[12];
  h->flags = in[13];
  h->fingerprint = (uint32_t)in[14] | ((uint32_t)in[15] << 8) |
                   ((uint32_t)in[16] << 16) | ((uint32_t)in[17] << 24);
  memcpy(h->version, in + 18, 3);
  return true;
}

// ------------------------------------------------------------ this block

PhylaxRole phylax_role(void) {
#if EMULATOR
  static int role = -1;
  if (role < 0) {
    const char *v = getenv("PHYLAX_ROLE");
    role = PHYLAX_ROLE_STOCK;
    if (v != NULL && strcmp(v, "main") == 0) role = PHYLAX_ROLE_MAIN;
    if (v != NULL && strcmp(v, "key") == 0) role = PHYLAX_ROLE_KEY;
  }
  return (PhylaxRole)role;
#else
  return (PhylaxRole)PHYLAX_ROLE;
#endif
}

// Key blocks start without an address; the main block assigns one.
static uint8_t my_addr = PHYLAX_ADDR_NONE;

uint8_t phylax_my_addr(void) {
  if (phylax_role() == PHYLAX_ROLE_MAIN) return PHYLAX_ADDR_MAIN;
  return my_addr;
}

static uint8_t hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

static void my_uid(uint8_t uid[PHYLAX_UID_SIZE]) {
  // The device id Trezor already keeps (Features.device_id): random from the
  // hardware RNG, stored in flash, renewed when the block is wiped.
  for (int i = 0; i < PHYLAX_UID_SIZE; i++) {
    uid[i] = (hex_nibble(config_uuid_str[2 * i]) << 4) |
             hex_nibble(config_uuid_str[2 * i + 1]);
  }
#if EMULATOR
  // The emulator's RNG is deterministic, so every emulator would get the same
  // id. Mix in the bus node number to tell them apart.
  const char *node = getenv("PHYLAX_BUS_NODE");
  if (node != NULL) uid[PHYLAX_UID_SIZE - 1] ^= (uint8_t)atoi(node);
#endif
}

// Seed fingerprint, cached per device id (a wipe gives a new id).
static struct {
  bool valid;
  uint8_t uid[PHYLAX_UID_SIZE];
  uint32_t fingerprint;
} fp_cache;

static bool my_fingerprint(const uint8_t uid[PHYLAX_UID_SIZE], uint32_t *fp) {
  if (fp_cache.valid && memcmp(fp_cache.uid, uid, PHYLAX_UID_SIZE) == 0) {
    *fp = fp_cache.fingerprint;
    return true;
  }
  // Derived straight from the mnemonic with an empty passphrase, so it never
  // prompts the user. Slow the first time on real hardware (PBKDF2), then
  // cached.
  static char mnemonic[MAX_MNEMONIC_LEN + 1];
  static uint8_t seed[64];
  static HDNode node;
  bool ok = false;
  if (config_getMnemonic(mnemonic, sizeof(mnemonic))) {
    mnemonic_to_seed(mnemonic, "", seed, NULL);
    if (hdnode_from_seed(seed, sizeof(seed), SECP256K1_NAME, &node) == 1) {
      hdnode_fill_public_key(&node);
      fp_cache.fingerprint = hdnode_fingerprint(&node);
      memcpy(fp_cache.uid, uid, PHYLAX_UID_SIZE);
      fp_cache.valid = true;
      *fp = fp_cache.fingerprint;
      ok = true;
    }
  }
  memzero(mnemonic, sizeof(mnemonic));
  memzero(seed, sizeof(seed));
  memzero(&node, sizeof(node));
  return ok;
}

void phylax_my_hello(PhylaxHello *h) {
  memzero(h, sizeof(*h));
  my_uid(h->uid);
  h->kind = phylax_role() == PHYLAX_ROLE_MAIN ? PHYLAX_KIND_MAIN
                                               : PHYLAX_KIND_KEY;
  h->version[0] = VERSION_MAJOR;
  h->version[1] = VERSION_MINOR;
  h->version[2] = VERSION_PATCH;
  if (!config_isInitialized()) return;
  h->flags |= PHYLAX_HELLO_INITIALIZED;
  if (!session_isUnlocked()) {
    h->flags |= PHYLAX_HELLO_LOCKED;
    return;
  }
  if (my_fingerprint(h->uid, &h->fingerprint)) {
    h->flags |= PHYLAX_HELLO_HAS_FP;
  }
}

#if EMULATOR
bool phylax_emu_autoconfirm(void) {
  static int on = -1;
  if (on < 0) {
    const char *v = getenv("PHYLAX_EMU_AUTOCONFIRM");
    on = (v != NULL && strcmp(v, "1") == 0) ? 1 : 0;
  }
  return on == 1;
}
#endif

// ------------------------------------------------------------ sending

static uint8_t tx_seq = 0;

bool phylax_bus_send(uint8_t dst, uint8_t type, const uint8_t *payload,
                     uint16_t len) {
  static PhylaxFrame f;
  static uint8_t wire[PHYLAX_FRAME_MAX];
  if (len > PHYLAX_MAX_PAYLOAD) return false;
  f.dst = dst;
  f.src = phylax_my_addr();
  f.type = type;
  f.seq = tx_seq++;
  f.flags = 0;
  f.len = len;
  if (len) memcpy(f.payload, payload, len);
  size_t n = phylax_frame_encode(&f, wire, sizeof(wire));
  return n > 0 && phylax_transport_send(wire, n);
}

bool phylax_bus_send_trezor_msg(uint8_t dst, uint16_t msg_id,
                                const uint8_t *data, uint32_t len) {
  uint8_t pkt[USB_PACKET_SIZE];
  uint32_t pos = 0;
  // First packet: '?' '#' '#' id(2, big-endian) len(4, big-endian) data...
  memset(pkt, 0, sizeof(pkt));
  pkt[0] = '?';
  pkt[1] = '#';
  pkt[2] = '#';
  pkt[3] = msg_id >> 8;
  pkt[4] = msg_id & 0xFF;
  pkt[5] = (len >> 24) & 0xFF;
  pkt[6] = (len >> 16) & 0xFF;
  pkt[7] = (len >> 8) & 0xFF;
  pkt[8] = len & 0xFF;
  uint32_t chunk = len < USB_PACKET_SIZE - 9 ? len : USB_PACKET_SIZE - 9;
  if (chunk) memcpy(pkt + 9, data, chunk);
  pos = chunk;
  if (!phylax_bus_send(dst, PHYLAX_MSG_TREZOR, pkt, sizeof(pkt))) return false;
  // Continuation packets: '?' + 63 bytes.
  while (pos < len) {
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = '?';
    chunk = len - pos < USB_PACKET_SIZE - 1 ? len - pos : USB_PACKET_SIZE - 1;
    memcpy(pkt + 1, data + pos, chunk);
    pos += chunk;
    if (!phylax_bus_send(dst, PHYLAX_MSG_TREZOR, pkt, sizeof(pkt))) {
      return false;
    }
  }
  return true;
}

// ------------------------------------------------------------ key block

static bool reply_via_bus = false;
static uint8_t reply_to = PHYLAX_ADDR_MAIN;

bool phylax_reply_via_bus(void) { return reply_via_bus; }

void phylax_note_host_input(void) { reply_via_bus = false; }

void phylax_bus_send_reply_packet(const uint8_t *packet) {
  phylax_bus_send(reply_to, PHYLAX_MSG_TREZOR, packet, USB_PACKET_SIZE);
}

// HELLO is sent after a short delay picked from the block's id, so blocks
// that hear the same DISCOVER do not all answer at the same moment.
#define HELLO_SLOT_MS 3
static bool hello_pending = false;
static uint32_t hello_at_ms = 0;

static void key_on_discover(void) {
  uint8_t uid[PHYLAX_UID_SIZE];
  my_uid(uid);
  hello_pending = true;
  hello_at_ms = timer_ms() + (uid[PHYLAX_UID_SIZE - 1] % 16) * HELLO_SLOT_MS;
}

static void key_send_hello(void) {
  PhylaxHello h;
  uint8_t payload[PHYLAX_HELLO_SIZE];
  phylax_my_hello(&h);
  phylax_hello_encode(&h, payload);
  phylax_bus_send(PHYLAX_ADDR_MAIN, PHYLAX_MSG_HELLO, payload,
                  sizeof(payload));
}

static void key_on_assign(const uint8_t *payload, uint16_t len) {
  if (len < PHYLAX_UID_SIZE + 1) return;
  uint8_t uid[PHYLAX_UID_SIZE];
  my_uid(uid);
  if (memcmp(uid, payload, PHYLAX_UID_SIZE) != 0) return;  // someone else
  my_addr = payload[PHYLAX_UID_SIZE];
}

// ------------------------------------------------------------ receiving

// Main block: reassemble Trezor messages coming back from the peer block.
static uint8_t peer = PHYLAX_ADDR_NONE;
static PhylaxTrezorHandler peer_handler = NULL;

static struct {
  bool reading;
  uint16_t id;
  uint32_t size;
  uint32_t pos;
  uint8_t data[4096];
} rx_msg;

static void main_rx_trezor_packet(const uint8_t *p) {
  if (!rx_msg.reading) {
    if (p[0] != '?' || p[1] != '#' || p[2] != '#') return;
    rx_msg.id = ((uint16_t)p[3] << 8) | p[4];
    rx_msg.size = ((uint32_t)p[5] << 24) | ((uint32_t)p[6] << 16) |
                  ((uint32_t)p[7] << 8) | p[8];
    if (rx_msg.size > sizeof(rx_msg.data)) {
      return;  // too big: dropped, the conversation will time out
    }
    uint32_t chunk = rx_msg.size < USB_PACKET_SIZE - 9
                         ? rx_msg.size
                         : USB_PACKET_SIZE - 9;
    memcpy(rx_msg.data, p + 9, chunk);
    rx_msg.pos = chunk;
    rx_msg.reading = true;
  } else {
    if (p[0] != '?') {
      rx_msg.reading = false;
      return;
    }
    uint32_t chunk = rx_msg.size - rx_msg.pos < USB_PACKET_SIZE - 1
                         ? rx_msg.size - rx_msg.pos
                         : USB_PACKET_SIZE - 1;
    memcpy(rx_msg.data + rx_msg.pos, p + 1, chunk);
    rx_msg.pos += chunk;
  }
  if (rx_msg.pos >= rx_msg.size) {
    rx_msg.reading = false;
    if (peer_handler != NULL) {
      peer_handler(rx_msg.id, rx_msg.data, rx_msg.size);
    }
  }
}

void phylax_bus_set_trezor_peer(uint8_t addr, PhylaxTrezorHandler handler) {
  peer = addr;
  peer_handler = addr == PHYLAX_ADDR_NONE ? NULL : handler;
  rx_msg.reading = false;
}

uint8_t phylax_bus_trezor_peer(void) { return peer; }

static void main_on_frame(const PhylaxFrame *f) {
  // Any frame counts as a sign of life.
  phylax_stack_note_seen(f->src);
  if (f->src != PHYLAX_ADDR_NONE && f->src == phylax_relay_target()) {
    phylax_relay_note_key_alive();
  }
  switch (f->type) {
    case PHYLAX_MSG_HELLO: {
      PhylaxHello h;
      if (phylax_hello_decode(f->payload, f->len, &h)) {
        phylax_stack_on_hello(f->src, &h);
      }
      break;
    }
    case PHYLAX_MSG_TREZOR:
      if (f->len == USB_PACKET_SIZE && f->src != PHYLAX_ADDR_NONE &&
          f->src == peer) {
        main_rx_trezor_packet(f->payload);
      }
      break;
    case PHYLAX_MSG_STORE_DATA:
    case PHYLAX_MSG_STORE_ACK:
      phylax_wallet_on_store_reply(f);
      break;
    default:
      break;
  }
}

// Key blocks.
static void block_on_frame(const PhylaxFrame *f) {
  switch (f->type) {
    case PHYLAX_MSG_DISCOVER:
      key_on_discover();
      break;
    case PHYLAX_MSG_ASSIGN:
      key_on_assign(f->payload, f->len);
      break;
    case PHYLAX_MSG_STORE_GET:
    case PHYLAX_MSG_STORE_PUT:
      // Every key block keeps a copy of the saved wallets.
      if (my_addr != PHYLAX_ADDR_NONE) phylax_storage_on_request(f);
      break;
    case PHYLAX_MSG_LOCAL_UI:
      if (f->len >= 1 && my_addr != PHYLAX_ADDR_NONE) {
        phylax_set_local_ui(f->payload[0] != 0);
      }
      break;
    case PHYLAX_MSG_TREZOR:
      if (f->len != USB_PACKET_SIZE || my_addr == PHYLAX_ADDR_NONE) {
        break;
      }
      reply_via_bus = true;
      reply_to = f->src;
      // A key block may never get an Initialize (no USB): open a session
      // when a new message starts and none is active, as Initialize would.
      if (f->payload[0] == '?' && f->payload[1] == '#' &&
          f->payload[2] == '#' && !session_isActive()) {
        session_startSession(NULL);
      }
      usbDeliverPacket(f->payload);
      break;
    default:
      break;
  }
}

static void on_frame(const PhylaxFrame *f) {
  uint8_t me = phylax_my_addr();
  bool for_me = f->dst == PHYLAX_ADDR_BROADCAST ||
                (me != PHYLAX_ADDR_NONE && f->dst == me);
  if (!for_me) return;

  if (f->type == PHYLAX_MSG_PING) {
    // Answered here, below the Trezor message layer, so a key block that is
    // waiting for a button press still answers.
    phylax_bus_send(f->src, PHYLAX_MSG_PONG, f->payload, f->len);
  }

  if (phylax_role() == PHYLAX_ROLE_MAIN) {
    main_on_frame(f);
  } else if (phylax_role() != PHYLAX_ROLE_STOCK) {
    block_on_frame(f);
  }
}

void phylax_bus_rx(const uint8_t *data, size_t len) {
  static PhylaxDecoder dec;
  static bool init = false;
  if (!init) {
    phylax_decoder_reset(&dec);
    init = true;
  }
  for (size_t i = 0; i < len; i++) {
    if (phylax_decoder_feed(&dec, data[i])) {
      on_frame(&dec.frame);
    }
  }
}

void phylax_poll(void) {
  switch (phylax_role()) {
    case PHYLAX_ROLE_MAIN:
      phylax_stack_poll();
      phylax_relay_poll();
      phylax_wallet_poll();
      break;
    case PHYLAX_ROLE_KEY:
      if (hello_pending && (int32_t)(timer_ms() - hello_at_ms) >= 0) {
        hello_pending = false;
        key_send_hello();
      }
      break;
    default:
      break;
  }
}
