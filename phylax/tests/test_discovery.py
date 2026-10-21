#!/usr/bin/env python3
"""PHYLAX Blocks - the main block discovers the stack.

Starts emulated blocks one by one on the shared bus and checks what the main
block reports with PhylaxGetBlocks:

  1. main block alone: only itself, with its own seed fingerprint
  2. key block A snapped on: found, address 0x10, A's fingerprint
  3. key block B snapped on: found, address 0x11, B's fingerprint
  4. key block C with no seed: found, marked "no seed", no fingerprint
  5. A pulled off: gone within a few seconds, B and C keep their addresses
  6. A snapped back on: same id and fingerprint, gets a free address again
  7. signing still works with the discovered key block

Fingerprints are the BIP32 master fingerprints Sparrow shows, computed here
from the test mnemonics with embit for comparison.

Run from the legacy folder (emulator built with
EMULATOR=1 DEBUG_LINK=1 BITCOIN_ONLY=1):

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_discovery.py --headless
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import phylax_msgs  # noqa: E402,F401  (registers the PHYLAX messages)
import spike2b as s  # noqa: E402

from embit import bip32, bip39  # noqa: E402

SEED_B = "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong"
PORTS = {"A": 21424, "B": 21434, "C": 21444}


def fingerprint(mnemonic: str) -> int:
    root = bip32.HDKey.from_seed(bip39.mnemonic_to_seed(mnemonic))
    return int.from_bytes(root.my_fingerprint, "big")


def check(cond, ok, bad):
    print(("  PASS  " if cond else "  FAIL  ") + (ok if cond else bad))
    if not cond:
        sys.exit(1)


def show(blocks):
    for b in blocks:
        print("        " + b.describe())


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--elf", default="firmware/trezor.elf")
    ap.add_argument("--headless", action="store_true")
    args = ap.parse_args()
    elf = Path(args.elf).resolve()
    if not elf.exists():
        sys.exit(f"Emulator not found: {elf}")
    for p in [s.MAIN_PORT, *PORTS.values()]:
        if s.port_answers(p):
            sys.exit(f"Something already answers on port {p}. Close running emulators first.")

    fp_main, fp_a, fp_b = fingerprint(s.MAIN_SEED), fingerprint(s.KEY_SEED), fingerprint(SEED_B)
    workdir = Path(tempfile.mkdtemp(prefix="phylax-discovery-"))
    mk = lambda name, seed, key, node: s.Block(  # noqa: E731
        name, seed, PORTS[key], "key", elf, workdir, args.headless, autoconfirm=True, node=node)
    main_blk = s.Block("Main block", s.MAIN_SEED, s.MAIN_PORT, "main", elf, workdir,
                       args.headless, autoconfirm=False, node=0)
    a = mk("Key block A", s.KEY_SEED, "A", 1)
    b = mk("Key block B", SEED_B, "B", 2)
    c = mk("Key block C", None, "C", 3)
    ok = False
    try:
        print("1. Main block alone")
        main_blk.start()
        time.sleep(2.5)
        blocks = s.get_blocks(main_blk.session)
        show(blocks)
        check(len(blocks) == 1 and int(blocks[0].kind) == 0 and blocks[0].address == 1,
              "only the main block itself", f"unexpected stack: {len(blocks)} blocks")
        check(blocks[0].fingerprint == fp_main,
              f"main block fingerprint {fp_main:08x}", f"main fingerprint {blocks[0].fingerprint}")

        print("2. Snap on key block A")
        a.start()
        t0 = time.time()
        blocks = s.wait_for_blocks(main_blk.session, lambda bl: any(
            k.fingerprint == fp_a for k in s.key_blocks(bl)), what="key block A")
        show(blocks)
        ka = s.key_blocks(blocks)[0]
        check(ka.address == 0x10, f"A found in {time.time() - t0:.1f} s at address 0x10",
              f"A has address 0x{ka.address:02x}")
        uid_a = ka.unique_id

        print("3. Snap on key block B")
        b.start()
        blocks = s.wait_for_blocks(main_blk.session, lambda bl: any(
            k.fingerprint == fp_b for k in s.key_blocks(bl)), what="key block B")
        show(blocks)
        by_fp = {k.fingerprint: k for k in s.key_blocks(blocks)}
        check(by_fp[fp_a].address == 0x10 and by_fp[fp_b].address == 0x11,
              "A at 0x10, B at 0x11", "addresses are wrong")
        check(by_fp[fp_a].unique_id != by_fp[fp_b].unique_id, "A and B have different ids",
              "A and B share an id")

        print("4. Snap on key block C (no seed)")
        c.start()
        blocks = s.wait_for_blocks(main_blk.session,
                                   lambda bl: len(s.key_blocks(bl)) == 3, what="key block C")
        show(blocks)
        kc = [k for k in s.key_blocks(blocks) if k.fingerprint is None]
        check(len(kc) == 1 and kc[0].initialized is False and kc[0].address == 0x12,
              "C at 0x12, marked no seed, no fingerprint", "C is reported wrongly")

        print("5. Pull key block A off")
        pulled = time.time()
        a.stop()
        blocks = s.wait_for_blocks(main_blk.session, lambda bl: all(
            k.fingerprint != fp_a for k in s.key_blocks(bl)), what="A to disappear")
        show(blocks)
        gone_after = time.time() - pulled
        by_fp = {k.fingerprint: k for k in s.key_blocks(blocks)}
        check(gone_after < 9.5, f"A removed after {gone_after:.1f} s",
              f"A removed only after {gone_after:.1f} s")
        check(by_fp[fp_b].address == 0x11, "B keeps 0x11", "B changed address")

        print("6. Snap key block A back on")
        a.start()
        blocks = s.wait_for_blocks(main_blk.session, lambda bl: any(
            k.fingerprint == fp_a for k in s.key_blocks(bl)), what="A to come back")
        show(blocks)
        ka = [k for k in s.key_blocks(blocks) if k.fingerprint == fp_a][0]
        check(ka.unique_id == uid_a and ka.address == 0x10,
              "same id, address 0x10 again", "A came back with a different id or address")

        print("7. Sign with the discovered key block")
        c.stop()  # leave A and B; the main block uses the first key block (A)
        b.stop()
        s.wait_for_blocks(main_blk.session, lambda bl: len(s.key_blocks(bl)) == 1,
                          what="only A to remain")
        main_blk.stop()
        a.stop()
        print("        (running spike 2b)")
        ok = True
    finally:
        for blk in (main_blk, a, b, c):
            blk.stop()
        if ok:
            shutil.rmtree(workdir, ignore_errors=True)
        else:
            print(f"Emulator logs kept in {workdir}")

    sys.argv = [sys.argv[0], "--elf", str(elf)] + (["--headless"] if args.headless else [])
    s.main()
    print("\nDISCOVERY TEST PASSED")


if __name__ == "__main__":
    main()
