#!/usr/bin/env python3
"""PHYLAX Snap - signing asks only the key blocks of the wallet being spent.

Found on hardware: main block + key blocks 1, 2, 3, a saved 2-of-2 of main +
key block 2, key block 1 locked ("Later"). Sending from the 2-of-2, the main
block passed the transaction to every key block, also key block 1: being
locked, it asked for its PIN, and the request went to PHYLAX Suite. Now the
main block finds the saved wallet of the input (its keys are the input's
cosigners) and asks only that wallet's key blocks.

  1. Main block + key block W (no PIN): the 2-of-2 is saved.
  2. Key block L (PIN) joins: the 3-of-3 is answered "Later", then L is
     locked. It is in no saved wallet.
  3. A spend from the 2-of-2: signed by W and the main block; L, locked, is
     not asked (before: its PIN request reached the PC and the signing
     failed).

A multisig no saved wallet matches still goes to every key block (each one
declines if it is not a cosigner): test_4of4.py step 3.

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_relay_wallet.py --headless
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spike2b as s  # noqa: E402
from test_3of3 import check, sign_and_verify  # noqa: E402
from test_unlock import KEY_FP, KEY_PIN  # noqa: E402
from test_wallets import PORT_B, SEED_B, wait_wallets, wallets  # noqa: E402
from trezorlib import btc, messages  # noqa: E402
from trezorlib.debuglink import load_device  # noqa: E402
from trezorlib.tools import parse_path  # noqa: E402

OFFER_WAIT_S = 10


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--elf", default="firmware/trezor.elf")
    ap.add_argument("--headless", action="store_true")
    args = ap.parse_args()
    elf = Path(args.elf).resolve()
    if not elf.exists():
        sys.exit(f"Emulator not found: {elf}")
    for p in (s.MAIN_PORT, s.KEY_PORT, PORT_B):
        if s.port_answers(p):
            sys.exit(f"Something already answers on port {p}. Close running emulators first.")

    workdir = Path(tempfile.mkdtemp(prefix="phylax-relay-wallet-"))
    blocks = []

    def start_main(answer):
        if answer is None:
            os.environ.pop("PHYLAX_EMU_NEW_WALLET", None)
        else:
            os.environ["PHYLAX_EMU_NEW_WALLET"] = answer
        blk = s.Block("Main block", s.MAIN_SEED, s.MAIN_PORT, "main", elf, workdir,
                      args.headless, autoconfirm=True, node=0)
        blocks.append(blk)
        blk.start()
        os.environ.pop("PHYLAX_EMU_NEW_WALLET", None)
        return blk

    key_w = s.Block("Key block W", SEED_B, PORT_B, "key", elf, workdir,
                    args.headless, autoconfirm=True, node=2)
    key_l = s.Block("Key block L", None, s.KEY_PORT, "key", elf, workdir,
                    args.headless, autoconfirm=True, node=1)
    blocks += [key_w, key_l]
    ok = False
    try:
        print("1. Main block + key block W: the 2-of-2 is saved")
        key_w.start()
        node_w = btc.get_public_node(key_w.session, parse_path(s.ACCOUNT), coin_name=s.COIN).node
        main_blk = start_main(None)
        node_main = btc.get_public_node(main_blk.session, parse_path(s.ACCOUNT),
                                        coin_name=s.COIN).node
        wait_wallets(main_blk.session, lambda w: len(w.wallets) == 1, "the 2-of-2")

        print("2. Key block L (PIN) joins: 3-of-3 answered \"Later\"; L locked")
        main_blk.stop()
        key_l.start()
        load_device(key_l.ctx.get_session(passphrase=None), mnemonic=s.KEY_SEED, pin=KEY_PIN,
                    passphrase_protection=False, label="Key block L")
        main_blk = start_main("later")
        s.wait_for_blocks(main_blk.session, lambda bl: len(s.key_blocks(bl)) == 2, what="W and L")
        time.sleep(OFFER_WAIT_S)
        check(len(wallets(main_blk.session).wallets) == 1, "still only the 2-of-2",
              "another wallet was saved")
        key_l.ctx.get_session(passphrase=None).call(messages.LockDevice(), expect=messages.Success)
        deadline = time.time() + 30
        while not any(b.fingerprint == KEY_FP and b.locked for b in s.get_blocks(main_blk.session)):
            if time.time() > deadline:
                raise TimeoutError("L, locked, in the stack")
            time.sleep(0.5)

        print("3. Spend from the 2-of-2: W and the main block sign, L (locked) is not asked")
        two = {main_blk.name: node_main, key_w.name: node_w}
        sign_and_verify(main_blk.session, two, "2-of-2")
        check(any(b.fingerprint == KEY_FP and b.locked for b in s.get_blocks(main_blk.session)),
              "L stayed locked (never asked for its PIN)", "L was unlocked")

        print("\nRELAY WALLET TEST PASSED")
        ok = True
    finally:
        for blk in blocks:
            blk.stop()
        if ok:
            shutil.rmtree(workdir, ignore_errors=True)
        else:
            print(f"Emulator logs kept in {workdir}")


if __name__ == "__main__":
    main()
