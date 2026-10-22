#!/usr/bin/env python3
"""PHYLAX Blocks - set up a new key block from the main block's buttons, no PC.

  1. A brand-new key block joins: the main block asks "New key block found.
     Set it up?" and (emulator: autoconfirm) says yes. The PC does nothing
     but watch.
  2. The PIN is chosen on the KEY block's own buttons (left/right through
     1..9, OK, DEL; both buttons = select). First attempt: the second PIN is
     mistyped, the key block refuses, the main block asks again.
  3. Second attempt: same PIN twice, 24 words shown on the key block; the
     main block supplied the entropy (no PC). Discovery shows its new
     fingerprint, and the main block offers and saves "2-of-2 #1".
  4. The PIN chosen on the buttons is the one Suite's grid unlocks: lock the
     key block and unlock it through the main block with the same digits.

The emulated key block's buttons are replaced by PHYLAX_EMU_PIN_KEYS (L, R,
B = both); its other prompts confirm themselves (PHYLAX_EMU_AUTOCONFIRM=1).

Run from the legacy folder (emulator built with
EMULATOR=1 DEBUG_LINK=1 BITCOIN_ONLY=1):

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_setup.py --headless
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
from test_keyblocks import check, fingerprint, key_entries, select, wait_key  # noqa: E402
from test_wallets import wait_wallets  # noqa: E402

from trezorlib import messages  # noqa: E402
from trezorlib.tools import parse_path  # noqa: E402

PIN = "1357"

# The carousel is 1..9, OK, DEL and (in tests) starts on "1".
#   1357 with a stray 9 deleted, then OK  /  "13", OK (mistyped)
ATTEMPT_1 = ("B" + "RRB" * 4 + "RRB" + "LB") + ("B" + "RRB" + "RRRRRRRB")
#   1357, OK  -- twice
GOOD = "B" + "RRB" * 3 + "RRRB"
ATTEMPT_2 = GOOD + GOOD


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

    workdir = Path(tempfile.mkdtemp(prefix="phylax-setup-"))
    main_blk = s.Block("Main block", s.MAIN_SEED, s.MAIN_PORT, "main", elf, workdir,
                       args.headless, autoconfirm=True, node=0)
    key = s.Block("Key block 2", None, s.KEY_PORT, "key", elf, workdir,
                  args.headless, autoconfirm=True, node=1)
    os.environ["PHYLAX_EMU_PIN_KEYS"] = ATTEMPT_1 + ATTEMPT_2
    ok = False
    try:
        print("1. A brand-new key block joins; the main block offers to set it up")
        key.start()
        os.environ.pop("PHYLAX_EMU_PIN_KEYS")  # the main block has no script
        main_blk.start()
        k = wait_key(main_blk.session, lambda k: True, "the key block")
        check(k.initialized is False, "found, no seed", "key block reported wrongly")

        print("2-3. PIN on its own buttons (one mistyped attempt), 24 words, no PC")
        k = wait_key(main_blk.session, lambda k: k.fingerprint is not None,
                     "the key block to be set up", timeout=60)
        seed = key.ctx.debug.state().mnemonic_secret.decode()
        check(len(seed.split()) == 24, "24-word seed made on the key block",
              f"{len(seed.split())} words")
        check(k.fingerprint == fingerprint(seed),
              f"discovery shows its new fingerprint {k.fingerprint:08x}", "wrong fingerprint")
        w = wait_wallets(main_blk.session, lambda w: len(w.wallets) == 1,
                         "the 2-of-2 to be saved")
        two = w.wallets[0]
        check(two.name == "2-of-2 #1" and k.fingerprint in two.fingerprints and two.complete,
              "\"2-of-2 #1\" offered and saved with the new key block",
              f"unexpected wallet {two.describe()}")

        print("4. The PIN chosen on its buttons unlocks it from the PC's grid")
        pc = main_blk.ctx.get_seedless_session()
        select(pc, k.address)
        check(isinstance(pc.call_raw(messages.LockDevice()), messages.Success),
              "locked", "lock failed")
        ret = pc.call_raw(messages.GetPublicKey(address_n=parse_path(s.ACCOUNT),
                                                coin_name=s.COIN,
                                                script_type=messages.InputScriptType.SPENDMULTISIG))
        check(isinstance(ret, messages.PinMatrixRequest), "it asks for its PIN on the PC",
              f"no PIN asked: {ret}")
        ret = pc.call_raw(messages.PinMatrixAck(pin=key.ctx.debug.encode_pin("1358")))
        check(isinstance(ret, messages.Failure), "a wrong PIN is refused", f"accepted: {ret}")
        ret = pc.call_raw(messages.GetPublicKey(address_n=parse_path(s.ACCOUNT),
                                                coin_name=s.COIN,
                                                script_type=messages.InputScriptType.SPENDMULTISIG))
        if isinstance(ret, messages.PinMatrixRequest):
            ret = pc.call_raw(messages.PinMatrixAck(pin=key.ctx.debug.encode_pin(PIN)))
        check(isinstance(ret, messages.PublicKey), f"unlocked with {PIN}, the PIN set on its buttons",
              f"unlock failed: {ret}")
        select(pc, 1)

        print("\nSETUP TEST PASSED")
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
