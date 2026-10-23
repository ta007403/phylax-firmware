/*
 * PHYLAX Blocks - hashes of PHYLAX bootloaders (memory_bootloader_hash, the
 * same double SHA-256 as bl_check.c), one per released PHYLAX bootloader.
 *
 * PHYLAX firmware runs on these as well as on Trezor's bootloaders, and never
 * replaces either. Add a line for every new PHYLAX bootloader release (the
 * CI log and phylax/release/README.md show the hash).
 */

#ifndef __PHYLAX_BL_HASHES_H__
#define __PHYLAX_BL_HASHES_H__

static const char *const phylax_bootloader_hashes[] = {
    // "\x..." x 32 per released PHYLAX bootloader
    NULL,  // end
};

#endif
