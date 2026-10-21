/*
 * PHYLAX Blocks - main block: which blocks are in the stack.
 * See phylax_stack.h.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "phylax_stack.h"

#include <string.h>

#include "memzero.h"
#include "timer.h"

static PhylaxStackEntry stack[PHYLAX_STACK_MAX];
static bool discover_started = false;
static uint32_t last_discover_ms = 0;

static PhylaxStackEntry *find_uid(const uint8_t *uid) {
  for (int i = 0; i < PHYLAX_STACK_MAX; i++) {
    if (stack[i].used &&
        memcmp(stack[i].hello.uid, uid, PHYLAX_UID_SIZE) == 0) {
      return &stack[i];
    }
  }
  return NULL;
}

static bool addr_taken(uint8_t addr) {
  for (int i = 0; i < PHYLAX_STACK_MAX; i++) {
    if (stack[i].used && stack[i].addr == addr) return true;
  }
  return false;
}

static void send_assign(const PhylaxStackEntry *e) {
  uint8_t payload[PHYLAX_UID_SIZE + 1];
  memcpy(payload, e->hello.uid, PHYLAX_UID_SIZE);
  payload[PHYLAX_UID_SIZE] = e->addr;
  phylax_bus_send(PHYLAX_ADDR_BROADCAST, PHYLAX_MSG_ASSIGN, payload,
                  sizeof(payload));
}

void phylax_stack_on_hello(uint8_t src, const PhylaxHello *hello) {
  uint32_t now = timer_ms();
  PhylaxStackEntry *e = find_uid(hello->uid);
  if (e == NULL) {
    // New block: take a free slot and the next free address.
    for (int i = 0; i < PHYLAX_STACK_MAX && e == NULL; i++) {
      if (!stack[i].used) e = &stack[i];
    }
    if (e == NULL) return;  // stack full
    uint8_t addr = PHYLAX_ADDR_KEY_FIRST;
    while (addr <= PHYLAX_ADDR_KEY_LAST && addr_taken(addr)) addr++;
    if (addr > PHYLAX_ADDR_KEY_LAST) return;
    memzero(e, sizeof(*e));
    e->used = true;
    e->addr = addr;
  }
  e->hello = *hello;
  e->last_seen_ms = now;
  if (src != e->addr) {
    send_assign(e);  // new, or restarted and lost its address
  }
}

void phylax_stack_note_seen(uint8_t src) {
  if (src == PHYLAX_ADDR_NONE) return;
  for (int i = 0; i < PHYLAX_STACK_MAX; i++) {
    if (stack[i].used && stack[i].addr == src) {
      stack[i].last_seen_ms = timer_ms();
    }
  }
}

void phylax_stack_poll(void) {
  uint32_t now = timer_ms();
  if (!discover_started ||
      now - last_discover_ms >= PHYLAX_DISCOVER_INTERVAL_MS) {
    discover_started = true;
    last_discover_ms = now;
    phylax_bus_send(PHYLAX_ADDR_BROADCAST, PHYLAX_MSG_DISCOVER, NULL, 0);
  }
  for (int i = 0; i < PHYLAX_STACK_MAX; i++) {
    if (stack[i].used && now - stack[i].last_seen_ms >= PHYLAX_BLOCK_GONE_MS) {
      memzero(&stack[i], sizeof(stack[i]));  // block removed
    }
  }
}

size_t phylax_stack_count(void) {
  size_t n = 0;
  for (int i = 0; i < PHYLAX_STACK_MAX; i++) {
    if (stack[i].used) n++;
  }
  return n;
}

const PhylaxStackEntry *phylax_stack_get(size_t index) {
  // Sorted by address, so the order is stable for the host.
  const PhylaxStackEntry *sorted[PHYLAX_STACK_MAX];
  size_t n = 0;
  for (uint16_t a = 0; a <= 0xFF; a++) {
    for (int i = 0; i < PHYLAX_STACK_MAX; i++) {
      if (stack[i].used && stack[i].addr == a) sorted[n++] = &stack[i];
    }
  }
  return index < n ? sorted[index] : NULL;
}

uint8_t phylax_stack_first_key(void) {
  const PhylaxStackEntry *e;
  for (size_t i = 0; (e = phylax_stack_get(i)) != NULL; i++) {
    if (e->hello.kind == PHYLAX_KIND_KEY) return e->addr;
  }
  return PHYLAX_ADDR_NONE;
}
