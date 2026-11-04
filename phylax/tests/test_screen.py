#!/usr/bin/env python3
"""PHYLAX Snap - the main block's screen stays put while the PC polls it.

PHYLAX Suite asks the main block for the stack every 2-3 s. Every PC
request ends with the home screen, and on a main block the home screen is
the PHYLAX prompt or status, so the screen must not change:

1. No auto-confirm: "New 2-of-2 wallet?" is on screen and stays there,
   pixel for pixel, through Connect's state address, PhylaxGetBlocks and
   GetFeatures requests.
2. Auto-confirm: the wallet is created; "2-of-2 #1 is ready." stays on
   screen through the same requests.

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_screen.py --headless
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
from test_wallets import check, wait_wallets  # noqa: E402
from trezorlib import btc, messages  # noqa: E402
from trezorlib.tools import parse_path  # noqa: E402


def screen(blk):
    return bytes(blk.ctx.debug.state().layout)


def wait_screen_stable(blk, seconds=4.0):
    """The screen once it stopped changing for 1.5 s."""
    deadline = time.time() + seconds + 10
    last, since = screen(blk), time.time()
    while time.time() < deadline:
        time.sleep(0.2)
        now = screen(blk)
        if now != last:
            last, since = now, time.time()
        elif time.time() - since >= 1.5:
            return now
    raise TimeoutError("the screen kept changing")


def poll_like_suite(blk, rounds=6):
    """What PHYLAX Suite's stack page sends every few seconds: Connect's
    device-state address (GetAddress m/44'/1'/0'/0/0, no display; it ends
    with the home screen), PhylaxGetBlocks and GetFeatures. The screen right
    after each request."""
    shots = []
    for _ in range(rounds):
        btc.get_address(blk.session, "Testnet", parse_path("m/44h/1h/0h/0/0"))
        shots.append(screen(blk))
        blk.session.call(pm.PhylaxGetBlocks(), expect=pm.PhylaxBlocks)
        shots.append(screen(blk))
        blk.session.call(messages.GetFeatures(), expect=messages.Features)
        shots.append(screen(blk))
        time.sleep(0.3)
    return shots


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

    ok = False
    for autoconfirm in (False, True):
        workdir = Path(tempfile.mkdtemp(prefix="phylax-screen-"))
        main_blk = s.Block("Main block", s.MAIN_SEED, s.MAIN_PORT, "main", elf, workdir,
                           args.headless, autoconfirm=autoconfirm, node=0)
        key = s.Block("Key block", s.KEY_SEED, s.KEY_PORT, "key", elf, workdir,
                      args.headless, autoconfirm=True, node=1)
        ok = False
        try:
            key.start()
            main_blk.start()
            if not autoconfirm:
                print("1. \"New 2-of-2 wallet?\" stays on screen while the PC polls")
                time.sleep(8)  # found, settled, xpubs read, prompt shown
            else:
                print("2. \"2-of-2 #1 is ready.\" stays on screen while the PC polls")
                wait_wallets(main_blk.session, lambda w: len(w.wallets) == 1, "the 2-of-2")
            before = wait_screen_stable(main_blk)
            shots = poll_like_suite(main_blk)
            changed = sum(1 for shot in shots if shot != before)
            check(changed == 0, f"screen identical after all {len(shots)} requests",
                  f"screen changed after {changed} of {len(shots)} requests")
            if not autoconfirm:
                w = main_blk.session.call(pm.PhylaxGetWallets(), expect=pm.PhylaxWallets)
                check(len(w.wallets) == 0, "no wallet created without a button press",
                      "a wallet was created although nobody pressed a button")
            ok = True
        finally:
            for blk in (main_blk, key):
                blk.stop()
            if ok:
                shutil.rmtree(workdir, ignore_errors=True)
            else:
                print(f"Emulator logs kept in {workdir}")
        if not ok:
            sys.exit(1)
    print("\nSCREEN TEST PASSED")


if __name__ == "__main__":
    main()
