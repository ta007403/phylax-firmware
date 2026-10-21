/*
 * PHYLAX Blocks - storage block: wallet records in flash, and the record
 * format shared with the main block. See phylax_wallet.h.
 *
 * Record layout (PHYLAX_WALLET_RECORD_SIZE bytes, little-endian):
 *   0      'P'  magic
 *   1      1    format version
 *   2      n    keys (= threshold)
 *   3      0    script type: wsh(sortedmulti)
 *   4..20       name, NUL padded (17 bytes)
 *   21..        n x key: fingerprint(4), then for mainnet and testnet:
 *               parent fingerprint(4), child number(4), chain code(32),
 *               public key(33)
 *   then        CRC-16/CCITT-FALSE over everything before it
 *
 * The records live in Trezor's own flash storage under public, writable
 * keys (readable and writable without a PIN: a storage block has no screen,
 * and fingerprints and xpubs are not secret).
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <string.h>

#include "memzero.h"
#include "phylax_wallet.h"
#include "storage.h"

#define REC_HEADER 21
#define REC_XPUB (4 + 4 + 32 + 33)
#define REC_KEY (4 + 2 * REC_XPUB)

_Static_assert(REC_HEADER + PHYLAX_WALLET_MAX_KEYS * REC_KEY + 2 <=
                   PHYLAX_WALLET_RECORD_SIZE,
               "PHYLAX_WALLET_RECORD_SIZE too small for a full record");

// Trezor storage key: app 0x02 with the public and writable flags.
#define PHYLAX_STORAGE_APP (0x02 | FLAGS_WRITE)
#define SLOT_KEY(slot) ((uint16_t)((PHYLAX_STORAGE_APP << 8) | (slot)))

static void put32(uint8_t *p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint8_t *put_xpub(uint8_t *p, const PhylaxXpub *x) {
  put32(p, x->parent_fingerprint);
  put32(p + 4, x->child_num);
  memcpy(p + 8, x->chain_code, 32);
  memcpy(p + 40, x->public_key, 33);
  return p + REC_XPUB;
}

static const uint8_t *get_xpub(const uint8_t *p, PhylaxXpub *x) {
  x->parent_fingerprint = get32(p);
  x->child_num = get32(p + 4);
  memcpy(x->chain_code, p + 8, 32);
  memcpy(x->public_key, p + 40, 33);
  return p + REC_XPUB;
}

size_t phylax_wallet_encode(const PhylaxWalletRec *w,
                            uint8_t out[PHYLAX_WALLET_RECORD_SIZE]) {
  if (w->n < 1 || w->n > PHYLAX_WALLET_MAX_KEYS) return 0;
  memzero(out, PHYLAX_WALLET_RECORD_SIZE);
  out[0] = 'P';
  out[1] = 1;
  out[2] = w->n;
  out[3] = 0;
  strlcpy((char *)out + 4, w->name, PHYLAX_WALLET_NAME_LEN + 1);
  uint8_t *p = out + REC_HEADER;
  for (int i = 0; i < w->n; i++) {
    put32(p, w->keys[i].fingerprint);
    p = put_xpub(p + 4, &w->keys[i].mainnet);
    p = put_xpub(p, &w->keys[i].testnet);
  }
  size_t body = p - out;
  uint16_t crc = phylax_crc16(out, body, 0xFFFF);
  out[body] = crc & 0xFF;
  out[body + 1] = crc >> 8;
  return body + 2;
}

bool phylax_wallet_decode(const uint8_t *in, size_t len, PhylaxWalletRec *w) {
  memzero(w, sizeof(*w));
  if (len < REC_HEADER + 2 || in[0] != 'P' || in[1] != 1 || in[3] != 0) {
    return false;
  }
  uint8_t n = in[2];
  if (n < 1 || n > PHYLAX_WALLET_MAX_KEYS) return false;
  size_t body = REC_HEADER + (size_t)n * REC_KEY;
  if (len < body + 2) return false;
  uint16_t crc = phylax_crc16(in, body, 0xFFFF);
  if ((in[body] | (in[body + 1] << 8)) != crc) return false;
  w->n = n;
  memcpy(w->name, in + 4, PHYLAX_WALLET_NAME_LEN);
  w->name[PHYLAX_WALLET_NAME_LEN] = '\0';
  const uint8_t *p = in + REC_HEADER;
  for (int i = 0; i < n; i++) {
    w->keys[i].fingerprint = get32(p);
    p = get_xpub(p + 4, &w->keys[i].mainnet);
    p = get_xpub(p, &w->keys[i].testnet);
  }
  w->used = true;
  return true;
}

// ------------------------------------------------------------ storage block

// A record being written: chunks must arrive in order, offset 0 first.
static struct {
  int slot;
  uint16_t total;
  uint16_t received;
  uint8_t data[PHYLAX_WALLET_RECORD_SIZE];
} staging = {.slot = -1};

static void reply(uint8_t dst, uint8_t type, uint8_t slot, uint16_t offset,
                  uint8_t status, const uint8_t *data, uint8_t len) {
  uint8_t payload[4 + PHYLAX_STORE_CHUNK];
  payload[0] = slot;
  payload[1] = offset & 0xFF;
  payload[2] = offset >> 8;
  payload[3] = status;
  if (len) memcpy(payload + 4, data, len);
  phylax_bus_send(dst, type, payload, 4 + len);
}

static void on_get(const PhylaxFrame *f) {
  if (f->len < 4) return;
  uint8_t slot = f->payload[0];
  uint16_t offset = f->payload[1] | (f->payload[2] << 8);
  uint8_t want = f->payload[3];
  static uint8_t record[PHYLAX_WALLET_RECORD_SIZE];
  uint16_t len = 0;
  if (slot >= PHYLAX_WALLET_SLOTS || want > PHYLAX_STORE_CHUNK) {
    reply(f->src, PHYLAX_MSG_STORE_DATA, slot, offset, PHYLAX_STORE_ERROR,
          NULL, 0);
    return;
  }
  if (sectrue !=
      storage_get(SLOT_KEY(slot), record, sizeof(record), &len)) {
    reply(f->src, PHYLAX_MSG_STORE_DATA, slot, offset, PHYLAX_STORE_EMPTY,
          NULL, 0);
    return;
  }
  if (offset >= len) {
    reply(f->src, PHYLAX_MSG_STORE_DATA, slot, offset, PHYLAX_STORE_OK, NULL,
          0);
    return;
  }
  uint8_t n = (len - offset) < want ? (len - offset) : want;
  reply(f->src, PHYLAX_MSG_STORE_DATA, slot, offset, PHYLAX_STORE_OK,
        record + offset, n);
}

static void on_put(const PhylaxFrame *f) {
  if (f->len < 5) return;
  uint8_t slot = f->payload[0];
  uint16_t offset = f->payload[1] | (f->payload[2] << 8);
  uint16_t total = f->payload[3] | (f->payload[4] << 8);
  const uint8_t *data = f->payload + 5;
  uint16_t n = f->len - 5;
  uint8_t status = PHYLAX_STORE_OK;

  if (slot >= PHYLAX_WALLET_SLOTS || n > PHYLAX_STORE_CHUNK ||
      total > PHYLAX_WALLET_RECORD_SIZE) {
    status = PHYLAX_STORE_ERROR;
  } else if (total == 0) {
    // Delete the slot.
    storage_delete(SLOT_KEY(slot));
  } else {
    if (offset == 0) {
      staging.slot = slot;
      staging.total = total;
      staging.received = 0;
    }
    if (staging.slot != slot || staging.total != total ||
        offset != staging.received || offset + n > total) {
      status = PHYLAX_STORE_ERROR;
      staging.slot = -1;
    } else {
      memcpy(staging.data + offset, data, n);
      staging.received += n;
      if (staging.received == total) {
        PhylaxWalletRec check;
        if (!phylax_wallet_decode(staging.data, total, &check)) {
          status = PHYLAX_STORE_BAD_RECORD;
        } else if (sectrue !=
                   storage_set(SLOT_KEY(slot), staging.data, total)) {
          status = PHYLAX_STORE_ERROR;
        }
        staging.slot = -1;
      }
    }
  }
  reply(f->src, PHYLAX_MSG_STORE_ACK, slot, offset, status, NULL, 0);
}

void phylax_storage_on_request(const PhylaxFrame *f) {
  if (f->type == PHYLAX_MSG_STORE_GET) on_get(f);
  if (f->type == PHYLAX_MSG_STORE_PUT) on_put(f);
}
