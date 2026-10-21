/*
 * PHYLAX Blocks - main block: SignTx relay to a key block.
 * See phylax_relay.h for the two-pass flow.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "phylax_relay.h"

#include <string.h>

#include "fsm.h"
#include "gettext.h"
#include "layout2.h"
#include "memzero.h"
#include "messages.h"
#include "messages.pb.h"
#include "pb_decode.h"
#include "phylax_bus.h"

static PhylaxRelayState state = PHYLAX_RELAY_IDLE;
static uint8_t key_sig[PHYLAX_MAX_INPUTS][PHYLAX_MAX_SIG];
static uint8_t key_sig_len[PHYLAX_MAX_INPUTS];

PhylaxRelayState phylax_relay_state(void) { return state; }

void phylax_relay_reset(void) {
  state = PHYLAX_RELAY_IDLE;
  memzero(key_sig, sizeof(key_sig));
  memzero(key_sig_len, sizeof(key_sig_len));
}

static bool forward_last_host_message(void) {
  uint16_t id = 0;
  const uint8_t *data = NULL;
  uint32_t len = 0;
  msg_get_last_in(&id, &data, &len);
  return phylax_bus_send_trezor_msg(phylax_key_addr(), id, data, len);
}

static void show_waiting(void) {
  layoutDialog(&bmp_icon_info, NULL, NULL, NULL, _("PHYLAX 2-of-2"),
               _("Check and confirm"), _("on key block 2."), NULL,
               _("Then confirm here."), NULL);
}

bool phylax_relay_begin(void) {
  phylax_relay_reset();
  if (!forward_last_host_message()) {
    return false;
  }
  state = PHYLAX_RELAY_PASS1;
  show_waiting();
  return true;
}

bool phylax_relay_forward_txack(void) {
  if (state != PHYLAX_RELAY_PASS1) return false;
  if (!forward_last_host_message()) {
    phylax_relay_on_key_error(_("Key block unreachable"));
  }
  return true;
}

const uint8_t *phylax_relay_key_sig(uint32_t index, size_t *len) {
  if (index >= PHYLAX_MAX_INPUTS || key_sig_len[index] == 0) return NULL;
  *len = key_sig_len[index];
  return key_sig[index];
}

void phylax_relay_on_key_error(const char *text) {
  if (state == PHYLAX_RELAY_IDLE) return;
  phylax_relay_reset();
  fsm_sendFailure(FailureType_Failure_ProcessError, text);
  layoutHome();
}

static void send_button_ack(void) {
  // ButtonAck has no fields: an empty body.
  phylax_bus_send_trezor_msg(phylax_key_addr(),
                             MessageType_MessageType_ButtonAck, NULL, 0);
}

void phylax_relay_on_key_message(uint16_t msg_id, const uint8_t *data,
                                 uint32_t len) {
  if (state != PHYLAX_RELAY_PASS1) return;  // not waiting for the key block

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
        memcpy(key_sig[i], req.serialized.signature.bytes,
               req.serialized.signature.size);
        key_sig_len[i] = req.serialized.signature.size;
      }
      if (req.request_type == RequestType_TXFINISHED) {
        state = PHYLAX_RELAY_PASS2;
        fsm_phylaxStartOwnSigning();  // pass 2: the main block signs
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
    case MessageType_MessageType_Failure: {
      static Failure fail;
      memzero(&fail, sizeof(fail));
      pb_decode(&stream, Failure_fields, &fail);
      phylax_relay_reset();
      msg_write(MessageType_MessageType_Failure, &fail);
      layoutHome();
      return;
    }
    default:
      phylax_relay_on_key_error(_("Unexpected message from key block"));
      return;
  }
}
