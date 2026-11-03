#!/usr/bin/env python3
"""PHYLAX Snap - a saved wallet's receive address, shown on the main block.

PHYLAX Suite's "Show on main block" sends the main block a GetAddress for the
2-of-2 (its own key at m/48'/coin'/0'/2'/0/i, the saved xpubs of all blocks,
sortedmulti = LEXICOGRAPHIC, P2WSH) with show_display. The main block draws
the address and the "Multisig 2 of 2" line on its own screen and returns it.

The address must equal the descriptor's (wsh(sortedmulti(...)), embit), for
mainnet and testnet and for several indexes, whatever order the PC sends
the keys in. A multisig the main block holds no key of is refused.

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_receive.py --headless
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
from embit.bip32 import HDKey  # noqa: E402
from embit.descriptor import Descriptor  # noqa: E402
from embit.ec import PublicKey  # noqa: E402
from embit.networks import NETWORKS  # noqa: E402
from test_wallets import check, wait_wallets  # noqa: E402
from trezorlib import btc, messages  # noqa: E402
from trezorlib.exceptions import TrezorFailure  # noqa: E402

NETS = {"main": ("Bitcoin", 0), "test": ("Testnet", 1)}


def node_of(key, net):
    return key.testnet if net == "test" else key.mainnet


def descriptor_address(wallet, net, index):
    parts = []
    for k in wallet.keys:
        node = node_of(k, net)
        key = HDKey(PublicKey.parse(node.public_key), node.chain_code,
                    version=NETWORKS[net]["xpub"], depth=node.depth,
                    fingerprint=node.fingerprint.to_bytes(4, "big"),
                    child_number=node.child_num)
        parts.append(f"[{k.fingerprint:08x}/48h/{NETS[net][1]}h/0h/2h]{key.to_base58()}/<0;1>/*")
    desc = Descriptor.from_string(f"wsh(sortedmulti({wallet.threshold},{','.join(parts)}))")
    return desc.derive(index, branch_index=0).address(NETWORKS[net])


def multisig(wallet, net, index, keys=None):
    keys = wallet.keys if keys is None else keys
    return messages.MultisigRedeemScriptType(
        pubkeys=[messages.HDNodePathType(node=node_of(k, net), address_n=[0, index]) for k in keys],
        signatures=[b"" for _ in keys],
        m=wallet.threshold,
        pubkeys_order=messages.MultisigPubkeysOrder.LEXICOGRAPHIC,
    )


def path(net, index):
    h = 0x80000000
    return [48 | h, NETS[net][1] | h, 0 | h, 2 | h, 0, index]


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

    workdir = Path(tempfile.mkdtemp(prefix="phylax-receive-"))
    main_blk = s.Block("Main block", s.MAIN_SEED, s.MAIN_PORT, "main", elf, workdir,
                       args.headless, autoconfirm=True, node=0)
    key = s.Block("Key block", s.KEY_SEED, s.KEY_PORT, "key", elf, workdir,
                  args.headless, autoconfirm=True, node=1)
    ok = False
    try:
        print("1. Main block + key block: the 2-of-2 is saved")
        key.start()
        main_blk.start()
        w = wait_wallets(main_blk.session, lambda w: len(w.wallets) == 1 and w.wallets[0].complete,
                         "the 2-of-2 to be saved")
        full = main_blk.session.call(pm.PhylaxGetWallet(slot=w.wallets[0].slot),
                                     expect=pm.PhylaxWallet)

        print("2. Shown on the main block = the descriptor's address")
        for net in ("main", "test"):
            for index in (0, 1, 7):
                expect = descriptor_address(full, net, index)
                got = btc.get_address(main_blk.session, NETS[net][0], path(net, index),
                                      show_display=True, multisig=multisig(full, net, index),
                                      script_type=messages.InputScriptType.SPENDWITNESS)
                check(got == expect, f"{net} /0/{index}: {got}",
                      f"{net} /0/{index}: main block says {got}, descriptor {expect}")

        print("3. Keys sent in the other order: same address (sorted on the block)")
        got = btc.get_address(main_blk.session, "Bitcoin", path("main", 3), show_display=True,
                              multisig=multisig(full, "main", 3, list(reversed(full.keys))),
                              script_type=messages.InputScriptType.SPENDWITNESS)
        check(got == descriptor_address(full, "main", 3), "reversed key order gives the same address",
              f"reversed order gives {got}")

        print("4. A multisig without the main block's key is refused")
        mine = btc.get_public_node(main_blk.session, [0x80000000], coin_name="Bitcoin").root_fingerprint
        others = [k for k in full.keys if k.fingerprint != mine]
        stranger = messages.MultisigRedeemScriptType(
            pubkeys=[messages.HDNodePathType(node=node_of(k, "main"), address_n=[0, 0]) for k in others * 2],
            signatures=[b"", b""], m=2,
            pubkeys_order=messages.MultisigPubkeysOrder.LEXICOGRAPHIC)
        try:
            btc.get_address(main_blk.session, "Bitcoin", path("main", 0), show_display=True,
                            multisig=stranger, script_type=messages.InputScriptType.SPENDWITNESS)
            check(False, "", "the main block showed an address of a wallet it is not in")
        except TrezorFailure as e:
            check(True, f"refused: {e.message}", "")

        print("\nRECEIVE TEST PASSED")
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
