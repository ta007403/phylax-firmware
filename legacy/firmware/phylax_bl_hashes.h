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
    // 2nd PHYLAX bootloader: "Install / PHYLAX / firmware." screen, CI run of
    // commit 16d2a28
    "\x88\xa0\xbc\xa0\xf9\x7f\x9f\x62\x35\xeb\x72\x6c\x5d\x9e\xa8\xdb"
    "\x4b\x7b\x4d\xc6\x0e\xe7\xbc\x59\x63\x00\x79\x33\x8b\xfc\xaa\xea",
    NULL,  // end
};

#endif
