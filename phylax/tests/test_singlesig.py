#!/usr/bin/env python3
"""PHYLAX Snap - a single-sig spend is signed by the main block alone.

Found on hardware: spending the main block's own (single-sig) coins in Suite
asked the key block to sign first, as for the multisig. The main block used
to send every SignTx to the key blocks; now it asks the host for the first
input before deciding: multisig inputs go to the key blocks first, single-sig
ones are signed by the main block alone, as on a Trezor One.

  1. Main block alone (no key block attached): a P2WPKH spend of its own
     coins is signed (it used to fail with "No key block attached").
  2. Key block attached, which never confirms anything (no auto-confirm): the
     same spend is still signed by the main block alone, without waiting for
     the key block.
  3. The multisig still goes to the key block first: test_sortedmulti.py.

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_singlesig.py --headless
"""

from __future__ import annotations

import argparse
import concurrent.futures
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spike2b as s  # noqa: E402
from embit import hashes  # noqa: E402
from embit.transaction import Transaction  # noqa: E402
from test_wallets import check  # noqa: E402
from trezorlib import btc, messages  # noqa: E402
from trezorlib.tools import parse_path  # noqa: E402

PATH = "m/84h/1h/0h/0/0"
CHANGE_PATH = "m/84h/1h/0h/1/0"
SIGN_TIMEOUT_S = 60


def sign_single_sig(main_blk):
    """A P2WPKH spend of the main block's own coin; returns (raw tx, pubkey)."""
    pubkey = btc.get_public_node(main_blk.session, parse_path(PATH),
                                 coin_name=s.COIN).node.public_key
    spk = b"\x00\x14" + hashes.hash160(pubkey)
    txid, prev = s.fake_funding_tx(spk)
    inp = messages.TxInputType(
        address_n=parse_path(PATH), prev_hash=txid, prev_index=0, amount=s.FUNDING,
        script_type=messages.InputScriptType.SPENDWITNESS, sequence=0xFFFFFFFD,
    )
    outs = [
        messages.TxOutputType(address=s.OUTSIDE_ADDRESS, amount=s.PAY,
                              script_type=messages.OutputScriptType.PAYTOADDRESS),
        messages.TxOutputType(address_n=parse_path(CHANGE_PATH), amount=s.CHANGE,
                              script_type=messages.OutputScriptType.PAYTOWITNESS),
    ]
    # In a thread: if the key block were asked, it would wait for its buttons forever.
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
        future = pool.submit(btc.sign_tx, main_blk.session, s.COIN, [inp], outs,
                             prev_txes={txid: prev})
        try:
            _sigs, raw = future.result(timeout=SIGN_TIMEOUT_S)
        except concurrent.futures.TimeoutError:
            check(False, "", f"no signature after {SIGN_TIMEOUT_S} s (waiting for a key block?)")
    return raw, pubkey


def check_signed(raw, pubkey):
    tx = Transaction.parse(raw)
    items = tx.vin[0].witness.items
    check(len(items) == 2 and items[1] == pubkey,
          "signed by the main block's own key (P2WPKH witness)",
          f"unexpected witness: {[i.hex()[:16] for i in items]}")


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

    workdir = Path(tempfile.mkdtemp(prefix="phylax-singlesig-"))
    main_blk = s.Block("Main block", s.MAIN_SEED, s.MAIN_PORT, "main", elf, workdir,
                       args.headless, autoconfirm=True, node=0)
    # never confirms: a request to it would hang
    key = s.Block("Key block 2", s.KEY_SEED, s.KEY_PORT, "key", elf, workdir, args.headless,
                  autoconfirm=False, node=1)
    ok = False
    try:
        print("1. Main block alone: single-sig spend")
        main_blk.start()
        check_signed(*sign_single_sig(main_blk))

        print("2. Key block attached (never confirms): single-sig spend")
        key.start()
        s.wait_for_blocks(main_blk.session, lambda bl: len(s.key_blocks(bl)) == 1,
                          what="the key block to be discovered")
        check_signed(*sign_single_sig(main_blk))

        print("\nSINGLE-SIG TEST PASSED")
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
