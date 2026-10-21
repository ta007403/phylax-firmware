/*
 * PHYLAX Blocks - messages handled by the main block.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

static void phylax_fill_block_info(PhylaxBlockInfo *info, uint8_t addr,
                                   const PhylaxHello *h) {
  info->address = addr;
  info->kind = (PhylaxBlockKind)h->kind;
  info->unique_id.size = PHYLAX_UID_SIZE;
  memcpy(info->unique_id.bytes, h->uid, PHYLAX_UID_SIZE);
  info->has_initialized = true;
  info->initialized = (h->flags & PHYLAX_HELLO_INITIALIZED) != 0;
  info->has_locked = true;
  info->locked = (h->flags & PHYLAX_HELLO_LOCKED) != 0;
  if (h->flags & PHYLAX_HELLO_HAS_FP) {
    info->has_fingerprint = true;
    info->fingerprint = h->fingerprint;
  }
  info->has_major_version = true;
  info->major_version = h->version[0];
  info->has_minor_version = true;
  info->minor_version = h->version[1];
  info->has_patch_version = true;
  info->patch_version = h->version[2];
}

void fsm_msgPhylaxGetBlocks(const PhylaxGetBlocks *msg) {
  (void)msg;
  if (phylax_role() != PHYLAX_ROLE_MAIN) {
    fsm_sendFailure(FailureType_Failure_UnexpectedMessage,
                    _("Not a PHYLAX main block"));
    return;
  }

  RESP_INIT(PhylaxBlocks);
  const size_t max = sizeof(resp->blocks) / sizeof(resp->blocks[0]);

  // The main block itself first.
  PhylaxHello me;
  phylax_my_hello(&me);
  phylax_fill_block_info(&resp->blocks[resp->blocks_count++],
                         PHYLAX_ADDR_MAIN, &me);

  const PhylaxStackEntry *e;
  for (size_t i = 0;
       resp->blocks_count < max && (e = phylax_stack_get(i)) != NULL; i++) {
    phylax_fill_block_info(&resp->blocks[resp->blocks_count++], e->addr,
                           &e->hello);
  }
  msg_write(MessageType_MessageType_PhylaxBlocks, resp);
}

static void phylax_fill_xpub(HDNodeType *out, const PhylaxXpub *x) {
  out->depth = 4;  // m/48'/coin'/0'/2'
  out->fingerprint = x->parent_fingerprint;
  out->child_num = x->child_num;
  out->chain_code.size = 32;
  memcpy(out->chain_code.bytes, x->chain_code, 32);
  out->public_key.size = 33;
  memcpy(out->public_key.bytes, x->public_key, 33);
  out->has_private_key = false;
}

void fsm_msgPhylaxGetWallets(const PhylaxGetWallets *msg) {
  (void)msg;
  if (phylax_role() != PHYLAX_ROLE_MAIN) {
    fsm_sendFailure(FailureType_Failure_UnexpectedMessage,
                    _("Not a PHYLAX main block"));
    return;
  }

  RESP_INIT(PhylaxWallets);
  resp->storage_present = phylax_wallet_storage_present();
  const size_t max = sizeof(resp->wallets) / sizeof(resp->wallets[0]);
  for (int slot = 0; slot < PHYLAX_WALLET_SLOTS && resp->wallets_count < max;
       slot++) {
    bool complete = false;
    const PhylaxWalletRec *w = phylax_wallet_get(slot, &complete);
    if (w == NULL) continue;
    PhylaxWalletSummary *info = &resp->wallets[resp->wallets_count++];
    info->slot = slot;
    strlcpy(info->name, w->name, sizeof(info->name));
    info->threshold = w->n;
    info->has_complete = true;
    info->complete = complete;
    for (int k = 0; k < w->n; k++) {
      info->fingerprints[info->fingerprints_count++] = w->keys[k].fingerprint;
    }
  }
  msg_write(MessageType_MessageType_PhylaxWallets, resp);
}

void fsm_msgPhylaxGetWallet(const PhylaxGetWallet *msg) {
  if (phylax_role() != PHYLAX_ROLE_MAIN) {
    fsm_sendFailure(FailureType_Failure_UnexpectedMessage,
                    _("Not a PHYLAX main block"));
    return;
  }
  bool complete = false;
  const PhylaxWalletRec *w = phylax_wallet_get(msg->slot, &complete);
  if (w == NULL) {
    fsm_sendFailure(FailureType_Failure_DataError, _("No wallet in that slot"));
    return;
  }

  RESP_INIT(PhylaxWallet);
  resp->slot = msg->slot;
  strlcpy(resp->name, w->name, sizeof(resp->name));
  resp->threshold = w->n;
  resp->has_complete = true;
  resp->complete = complete;
  for (int k = 0; k < w->n; k++) {
    PhylaxWalletKey *out = &resp->keys[resp->keys_count++];
    out->fingerprint = w->keys[k].fingerprint;
    phylax_fill_xpub(&out->mainnet, &w->keys[k].mainnet);
    phylax_fill_xpub(&out->testnet, &w->keys[k].testnet);
  }
  msg_write(MessageType_MessageType_PhylaxWallet, resp);
}
