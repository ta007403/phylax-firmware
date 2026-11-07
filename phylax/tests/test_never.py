#!/usr/bin/env python3
"""PHYLAX Blocks - "Never" on "New ... wallet?", kept in the main block's flash.

  1. Main block + key block A, answered "No", then "Never": no wallet, and the
     combination is on the main block's "Never" list (PhylaxWallets.ignored).
  2. The main block restarts (same flash) and would confirm everything: the
     2-of-2 is still not offered.
  3. Key block B joins: the 3-of-3 is a new combination and is saved.
  4. The PC forgets the "Never" (PhylaxForgetIgnored): the 2-of-2 is offered
     at once and saved. Forgetting it again is refused.
  5. The PC deletes the 2-of-2 (confirmed on the main block): it goes on the
     "Never" list, and after a restart with A still attached it is not offered.
  6. Main block + B only, answered "No", then "Later": not saved, not on the
     list; after a restart the 2-of-2 with B is offered and saved.

Testnet, nothing spent. Run from the legacy folder (emulator built with
EMULATOR=1 DEBUG_LINK=1 BITCOIN_ONLY=1):

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_never.py --headless
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import phylax_msgs as pm  # noqa: E402
import spike2b as s  # noqa: E402
from test_rename import on_device  # noqa: E402
from test_wallets import PORT_B, SEED_B, check, wait_wallets, wallets  # noqa: E402

from trezorlib import messages  # noqa: E402

# Long enough for the main block to notice the blocks, let the stack settle,
# fetch the xpubs and ask (with autoconfirm it answers at once).
OFFER_WAIT_S = 8


def fingerprints(session):
    blocks = s.get_blocks(session)
    return sorted(b.fingerprint for b in blocks if b.fingerprint is not None)


def ignored(session):
    return [list(x.fingerprints) for x in wallets(session).ignored]


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

    workdir = Path(tempfile.mkdtemp(prefix="phylax-never-"))
    blocks = []

    def start_main(answer):
        """The main block (same flash every time); `answer`: None confirms
        "New ... wallet?", else "No" then "never" or "later"."""
        if answer is None:
            os.environ.pop("PHYLAX_EMU_NEW_WALLET", None)
        else:
            os.environ["PHYLAX_EMU_NEW_WALLET"] = answer
        blk = s.Block("Main block", s.MAIN_SEED, s.MAIN_PORT, "main", elf, workdir,
                      args.headless, autoconfirm=True, node=0)
        blocks.append(blk)
        blk.start()
        os.environ.pop("PHYLAX_EMU_NEW_WALLET", None)
        return blk

    a = s.Block("Key block A", s.KEY_SEED, s.KEY_PORT, "key", elf, workdir,
                args.headless, autoconfirm=True, node=1)
    b = s.Block("Key block B", SEED_B, PORT_B, "key", elf, workdir,
                args.headless, autoconfirm=True, node=2)
    blocks += [a, b]
    ok = False
    try:
        print("1. Main block + A, \"No\" then \"Never\"")
        a.start()
        main_blk = start_main("never")
        s.wait_for_blocks(main_blk.session, lambda bl: len(s.key_blocks(bl)) == 1, what="A")
        pair_ma = fingerprints(main_blk.session)
        time.sleep(OFFER_WAIT_S)
        w = wallets(main_blk.session)
        check(not w.wallets and ignored(main_blk.session) == [pair_ma],
              f"no wallet; on the Never list: {pair_ma}",
              f"unexpected: wallets {[x.name for x in w.wallets]}, "
              f"ignored {ignored(main_blk.session)}")

        print("2. Restart the main block, confirming everything: still not offered")
        main_blk.stop()
        main_blk = start_main(None)
        s.wait_for_blocks(main_blk.session, lambda bl: len(s.key_blocks(bl)) == 1, what="A")
        time.sleep(OFFER_WAIT_S)
        w = wallets(main_blk.session)
        check(not w.wallets and ignored(main_blk.session) == [pair_ma],
              "the 2-of-2 stays unoffered after the restart",
              f"unexpected: wallets {[x.name for x in w.wallets]}")

        print("3. B joins: the 3-of-3 is offered and saved")
        b.start()
        wait_wallets(main_blk.session, lambda w: len(w.wallets) == 1 and w.wallets[0].threshold == 3,
                     "the 3-of-3")

        print("4. The PC forgets the \"Never\": the 2-of-2 is offered again")
        b.stop()
        s.wait_for_blocks(main_blk.session, lambda bl: len(s.key_blocks(bl)) == 1, what="B gone")
        ret = main_blk.session.call_raw(pm.PhylaxForgetIgnored(fingerprints=list(reversed(pair_ma))))
        check(isinstance(ret, messages.Success), "forgotten (fingerprints in any order)",
              f"not forgotten: {ret}")
        w = wait_wallets(main_blk.session,
                         lambda w: any(x.threshold == 2 for x in w.wallets), "the 2-of-2")
        check(ignored(main_blk.session) == [], "the Never list is empty",
              f"still there: {ignored(main_blk.session)}")
        ret = main_blk.session.call_raw(pm.PhylaxForgetIgnored(fingerprints=pair_ma))
        check(isinstance(ret, messages.Failure), "forgetting it again is refused",
              f"not refused: {ret}")

        print("5. The PC deletes the 2-of-2: it goes on the Never list")
        two = next(x.slot for x in w.wallets if x.threshold == 2)
        ret = on_device(main_blk, pm.PhylaxDeleteWallet(slot=two), "yes")
        check(isinstance(ret, messages.Success) and ignored(main_blk.session) == [pair_ma],
              "deleted, and on the Never list", f"unexpected: {ret} {ignored(main_blk.session)}")
        time.sleep(2)  # the delete reaches A's copy
        main_blk.stop()
        main_blk = start_main(None)
        s.wait_for_blocks(main_blk.session, lambda bl: len(s.key_blocks(bl)) == 1, what="A")
        time.sleep(OFFER_WAIT_S)
        w = wallets(main_blk.session)
        check(not any(x.threshold == 2 for x in w.wallets),
              "not offered again after a restart", f"offered: {[x.name for x in w.wallets]}")

        print("6. Main block + B, \"No\" then \"Later\": asked again next time")
        a.stop()
        main_blk.stop()
        b.start()
        main_blk = start_main("later")
        s.wait_for_blocks(main_blk.session, lambda bl: len(s.key_blocks(bl)) == 1, what="B")
        pair_mb = fingerprints(main_blk.session)
        time.sleep(OFFER_WAIT_S)
        w = wallets(main_blk.session)
        check(not any(sorted(x.fingerprints) == pair_mb for x in w.wallets)
              and pair_mb not in ignored(main_blk.session),
              "not saved, not on the Never list", f"unexpected: {w}")
        main_blk.stop()
        main_blk = start_main(None)
        wait_wallets(main_blk.session,
                     lambda w: any(sorted(x.fingerprints) == pair_mb for x in w.wallets),
                     "the 2-of-2 with B")

        print("\nNEVER TEST PASSED")
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
