#!/usr/bin/env python3
"""PHYLAX Snap - no new wallet is offered while a key block is locked.

Found on hardware: main block + 3 key blocks with a saved 4-of-4; one key
block put off with "Later" stayed locked, and the main block offered a new
3-of-3 of the other blocks. A locked key block does not count as present,
but the user put it in the stack: the blocks without it are not a
combination they chose.

  1. Key block B (PIN 1357) meets the main block, which is told "Later" for
     the new 2-of-2: nothing saved, B's fingerprint is known. B is locked.
  2. Key block A (no PIN) joins and the main block restarts (confirming
     everything): main + A would be a new 2-of-2, but B is locked: no offer.
  3. The PC wakes B (PhylaxUnlockBlock, PIN on B's own buttons): now all
     three blocks are unlocked and the 3-of-3 is offered and saved; no
     2-of-2 ever was.

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_locked_offer.py --headless
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
import phylax_msgs as pm  # noqa: E402
import spike2b as s  # noqa: E402
from test_setup import GOOD  # noqa: E402  (PIN 1357 on the carousel)
from test_unlock import KEY_FP, KEY_PIN  # noqa: E402
from test_wallets import PORT_B, SEED_B, check, wait_wallets, wallets  # noqa: E402
from trezorlib import messages  # noqa: E402
from trezorlib.debuglink import load_device  # noqa: E402

# Long enough for the main block to see the blocks, let the stack settle and
# offer (with auto-confirm it saves at once).
OFFER_WAIT_S = 10


def key_by_fp(session, fp):
    return next((b for b in s.get_blocks(session) if int(b.kind) == 1 and b.fingerprint == fp),
                None)


def wait_key_by_fp(session, fp, cond, what, timeout=60.0):
    deadline = time.time() + timeout
    k = None
    while time.time() < deadline:
        k = key_by_fp(session, fp)
        if k is not None and cond(k):
            return k
        time.sleep(0.3)
    raise TimeoutError(f"waited {timeout:.0f} s for {what}; key block: {k}")


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

    workdir = Path(tempfile.mkdtemp(prefix="phylax-locked-offer-"))
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

    # B: the key block with a PIN (its PIN typed on its own buttons when woken)
    key_b = s.Block("Key block B", None, s.KEY_PORT, "key", elf, workdir,
                    args.headless, autoconfirm=True, node=1)
    key_a = s.Block("Key block A", SEED_B, PORT_B, "key", elf, workdir,
                    args.headless, autoconfirm=True, node=2)
    blocks += [key_b, key_a]
    ok = False
    try:
        print("1. B meets the main block (\"Later\" for the 2-of-2), then locks")
        os.environ["PHYLAX_EMU_PIN_KEYS"] = GOOD
        key_b.start()
        os.environ.pop("PHYLAX_EMU_PIN_KEYS")
        load_device(key_b.ctx.get_session(passphrase=None), mnemonic=s.KEY_SEED, pin=KEY_PIN,
                    passphrase_protection=False, label="Key block B")
        main_blk = start_main("later")
        wait_key_by_fp(main_blk.session, KEY_FP, lambda k: not k.locked, "B, unlocked")
        time.sleep(OFFER_WAIT_S)
        check(not wallets(main_blk.session).wallets, "nothing saved (\"Later\")",
              "a wallet was saved")
        key_b.ctx.get_session(passphrase=None).call(messages.LockDevice(), expect=messages.Success)
        wait_key_by_fp(main_blk.session, KEY_FP, lambda k: k.locked, "B to be locked")

        print("2. A joins, the main block restarts: main + A is new, but B is locked")
        main_blk.stop()
        key_a.start()
        main_blk = start_main(None)
        s.wait_for_blocks(main_blk.session, lambda bl: len(s.key_blocks(bl)) == 2, what="A and B")
        time.sleep(OFFER_WAIT_S)
        w = wallets(main_blk.session)
        check(not w.wallets, "no wallet offered while B is locked",
              f"offered anyway: {[(x.name, x.threshold) for x in w.wallets]}")
        check(key_by_fp(main_blk.session, KEY_FP).locked, "B is still locked", "B unlocked")

        print("3. The PC wakes B: the 3-of-3 is offered and saved")
        b = key_by_fp(main_blk.session, KEY_FP)
        ret = main_blk.session.call_raw(pm.PhylaxUnlockBlock(address=b.address))
        check(isinstance(ret, messages.Success), "unlock started", f"answer: {ret}")
        w = wait_wallets(main_blk.session, lambda w: len(w.wallets) >= 1, "a wallet", )
        check([x.threshold for x in w.wallets] == [3], "only the 3-of-3 is saved",
              f"saved: {[(x.name, x.threshold) for x in w.wallets]}")

        print("\nLOCKED OFFER TEST PASSED")
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
