/*
 * PHYLAX Blocks - RELEASE keys for PHYLAX firmware signatures.
 *
 * Written by phylax/release/phylax_sign.py keys from the PHYLAX release
 * Trezor (keys pasted by the owner from phylax_sign.py keys). The PHYLAX bootloader trusts exactly these keys;
 * changing them needs a new bootloader on every board.
 */

#ifndef __PHYLAX_FW_KEYS_RELEASE_H__
#define __PHYLAX_FW_KEYS_RELEASE_H__

// clang-format off
#define PHYLAX_FW_KEY_1 \
  "\x03\xd5\xc5\x37\xec\xdd\xc3\xa3\x6f\x6d\xc6\xda\x5f\x2d\xd8\x5a\xe1\x20" \
  "\x81\x51\xb5\x1b\x68\x0e\xab\xcf\xb1\x9f\x65\x9e\xc9\x7e\xc3"  // m/44h/0h/97h/0/0
#define PHYLAX_FW_KEY_2 \
  "\x02\xc9\x48\x22\x69\x00\x6a\xe2\x18\xad\xf9\x9c\x98\x3a\x91\xba\x4c\x18" \
  "\xdc\x49\xcf\x6d\x0b\x12\xff\x5f\x35\x09\xad\xf9\xf8\x51\x5b"  // m/44h/0h/98h/0/0
#define PHYLAX_FW_KEY_3 \
  "\x02\x1e\x60\x9e\x72\x45\xd9\xbc\xcb\x3c\x0c\x54\xf8\xfe\x40\x09\x4c\xf0" \
  "\x2b\x95\x41\xfb\x6a\x75\x0c\x4b\xb6\xa1\x40\x4d\x75\xd0\xf8"  // m/44h/0h/99h/0/0
// clang-format on

#endif
