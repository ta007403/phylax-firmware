#!/usr/bin/env python3
"""PHYLAX Snap - a locked key block is unlocked from the main block.

A key block locks itself after a while (Trezor auto-lock). Over the bus it
then still says who it is: its seed fingerprint is kept in its public
storage from its last unlock. The main block sees that a saved wallet needs
it and asks "Key block 73c5da0a is locked. Unlock it ...?"; on Unlock the PIN
is entered on the key block's own buttons (LOCAL_UI), never on the PC.

  1. Main block + key block (PIN 1357): the 2-of-2 is saved.
  2. The key block is locked: discovery still shows its fingerprint, as
     locked, and the wallet is not counted as ready.
  3. The main block (auto-confirm = "Unlock") unlocks it: the key block
     takes the PIN on its own buttons (scripted), and is unlocked again.

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_unlock.py --headless
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
from test_setup import GOOD  # noqa: E402  (PIN 1357 on the carousel)
from test_wallets import check, wait_wallets  # noqa: E402
from trezorlib import messages  # noqa: E402
from trezorlib.debuglink import load_device  # noqa: E402

KEY_PIN = "1357"
KEY_FP = 0x73C5DA0A  # "abandon ... about"


def key_entry(main_session):
    blocks = s.get_blocks(main_session)
    keys = [b for b in blocks if int(b.kind) == 1]
    return keys[0] if keys else None


def wait_key(main_session, cond, what, timeout=30.0):
    deadline = time.time() + timeout
    k = None
    while time.time() < deadline:
        k = key_entry(main_session)
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
    for p in (s.MAIN_PORT, s.KEY_PORT):
        if s.port_answers(p):
            sys.exit(f"Something already answers on port {p}. Close running emulators first.")

    workdir = Path(tempfile.mkdtemp(prefix="phylax-unlock-"))
    main_blk = s.Block("Main block", s.MAIN_SEED, s.MAIN_PORT, "main", elf, workdir,
                       args.headless, autoconfirm=True, node=0)
    key = s.Block("Key block", None, s.KEY_PORT, "key", elf, workdir,
                  args.headless, autoconfirm=True, node=1)
    ok = False
    try:
        print("1. Main block + key block with a PIN: the 2-of-2 is saved")
        os.environ["PHYLAX_EMU_PIN_KEYS"] = GOOD  # the key block's PIN, on its buttons
        key.start()
        os.environ.pop("PHYLAX_EMU_PIN_KEYS")
        load_device(key.ctx.get_session(passphrase=None), mnemonic=s.KEY_SEED, pin=KEY_PIN,
                    passphrase_protection=False, label="Key block")
        main_blk.start()
        w = wait_wallets(main_blk.session, lambda w: len(w.wallets) == 1, "the 2-of-2")
        k = wait_key(main_blk.session, lambda k: not k.locked and k.fingerprint == KEY_FP,
                     "the key block, unlocked")
        check(True, f"key block {k.fingerprint:08x} unlocked in the stack", "")

        print("2. The key block locks itself: still known, but locked")
        key.ctx.get_session(passphrase=None).call(messages.LockDevice(), expect=messages.Success)
        k = wait_key(main_blk.session, lambda k: k.locked, "the key block to be locked")
        check(k.fingerprint == KEY_FP, f"locked key block still reports {KEY_FP:08x}",
              f"locked key block reports {k.fingerprint}")

        print("3. The main block unlocks it (PIN on the key block's own buttons)")
        k = wait_key(main_blk.session, lambda k: not k.locked, "the key block to be unlocked",
                     timeout=60)
        check(k.fingerprint == KEY_FP, "key block unlocked again", "wrong key block")
        w = wait_wallets(main_blk.session, lambda w: w.wallets and w.wallets[0].complete,
                         "the wallet")
        check(len(w.wallets) == 1, "the 2-of-2 is still the only wallet", "wallets changed")

        print("\nUNLOCK TEST PASSED")
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
