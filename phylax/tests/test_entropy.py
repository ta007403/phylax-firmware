#!/usr/bin/env python3
"""PHYLAX Blocks - your own randomness (coin flips or dice) when the main block
sets up a new key block.

After "New key block found. Set it up?" the main block asks "Add your own
randomness?" (Skip / Add) and "Coin flips or dice?" (Coin / Dice):

  coin  left = heads, right = tails, both = undo (or back, before the first)
  dice  carousel 1..6, UNDO; both = select (UNDO before the first = back)

256 flips or 99 rolls are hashed and mixed into the random bytes the main
block sends the key block (EntropyAck), never replacing them.

  --mode coin: Add, Dice, back out of the dice right away, Add, Coin,
               one flip undone, then 256 flips.
  --mode dice: Add, Dice, one roll undone, then 99 rolls.
  --mode short: Add, Coin, 255 flips: nothing may happen.

Either way the key block must end up with a 24-word seed and be offered as
"2-of-2 #1".

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_entropy.py --headless --mode coin
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spike2b as s  # noqa: E402
from test_keyblocks import check, fingerprint, wait_key  # noqa: E402
from test_setup import GOOD  # noqa: E402
from test_wallets import wait_wallets  # noqa: E402

MAIN_SCRIPTS = {
    # Add, Dice, (on "1") left to UNDO = BACK, both -> asked again;
    # Add, Coin, heads + undo, then 256 flips.
    "coin": "RR" + "LB" + "RL" + "LB" + "LR" * 128,
    # Add, Dice, "1" then UNDO it (left to UNDO, both, right back to 1),
    # then 99 rolls: 98 x "1", right to "2", one "2".
    "dice": "RR" + "B" + "LB" + "R" + "B" * 98 + "RB",
    # One flip short: the setup must not start.
    "short": "RRL" + "LR" * 127 + "L",
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--elf", default="firmware/trezor.elf")
    ap.add_argument("--headless", action="store_true")
    ap.add_argument("--mode", choices=sorted(MAIN_SCRIPTS), default="coin")
    args = ap.parse_args()
    elf = Path(args.elf).resolve()
    if not elf.exists():
        sys.exit(f"Emulator not found: {elf}")
    for p in (s.MAIN_PORT, s.KEY_PORT):
        if s.port_answers(p):
            sys.exit(f"Something already answers on port {p}. Close running emulators first.")

    workdir = Path(tempfile.mkdtemp(prefix="phylax-entropy-"))
    main_blk = s.Block("Main block", s.MAIN_SEED, s.MAIN_PORT, "main", elf, workdir,
                       args.headless, autoconfirm=True, node=0)
    key = s.Block("Key block 2", None, s.KEY_PORT, "key", elf, workdir,
                  args.headless, autoconfirm=True, node=1)
    ok = False
    try:
        print(f"1. A brand-new key block joins; {args.mode} entered on the main block")
        os.environ["PHYLAX_EMU_PIN_KEYS"] = GOOD + GOOD  # the key block's PIN, twice
        key.start()
        os.environ["PHYLAX_EMU_PIN_KEYS"] = MAIN_SCRIPTS[args.mode]
        main_blk.start()
        os.environ.pop("PHYLAX_EMU_PIN_KEYS")

        if args.mode == "short":
            print("2. One flip short: the key block must stay without a seed")
            try:
                wait_key(main_blk.session, lambda k: k.fingerprint is not None,
                         "the key block to be set up", timeout=30)
            except TimeoutError:
                print("  PASS  not set up after 255 flips")
                print("\nENTROPY TEST (short) PASSED")
                ok = True
                return
            check(False, "", "set up although a flip was missing")

        print("2. The key block is set up with the mixed-in randomness")
        k = wait_key(main_blk.session, lambda k: k.fingerprint is not None,
                     "the key block to be set up", timeout=120)
        seed = key.ctx.debug.state().mnemonic_secret.decode()
        check(len(seed.split()) == 24, "24-word seed made on the key block",
              f"{len(seed.split())} words")
        check(k.fingerprint == fingerprint(seed),
              f"discovery shows its new fingerprint {k.fingerprint:08x}", "wrong fingerprint")
        w = wait_wallets(main_blk.session, lambda w: len(w.wallets) == 1,
                         "the 2-of-2 to be saved")
        check(w.wallets[0].name == "2-of-2 #1", "\"2-of-2 #1\" saved",
              f"unexpected wallet {w.wallets[0].describe()}")

        print(f"\nENTROPY TEST ({args.mode}) PASSED")
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
