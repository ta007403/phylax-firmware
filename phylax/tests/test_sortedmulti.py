#!/usr/bin/env python3
"""PHYLAX Snap - sign when the host lists the keys unsorted (sortedmulti).

PHYLAX Suite sends the wallet's keys in the order the main block saved them
(by seed fingerprint) with pubkeys_order = LEXICOGRAPHIC: the device sorts
them for the script (sortedmulti). Found on hardware: on the owner's Mainnet
wallet the two orders differ, and the main block placed the key block's valid
signature by the host's order while the signature slots follow the script's
sorted order. The key block's slot already held the main block's own
signature, so signing failed with "A key block did not sign: ... no-match".
On Testnet the orders happened to match; the earlier tests sent the keys
already sorted.

  1. Main block + key block, the 2-of-2's keys.
  2. SignTx with the keys listed in the reverse of the script's order and
     pubkeys_order LEXICOGRAPHIC, on the input and the change.
  3. The transaction comes back fully signed, with the sorted script.

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_sortedmulti.py --headless
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spike2b as s  # noqa: E402
from embit.transaction import Transaction  # noqa: E402
from test_wallets import check  # noqa: E402
from trezorlib import btc, messages  # noqa: E402
from trezorlib.tools import parse_path  # noqa: E402

LEXICOGRAPHIC = 1


def unsorted_multisig(nodes, path):
    """The 2-of-2 with its keys in the reverse of the script's (sorted) order."""
    _, ws, order = s.build_multisig(nodes, path)
    listed = list(reversed(order))
    ms = messages.MultisigRedeemScriptType(
        pubkeys=[messages.HDNodePathType(node=nodes[n], address_n=path) for n in listed],
        signatures=[b"" for _ in listed],
        m=len(listed),
        pubkeys_order=LEXICOGRAPHIC,
    )
    return ms, ws


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

    workdir = Path(tempfile.mkdtemp(prefix="phylax-sortedmulti-"))
    key = s.Block("Key block 2", s.KEY_SEED, s.KEY_PORT, "key", elf, workdir, args.headless,
                  autoconfirm=True, node=1)
    main_blk = s.Block("Main block", s.MAIN_SEED, s.MAIN_PORT, "main", elf, workdir,
                       args.headless, autoconfirm=True, node=0)
    ok = False
    try:
        print("1. Main block + key block")
        key.start()
        key_node = btc.get_public_node(key.session, parse_path(s.ACCOUNT), coin_name=s.COIN).node
        main_blk.start()
        s.wait_for_blocks(main_blk.session, lambda bl: len(s.key_blocks(bl)) == 1,
                          what="the key block to be discovered")
        nodes = {main_blk.name: btc.get_public_node(main_blk.session, parse_path(s.ACCOUNT),
                                                    coin_name=s.COIN).node,
                 key.name: key_node}

        print("2. Keys listed unsorted, pubkeys_order LEXICOGRAPHIC")
        ms_in, ws = unsorted_multisig(nodes, [0, 0])
        ms_chg, ws_chg = unsorted_multisig(nodes, [1, 0])
        txid, prev = s.fake_funding_tx(s.p2wsh(ws))
        inp = messages.TxInputType(
            address_n=parse_path(f"{s.ACCOUNT}/0/0"), prev_hash=txid, prev_index=0,
            amount=s.FUNDING, script_type=messages.InputScriptType.SPENDWITNESS,
            multisig=ms_in,
        )
        outs = [
            messages.TxOutputType(address=s.OUTSIDE_ADDRESS, amount=s.PAY,
                                  script_type=messages.OutputScriptType.PAYTOADDRESS),
            messages.TxOutputType(address_n=parse_path(f"{s.ACCOUNT}/1/0"), amount=s.CHANGE,
                                  script_type=messages.OutputScriptType.PAYTOWITNESS,
                                  multisig=ms_chg),
        ]
        _sigs, raw = btc.sign_tx(main_blk.session, s.COIN, [inp], outs, prev_txes={txid: prev})

        print("3. Result")
        tx = Transaction.parse(raw)
        items = tx.vin[0].witness.items
        check(len(items) == 4 and items[0] == b"" and items[1] and items[2],
              "fully signed: empty, signature, signature, script",
              f"not fully signed: {[len(i) for i in items]}")
        check(items[3] == ws, "witness script = the sorted 2-of-2 script", "other script")
        check(tx.vout[1].script_pubkey.data == s.p2wsh(ws_chg),
              "change back to the 2-of-2 (sorted)", "change goes elsewhere")

        print("\nSORTEDMULTI TEST PASSED")
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
