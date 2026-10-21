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
