#!/usr/bin/env python3
"""PHYLAX Blocks - rename and delete saved wallets from the PC.

  1. Main block + key blocks A and B: "2-of-2 #1" and "3-of-3 #1" are saved.
  2. Bad names are refused before anything is shown: too long, not ASCII,
     blank, unchanged, another wallet's name (case-insensitive), no wallet.
  3. Rename, but the user presses Cancel on the main block: nothing changes.
  4. Rename "2-of-2 #1" to "Savings", confirmed on the main block.
  5. Key block B removed; delete "3-of-3 #1", confirmed on the main block.
  6. B comes back with its old copy (the 3-of-3 still in it): the wallet
     does not come back, the delete wins.
  7. A brand-new main block with only B: B's copy has "Savings" and no
     3-of-3, so both changes reached a block that missed one of them.
  8. All three blocks again on that main block: "New 3-of-3 wallet?" is
     offered again and saved in the old slot (emulator: autoconfirm).

Confirmations are pressed through the main block's DebugLink, the way the
user presses its buttons. Testnet, nothing spent.

Run from the legacy folder (emulator built with
EMULATOR=1 DEBUG_LINK=1 BITCOIN_ONLY=1):

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_rename.py --headless
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
from test_wallets import PORT_B, SEED_B, check, wait_wallets, wallets  # noqa: E402

from trezorlib import messages  # noqa: E402


def on_device(blk, msg, press):
    """Send msg; when the main block asks for a button, press it."""
    ret = blk.session.call_raw(msg)
    if isinstance(ret, messages.ButtonRequest):
        if press == "yes":
            blk.ctx.debug.press_yes()
        else:
            blk.ctx.debug.press_no()
        ret = blk.session.call_raw(messages.ButtonAck())
    return ret


def by_name(session):
    return {w.name: w for w in wallets(session).wallets}


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

    workdir = Path(tempfile.mkdtemp(prefix="phylax-rename-"))

    def new_main(folder, autoconfirm):
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
        print("1. Main block + A + B: a 2-of-2 and a 3-of-3 are saved")
        for blk in (a, main_blk):
            blk.start()
        wait_wallets(main_blk.session, lambda w: len(w.wallets) == 1, "the 2-of-2")
        b.start()
        w = wait_wallets(main_blk.session, lambda w: len(w.wallets) == 2 and w.storage_present,
                         "the 3-of-3")
        time.sleep(2)  # let the last copies be written
        # From here on the main block confirms nothing by itself.
        main_blk.stop()
        main_blk = new_main("main-1", False)
        blocks.append(main_blk)
        main_blk.start()
        w = wait_wallets(main_blk.session, lambda w: len(w.wallets) == 2 and w.storage_present,
                         "the wallets after the restart")
        names = {x.name: x for x in w.wallets}
        two, three = names["2-of-2 #1"].slot, names["3-of-3 #1"].slot

        print("2. Bad names are refused before anything is shown")
        for name, why in (("Sixteen chars!!!", "16 characters"), ("Café", "not ASCII"),
                          ("   ", "blank"), ("2-of-2 #1", "unchanged"),
                          ("3-OF-3 #1", "another wallet's name, other case")):
            ret = main_blk.session.call_raw(pm.PhylaxSetWalletName(slot=two, name=name))
            check(isinstance(ret, messages.Failure), f"{name!r} refused ({why}): {ret.message}"
                  if isinstance(ret, messages.Failure) else "", f"{name!r} not refused: {ret}")
        ret = main_blk.session.call_raw(pm.PhylaxSetWalletName(slot=7, name="Nope"))
        check(isinstance(ret, messages.Failure), f"empty slot refused: {ret.message}"
              if isinstance(ret, messages.Failure) else "", f"empty slot not refused: {ret}")

        print("3. Rename, but Cancel on the main block")
        ret = on_device(main_blk, pm.PhylaxSetWalletName(slot=two, name="Savings"), "no")
        check(isinstance(ret, messages.Failure)
              and ret.code == messages.FailureType.ActionCancelled
              and "2-of-2 #1" in by_name(main_blk.session),
              "cancelled, name unchanged", f"unexpected: {ret}")

        print("4. Rename \"2-of-2 #1\" to \"Savings\", confirmed")
        ret = on_device(main_blk, pm.PhylaxSetWalletName(slot=two, name="Savings"), "yes")
        names = by_name(main_blk.session)
        check(isinstance(ret, messages.Success) and set(names) == {"Savings", "3-of-3 #1"}
              and names["Savings"].slot == two,
              "renamed, same slot", f"unexpected: {ret} {list(names)}")
        time.sleep(2)  # written to A and B

        print("5. Remove B, delete \"3-of-3 #1\", confirmed")
        b.stop()
        ret = on_device(main_blk, pm.PhylaxDeleteWallet(slot=three), "yes")
        names = by_name(main_blk.session)
        check(isinstance(ret, messages.Success) and set(names) == {"Savings"},
              "deleted: only \"Savings\" left", f"unexpected: {ret} {list(names)}")
        ret = main_blk.session.call_raw(pm.PhylaxDeleteWallet(slot=three))
        check(isinstance(ret, messages.Failure), "deleting it again is refused",
              f"not refused: {ret}")

        print("6. B comes back with its old copy (3-of-3 still in it)")
        b.start()
        time.sleep(1)
        w = wait_wallets(main_blk.session, lambda w: w.storage_present, "B's copy to be read")
        time.sleep(3)  # merged, written back
        names = by_name(main_blk.session)
        check(set(names) == {"Savings"}, "the deleted wallet stays deleted",
              f"it came back: {list(names)}")

        print("7. A brand-new main block with only B")
        main_blk.stop()
        a.stop()
        main_blk = new_main("main-2", False)
        blocks.append(main_blk)
        main_blk.start()
        w = wait_wallets(main_blk.session, lambda w: w.storage_present and len(w.wallets) >= 1,
                         "B's copy")
        names = {x.name for x in w.wallets}
        check(names == {"Savings"}, "B's copy has \"Savings\" and no 3-of-3",
              f"B's copy has {names}")

        print("8. All three blocks again: the 3-of-3 is offered again")
        main_blk.stop()
        main_blk = new_main("main-2", True)
        blocks.append(main_blk)
        a.start()
        main_blk.start()
        w = wait_wallets(main_blk.session, lambda w: len(w.wallets) == 2, "the 3-of-3 again")
        names = {x.name: x for x in w.wallets}
        check(set(names) == {"Savings", "3-of-3 #1"} and names["3-of-3 #1"].slot == three
              and names["3-of-3 #1"].complete,
              "\"3-of-3 #1\" saved again, in its old slot", f"unexpected: {list(names)}")

        print("\nRENAME TEST PASSED")
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
