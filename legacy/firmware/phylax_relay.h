/*
 * PHYLAX Blocks - main block: SignTx relay to the key blocks.
 *
 * Pass 1: for each key block in the stack, one after the other, the host's
 *         SignTx and every TxAck are forwarded unchanged to that block; its
 *         TxRequests go back to the host with the "serialized" part removed
 *         (its signatures are kept here). ButtonRequests from a key block are
 *         answered by the main block; the user confirms on the key block's
 *         own screen. A key block whose key is not in the wallet is skipped.
 * Pass 2: the main block signs the same SignTx itself. When it signs an
 *         input, every kept signature is checked against every key of the
 *         input's multisig script and placed in the slot whose key verifies
 *         it, so the main block returns a fully signed transaction.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef __PHYLAX_RELAY_H__
#define __PHYLAX_RELAY_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "coins.h"
#include "messages-bitcoin.pb.h"

// Design limit: main block + 2 key blocks = 3-of-3.
#define PHYLAX_MAX_KEY_BLOCKS 2
#define PHYLAX_MAX_INPUTS 16
#define PHYLAX_MAX_SIG 73

// A key block that sends nothing for this long is treated as removed. While
// it waits for its user's button press its bus layer still answers PINGs,
// so slow users never hit this.
#ifndef PHYLAX_KEY_TIMEOUT_MS
#define PHYLAX_KEY_TIMEOUT_MS 10000
#endif
#define PHYLAX_PING_INTERVAL_MS 1000

typedef enum {
  PHYLAX_RELAY_IDLE = 0,
  PHYLAX_RELAY_PASS1,  // a key block is signing, main block relays
  PHYLAX_RELAY_PASS2,  // main block signs with the key blocks' signatures
} PhylaxRelayState;

PhylaxRelayState phylax_relay_state(void);
void phylax_relay_reset(void);
// Bus address of the key block being relayed to (pass 1), else ADDR_NONE.
uint8_t phylax_relay_target(void);

// Called by fsm_msgSignTx on the main block with the host's SignTx. Starts
// pass 1 with the first key block. Returns false if there is no key block
// (then the caller reports the failure).
bool phylax_relay_begin(const SignTx *msg);

// Called by fsm_msgTxAck. Returns true if the TxAck was forwarded to a key
// block (pass 1) and must not be processed locally.
bool phylax_relay_forward_txack(void);

// Called often from the main loop: keeps the key block alive with PINGs and
// cancels signing if it has been silent for PHYLAX_KEY_TIMEOUT_MS.
void phylax_relay_poll(void);
// The bus layer saw a frame from the key block being relayed to.
void phylax_relay_note_key_alive(void);

// Called by signing.c right after the main block put its own signature into
// a multisig input (pass 2): adds the key blocks' signatures for that input
// to the slots whose keys verify them against `digest`.
void phylax_relay_fill_signatures(const CoinInfo *coin,
                                  MultisigRedeemScriptType *multisig,
                                  uint32_t input_index, const uint8_t *digest);

// From the bus layer: a complete Trezor message from the key block.
void phylax_relay_on_key_message(uint16_t msg_id, const uint8_t *data,
                                 uint32_t len);
void phylax_relay_on_key_error(const char *text);

// Provided by fsm.c: start the main block's own signing (pass 2) of the
// SignTx given to phylax_relay_begin.
void fsm_phylaxStartOwnSigning(void);

#endif
