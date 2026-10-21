/*
 * PHYLAX Blocks - main block: SignTx relay to a key block.
 *
 * Pass 1: the host's SignTx and every TxAck are forwarded unchanged to the
 *         key block; its TxRequests go back to the host with the
 *         "serialized" part removed (the key block's signature is kept here).
 *         ButtonRequests from the key block are answered by the main block;
 *         the user confirms on the key block's own screen.
 * Pass 2: when the key block reports TXFINISHED, the main block starts its
 *         own signing of the same SignTx. Every input the host sends is given
 *         the key block's signature in multisig.signatures, so the main block
 *         returns a fully signed transaction.
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
  PHYLAX_RELAY_PASS1,  // key block is signing, main block relays
  PHYLAX_RELAY_PASS2,  // main block signs with the key block's signatures
} PhylaxRelayState;

PhylaxRelayState phylax_relay_state(void);
// Bus address of the key block being relayed to (pass 1), else ADDR_NONE.
uint8_t phylax_relay_target(void);
void phylax_relay_reset(void);

// Called by fsm_msgSignTx on the main block. Forwards the SignTx the host
// just sent to the first key block found by discovery. Returns false if
// there is no key block (then the caller reports the failure).
bool phylax_relay_begin(void);

// Called by fsm_msgTxAck. Returns true if the TxAck was forwarded to the key
// block (pass 1) and must not be processed locally.
bool phylax_relay_forward_txack(void);

// Called often from the main loop: keeps the key block alive with PINGs and
// cancels signing if it has been silent for PHYLAX_KEY_TIMEOUT_MS.
void phylax_relay_poll(void);
// The bus layer saw a frame from the key block.
void phylax_relay_note_key_alive(void);

// The key block's signature for input `index`, or NULL.
const uint8_t *phylax_relay_key_sig(uint32_t index, size_t *len);

// From the bus layer: a complete Trezor message from the key block.
void phylax_relay_on_key_message(uint16_t msg_id, const uint8_t *data,
                                 uint32_t len);
void phylax_relay_on_key_error(const char *text);

// Provided by fsm.c: start the main block's own signing (pass 2).
void fsm_phylaxStartOwnSigning(void);

#endif
