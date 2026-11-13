#!/usr/bin/env python3
"""PHYLAX Snap - the key block the main block asks about says so on its screen.

Owner's request: with several locked key blocks, the main block asks
"Key block 2b69c617 is locked. Unlock it?" for one after the other, and
every key block showed only the PHYLAX logo: which one is meant? Now that
key block shows "Main block asks: Unlock this block?" with its fingerprint.

  1. Main block + key blocks A (PIN 1357) and B (no PIN): the 3-of-3 is
     saved. Their home screens are noted.
  2. The main block restarts without auto-confirm, A is locked: the main
     block asks "Unlock it?" for A. A's screen changes (the request), and
     stays so (repeated every 2 s); B's does not.
  3. The PC wakes A (PhylaxUnlockBlock): A takes its PIN and is unlocked;
     its screen is its home screen again.
  4. A is locked again and the main block is stopped while it asks: A shows
     its home screen again by itself within 6 s.
  5. A blank key block C instead of B, the main block asks "Set it up?":
     C shows "Main block asks: Set up this block?".

The key block screens are saved as PNG files (--shots DIR).

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_attention.py --headless
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import sys
import tempfile
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import phylax_msgs as pm  # noqa: E402
import spike2b as s  # noqa: E402
from test_setup import GOOD  # noqa: E402  (PIN 1357 on the carousel)
from test_unlock import KEY_FP, KEY_PIN, wait_key  # noqa: E402
from test_wallets import PORT_B, SEED_B, check, wait_wallets  # noqa: E402
from trezorlib import messages  # noqa: E402
from trezorlib.debuglink import load_device  # noqa: E402

WIDTH, HEIGHT = 128, 64


def screen(blk):
    return bytes(blk.ctx.debug.state().layout)


def wait_screen(blk, cond, what, timeout=20.0):
    deadline = time.time() + timeout
    shot = screen(blk)
    while time.time() < deadline:
        shot = screen(blk)
        if cond(shot):
            return shot
        time.sleep(0.2)
    raise TimeoutError(f"waited {timeout:.0f} s for {what}")


def save_png(buf: bytes, path: Path, scale: int = 4) -> None:
    """The OLED buffer (legacy oled.c layout) as a white-on-blue PNG."""
    size = len(buf)

    def lit(x, y):
        offset = size - 1 - x - (y // 8) * WIDTH
        return bool(buf[offset] & (1 << (7 - y % 8)))

    rows = []
    for y in range(HEIGHT * scale):
        row = bytearray([0])
        for x in range(WIDTH * scale):
            row += b"\xff\xff\xff" if lit(x // scale, y // scale) else b"\x10\x18\x30"
        rows.append(bytes(row))
    raw = zlib.compress(b"".join(rows), 9)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    header = struct.pack(">IIBBBBB", WIDTH * scale, HEIGHT * scale, 8, 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", raw) + chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--elf", default="firmware/trezor.elf")
    ap.add_argument("--headless", action="store_true")
    ap.add_argument("--shots", default=None, help="folder for PNGs of the key block screens")
    args = ap.parse_args()
    elf = Path(args.elf).resolve()
    if not elf.exists():
        sys.exit(f"Emulator not found: {elf}")
    for p in (s.MAIN_PORT, s.KEY_PORT, PORT_B):
        if s.port_answers(p):
            sys.exit(f"Something already answers on port {p}. Close running emulators first.")
    shots = Path(args.shots) if args.shots else None
    if shots:
        shots.mkdir(parents=True, exist_ok=True)

    workdir = Path(tempfile.mkdtemp(prefix="phylax-attention-"))
    blocks = []

    def start_main(autoconfirm):
        blk = s.Block("Main block", s.MAIN_SEED, s.MAIN_PORT, "main", elf, workdir,
                      args.headless, autoconfirm=autoconfirm, node=0)
        blocks.append(blk)
        blk.start()
        return blk

    key_a = s.Block("Key block A", None, s.KEY_PORT, "key", elf, workdir,
                    args.headless, autoconfirm=True, node=1)
    key_b = s.Block("Key block B", SEED_B, PORT_B, "key", elf, workdir,
                    args.headless, autoconfirm=True, node=2)
    blocks += [key_a, key_b]
    ok = False
    try:
        print("1. Main block + A (PIN) + B: the 3-of-3 is saved")
        # A's PIN on its own buttons, twice (two unlocks in this test)
        os.environ["PHYLAX_EMU_PIN_KEYS"] = GOOD + GOOD
        key_a.start()
        os.environ.pop("PHYLAX_EMU_PIN_KEYS")
        load_device(key_a.ctx.get_session(passphrase=None), mnemonic=s.KEY_SEED, pin=KEY_PIN,
                    passphrase_protection=False, label="Key block A")
        key_b.start()
        main_blk = start_main(autoconfirm=True)
        wait_wallets(main_blk.session, lambda w: len(w.wallets) == 1 and w.wallets[0].threshold == 3,
                     "the 3-of-3")
        time.sleep(1)
        home_a, home_b = screen(key_a), screen(key_b)
        if shots:
            save_png(home_a, shots / "key-a-home.png")

        print("2. Main block without auto-confirm; A locked: it asks about A")
        main_blk.stop()
        main_blk = start_main(autoconfirm=False)
        key_a.ctx.get_session(passphrase=None).call(messages.LockDevice(), expect=messages.Success)
        wait_key(main_blk.session, lambda k: k.locked and k.fingerprint == KEY_FP, "A, locked")
        asked = wait_screen(key_a, lambda shot: shot != home_a, "A to show the request")
        if shots:
            save_png(asked, shots / "key-a-unlock-request.png")
        time.sleep(PHYLAX_REPEAT_CHECK_S)
        check(screen(key_a) == asked, "A shows the request, and keeps showing it",
              "A's screen changed again")
        check(screen(key_b) == home_b, "B's screen did not change", "B's screen changed")

        print("3. The PC wakes A: PIN on A, unlocked, home screen again")
        a = next(b for b in s.get_blocks(main_blk.session) if b.fingerprint == KEY_FP)
        ret = main_blk.session.call_raw(pm.PhylaxUnlockBlock(address=a.address))
        check(isinstance(ret, messages.Success), "unlock started", f"answer: {ret}")
        wait_key(main_blk.session, lambda k: not k.locked and k.fingerprint == KEY_FP,
                 "A, unlocked", timeout=60)
        wait_screen(key_a, lambda shot: shot == home_a, "A's home screen", timeout=10)
        check(True, "A shows its home screen again", "")

        print("4. A locked again, the main block stops while asking: A clears by itself")
        key_a.ctx.get_session(passphrase=None).call(messages.LockDevice(), expect=messages.Success)
        # (right after locking, the screen may be blank for a moment)
        asked_again = wait_screen(key_a, lambda shot: shot == asked, "A to show the request again")
        main_blk.stop()
        started = time.time()
        try:
            after = wait_screen(key_a, lambda shot: shot != asked_again, "the request to go", timeout=15)
        finally:
            if shots:
                save_png(screen(key_a), shots / "key-a-after-main-gone.png")
        took = time.time() - started
        check(took < 8, f"the request went after {took:.1f} s", f"took {took:.1f} s")
        check(after == home_a, "A shows its home screen", "A shows something else")

        print("5. A blank key block C: the main block asks to set it up")
        key_b.stop()
        key_c = s.Block("Key block C", None, PORT_B, "key", elf, workdir,
                        args.headless, autoconfirm=True, node=3)
        blocks.append(key_c)
        key_c.start()
        time.sleep(2)
        home_c = screen(key_c)
        main_blk = start_main(autoconfirm=False)
        wait_screen(key_c, lambda shot: shot != home_c, "C to show the request", timeout=30)
        time.sleep(1)
        setup = screen(key_c)
        if shots:
            save_png(setup, shots / "key-c-setup-request.png")
        check(setup != home_c and setup != asked, "C shows the set-up request", "C did not change")

        print("\nATTENTION TEST PASSED")
        ok = True
    finally:
        for blk in blocks:
            blk.stop()
        if ok:
            shutil.rmtree(workdir, ignore_errors=True)
        else:
            print(f"Emulator logs kept in {workdir}")


# longer than the main block's repeat (2 s): the request stays on screen
PHYLAX_REPEAT_CHECK_S = 5

if __name__ == "__main__":
    main()
