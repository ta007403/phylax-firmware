/*
 * PHYLAX Blocks - saved wallets, a copy on every block.
 *
 * A wallet is wsh(sortedmulti(n, ...)), n-of-n, one key per block (main block
 * included). Every block keeps up to PHYLAX_WALLET_SLOTS records in its own
 * flash, the same on all blocks; nothing in them is secret (fingerprints and
 * xpubs). A wallet's identity is its set of fingerprints; when copies
 * disagree, the higher generation wins.
 *
 * The main block reads the copy of every block that joins the stack, merges
 * it with its own, writes the result back wherever a copy is missing or
 * older, compares the seed fingerprints in the stack with the wallets, and:
 *   - shows "<name> ready" when a saved wallet's blocks are all present,
 *   - shows "Attach key block <fp>" when a saved wallet misses one block,
 *   - asks "New n-of-n wallet?" for a new combination of blocks, fetches the
 *     key blocks' xpubs over the bus and saves the record once confirmed.
 *
 * Records travel over the bus in chunks of at most PHYLAX_STORE_CHUNK bytes,
 * so a block could also write them page by page (e.g. to an EEPROM).
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
#define PHYLAX_WALLET_RECORD_SIZE 480  // 3-of-3 record = 477 bytes

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
  // Deleted from the PC: kept as a hidden record (same identity, higher
  // generation) so a block that missed the delete cannot bring it back.
  bool deleted;
  uint32_t generation;  // raised on every change; newest copy wins
  char name[PHYLAX_WALLET_NAME_LEN + 1];
  uint8_t n;  // number of keys = threshold (n-of-n)
  PhylaxKeyRec keys[PHYLAX_WALLET_MAX_KEYS];  // sorted by fingerprint
} PhylaxWalletRec;

// --- record codec (shared by main and storage blocks) ---
size_t phylax_wallet_encode(const PhylaxWalletRec *w,
                            uint8_t out[PHYLAX_WALLET_RECORD_SIZE]);
bool phylax_wallet_decode(const uint8_t *in, size_t len, PhylaxWalletRec *w);

// --- this block's own copy (flash) ---
bool phylax_store_get(int slot, uint8_t *buf, uint16_t *len);
bool phylax_store_put(int slot, const uint8_t *buf, uint16_t len);  // 0: delete

// --- main block ---
void phylax_wallet_poll(void);
void phylax_wallet_on_store_reply(const PhylaxFrame *f);
// The wallets are known: own copy loaded and every block in the stack read.
bool phylax_wallet_ready(void);
// Saved wallet in `slot`, or NULL. `complete`: all its blocks are present.
const PhylaxWalletRec *phylax_wallet_get(int slot, bool *complete);
// Main loop, after the buttons were read: the "New wallet?" prompt.
void phylax_wallet_ui_poll(void);

// --- changes from the PC (fsm_msg_phylax.h confirms them on screen) ---
#define PHYLAX_WALLET_NAME_MAX 15  // characters a user may choose
// NULL if `slot` may be renamed to `name`, else why not.
const char *phylax_wallet_check_rename(int slot, const char *name);
// NULL if `slot` may be deleted, else why not.
const char *phylax_wallet_check_delete(int slot);
// Apply (after the user confirmed): raises the generation, the sync writes
// it to every block.
void phylax_wallet_rename(int slot, const char *name);
void phylax_wallet_delete(int slot);

// --- every other block: let the main block read and write its copy ---
void phylax_storage_on_request(const PhylaxFrame *f);

#endif
