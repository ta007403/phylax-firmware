#!/usr/bin/env python3
"""PHYLAX Snap - the right key right after the PIN, after a replug.

Found on hardware: PHYLAX Suite's safety check saw a wrong account key
(960d89fe instead of dc3b2d16) when the main block had been replugged and
its PIN was entered in Suite. The main block computed its own seed
fingerprint for the bus inside the progress callback of the session's seed
computation; both use mnemonic_to_seed, whose PBKDF2 state is static on the
STM32 (LOCAL_CONFIDENTIAL), so the inner one corrupted the outer one.

The emulator shows it because legacy/Makefile.include builds it with
LOCAL_CONFIDENTIAL static, as on the device (rebuild crypto and firmware
after changing that: make -C firmware clean && make -C ../crypto clean).

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_pin_seed.py --headless

  1. Main block with PIN 1234 + key block: the 2-of-2 is saved.
  2. The main block restarts (as after a replug): locked, nothing cached.
  3. The PC asks for the account key, the PIN is entered: the key must be
     the one saved in the wallet, and stay so in a new session.
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import phylax_msgs as pm  # noqa: E402
import spike2b as s  # noqa: E402
from test_wallets import check, wait_wallets  # noqa: E402
from trezorlib import messages  # noqa: E402
from trezorlib.debuglink import load_device  # noqa: E402
from trezorlib.tools import parse_path  # noqa: E402

PIN = "1234"
NETWORKS = (("m/48h/0h/0h/2h", "Bitcoin", "mainnet"), ("m/48h/1h/0h/2h", "Testnet", "testnet"))


def get_key(session, path, coin):
    return session.call_raw(messages.GetPublicKey(address_n=parse_path(path), coin_name=coin))


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

    workdir = Path(tempfile.mkdtemp(prefix="phylax-pin-seed-"))
    main_blk = s.Block("Main block", None, s.MAIN_PORT, "main", elf, workdir,
                       args.headless, autoconfirm=True, node=0)
    key = s.Block("Key block", s.KEY_SEED, s.KEY_PORT, "key", elf, workdir,
                  args.headless, autoconfirm=True, node=1)
    ok = False
    try:
        print("1. Main block with a PIN + key block: the 2-of-2 is saved")
        key.start()
        main_blk.start()
        load_device(main_blk.ctx.get_session(passphrase=None), mnemonic=s.MAIN_SEED, pin=PIN,
                    passphrase_protection=False, label="Main block")
        pc = main_blk.ctx.get_session(passphrase="")
        w = wait_wallets(pc, lambda w: len(w.wallets) == 1, "the 2-of-2")
        wallet = pc.call(pm.PhylaxGetWallet(slot=w.wallets[0].slot), expect=pm.PhylaxWallet)

        print("2. The main block restarts: locked, nothing cached")
        main_blk.stop()
        main_blk.start()

        for path, coin, net in NETWORKS:
            saved = {getattr(k, net).public_key for k in wallet.keys}
            print(f"3. {coin}: the PC asks for the account key, the PIN is entered")
            # The PIN is asked while the session opens; its seed is computed
            # right after it, as when the PIN is entered in Suite.
            main_blk.ctx.use_pin_sequence([PIN])
            pc = main_blk.ctx.get_session(passphrase="")
            ret = get_key(pc, path, coin)
            check(isinstance(ret, messages.PublicKey), "a key came back", f"no key: {ret}")
            check(ret.node.public_key in saved, f"{coin}: the key saved in the wallet",
                  f"{coin}: a DIFFERENT key than the one saved in the wallet")
            again = get_key(main_blk.ctx.get_session(passphrase=""), path, coin)
            check(isinstance(again, messages.PublicKey) and again.node == ret.node,
                  f"{coin}: the same key in a new session", f"{coin}: new session differs")
            if net == "mainnet":
                # Testnet after a fresh restart too: the seed again computed after the PIN.
                main_blk.stop()
                main_blk.start()

        print("\nPIN SEED TEST PASSED")
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
