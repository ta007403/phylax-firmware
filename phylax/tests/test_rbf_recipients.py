#!/usr/bin/env python3
"""PHYLAX Snap - a replaceable (RBF) send to two recipients with change.

PHYLAX Suite's Send page pays several recipients in one transaction and marks every input
replaceable (nSequence 0xfffffffd, BIP125), as Trezor Suite does. The main block passes the
SignTx on to the key block unchanged, so both blocks must sign the same inputs and outputs.

  1. Main block + key block, the 2-of-2's keys (listed unsorted, as Suite sends them).
  2. SignTx: one input with nSequence 0xfffffffd, two outside payments and the change.
  3. The transaction comes back fully signed, with the sequence and all three outputs.

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_rbf_recipients.py --headless
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

    workdir = Path(tempfile.mkdtemp(prefix="phylax-rbf-recipients-"))
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
            multisig=ms_in, sequence=0xFFFFFFFD,
        )
        outs = [
            messages.TxOutputType(address=s.OUTSIDE_ADDRESS, amount=s.PAY // 2,
                                  script_type=messages.OutputScriptType.PAYTOADDRESS),
            messages.TxOutputType(address=s.OUTSIDE_ADDRESS, amount=s.PAY - s.PAY // 2,
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
        check(tx.vout[2].script_pubkey.data == s.p2wsh(ws_chg),
              "change back to the 2-of-2 (sorted)", "change goes elsewhere")

        check(all(i.sequence == 0xFFFFFFFD for i in tx.vin), "RBF sequence kept", "sequence changed")
        check(len(tx.vout) == 3, "two recipients + change", "outputs wrong")
        print("\nRBF + 2 RECIPIENTS PASSED")
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
