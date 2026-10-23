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
    // 1st PHYLAX bootloader (1.12.1 base), CI run of commit 8c4847e
    "\x95\xb8\x0f\x96\xcf\x3f\x71\x3f\x01\x71\x1b\x28\xb1\x6c\x4b\xe5"
    "\xfe\xf3\xa8\xb4\xa6\x54\x12\x88\x5d\xc6\xc8\x79\x34\xc1\x3d\xab",
    NULL,  // end
};

#endif
