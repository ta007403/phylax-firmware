#!/usr/bin/env python3
"""PHYLAX Snap - wake up (unlock) a locked key block from the PC.

PHYLAX Suite's "Wake up" sends PhylaxUnlockBlock(address) to the main block,
which starts its unlock for that key block: the PIN goes on the key block's
own buttons, as after "Unlock" on the main block's screen.

Found on hardware: a key block locked before it ever reported its seed
fingerprint (it is stored at its first unlock with this firmware) looked
"missing", and nothing offered to unlock it.

  1. A key block with PIN 1357 is locked before it ever met the main block:
     the stack shows it locked, without a fingerprint.
  2. Wrong requests are refused: no block at 0x55; the main block itself.
  3. PhylaxUnlockBlock(0x10): the key block takes its PIN on its buttons
     (scripted) and reports its fingerprint, unlocked.
  4. Asking again is refused: it is not locked any more.

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_wake.py --headless
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
from test_unlock import KEY_FP, KEY_PIN, key_entry, wait_key  # noqa: E402
from test_wallets import check  # noqa: E402
from trezorlib import messages  # noqa: E402
from trezorlib.debuglink import load_device  # noqa: E402


def unlock(session, address):
    return session.call_raw(pm.PhylaxUnlockBlock(address=address))


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

    workdir = Path(tempfile.mkdtemp(prefix="phylax-wake-"))
    # No auto-confirm on the main block: its own "Unlock it?" must not answer itself.
    main_blk = s.Block("Main block", s.MAIN_SEED, s.MAIN_PORT, "main", elf, workdir,
                       args.headless, autoconfirm=False, node=0)
    key = s.Block("Key block", None, s.KEY_PORT, "key", elf, workdir,
                  args.headless, autoconfirm=True, node=1)
    ok = False
    try:
        print("1. A key block locked before it ever reported its fingerprint")
        os.environ["PHYLAX_EMU_PIN_KEYS"] = GOOD  # its PIN, on its own buttons
        key.start()
        os.environ.pop("PHYLAX_EMU_PIN_KEYS")
        seedless = key.ctx.get_session(passphrase=None)
        load_device(seedless, mnemonic=s.KEY_SEED, pin=KEY_PIN, passphrase_protection=False,
                    label="Key block")
        key.ctx.get_session(passphrase=None).call(messages.LockDevice(), expect=messages.Success)
        main_blk.start()
        pc = main_blk.session
        k = wait_key(pc, lambda k: k.locked, "the locked key block")
        check(k.fingerprint is None, "stack: locked, no fingerprint yet",
              f"it reports fingerprint {k.fingerprint}")

        print("2. Wrong requests are refused")
        ret = unlock(pc, 0x55)
        check(isinstance(ret, messages.Failure), "no block at 0x55: refused", f"answer: {ret}")
        ret = unlock(pc, 1)
        check(isinstance(ret, messages.Failure), "the main block itself: refused",
              f"answer: {ret}")

        print("3. Wake it up from the PC; its PIN on its own buttons")
        ret = unlock(pc, k.address)
        check(isinstance(ret, messages.Success), f"started: {getattr(ret, 'message', '')}",
              f"not started: {ret}")
        k = wait_key(pc, lambda k: not k.locked, "the key block to be unlocked", timeout=60)
        k = wait_key(pc, lambda k: k.fingerprint == KEY_FP, "its fingerprint", timeout=30)
        check(True, f"unlocked, reports {k.fingerprint:08x}", "")

        print("4. Asking again: it is not locked any more")
        time.sleep(1)
        ret = unlock(pc, k.address)
        check(isinstance(ret, messages.Failure) and "not locked" in (ret.message or ""),
              "refused: not locked", f"answer: {ret}")
        check(key_entry(pc) is not None, "the key block is still in the stack", "it is gone")

        print("\nWAKE TEST PASSED")
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
