#!/usr/bin/env python3
"""PHYLAX Blocks - firmware signatures for the PHYLAX bootloader.

An emulated Trezor One (stock role) holding the public test mnemonic signs a
firmware header with phylax/release/phylax_sign.py, the way the PHYLAX
release Trezor will. The real legacy/fw_signatures.c, compiled for this PC
with PHYLAX_SIGNING=1 and the development keys, then decides like the
bootloader would:

  1. the keys at m/44'/0'/97'..99'/0/0 are the ones in phylax_fw_keys_dev.h
  2. signed with keys 1 and 2: accepted (v3), the old v2 scheme refused
  3. unsigned, one changed header byte, or one key twice: refused
  4. signed with Trezor's own debug key path (m/44'/0'/0'/0/0): refused

    cd ~/phylax-firmware/legacy
    uv run --with embit --with ecdsa python ../phylax/tests/test_fw_signatures.py --headless
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "phylax" / "release"))
import phylax_sign as ps  # noqa: E402
import spike2b as s  # noqa: E402
from test_keyblocks import check  # noqa: E402

TEST_MNEMONIC = " ".join(["table"] * 11 + ["advance"])
CRYPTO = [
    "ecdsa", "secp256k1", "bignum", "sha2", "memzero", "hmac", "rfc6979", "hmac_drbg",
    "address", "ripemd160", "base58", "sha3", "blake256", "groestl", "blake2b",
    "nist256p1", "hasher",
]


def build_checker(out: Path) -> Path:
    exe = out / "fw_sig_check"
    cmd = [
        "gcc", "-O1", "-DPHYLAX_SIGNING=1", "-DDEBUG_T1_SIGNATURES=1", "-DPRODUCTION=0",
        "-DEMULATOR=1", "-DUSE_PRECOMPUTED_CP=0",
        f"-I{REPO}/legacy", f"-I{REPO}/crypto", f"-I{REPO}/legacy/emulator", f"-I{REPO}/common",
        str(HERE / "fw_sig_check.c"), str(REPO / "legacy" / "fw_signatures.c"),
        *[str(REPO / "crypto" / f"{c}.c") for c in CRYPTO],
        "-o", str(exe),
    ]
    subprocess.run(cmd, check=True)
    return exe


def decide(exe: Path, fw: bytes, tmp: Path) -> str:
    f = tmp / "fw.bin"
    f.write_bytes(fw)
    return subprocess.run([str(exe), str(f)], check=True, capture_output=True,
                          text=True).stdout.strip()


def fake_firmware() -> bytearray:
    """A TRZF header (signature fields empty) and some code: only the header
    is signed, so its contents only have to be fixed."""
    hdr = bytearray(ps.HEADER_LEN)
    hdr[:4] = ps.MAGIC
    hdr[4:8] = (1024).to_bytes(4, "little")       # hdrlen
    hdr[16:20] = (4096).to_bytes(4, "little")     # codelen
    hdr[20:24] = bytes([1, 14, 1, 0])              # version 1.14.1
    hdr[32:64] = bytes(range(32))                  # stand-in chunk hash
    return hdr + bytes(4096)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--elf", default="firmware/trezor.elf")
    ap.add_argument("--headless", action="store_true")
    args = ap.parse_args()
    elf = Path(args.elf).resolve()
    if not elf.exists():
        sys.exit(f"Emulator not found: {elf}")
    if s.port_answers(s.MAIN_PORT):
        sys.exit(f"Something already answers on port {s.MAIN_PORT}. Close running emulators first.")

    workdir = Path(tempfile.mkdtemp(prefix="phylax-fwsig-"))
    signer = s.Block("Release Trezor", TEST_MNEMONIC, s.MAIN_PORT, "stock", elf, workdir,
                     args.headless, autoconfirm=False, node=0)
    ok = False
    try:
        exe = build_checker(workdir)
        signer.start()
        session = signer.session
        dev_keys = ps.read_keys(ps.DEV_KEYS)

        print("1. The signing keys")
        check(ps.public_keys(session) == dev_keys,
              "m/44'/0'/97'..99'/0/0 on the Trezor = phylax_fw_keys_dev.h", "keys differ")

        print("2. Signed with keys 1 and 2")
        fw = fake_firmware()
        ps.sign_image(session, fw, dev_keys)
        check(decide(exe, fw, workdir) == "v3=OK v2=FAIL match=OK",
              "the bootloader code accepts it (v3), v2 refused", decide(exe, fw, workdir))

        print("3. Refused")
        check(decide(exe, fake_firmware(), workdir) == "v3=FAIL v2=FAIL match=FAIL",
              "unsigned", "unsigned accepted")
        bad = bytearray(fw)
        bad[100] ^= 1
        check(decide(exe, bad, workdir).endswith("match=FAIL"),
              "one header byte changed after signing", "tampered accepted")
        twice = bytearray(fw)
        twice[ps.SIGINDEX_OFFSETS[1]] = twice[ps.SIGINDEX_OFFSETS[0]]
        twice[ps.SIG_OFFSETS[1]:ps.SIG_OFFSETS[1] + 64] = twice[ps.SIG_OFFSETS[0]:ps.SIG_OFFSETS[0] + 64]
        check(decide(exe, twice, workdir).endswith("match=FAIL"),
              "the same key twice", "duplicate key accepted")

        print("4. Trezor's own (debug) key path is not a PHYLAX key")
        from trezorlib import btc
        from trezorlib.tools import parse_path
        other = fake_firmware()
        digest = ps.header_hash(other)
        sigs = [btc.sign_message(session, "Bitcoin", parse_path(f"m/44h/0h/{i}h/0/0"), digest)
                .signature[1:] for i in (0, 1)]
        ps.patch(other, [(1, sigs[0]), (2, sigs[1])])
        check(decide(exe, other, workdir).endswith("match=FAIL"),
              "signed at m/44'/0'/0'..1'/0/0: refused", "accepted with non-PHYLAX keys")

        print("\nFIRMWARE SIGNATURE TEST PASSED")
        ok = True
    finally:
        signer.stop()
        if ok:
            shutil.rmtree(workdir, ignore_errors=True)
        else:
            print(f"Logs kept in {workdir}")


if __name__ == "__main__":
    main()
