/*
 * PHYLAX Blocks - DEVELOPMENT keys for PHYLAX firmware signatures.
 *
 * Only for DEBUG_T1_SIGNATURES builds (never PRODUCTION): they belong to the
 * public test mnemonic "table table table table table table table table table
 * table table advance", so anybody can sign with them. Same paths as the
 * release keys: m/44'/0'/97'/0/0, m/44'/0'/98'/0/0, m/44'/0'/99'/0/0.
 */

#ifndef __PHYLAX_FW_KEYS_DEV_H__
#define __PHYLAX_FW_KEYS_DEV_H__

#if PRODUCTION
#error "PHYLAX development signing keys in a PRODUCTION build"
#endif

#define PHYLAX_FW_KEY_1                                                      \
  "\x02\xf5\x49\xfe\xbf\xb3\xe2\xde\x2f\x86\xf5\x5c\xf7\x18\xa6\xba\xe8\xa0" \
  "\xb5\xeb\x0c\xca\x42\x2d\x0c\xd2\x3c\x12\x71\x13\x41\xd6\xcf"
#define PHYLAX_FW_KEY_2                                                      \
  "\x02\x23\x6d\x25\x3b\x16\x7e\xce\x7c\x87\xa4\x25\x28\x9d\x52\xa3\xcf\x95" \
  "\x37\xea\xe1\x54\xe1\x58\xed\x1b\x35\x6f\xdb\xb7\xc9\x08\x0a"
#define PHYLAX_FW_KEY_3                                                      \
  "\x02\x10\x2f\xcf\x26\x3c\x50\xe6\xfd\xd2\x92\x1f\x0c\xbc\x7d\xb8\x0e\x35" \
  "\x6f\x92\xf7\x14\xc7\xf6\x84\xb2\x6c\xdd\x94\x28\x75\x34\x4b"

#endif
