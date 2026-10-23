#!/usr/bin/env python3
"""PHYLAX Blocks - sign PHYLAX firmware with the PHYLAX release Trezor.

The PHYLAX bootloader accepts a firmware when it carries two SignMessage
("v3") signatures from the three PHYLAX firmware keys. All three keys live
on one Trezor One, at

    m/44'/0'/97'/0/0   key 1
    m/44'/0'/98'/0/0   key 2
    m/44'/0'/99'/0/0   key 3

so they come back from that Trezor's 24 words. Commands:

  keys                 read the three public keys from the connected Trezor
                       and write legacy/phylax_fw_keys_release.h (commit it;
                       the PHYLAX bootloader is built with these keys)
  sign FW.bin [-o OUT] sign with keys 1 and 2 (confirm both on the Trezor:
                       it shows the firmware's header hash), check the
                       result against the release keys, write OUT
                       (default FW.signed.bin)
  verify FW.bin        check a signed firmware against the release keys
  hash FW.bin          print the header hash the Trezor will show

  --dev                use the development keys (legacy/phylax_fw_keys_dev.h,
                       the public "table ... advance" test mnemonic) instead:
                       for emulator and host tests only.

Run from anywhere with trezorlib installed, e.g.

    uv run --with trezor python phylax/release/phylax_sign.py keys
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
RELEASE_KEYS = REPO / "legacy" / "phylax_fw_keys_release.h"
DEV_KEYS = REPO / "legacy" / "phylax_fw_keys_dev.h"
PATHS = ["m/44h/0h/97h/0/0", "m/44h/0h/98h/0/0", "m/44h/0h/99h/0/0"]
SIGN_WITH = [1, 2]  # key indexes used for each release

HEADER_LEN = 1024
MAGIC = b"TRZF"
SIG_OFFSETS = [544, 608, 672]
SIGINDEX_OFFSETS = [736, 737, 738]


# ------------------------------------------------------------------ firmware


def load(path: Path) -> bytearray:
    data = bytearray(path.read_bytes())
    if len(data) < HEADER_LEN or data[:4] != MAGIC:
        sys.exit(f"{path}: not a Trezor One firmware image (no TRZF header)")
    return data


def header_hash(fw: bytes) -> bytes:
    """SHA-256 of the header with the signature fields zeroed (the message)."""
    hdr = bytearray(fw[:HEADER_LEN])
    for i in range(3):
        hdr[SIGINDEX_OFFSETS[i]] = 0
        hdr[SIG_OFFSETS[i] : SIG_OFFSETS[i] + 64] = bytes(64)
    return hashlib.sha256(hdr).digest()


def signmessage_digest(digest: bytes) -> bytes:
    """What the bootloader verifies (fw_signatures.c, v3)."""
    inner = hashlib.sha256(b"\x18Bitcoin Signed Message:\n\x20" + digest).digest()
    return hashlib.sha256(inner).digest()


def patch(fw: bytearray, pairs: list[tuple[int, bytes]]) -> None:
    for i in range(3):
        fw[SIGINDEX_OFFSETS[i]] = 0
        fw[SIG_OFFSETS[i] : SIG_OFFSETS[i] + 64] = bytes(64)
    for i, (index, sig) in enumerate(pairs):
        assert 1 <= index <= 3 and len(sig) == 64
        fw[SIGINDEX_OFFSETS[i]] = index
        fw[SIG_OFFSETS[i] : SIG_OFFSETS[i] + 64] = sig


# ------------------------------------------------------------------ keys


def read_keys(path: Path) -> list[bytes]:
    if not path.exists():
        sys.exit(f"{path} does not exist: run `phylax_sign.py keys` first")
    text = path.read_text()
    keys = []
    for n in (1, 2, 3):
        m = re.search(rf"#define PHYLAX_FW_KEY_{n}\s+((?:\s*\\?\s*\"[^\"]*\")+)", text)
        if not m:
            sys.exit(f"{path}: PHYLAX_FW_KEY_{n} missing")
        hexes = re.findall(r"\\x([0-9a-fA-F]{2})", m.group(1))
        keys.append(bytes(int(h, 16) for h in hexes))
    for k in keys:
        if len(k) != 33 or k[0] not in (2, 3):
            sys.exit(f"{path}: a key is not a 33-byte compressed public key")
    return keys


def c_string(key: bytes) -> str:
    esc = "".join(f"\\x{b:02x}" for b in key)
    return f'  "{esc[:72]}" \\\n  "{esc[72:]}"'


def write_release_keys(keys: list[bytes], xpub_note: str) -> None:
    body = "\n".join(
        f"#define PHYLAX_FW_KEY_{n} \\\n{c_string(k)}  // {PATHS[n - 1]}"
        for n, k in enumerate(keys, 1)
    )
    RELEASE_KEYS.write_text(
        f"""/*
 * PHYLAX Blocks - RELEASE keys for PHYLAX firmware signatures.
 *
 * Written by phylax/release/phylax_sign.py keys from the PHYLAX release
 * Trezor ({xpub_note}). The PHYLAX bootloader trusts exactly these keys;
 * changing them needs a new bootloader on every board.
 */

#ifndef __PHYLAX_FW_KEYS_RELEASE_H__
#define __PHYLAX_FW_KEYS_RELEASE_H__

// clang-format off
{body}
// clang-format on

#endif
"""
    )


def verify(fw: bytes, keys: list[bytes]) -> list[int]:
    """Raise unless the image has two valid, distinct v3 signatures."""
    import ecdsa  # the same library trezorlib uses

    digest = signmessage_digest(header_hash(fw))
    used = []
    for i in range(2):
        index = fw[SIGINDEX_OFFSETS[i]]
        if not 1 <= index <= 3:
            raise ValueError(f"signature slot {i + 1}: no key index")
        sig = bytes(fw[SIG_OFFSETS[i] : SIG_OFFSETS[i] + 64])
        vk = ecdsa.VerifyingKey.from_string(keys[index - 1], curve=ecdsa.SECP256k1)
        vk.verify_digest(sig, digest)  # raises BadSignatureError
        used.append(index)
    if used[0] == used[1]:
        raise ValueError("the same key signed twice")
    if fw[SIGINDEX_OFFSETS[2]] != 0 or any(fw[SIG_OFFSETS[2] : SIG_OFFSETS[2] + 64]):
        raise ValueError("third signature slot must be empty for v3")
    return used


# ------------------------------------------------------------------ Trezor


def client():
    from trezorlib.client import get_default_client

    return get_default_client()


def public_keys(session) -> list[bytes]:
    from trezorlib import btc
    from trezorlib.tools import parse_path

    return [btc.get_public_node(session, parse_path(p)).node.public_key for p in PATHS]


def sign_image(session, fw: bytearray, keys: list[bytes], indexes=SIGN_WITH) -> None:
    """Sign `fw` in place on the Trezor and check it against `keys`."""
    from trezorlib import btc
    from trezorlib.tools import parse_path

    digest = header_hash(fw)
    pairs = []
    for index in indexes:
        path = PATHS[index - 1]
        print(f"Key {index} ({path}): confirm on the Trezor...")
        res = btc.sign_message(session, "Bitcoin", parse_path(path), digest)
        if len(res.signature) != 65 or res.signature[0] < 27:
            sys.exit("unexpected signature format from the Trezor")
        pairs.append((index, bytes(res.signature[1:])))
    patch(fw, pairs)
    verify(fw, keys)  # never write a firmware the bootloader would refuse


def session_of(c):
    return c.get_session() if hasattr(c, "get_session") else c


def cmd_keys(args) -> None:
    from trezorlib import btc
    from trezorlib.tools import parse_path

    session = session_of(client())
    keys = public_keys(session)
    for p, k in zip(PATHS, keys):
        print(f"{p}  {k.hex()}")
    fp = btc.get_public_node(session, parse_path("m/44h/0h/97h")).node.fingerprint
    write_release_keys(keys, f"key 1 parent fingerprint {fp:08x}")
    print(f"\nWrote {RELEASE_KEYS.relative_to(REPO)}. Commit it, then build the bootloader.")


def cmd_sign(args) -> None:
    fw = load(Path(args.firmware))
    keys = read_keys(DEV_KEYS if args.dev else RELEASE_KEYS)
    print(f"Firmware header hash (the Trezor shows it):\n  {header_hash(fw).hex()}")
    sign_image(session_of(client()), fw, keys)
    out = Path(args.output or Path(args.firmware).with_suffix(".signed.bin"))
    out.write_bytes(fw)
    print(f"Signed with keys {SIGN_WITH}, checked: {out}")


def cmd_verify(args) -> None:
    fw = load(Path(args.firmware))
    keys = read_keys(DEV_KEYS if args.dev else RELEASE_KEYS)
    try:
        used = verify(fw, keys)
    except Exception as e:  # noqa: BLE001
        sys.exit(f"NOT signed for the PHYLAX bootloader: {e}")
    print(f"OK: signed with PHYLAX keys {used} (header hash {header_hash(fw).hex()})")


def cmd_hash(args) -> None:
    print(header_hash(load(Path(args.firmware))).hex())


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dev", action="store_true", help="development keys (tests only)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("keys")
    s = sub.add_parser("sign")
    s.add_argument("firmware")
    s.add_argument("-o", "--output")
    v = sub.add_parser("verify")
    v.add_argument("firmware")
    h = sub.add_parser("hash")
    h.add_argument("firmware")
    args = ap.parse_args()
    if args.cmd == "keys" and args.dev:
        sys.exit("--dev keys are fixed in legacy/phylax_fw_keys_dev.h")
    {"keys": cmd_keys, "sign": cmd_sign, "verify": cmd_verify, "hash": cmd_hash}[args.cmd](args)


if __name__ == "__main__":
    main()
