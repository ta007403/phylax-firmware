#!/usr/bin/env python3
"""PHYLAX Blocks - saved wallets, a copy on every block.

  1. main block + key block A: the main block offers a new 2-of-2, confirms
     it (auto-confirm stands in for the main block's "Create" button),
     fetches A's xpubs over the bus and saves the record on itself and on A,
     in chunks of at most 128 bytes.
  2. The saved xpubs (mainnet and testnet) are exactly what each block
     reports itself, and the descriptor rebuilt from them gives the same
     2-of-2 address the earlier spikes signed for.
  3. Key block B is snapped on: B receives the 2-of-2 too, and a 3-of-3 is
     offered and saved on all three blocks.
  4. The main block is replaced by a brand-new one (same seed, empty
     flash): it gets both wallets back from the key blocks.
  5. A new main block with only key block B: B alone still holds both
     wallets (every block keeps all of them); both need a missing block.
  6. That main block restarted on its own: the wallets come from its own
     flash.

Run from the legacy folder (emulator built with
EMULATOR=1 DEBUG_LINK=1 BITCOIN_ONLY=1):

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_wallets.py --headless
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

from embit.descriptor import Descriptor  # noqa: E402
from embit.networks import NETWORKS  # noqa: E402
from embit.bip32 import HDKey  # noqa: E402
from embit.ec import PublicKey  # noqa: E402
from trezorlib import btc  # noqa: E402
from trezorlib.tools import parse_path  # noqa: E402

SEED_B = "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong"
PORT_B = 21434
EXPECTED_2OF2 = "tb1qwsqvuucn402dasyuvx3xaph0rzep4wj0f7e9n9egae7utaktnmjsv0dtmy"


def check(cond, ok, bad):
    print(("  PASS  " if cond else "  FAIL  ") + (ok if cond else bad))
    if not cond:
        sys.exit(1)


def wallets(session):
    return session.call(pm.PhylaxGetWallets(), expect=pm.PhylaxWallets)


def wait_wallets(session, cond, what, timeout=15.0):
    deadline = time.time() + timeout
    w = None
    while time.time() < deadline:
        w = wallets(session)
        if cond(w):
            for x in w.wallets:
                print("        " + x.describe())
            return w
        time.sleep(0.3)
    raise TimeoutError(f"waited {timeout:.0f} s for {what}; got: "
                       + ("; ".join(x.describe() for x in w.wallets) if w else "nothing"))


def same_node(a, b):
    return (a.depth == b.depth and a.fingerprint == b.fingerprint and
            a.child_num == b.child_num and a.chain_code == b.chain_code and
            a.public_key == b.public_key)


def descriptor_address(wallet, net):
    """Rebuild wsh(sortedmulti(...)) from the saved xpubs; first receive address."""
    parts = []
    for k in wallet.keys:
        node = k.testnet if net == "test" else k.mainnet
        key = HDKey(PublicKey.parse(node.public_key), node.chain_code,
                    version=NETWORKS[net]["xpub"], depth=node.depth,
                    fingerprint=node.fingerprint.to_bytes(4, "big"),
                    child_number=node.child_num)
        coin = 1 if net == "test" else 0
        parts.append(f"[{k.fingerprint:08x}/48h/{coin}h/0h/2h]{key.to_base58()}/<0;1>/*")
    desc = Descriptor.from_string(f"wsh(sortedmulti({wallet.threshold},{','.join(parts)}))")
    return desc.derive(0, branch_index=0).address(NETWORKS[net])


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

    workdir = Path(tempfile.mkdtemp(prefix="phylax-wallets-"))

    def new_main(folder, autoconfirm):
        # A main block with its own flash folder; auto-confirm = "Create".
        return s.Block("Main block", s.MAIN_SEED, s.MAIN_PORT, "main", elf, workdir / folder,
                       args.headless, autoconfirm=autoconfirm, node=0)

    main_blk = new_main("main-1", True)
    a = s.Block("Key block A", s.KEY_SEED, s.KEY_PORT, "key", elf, workdir,
                args.headless, autoconfirm=True, node=1)
    b = s.Block("Key block B", SEED_B, PORT_B, "key", elf, workdir,
                args.headless, autoconfirm=True, node=2)
    blocks = [main_blk, a, b]
    ok = False
    try:
        print("1. Main block + key block A: a 2-of-2 is created")
        for blk in (a, main_blk):
            blk.start()
        w = wait_wallets(main_blk.session, lambda w: len(w.wallets) == 1,
                         "the 2-of-2 to be saved")
        two = w.wallets[0]
        check(two.name == "2-of-2 #1" and two.threshold == 2 and two.complete,
              "saved as \"2-of-2 #1\", complete", f"saved as {two.describe()}")

        print("2. Saved xpubs match the blocks, descriptor gives the known address")
        full = main_blk.session.call(pm.PhylaxGetWallet(slot=two.slot), expect=pm.PhylaxWallet)
        direct = {}
        for blk in (main_blk, a):
            fp = btc.get_public_node(blk.session, parse_path("m/0h"), coin_name=s.COIN).root_fingerprint
            direct[fp] = {
                "main": btc.get_public_node(blk.session, parse_path("m/48h/0h/0h/2h"), coin_name="Bitcoin").node,
                "test": btc.get_public_node(blk.session, parse_path("m/48h/1h/0h/2h"), coin_name=s.COIN).node,
            }
        for k in full.keys:
            d = direct.get(k.fingerprint)
            check(d is not None and same_node(k.mainnet, d["main"]) and same_node(k.testnet, d["test"]),
                  f"{k.fingerprint:08x}: mainnet and testnet xpubs identical to the block's own",
                  f"{k.fingerprint:08x}: saved xpubs differ from the block's own")
        addr = descriptor_address(full, "test")
        check(addr == EXPECTED_2OF2, f"descriptor first address {addr}",
              f"descriptor gives {addr}, expected {EXPECTED_2OF2}")
        print(f"        mainnet first address {descriptor_address(full, 'main')}")

        print("3. Snap on key block B: a 3-of-3 is created, all blocks get both")
        b.start()
        w = wait_wallets(main_blk.session, lambda w: len(w.wallets) == 2 and w.storage_present,
                         "the 3-of-3 to be saved")
        names = {x.name: x for x in w.wallets}
        check("3-of-3 #1" in names and names["3-of-3 #1"].complete and names["2-of-2 #1"].complete,
              "\"3-of-3 #1\" saved, both wallets complete", "3-of-3 missing or wrong")
        time.sleep(2)  # let the last copies be written

        print("4. Replace the main block with a brand-new one (empty flash)")
        main_blk.stop()
        main_blk = new_main("main-2", False)
        blocks.append(main_blk)
        main_blk.start()
        w = wait_wallets(main_blk.session, lambda w: len(w.wallets) == 2 and w.storage_present,
                         "the wallets to come back from the key blocks")
        check({x.name for x in w.wallets} == {"2-of-2 #1", "3-of-3 #1"},
              "both wallets recovered from key blocks A and B", "wallets not recovered")

        print("5. A new main block with only key block B")
        main_blk.stop()
        a.stop()
        main_blk = new_main("main-3", False)
        blocks.append(main_blk)
        main_blk.start()
        w = wait_wallets(main_blk.session, lambda w: len(w.wallets) == 2 and w.storage_present,
                         "B's copy to be read")
        names = {x.name: x for x in w.wallets}
        check(set(names) == {"2-of-2 #1", "3-of-3 #1"} and not names["2-of-2 #1"].complete
              and not names["3-of-3 #1"].complete,
              "B alone holds both wallets (it is not even in the 2-of-2); both need key block A",
              "B's copy is incomplete")
        time.sleep(2)  # let the main block write its own copy

        print("6. That main block restarted alone: wallets from its own flash")
        main_blk.stop()
        b.stop()
        main_blk.start()
        w = wait_wallets(main_blk.session, lambda w: len(w.wallets) == 2,
                         "the wallets from the main block's own flash")
        check({x.name for x in w.wallets} == {"2-of-2 #1", "3-of-3 #1"},
              "both wallets read from the main block's own copy", "own copy missing")

        print("\nWALLETS TEST PASSED")
        ok = True
    finally:
        for blk in blocks:
            blk.stop()
        if ok:
            shutil.rmtree(workdir, ignore_errors=True)
        else:
            print(f"Emulator logs kept in {workdir}")


if __name__ == "__main__":
    main()
