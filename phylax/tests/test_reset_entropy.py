#!/usr/bin/env python3
"""PHYLAX Blocks - your own randomness (coin flips or dice) when a block
creates its own wallet (ResetDevice from the PC, e.g. Suite).

After "create a new wallet?" (and the PIN) the block asks "Add your own
randomness?" (Skip / Add) and "Coin flips or dice?", the same screens as the
key-block setup (test_entropy.py). The PC just waits (ButtonRequest). The
hashed flips or rolls are mixed into each round of the block's own entropy,
so Suite's entropy check (commit, reveal, compare xpubs) still passes.

  --mode coin:    Add, Coin, one flip undone, 256 flips; 3 entropy-check rounds.
  --mode dice:    Add, Dice, 99 rolls; no entropy check.
  --mode skip:    Skip; 2 entropy-check rounds.
  --mode short:   Add, Coin, 255 flips: the wallet must not be created.
  --mode cancel:  the PC cancels while the block asks: no wallet, block answers.
  --mode noscript: no button script (old tests): not asked at all.

Each finished run checks that the stored seed is
BIP39(SHA256(the entropy the block committed to || the PC's entropy)).

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_reset_entropy.py --headless --mode coin
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spike2b as s  # noqa: E402

from mnemonic import Mnemonic  # noqa: E402
from trezorlib import device, messages  # noqa: E402
from trezorlib.exceptions import Cancelled, TrezorFailure  # noqa: E402

SCRIPTS = {
    "coin": "RL" + "LB" + "LR" * 128,
    "dice": "RR" + "B" * 98 + "RB",
    "skip": "L",
    "short": "RL" + "LR" * 127 + "L",
    "cancel": "R",  # Add, then the PC cancels on "Coin flips or dice?"
    "noscript": None,
}
CHECK_ROUNDS = {"coin": 3, "dice": 0, "skip": 2, "noscript": 0}
PC_ENTROPY = bytes(range(32))


def check(cond, ok, bad):
    if not cond:
        sys.exit(f"  FAIL  {bad}")
    print(f"  PASS  {ok}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--elf", default="firmware/trezor.elf")
    ap.add_argument("--headless", action="store_true")
    ap.add_argument("--mode", choices=sorted(SCRIPTS), default="coin")
    args = ap.parse_args()
    elf = Path(args.elf).resolve()
    if not elf.exists():
        sys.exit(f"Emulator not found: {elf}")
    if s.port_answers(s.MAIN_PORT):
        sys.exit(f"Something already answers on port {s.MAIN_PORT}. Close running emulators first.")

    workdir = Path(tempfile.mkdtemp(prefix="phylax-reset-entropy-"))
    blk = s.Block("Main block", None, s.MAIN_PORT, "main", elf, workdir,
                  args.headless, autoconfirm=True, node=0)
    ok = False
    try:
        script = SCRIPTS[args.mode]
        print(f"1. A main block without a seed; the PC creates a wallet ({args.mode})")
        if script is None:
            os.environ.pop("PHYLAX_EMU_PIN_KEYS", None)
        else:
            os.environ["PHYLAX_EMU_PIN_KEYS"] = script
        blk.start()
        os.environ.pop("PHYLAX_EMU_PIN_KEYS", None)
        session = blk.ctx.get_session(passphrase=None)
        check(not session.features.initialized, "no seed yet", "already initialized")

        if args.mode in ("short", "cancel"):
            outcome = {}

            def run():
                try:
                    device.setup(session, strength=256, passphrase_protection=False,
                                 skip_backup=True, entropy_check_count=0,
                                 _get_entropy=lambda: PC_ENTROPY)
                    outcome["result"] = "created"
                except (TrezorFailure, Cancelled) as e:
                    outcome["result"] = e
                except Exception as e:  # noqa: BLE001  (transport closed etc.)
                    outcome["result"] = e

            t = threading.Thread(target=run, daemon=True)
            t.start()
            if args.mode == "short":
                t.join(25)
                check(t.is_alive(), "still waiting after 255 flips (no wallet yet)",
                      f"setup ended: {outcome.get('result')}")
                print("\nRESET ENTROPY TEST (short) PASSED")
                ok = True
                return
            time.sleep(3)  # the block now asks "Coin flips or dice?"
            check(t.is_alive(), "the PC waits while the block asks", "setup ended early")
            session.cancel()
            t.join(15)
            res = outcome.get("result")
            check(isinstance(res, (TrezorFailure, Cancelled)),
                  f"setup cancelled ({type(res).__name__})", f"unexpected outcome {res!r}")
            fresh = blk.ctx.get_session(passphrase=None)
            check(not fresh.features.initialized, "still no seed", "a seed was stored")
            # The block is free again: it answers messages as usual.
            fresh.call(messages.Ping(message="still here"), expect=messages.Success)
            print("  PASS  the block answers the PC again")
            print("\nRESET ENTROPY TEST (cancel) PASSED")
            ok = True
            return

        # Read the entropy the block commits to in the last round (DebugLink),
        # just before the PC's EntropyAck.
        committed = {}

        def get_entropy():
            committed["int"] = blk.ctx.debug.state().reset_entropy
            return PC_ENTROPY

        rounds = CHECK_ROUNDS[args.mode]
        print(f"2. Wallet creation, {rounds} entropy-check round(s)")
        device.setup(session, strength=256, passphrase_protection=False, skip_backup=True,
                     entropy_check_count=rounds, _get_entropy=get_entropy)
        session.refresh_features()
        check(session.features.initialized, "wallet created", "no wallet")
        if rounds:
            print("  PASS  every entropy-check round verified by the PC (trezorlib)")

        seed = blk.ctx.debug.state().mnemonic_secret.decode()
        expect = Mnemonic("english").to_mnemonic(
            hashlib.sha256(committed["int"] + PC_ENTROPY).digest())
        check(len(seed.split()) == 24, "24-word seed", f"{len(seed.split())} words")
        check(seed == expect, "seed = BIP39(SHA256(block entropy || PC entropy))",
              "seed does not match the committed entropy")
        print(f"\nRESET ENTROPY TEST ({args.mode}) PASSED")
        ok = True
    finally:
        blk.stop()
        if ok:
            shutil.rmtree(workdir, ignore_errors=True)
        else:
            print(f"Emulator logs kept in {workdir}")


if __name__ == "__main__":
    main()
