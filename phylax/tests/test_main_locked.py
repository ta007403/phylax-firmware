#!/usr/bin/env python3
"""PHYLAX Snap - no wallet without the main block (main block locked).

Found on hardware: after a replug, before the main block's PIN was entered,
the main block offered "3-of-3 #2" and saved it: a wallet of the 3 unlocked
key blocks only. A locked main block did not count itself as present, but
still offered a wallet of the other blocks. PHYLAX Suite then refused it
("The connected main block ... is not part of this wallet"). Every wallet
needs the main block: no offer while it is locked.

  1. Main block with a PIN, locked; key blocks A and B (no PIN) join.
     Nothing is offered (before: a 2-of-2 of A and B was saved).
  2. The main block's PIN is entered: the 3-of-3 of main + A + B is offered
     and saved, with the main block's key in it.

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_main_locked.py --headless
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spike2b as s  # noqa: E402
from test_wallets import PORT_B, SEED_B, check, wait_wallets, wallets  # noqa: E402
from trezorlib import messages  # noqa: E402
from trezorlib.debuglink import load_device  # noqa: E402
from trezorlib.tools import parse_path  # noqa: E402

MAIN_PIN = "2468"
# Long enough for the main block to see the blocks, let the stack settle and
# offer (with auto-confirm it saves at once).
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

    workdir = Path(tempfile.mkdtemp(prefix="phylax-main-locked-"))
    # seed None: the main block's seed is loaded below, with a PIN
    main_blk = s.Block("Main block", None, s.MAIN_PORT, "main", elf, workdir,
                       args.headless, autoconfirm=True, node=0)
    key_a = s.Block("Key block A", SEED_B, PORT_B, "key", elf, workdir,
                    args.headless, autoconfirm=True, node=2)
    key_b = s.Block("Key block B", s.KEY_SEED, s.KEY_PORT, "key", elf, workdir,
                    args.headless, autoconfirm=True, node=1)
    blocks = [main_blk, key_a, key_b]
    ok = False
    try:
        print("1. Main block locked (PIN), key blocks A and B unlocked: nothing offered")
        main_blk.start()
        load_device(main_blk.ctx.get_session(passphrase=None), mnemonic=s.MAIN_SEED,
                    pin=MAIN_PIN, passphrase_protection=False, label="Main block")
        main_blk.stop()  # restarted: locked, as after a replug
        key_a.start()
        key_b.start()
        main_blk.start()
        pc = main_blk.ctx.get_session(passphrase=None)
        s.wait_for_blocks(pc, lambda bl: len(s.key_blocks(bl)) == 2, what="A and B")
        me = next(b for b in s.get_blocks(pc) if b.address == 1)
        check(me.locked, "the main block is locked", "the main block is unlocked")
        time.sleep(OFFER_WAIT_S)
        w = wallets(pc)
        check(not w.wallets, "no wallet offered while the main block is locked",
              f"offered anyway: {[(x.name, x.threshold) for x in w.wallets]}")

        print("2. The main block's PIN: the 3-of-3 with the main block is saved")
        ret = pc.call_raw(messages.GetPublicKey(address_n=parse_path(s.ACCOUNT),
                                                coin_name=s.COIN))
        check(isinstance(ret, messages.PinMatrixRequest), "the PIN is asked",
              f"no PIN asked: {ret}")
        ret = pc.call_raw(messages.PinMatrixAck(pin=main_blk.ctx.debug.encode_pin(MAIN_PIN)))
        check(isinstance(ret, messages.PublicKey), "unlocked", f"not unlocked: {ret}")
        main_fp = next(b for b in s.get_blocks(pc) if b.address == 1).fingerprint
        w = wait_wallets(pc, lambda w: len(w.wallets) >= 1, "a wallet", timeout=30)
        check([x.threshold for x in w.wallets] == [3], "only the 3-of-3 is saved",
              f"saved: {[(x.name, x.threshold) for x in w.wallets]}")
        check(main_fp in w.wallets[0].fingerprints, "the main block's key is in it",
              f"main block {main_fp:08x} not in {list(w.wallets[0].fingerprints)}")

        print("\nMAIN LOCKED TEST PASSED")
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
