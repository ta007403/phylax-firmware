#!/usr/bin/env python3
"""PHYLAX Snap - never a half-signed transaction.

Found on hardware: PHYLAX Suite broadcast a 2-of-2 spend that the network
rejected ("Operation not valid with the current stack size"): it carried only
the main block's signature. A key block that rejects the inputs with "Failed
to derive scriptPubKey" is skipped as "not part of this wallet" (right for a
stack with more blocks than the wallet), but a wallet member whose own key
came out wrong rejects them the same way.

  1. Main block + key block: the 2-of-2 is saved.
  2. The key block gets another seed (as if its key came out wrong).
  3. Signing the wallet's coins fails with "A key block did not sign"
     instead of returning a transaction with one signature.

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_missing_sig.py --headless
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spike2b as s  # noqa: E402
from test_timeout import build_tx  # noqa: E402
from test_wallets import check, wait_wallets  # noqa: E402
from trezorlib import btc, device  # noqa: E402
from trezorlib.debuglink import load_device  # noqa: E402
from trezorlib.exceptions import TrezorFailure  # noqa: E402
from trezorlib.tools import parse_path  # noqa: E402

OTHER_SEED = "legal winner thank year wave sausage worth useful legal winner thank yellow"


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

    workdir = Path(tempfile.mkdtemp(prefix="phylax-missing-sig-"))
    key = s.Block("Key block 2", s.KEY_SEED, s.KEY_PORT, "key", elf, workdir, args.headless,
                  autoconfirm=True, node=1)
    main_blk = s.Block("Main block", s.MAIN_SEED, s.MAIN_PORT, "main", elf, workdir,
                       args.headless, autoconfirm=True, node=0)
    ok = False
    try:
        print("1. Main block + key block: the 2-of-2 is saved")
        key.start()
        key_node = btc.get_public_node(key.session, parse_path(s.ACCOUNT), coin_name=s.COIN).node
        main_blk.start()
        wait_wallets(main_blk.session, lambda w: len(w.wallets) == 1, "the 2-of-2")
        nodes = {main_blk.name: btc.get_public_node(main_blk.session, parse_path(s.ACCOUNT),
                                                    coin_name=s.COIN).node,
                 key.name: key_node}
        inputs, outputs, prev = build_tx(main_blk.session, nodes)

        print("2. The key block's own key no longer matches the wallet")
        device.wipe(key.ctx.get_seedless_session())
        load_device(key.ctx.get_seedless_session(), mnemonic=OTHER_SEED, pin=None,
                    passphrase_protection=False, label="Key block")

        print("3. Signing the wallet's coins")
        try:
            btc.sign_tx(main_blk.session, s.COIN, inputs, outputs, prev_txes=prev)
            outcome = "signed"
        except TrezorFailure as e:
            outcome = e.message or ""
        check("A key block did not sign" in outcome,
              "refused: \"A key block did not sign\" (no half-signed transaction)",
              f"outcome: {outcome}")
        print(f"        message: {outcome}")

        print("\nMISSING SIGNATURE TEST PASSED")
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
