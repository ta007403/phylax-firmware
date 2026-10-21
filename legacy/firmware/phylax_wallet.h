/*
 * PHYLAX Blocks - wallets saved in the storage block.
 *
 * A wallet is wsh(sortedmulti(n, ...)), n-of-n, one key per block (main block
 * included). The storage block keeps up to PHYLAX_WALLET_SLOTS records in its
 * flash; nothing in them is secret (fingerprints and xpubs).
 *
 * The main block loads all records when a storage block appears, compares
 * the seed fingerprints in the stack with them, and:
 *   - shows "<name> ready" when a saved wallet's blocks are all present,
 *   - shows "Attach key block <fp>" when a saved wallet misses one block,
 *   - asks "New n-of-n wallet?" for a new combination of blocks, fetches the
 *     key blocks' xpubs over the bus and saves the record once confirmed.
 *
 * Records travel over the bus in chunks of at most PHYLAX_STORE_CHUNK bytes,
 * so a storage block can write them page by page (e.g. to an EEPROM).
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef __PHYLAX_WALLET_H__
#define __PHYLAX_WALLET_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "phylax_bus.h"

#define PHYLAX_WALLET_SLOTS 8
#define PHYLAX_WALLET_MAX_KEYS 3  // 3-of-3 design limit
#define PHYLAX_WALLET_NAME_LEN 16
#define PHYLAX_STORE_CHUNK 128     // max bytes per bus transfer / page write
#define PHYLAX_WALLET_RECORD_SIZE 480  // 3-of-3 record = 473 bytes

// Store status codes (STORE_DATA / STORE_ACK)
#define PHYLAX_STORE_OK 0
#define PHYLAX_STORE_EMPTY 1
#define PHYLAX_STORE_ERROR 2
#define PHYLAX_STORE_BAD_RECORD 3

typedef struct {
  uint32_t parent_fingerprint;
  uint32_t child_num;
  uint8_t chain_code[32];
  uint8_t public_key[33];
} PhylaxXpub;  // depth is always 4: m/48'/coin'/0'/2'

typedef struct {
  uint32_t fingerprint;
  PhylaxXpub mainnet;  // m/48'/0'/0'/2'
  PhylaxXpub testnet;  // m/48'/1'/0'/2'
} PhylaxKeyRec;

typedef struct {
  bool used;
  char name[PHYLAX_WALLET_NAME_LEN + 1];
  uint8_t n;  // number of keys = threshold (n-of-n)
  PhylaxKeyRec keys[PHYLAX_WALLET_MAX_KEYS];  // sorted by fingerprint
} PhylaxWalletRec;

// --- record codec (shared by main and storage blocks) ---
size_t phylax_wallet_encode(const PhylaxWalletRec *w,
                            uint8_t out[PHYLAX_WALLET_RECORD_SIZE]);
bool phylax_wallet_decode(const uint8_t *in, size_t len, PhylaxWalletRec *w);

// --- main block ---
void phylax_wallet_poll(void);
void phylax_wallet_on_store_reply(const PhylaxFrame *f);
bool phylax_wallet_storage_present(void);
// Saved wallet in `slot`, or NULL. `complete`: all its blocks are present.
const PhylaxWalletRec *phylax_wallet_get(int slot, bool *complete);
// Main loop, after the buttons were read: the "New wallet?" prompt.
void phylax_wallet_ui_poll(void);

// --- storage block ---
void phylax_storage_on_request(const PhylaxFrame *f);

#endif
