#!/usr/bin/env python3
"""PHYLAX Blocks - 3-of-3 with two key blocks, and skipping a stranger.

Part 1, 3-of-3: main block + key block A + key block B. The host sends ONE
SignTx for a 3-of-3 P2WSH input, only to the main block. The main block
relays to A, then B (pass 1), then signs itself (pass 2), checking each kept
signature against the script's keys to put it in the right slot. Expected:
a witness with 3 valid signatures in script-key order.

Part 2, a key block that is not in the wallet: same three blocks, but the
wallet is a 2-of-2 of main + A. Key block B's key is not in the script, so
the main block must skip B and still return a fully signed 2-of-2.

Testnet, made-up coins. Run from the legacy folder (emulator built with
EMULATOR=1 DEBUG_LINK=1 BITCOIN_ONLY=1):

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_3of3.py --headless
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import phylax_msgs  # noqa: E402,F401
import spike2b as s  # noqa: E402

from embit.ec import PublicKey, Signature  # noqa: E402
from embit.script import Script  # noqa: E402
from embit.transaction import Transaction  # noqa: E402
from trezorlib import btc, messages  # noqa: E402
from trezorlib.tools import parse_path  # noqa: E402

SEED_B = "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong"
PORT_B = 21434


def check(cond, ok, bad):
    print(("  PASS  " if cond else "  FAIL  ") + (ok if cond else bad))
    if not cond:
        sys.exit(1)


def sign_and_verify(main_session, nodes, label):
    """One SignTx to the main block for an n-of-n of `nodes`; verify result."""
    ms_in, ws, order = s.build_multisig(nodes, [0, 0])
    ms_chg, ws_chg, _ = s.build_multisig(nodes, [1, 0])
    spk = s.p2wsh(ws)
    txid, prev = s.fake_funding_tx(spk)
    addr = btc.get_address(main_session, s.COIN, parse_path(f"{s.ACCOUNT}/0/0"),
                           multisig=ms_in,
                           script_type=messages.InputScriptType.SPENDWITNESS)
    print(f"        {label} address: {addr}")
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
    _, raw = btc.sign_tx(main_session, s.COIN, [inp], outs, prev_txes={txid: prev})

    n = len(order)
    tx = Transaction.parse(raw)
    items = tx.vin[0].witness.items
    check(len(items) == n + 2 and items[0] == b"" and items[-1] == ws,
          f"witness: empty, {n} signatures, the {n}-of-{n} script",
          f"witness is not a complete {n}-of-{n}: {len(items)} items")
    check(tx.vout[1].script_pubkey.data == s.p2wsh(ws_chg) and tx.vout[1].value == s.CHANGE,
          "change back to the wallet (1/0)", "change output is wrong")
    sighash = tx.sighash_segwit(0, Script(ws), s.FUNDING)
    for slot, name in enumerate(order, start=1):
        pub = PublicKey.parse(s.child_pubkey(nodes[name], [0, 0]))
        der = items[slot][:-1]
        check(items[slot][-1] == 1 and pub.verify(Signature.parse(der), sighash),
              f"slot {slot}: {name} signature valid, in script-key order",
              f"slot {slot}: signature does not belong to {name}'s key")
    return raw


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

    workdir = Path(tempfile.mkdtemp(prefix="phylax-3of3-"))
    main_blk = s.Block("Main block", s.MAIN_SEED, s.MAIN_PORT, "main", elf, workdir,
                       args.headless, autoconfirm=False, node=0)
    a = s.Block("Key block A", s.KEY_SEED, s.KEY_PORT, "key", elf, workdir,
                args.headless, autoconfirm=True, node=1)
    b = s.Block("Key block B", SEED_B, PORT_B, "key", elf, workdir,
                args.headless, autoconfirm=True, node=2)
    ok = False
    try:
        # Key blocks' xpubs are read before the main block exists (see spike2b).
        nodes = {}
        for blk in (a, b):
            blk.start()
            nodes[blk.name] = btc.get_public_node(blk.session, parse_path(s.ACCOUNT),
                                                  coin_name=s.COIN).node
        main_blk.start()
        s.wait_for_blocks(main_blk.session, lambda bl: len(s.key_blocks(bl)) == 2,
                          what="both key blocks")
        nodes[main_blk.name] = btc.get_public_node(main_blk.session, parse_path(s.ACCOUNT),
                                                   coin_name=s.COIN).node
        nodes = {k: nodes[k] for k in (main_blk.name, a.name, b.name)}

        print("Part 1: 3-of-3, one SignTx to the main block")
        sign_and_verify(main_blk.session, nodes, "3-of-3")

        print("\nPart 2: 2-of-2 of main + A, with key block B attached but not in it")
        two = {k: v for k, v in nodes.items() if k != b.name}
        sign_and_verify(main_blk.session, two, "2-of-2")
        print("  PASS  key block B was skipped")

        print("\n3-OF-3 TEST PASSED")
        ok = True
    finally:
        for blk in (main_blk, a, b):
            blk.stop()
        if ok:
            shutil.rmtree(workdir, ignore_errors=True)
        else:
            print(f"Emulator logs kept in {workdir}")


if __name__ == "__main__":
    main()
