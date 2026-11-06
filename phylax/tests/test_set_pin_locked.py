#!/usr/bin/env python3
"""PHYLAX Snap - set a PIN on a block without a PIN after it auto-locked.

Found on hardware: "trezorctl set pin" on a main block without a PIN failed with
"The new PIN must be different from your wipe code." (no wipe code was set)
once the board had auto-locked: changing the PIN needs unlocked storage, and a
board without a PIN was not unlocked first. Now it is unlocked silently (as any
other request does), and a failed change no longer blames the wipe code.

  1. Main block without a PIN, locked (as after auto-lock).
  2. ChangePin (set): the new PIN is set.
  3. Locked again: the new PIN is asked and unlocks it.

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_set_pin_locked.py --headless
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
from test_wallets import check  # noqa: E402
from trezorlib import messages  # noqa: E402
from trezorlib.tools import parse_path  # noqa: E402

NEW_PIN = "2468"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--elf", default="firmware/trezor.elf")
    ap.add_argument("--headless", action="store_true")
    args = ap.parse_args()
    elf = Path(args.elf).resolve()
    if not elf.exists():
        sys.exit(f"Emulator not found: {elf}")
    if s.port_answers(s.MAIN_PORT):
        sys.exit(f"Something already answers on port {s.MAIN_PORT}. Close running emulators first.")

    workdir = Path(tempfile.mkdtemp(prefix="phylax-set-pin-"))
    main_blk = s.Block("Main block", s.MAIN_SEED, s.MAIN_PORT, "main", elf, workdir,
                       args.headless, autoconfirm=True, node=0)
    ok = False
    try:
        print("1. Main block without a PIN, locked")
        main_blk.start()
        # Restarted so the emulator confirms every screen (here "set new PIN?"); not while
        # the seed is loaded, which that option would block.
        main_blk.stop()
        os.environ["PHYLAX_EMU_CONFIRM_ALL"] = "1"
        main_blk.start()
        pc = main_blk.session
        pc.call(messages.LockDevice(), expect=messages.Success)

        print("2. Set a PIN")
        ret = pc.call_raw(messages.ChangePin(remove=False))
        while isinstance(ret, messages.ButtonRequest):
            ret = pc.call_raw(messages.ButtonAck())
        check(isinstance(ret, messages.PinMatrixRequest), "asks for the new PIN",
              f"no new PIN asked: {ret}")
        ret = pc.call_raw(messages.PinMatrixAck(pin=main_blk.ctx.debug.encode_pin(NEW_PIN)))
        check(isinstance(ret, messages.PinMatrixRequest), "asks for it again",
              f"no second PIN asked: {ret}")
        ret = pc.call_raw(messages.PinMatrixAck(pin=main_blk.ctx.debug.encode_pin(NEW_PIN)))
        check(isinstance(ret, messages.Success), "PIN set",
              f"PIN not set: {getattr(ret, 'message', ret)}")

        print("3. Locked again: the new PIN unlocks it")
        pc.call(messages.LockDevice(), expect=messages.Success)
        ret = pc.call_raw(messages.GetPublicKey(address_n=parse_path("m/84h/1h/0h"),
                                                coin_name=s.COIN))
        check(isinstance(ret, messages.PinMatrixRequest), "the PIN is asked",
              f"no PIN asked: {ret}")
        ret = pc.call_raw(messages.PinMatrixAck(pin=main_blk.ctx.debug.encode_pin(NEW_PIN)))
        check(isinstance(ret, messages.PublicKey), "the new PIN unlocks it", f"not unlocked: {ret}")

        print("\nSET PIN WHEN LOCKED TEST PASSED")
        ok = True
    finally:
        main_blk.stop()
        if ok:
            shutil.rmtree(workdir, ignore_errors=True)
        else:
            print(f"Emulator logs kept in {workdir}")


if __name__ == "__main__":
    main()
