/*
 * PHYLAX Blocks - main block: which blocks are in the stack.
 *
 * Every PHYLAX_DISCOVER_INTERVAL_MS the main block broadcasts DISCOVER.
 * Each block answers HELLO with its unique id, kind and seed fingerprint.
 * A new block gets the next free address (0x10..0x1F) with ASSIGN; a block
 * that restarted and lost its address gets the same one again. A block not
 * heard from for PHYLAX_BLOCK_GONE_MS is removed.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef __PHYLAX_STACK_H__
#define __PHYLAX_STACK_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "phylax_bus.h"

#define PHYLAX_STACK_MAX 7  // plus the main block itself = 8
#define PHYLAX_DISCOVER_INTERVAL_MS 2000
#define PHYLAX_BLOCK_GONE_MS 6500  // about 3 missed DISCOVER rounds

typedef struct {
  bool used;
  uint8_t addr;
  PhylaxHello hello;
  uint32_t last_seen_ms;
} PhylaxStackEntry;

void phylax_stack_poll(void);
void phylax_stack_on_hello(uint8_t src, const PhylaxHello *hello);
void phylax_stack_note_seen(uint8_t src);

// Blocks currently in the stack (main block not included).
size_t phylax_stack_count(void);
const PhylaxStackEntry *phylax_stack_get(size_t index);
// First key block with an address, or PHYLAX_ADDR_NONE.
uint8_t phylax_stack_first_key(void);

#endif
