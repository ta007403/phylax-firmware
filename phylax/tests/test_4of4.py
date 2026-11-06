#!/usr/bin/env python3
"""PHYLAX Snap - 4-of-4: the main block and three key blocks (the design limit).

  1. Main block + key blocks A, B, C: the 4-of-4 is created and saved, with
     all four keys.
  2. One SignTx for the 4-of-4 to the main block: A, B and C sign in turn,
     then the main block; four valid signatures in script-key order.
  3. A 2-of-2 of main + A while B and C are attached: B and C are skipped.
  4. A fourth key block D joins: more than three key blocks is refused
     ("Too many key blocks (max 3)"), no transaction is signed.

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_4of4.py --headless
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import phylax_msgs as pm  # noqa: E402
import spike2b as s  # noqa: E402
from test_3of3 import SEED_B, check, sign_and_verify  # noqa: E402
from test_wallets import wait_wallets  # noqa: E402
from trezorlib import btc  # noqa: E402
from trezorlib.exceptions import TrezorFailure  # noqa: E402
from trezorlib.tools import parse_path  # noqa: E402

SEED_C = "legal winner thank year wave sausage worth useful legal winner thank yellow"
SEED_D = "letter advice cage absurd amount doctor acoustic avoid letter advice cage above"
PORTS = {"B": 21434, "C": 21444, "D": 21454}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--elf", default="firmware/trezor.elf")
    ap.add_argument("--headless", action="store_true")
    args = ap.parse_args()
    elf = Path(args.elf).resolve()
    if not elf.exists():
        sys.exit(f"Emulator not found: {elf}")
    for p in (s.MAIN_PORT, s.KEY_PORT, *PORTS.values()):
        if s.port_answers(p):
            sys.exit(f"Something already answers on port {p}. Close running emulators first.")

    workdir = Path(tempfile.mkdtemp(prefix="phylax-4of4-"))
    # autoconfirm: the main block answers "New 4-of-4 wallet?" with Create
    main_blk = s.Block("Main block", s.MAIN_SEED, s.MAIN_PORT, "main", elf, workdir,
                       args.headless, autoconfirm=True, node=0)
    a = s.Block("Key block A", s.KEY_SEED, s.KEY_PORT, "key", elf, workdir,
                args.headless, autoconfirm=True, node=1)
    b = s.Block("Key block B", SEED_B, PORTS["B"], "key", elf, workdir,
                args.headless, autoconfirm=True, node=2)
    c = s.Block("Key block C", SEED_C, PORTS["C"], "key", elf, workdir,
                args.headless, autoconfirm=True, node=3)
    d = s.Block("Key block D", SEED_D, PORTS["D"], "key", elf, workdir,
                args.headless, autoconfirm=True, node=4)
    ok = False
    try:
        print("1. Main block + key blocks A, B, C: the 4-of-4 is saved")
        nodes = {}
        for blk in (a, b, c):
            blk.start()
            nodes[blk.name] = btc.get_public_node(blk.session, parse_path(s.ACCOUNT),
                                                  coin_name=s.COIN).node
        main_blk.start()
        s.wait_for_blocks(main_blk.session, lambda bl: len(s.key_blocks(bl)) == 3,
                          what="three key blocks")
        nodes = {main_blk.name: btc.get_public_node(main_blk.session, parse_path(s.ACCOUNT),
                                                    coin_name=s.COIN).node, **nodes}
        w = wait_wallets(main_blk.session,
                         lambda w: any(len(x.fingerprints) == 4 for x in w.wallets),
                         "the 4-of-4", timeout=60)
        four = next(x for x in w.wallets if len(x.fingerprints) == 4)
        check(four.threshold == 4, "saved as 4-of-4", f"threshold {four.threshold}")
        saved = main_blk.session.call(pm.PhylaxGetWallet(slot=four.slot), expect=pm.PhylaxWallet)
        check({k.testnet.public_key for k in saved.keys}
              == {n.public_key for n in nodes.values()},
              "its four keys are the four blocks' keys", "other keys saved")

        print("2. 4-of-4: one SignTx to the main block")
        sign_and_verify(main_blk.session, nodes, "4-of-4")

        print("3. 2-of-2 of main + A, with B and C attached but not in it")
        two = {k: v for k, v in nodes.items() if k in (main_blk.name, a.name)}
        sign_and_verify(main_blk.session, two, "2-of-2")
        print("  PASS  key blocks B and C were skipped")

        print("4. A fourth key block D joins")
        d.start()
        s.wait_for_blocks(main_blk.session, lambda bl: len(s.key_blocks(bl)) == 4,
                          what="four key blocks")
        try:
            sign_and_verify(main_blk.session, nodes, "4-of-4")
            outcome = "signed"
        except TrezorFailure as e:
            outcome = e.message or ""
        check("Too many key blocks" in outcome, "refused: \"Too many key blocks (max 3)\"",
              f"outcome: {outcome}")

        print("\n4-OF-4 TEST PASSED")
        ok = True
    finally:
        for blk in (main_blk, a, b, c, d):
            blk.stop()
        if ok:
            shutil.rmtree(workdir, ignore_errors=True)
        else:
            print(f"Emulator logs kept in {workdir}")


if __name__ == "__main__":
    main()
