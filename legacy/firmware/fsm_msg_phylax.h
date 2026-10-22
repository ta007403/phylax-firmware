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
  resp->storage_present = phylax_wallet_ready();
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

void fsm_msgPhylaxSelectBlock(const PhylaxSelectBlock *msg) {
  if (phylax_role() != PHYLAX_ROLE_MAIN) {
    fsm_sendFailure(FailureType_Failure_UnexpectedMessage,
                    _("Not a PHYLAX main block"));
    return;
  }
  if (msg->address > 0xFF || !phylax_select_block((uint8_t)msg->address)) {
    fsm_sendFailure(FailureType_Failure_DataError,
                    _("No key block at that address"));
    return;
  }
  if (phylax_selected_block() == PHYLAX_ADDR_NONE) {
    layoutHome();
    fsm_sendSuccess(_("Main block selected"));
  } else {
    fsm_sendSuccess(_("Key block selected"));
  }
}

// Shared by rename and delete: the main block's screen is about to show a
// confirmation. NULL if it may, else why not.
static const char *phylax_screen_for_pc(void) {
  if (phylax_setup_running()) return _("Busy setting up a key block");
  if (phylax_relay_state() != PHYLAX_RELAY_IDLE) return _("Busy signing");
  phylax_setup_abort();  // a "Set it up?" prompt comes back by itself
  return NULL;
}

void fsm_msgPhylaxSetWalletName(const PhylaxSetWalletName *msg) {
  if (phylax_role() != PHYLAX_ROLE_MAIN) {
    fsm_sendFailure(FailureType_Failure_UnexpectedMessage,
                    _("Not a PHYLAX main block"));
    return;
  }
  int slot = msg->slot < PHYLAX_WALLET_SLOTS ? (int)msg->slot : -1;
  const char *err = phylax_wallet_check_rename(slot, msg->name);
  if (err == NULL) err = phylax_screen_for_pc();
  if (err) {
    fsm_sendFailure(FailureType_Failure_DataError, err);
    return;
  }
  static char old_name[PHYLAX_WALLET_NAME_LEN + 1];
  strlcpy(old_name, phylax_wallet_get(slot, NULL)->name, sizeof(old_name));

  layoutDialogSwipe(&bmp_icon_question, _("Cancel"), _("Rename"), NULL,
                    _("Rename wallet"), old_name, _("to"), msg->name, NULL,
                    NULL);
  if (!protectButton(ButtonRequestType_ButtonRequest_ProtectCall, false)) {
    fsm_sendFailure(FailureType_Failure_ActionCancelled, NULL);
    layoutHome();
    return;
  }
  // The wallets may have changed while the user was reading.
  err = phylax_wallet_check_rename(slot, msg->name);
  if (err) {
    fsm_sendFailure(FailureType_Failure_DataError, err);
    layoutHome();
    return;
  }
  phylax_wallet_rename(slot, msg->name);
  fsm_sendSuccess(_("Wallet renamed"));
  layoutHome();
}

void fsm_msgPhylaxDeleteWallet(const PhylaxDeleteWallet *msg) {
  if (phylax_role() != PHYLAX_ROLE_MAIN) {
    fsm_sendFailure(FailureType_Failure_UnexpectedMessage,
                    _("Not a PHYLAX main block"));
    return;
  }
  int slot = msg->slot < PHYLAX_WALLET_SLOTS ? (int)msg->slot : -1;
  const char *err = phylax_wallet_check_delete(slot);
  if (err == NULL) err = phylax_screen_for_pc();
  if (err) {
    fsm_sendFailure(FailureType_Failure_DataError, err);
    return;
  }
  static char name[PHYLAX_WALLET_NAME_LEN + 1];
  strlcpy(name, phylax_wallet_get(slot, NULL)->name, sizeof(name));

  layoutDialogSwipe(&bmp_icon_warning, _("Cancel"), _("Delete"), NULL,
                    _("Delete wallet"), name, _("from every block?"), NULL,
                    _("Coins stay safe: the"), _("seeds rebuild it."));
  if (!protectButton(ButtonRequestType_ButtonRequest_ProtectCall, false)) {
    fsm_sendFailure(FailureType_Failure_ActionCancelled, NULL);
    layoutHome();
    return;
  }
  err = phylax_wallet_check_delete(slot);
  if (err) {
    fsm_sendFailure(FailureType_Failure_DataError, err);
    layoutHome();
    return;
  }
  phylax_wallet_delete(slot);
  fsm_sendSuccess(_("Wallet deleted"));
  layoutHome();
}
