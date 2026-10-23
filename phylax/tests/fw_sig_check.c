/*
 * PHYLAX Blocks - host check of the bootloader's firmware signature code.
 *
 * Built by test_fw_signatures.py with the real legacy/fw_signatures.c and
 * PHYLAX_SIGNING=1 DEBUG_T1_SIGNATURES=1 (the development keys). Reads a
 * firmware image and prints what the bootloader would decide:
 *
 *     v3=OK|FAIL v2=OK|FAIL match=OK|FAIL
 */

#include <stdio.h>
#include <stdlib.h>

#include "fw_signatures.h"

// Host stubs: no flash; randomness for the point blinding in ecdsa.c
// (it loops until it gets a non-zero value).
#include <stddef.h>
#include <sys/random.h>
uint8_t *emulator_flash_base = NULL;
void random_buffer(uint8_t *buf, size_t len) {
  if (getrandom(buf, len, 0) != (ssize_t)len) abort();
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  FILE *f = fopen(argv[1], "rb");
  if (!f) return 2;
  static unsigned char buf[1024];
  if (fread(buf, 1, sizeof(buf), f) != sizeof(buf)) return 2;
  fclose(f);
  const image_header *hdr = (const image_header *)buf;
  uint8_t fp[32];
  int v3 = signatures_ok(hdr, NULL, sectrue);
  int v2 = signatures_ok(hdr, fp, secfalse);
  int m = signatures_match(hdr, NULL);
  printf("v3=%s v2=%s match=%s\n", v3 == SIG_OK ? "OK" : "FAIL",
         v2 == SIG_OK ? "OK" : "FAIL", m == SIG_OK ? "OK" : "FAIL");
  return 0;
}
