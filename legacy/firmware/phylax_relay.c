/*
 * PHYLAX Blocks - main block: SignTx relay to the key blocks.
 * See phylax_relay.h for the two-pass flow.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "phylax_relay.h"

#include <string.h>

#include "crypto.h"
#include "ecdsa.h"
#include "fsm.h"
#include "gettext.h"
#include "layout2.h"
#include "memzero.h"
#include "messages.h"
#include "messages.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "phylax_bus.h"
#include "phylax_stack.h"
#include "timer.h"

static PhylaxRelayState state = PHYLAX_RELAY_IDLE;

// Key blocks to relay to, in address order, and which one is signing now.
static uint8_t targets[PHYLAX_MAX_KEY_BLOCKS];
static uint8_t target_count = 0;
static uint8_t current = 0;

static SignTx signtx;  // the host's SignTx, sent to every key block
static uint8_t sigs[PHYLAX_MAX_KEY_BLOCKS][PHYLAX_MAX_INPUTS][PHYLAX_MAX_SIG];
static uint8_t sig_len[PHYLAX_MAX_KEY_BLOCKS][PHYLAX_MAX_INPUTS];

static uint32_t key_last_seen_ms = 0;
static uint32_t last_ping_ms = 0;

PhylaxRelayState phylax_relay_state(void) { return state; }

static uint8_t target(void) {
  return current < target_count ? targets[current] : PHYLAX_ADDR_NONE;
}

uint8_t phylax_relay_target(void) {
  return state == PHYLAX_RELAY_PASS1 ? target() : PHYLAX_ADDR_NONE;
}

void phylax_relay_reset(void) {
  if (state != PHYLAX_RELAY_IDLE) {
    phylax_bus_set_trezor_peer(PHYLAX_ADDR_NONE, NULL);
  }
  state = PHYLAX_RELAY_IDLE;
  target_count = 0;
  current = 0;
  memzero(sigs, sizeof(sigs));
  memzero(sig_len, sizeof(sig_len));
}

static void show_waiting(void) {
  static char line[24];
  strlcpy(line, _("on key block   ."), sizeof(line));
  // "on key block 2/2." - which key block of how many
  line[13] = '1' + current;
  line[14] = '/';
  line[15] = '0' + target_count;
  line[16] = '.';
  line[17] = '\0';
  layoutDialog(&bmp_icon_info, NULL, NULL, NULL, _("PHYLAX multisig"),
               _("Check and confirm"), line, NULL, _("Then confirm here."),
               NULL);
}

static void show_pin_prompt(void) {
  static char line[24];
  strlcpy(line, _("of key block  /  on"), sizeof(line));
  // "of key block 1/2 on"
  line[13] = '1' + current;
  line[15] = '0' + target_count;
  layoutDialog(&bmp_icon_info, NULL, NULL, NULL, _("PHYLAX multisig"),
               _("Enter the PIN"), line, _("the PC. Its keypad is"),
               _("on its own screen."), NULL);
}

// Send the saved SignTx to the current key block.
static bool send_signtx(void) {
  static uint8_t buf[SignTx_size];
  pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
  if (!pb_encode(&stream, SignTx_fields, &signtx)) return false;
  key_last_seen_ms = last_ping_ms = timer_ms();
  phylax_bus_set_trezor_peer(target(), phylax_relay_on_key_message);
  show_waiting();
  return phylax_bus_send_trezor_msg(target(), MessageType_MessageType_SignTx,
                                    buf, stream.bytes_written);
}

bool phylax_relay_begin(const SignTx *msg) {
  phylax_relay_reset();
  const PhylaxStackEntry *e;
  for (size_t i = 0; target_count < PHYLAX_MAX_KEY_BLOCKS &&
                     (e = phylax_stack_get(i)) != NULL;
       i++) {
    if (e->hello.kind == PHYLAX_KIND_KEY &&
        (e->hello.flags & PHYLAX_HELLO_INITIALIZED)) {
      targets[target_count++] = e->addr;
    }
  }
  if (target_count == 0) return false;
  memcpy(&signtx, msg, sizeof(SignTx));
  state = PHYLAX_RELAY_PASS1;
  if (!send_signtx()) {
    phylax_relay_reset();
    return false;
  }
  return true;
}

// The current key block is done (signed, or not part of this wallet).
static void next_key_block(void) {
  current++;
  if (current < target_count) {
    if (!send_signtx()) {
      phylax_relay_on_key_error(_("Key block unreachable"));
    }
    return;
  }
  state = PHYLAX_RELAY_PASS2;
  phylax_bus_set_trezor_peer(PHYLAX_ADDR_NONE, NULL);
  fsm_phylaxStartOwnSigning();  // pass 2: the main block signs
}

bool phylax_relay_forward_txack(void) {
  if (state != PHYLAX_RELAY_PASS1) return false;
  uint16_t id = 0;
  const uint8_t *data = NULL;
  uint32_t len = 0;
  msg_get_last_in(&id, &data, &len);
  if (!phylax_bus_send_trezor_msg(target(), id, data, len)) {
    phylax_relay_on_key_error(_("Key block unreachable"));
  }
  return true;
}

void phylax_relay_note_key_alive(void) { key_last_seen_ms = timer_ms(); }

void phylax_relay_poll(void) {
  if (state != PHYLAX_RELAY_PASS1) return;
  uint32_t now = timer_ms();
  if (now - key_last_seen_ms >= PHYLAX_KEY_TIMEOUT_MS) {
    // Best effort: if the block comes back, it should not keep signing.
    phylax_bus_send_trezor_msg(target(), MessageType_MessageType_Cancel, NULL,
                               0);
    phylax_relay_on_key_error(_("Key block disconnected"));
    return;
  }
  if (now - key_last_seen_ms >= PHYLAX_PING_INTERVAL_MS &&
      now - last_ping_ms >= PHYLAX_PING_INTERVAL_MS) {
    last_ping_ms = now;
    phylax_bus_send(target(), PHYLAX_MSG_PING, NULL, 0);
  }
}

void phylax_relay_fill_signatures(const CoinInfo *coin,
                                  MultisigRedeemScriptType *ms,
                                  uint32_t input_index, const uint8_t *digest) {
  if (state != PHYLAX_RELAY_PASS2 || input_index >= PHYLAX_MAX_INPUTS) return;
  uint32_t count = cryptoMultisigPubkeyCount(ms);
  if (count > sizeof(ms->signatures) / sizeof(ms->signatures[0])) return;
  while (ms->signatures_count < count) {
    ms->signatures[ms->signatures_count++].size = 0;
  }
  for (int k = 0; k < target_count; k++) {
    uint8_t len = sig_len[k][input_index];
    if (len == 0) continue;
    uint8_t raw[64];
    if (ecdsa_sig_from_der(sigs[k][input_index], len, raw) != 0) continue;
    for (uint32_t i = 0; i < count; i++) {
      if (ms->signatures[i].size != 0) continue;  // filled already
      const HDNode *node = cryptoMultisigPubkey(coin, ms, i);
      if (node == NULL) continue;
      if (ecdsa_verify_digest(coin->curve->params, node->public_key, raw,
                              digest) == 0) {
        memcpy(ms->signatures[i].bytes, sigs[k][input_index], len);
        ms->signatures[i].size = len;
        break;
      }
    }
  }
}

void phylax_relay_on_key_error(const char *text) {
  if (state == PHYLAX_RELAY_IDLE) return;
  phylax_relay_reset();
  fsm_sendFailure(FailureType_Failure_ProcessError, text);
  layoutHome();
}

static void send_button_ack(void) {
  // ButtonAck has no fields: an empty body.
  phylax_bus_send_trezor_msg(target(), MessageType_MessageType_ButtonAck, NULL,
                             0);
}

void phylax_relay_on_key_message(uint16_t msg_id, const uint8_t *data,
                                 uint32_t len) {
  if (state != PHYLAX_RELAY_PASS1) return;  // not waiting for a key block

  pb_istream_t stream = pb_istream_from_buffer(data, len);

  switch (msg_id) {
    case MessageType_MessageType_TxRequest: {
      static TxRequest req;
      memzero(&req, sizeof(req));
      if (!pb_decode(&stream, TxRequest_fields, &req)) {
        phylax_relay_on_key_error(_("Bad TxRequest from key block"));
        return;
      }
      // Keep the key block's signature; never pass its serialized
      // transaction to the host (the host builds the tx from pass 2 only).
      if (req.has_serialized && req.serialized.has_signature_index &&
          req.serialized.has_signature &&
          req.serialized.signature_index < PHYLAX_MAX_INPUTS &&
          req.serialized.signature.size <= PHYLAX_MAX_SIG) {
        uint32_t i = req.serialized.signature_index;
        memcpy(sigs[current][i], req.serialized.signature.bytes,
               req.serialized.signature.size);
        sig_len[current][i] = req.serialized.signature.size;
      }
      if (req.request_type == RequestType_TXFINISHED) {
        next_key_block();
        return;
      }
      req.has_serialized = false;
      memzero(&req.serialized, sizeof(req.serialized));
      msg_write(MessageType_MessageType_TxRequest, &req);
      return;
    }
    case MessageType_MessageType_ButtonRequest:
      // The user confirms on the key block's own screen; the protocol only
      // needs the ButtonAck, which the main block sends itself.
      show_waiting();
      send_button_ack();
      return;
    case MessageType_MessageType_PublicKey:
      return;  // late answer to an interrupted xpub fetch: not for us
    case MessageType_MessageType_Failure: {
      static Failure fail;
      memzero(&fail, sizeof(fail));
      pb_decode(&stream, Failure_fields, &fail);
      // A key block whose key is not in the input's multisig script rejects
      // it: early in phase 1 ("Failed to derive scriptPubKey", before any
      // confirmation screen) or, in theory, when signing ("Pubkey not
      // found..."). That block is not part of this wallet: sign without it.
      if (fail.has_message &&
          (strstr(fail.message, "Failed to derive scriptPubKey") != NULL ||
           strstr(fail.message, "Pubkey not found in multisig script") !=
               NULL)) {
        memzero(sig_len[current], sizeof(sig_len[current]));
        next_key_block();
        return;
      }
      phylax_relay_reset();
      msg_write(MessageType_MessageType_Failure, &fail);
      layoutHome();
      return;
    }
    case MessageType_MessageType_PinMatrixRequest:
      show_pin_prompt();
      msg_write_raw(msg_id, data, len);  // the PC shows its PIN grid
      return;
    default:
      // Anything else the key block asks (e.g. a passphrase) goes to the PC
      // as is; the PC's answer comes back through
      // phylax_intercept_host_message.
      msg_write_raw(msg_id, data, len);
      return;
  }
}

// ------------------------------------------------------------ select mode

static uint8_t selected = PHYLAX_ADDR_NONE;

uint8_t phylax_selected_block(void) { return selected; }

static void on_selected_message(uint16_t msg_id, const uint8_t *data,
                                uint32_t len) {
  if (selected == PHYLAX_ADDR_NONE) return;
  msg_write_raw(msg_id, data, len);  // everything goes to the PC unchanged
}

static bool in_stack(uint8_t addr) {
  const PhylaxStackEntry *e;
  for (size_t i = 0; (e = phylax_stack_get(i)) != NULL; i++) {
    if (e->addr == addr && e->hello.kind == PHYLAX_KIND_KEY) return true;
  }
  return false;
}

bool phylax_select_block(uint8_t addr) {
  if (addr == PHYLAX_ADDR_NONE || addr == PHYLAX_ADDR_MAIN) {
    if (selected != PHYLAX_ADDR_NONE) {
      phylax_bus_set_trezor_peer(PHYLAX_ADDR_NONE, NULL);
    }
    selected = PHYLAX_ADDR_NONE;
    return true;
  }
  if (state != PHYLAX_RELAY_IDLE || !in_stack(addr)) return false;
  selected = addr;
  phylax_bus_set_trezor_peer(addr, on_selected_message);
  static char line[24];
  strlcpy(line, _("Key block 0x  "), sizeof(line));
  static const char hexd[] = "0123456789ABCDEF";
  line[12] = hexd[addr >> 4];
  line[13] = hexd[addr & 0xF];
  layoutDialog(&bmp_icon_info, NULL, NULL, NULL, _("PHYLAX"),
               _("The PC is talking to"), line, _("through this block."),
               _("Use the key block's"), _("own screen and buttons."));
  return true;
}

bool phylax_passthrough_active(void) {
  return phylax_role() == PHYLAX_ROLE_MAIN &&
         (selected != PHYLAX_ADDR_NONE || state == PHYLAX_RELAY_PASS1);
}

bool phylax_intercept_host_message(uint16_t msg_id, const uint8_t *data,
                                   uint32_t len) {
  if (phylax_role() != PHYLAX_ROLE_MAIN) return false;

  if (selected != PHYLAX_ADDR_NONE &&
      msg_id != MessageType_MessageType_PhylaxSelectBlock) {
    if (!in_stack(selected)) {
      phylax_select_block(PHYLAX_ADDR_MAIN);
      fsm_sendFailure(FailureType_Failure_ProcessError,
                      _("Key block disconnected"));
      layoutHome();
      return true;
    }
    phylax_bus_send_trezor_msg(selected, msg_id, data, len);
    return true;
  }

  if (state == PHYLAX_RELAY_PASS1) {
    if (msg_id == MessageType_MessageType_Initialize) {
      // The PC starts over: stop the key block too, then handle it here.
      phylax_bus_send_trezor_msg(target(), MessageType_MessageType_Cancel,
                                 NULL, 0);
      phylax_relay_reset();
      return false;
    }
    // TxAck, PinMatrixAck, PassphraseAck, Cancel, ...: for the key block.
    if (!phylax_bus_send_trezor_msg(target(), msg_id, data, len)) {
      phylax_relay_on_key_error(_("Key block unreachable"));
    }
    return true;
  }
  return false;
}
