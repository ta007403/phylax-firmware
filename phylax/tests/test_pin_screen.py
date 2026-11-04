#!/usr/bin/env python3
"""PHYLAX Snap - the main block's PIN grid stays on screen.

While the main block waits for its PIN (and then computes its seed), the
PHYLAX wallet code must neither draw over the PIN grid nor start work of its
own. Found on hardware: the status "Attach key block ..." covered the grid, so
the PIN could not be entered.

  1. Main block with PIN 1234 + key block: the 2-of-2 is saved.
  2. The main block is locked. The PC asks for an account key: PIN grid.
  3. The key block locks itself (the main block would offer to unlock it),
     is pulled out and comes back: the grid stays on screen, pixel for pixel.
  4. The PIN unlocks the main block and the key is the one in the wallet.

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_pin_screen.py --headless
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import phylax_msgs as pm  # noqa: E402
import spike2b as s  # noqa: E402
from test_screen import screen  # noqa: E402
from test_wallets import check, wait_wallets  # noqa: E402
from trezorlib import messages  # noqa: E402
from trezorlib.debuglink import load_device  # noqa: E402
from trezorlib.tools import parse_path  # noqa: E402

PIN = "1234"
ACCOUNT = "m/48h/1h/0h/2h"  # Testnet account key, as in the saved wallet


def get_key(session):
    return session.call_raw(messages.GetPublicKey(address_n=parse_path(ACCOUNT),
                                                  coin_name="Testnet"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--elf", default="firmware/trezor.elf")
    ap.add_argument("--headless", action="store_true")
    args = ap.parse_args()
    elf = Path(args.elf).resolve()
    if not elf.exists():
        sys.exit(f"Emulator not found: {elf}")
    for p in (s.MAIN_PORT, s.KEY_PORT):
        if s.port_answers(p):
            sys.exit(f"Something already answers on port {p}. Close running emulators first.")

    workdir = Path(tempfile.mkdtemp(prefix="phylax-pin-screen-"))
    main_blk = s.Block("Main block", None, s.MAIN_PORT, "main", elf, workdir,
                       args.headless, autoconfirm=True, node=0)
    key = s.Block("Key block", s.KEY_SEED, s.KEY_PORT, "key", elf, workdir,
                  args.headless, autoconfirm=True, node=1)
    ok = False
    try:
        print("1. Main block with a PIN + key block: the 2-of-2 is saved")
        key.start()
        main_blk.start()
        load_device(main_blk.ctx.get_session(passphrase=None), mnemonic=s.MAIN_SEED, pin=PIN,
                    passphrase_protection=False, label="Main block")
        # A session with a seed (empty passphrase), as Suite's; opened while unlocked.
        pc = main_blk.ctx.get_session(passphrase="")
        w = wait_wallets(pc, lambda w: len(w.wallets) == 1, "the 2-of-2")
        wallet = pc.call(pm.PhylaxGetWallet(slot=w.wallets[0].slot), expect=pm.PhylaxWallet)
        saved = {k.testnet.public_key for k in wallet.keys}

        print("2. Locked; the PC asks for the account key: PIN grid")
        pc.call(messages.LockDevice(), expect=messages.Success)
        # As after a replug: locked, on the home screen (a PC request ends there).
        pc.call(pm.PhylaxGetBlocks(), expect=pm.PhylaxBlocks)
        time.sleep(1.0)
        ret = get_key(pc)
        check(isinstance(ret, messages.PinMatrixRequest), "the main block asks for its PIN",
              f"no PIN asked: {ret}")
        time.sleep(1.0)
        grid = screen(main_blk)

        print("3. The key block locks, is pulled out and comes back while the grid is up")
        key.ctx.get_session(passphrase=None).call(messages.LockDevice(), expect=messages.Success)
        changed = 0
        for _ in range(int(15 / 0.25)):  # long enough for "Key block ... is locked."
            time.sleep(0.25)
            if screen(main_blk) != grid:
                changed += 1
        key.stop()
        for _ in range(int(20 / 0.25)):  # long enough for "Attach key block ..."
            time.sleep(0.25)
            if screen(main_blk) != grid:
                changed += 1
        key.start()
        for _ in range(int(10 / 0.25)):
            time.sleep(0.25)
            if screen(main_blk) != grid:
                changed += 1
        check(changed == 0, "the PIN grid stayed on screen, pixel for pixel",
              f"the screen changed {changed} times while the PIN grid was up")

        print("4. The PIN unlocks it; the key is the one saved in the wallet")
        ret = pc.call_raw(messages.PinMatrixAck(pin=main_blk.ctx.debug.encode_pin(PIN)))
        check(isinstance(ret, messages.PublicKey), "unlocked", f"unlock failed: {ret}")
        check(ret.node.public_key in saved, "same key as saved in the wallet",
              "a different key than the one saved in the wallet")

        print("\nPIN SCREEN TEST PASSED")
        ok = True
    finally:
        for blk in (main_blk, key):
            blk.stop()
        if ok:
            shutil.rmtree(workdir, ignore_errors=True)
        else:
            print(f"Emulator logs kept in {workdir}")


if __name__ == "__main__":
    main()
