#!/usr/bin/env python3
"""PHYLAX Blocks - main block notices a removed key block (10 s timeout).

Scenario A, slow user: key block 2 waits for its user's button press (no
auto-confirm, and nobody presses). It keeps answering the main block's
PINGs, so after 15 s the main block must still be waiting.

Scenario B, block pulled out: key block 2 is killed while waiting. The main
block must give up with "Key block disconnected" about 10 s later.

Scenario C, no key block at all: SignTx fails at once with "No key block
attached" instead of hanging forever.

Run from the legacy folder (emulator built with
EMULATOR=1 DEBUG_LINK=1 BITCOIN_ONLY=1):

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_timeout.py --headless

Takes about a minute.
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spike2b as s  # noqa: E402  (reuse blocks, seeds and transaction)

from trezorlib import btc, messages  # noqa: E402
from trezorlib.exceptions import TrezorFailure  # noqa: E402
from trezorlib.tools import parse_path  # noqa: E402

TIMEOUT_S = 10.0
DISCONNECTED = "Key block disconnected"


def is_disconnected(outcome):
    # Debug builds prefix failure texts with "file.c:line:".
    return bool(outcome) and outcome.startswith("failure:") and outcome.endswith(DISCONNECTED)


def build_tx(main_session, nodes):
    ms_in, ws, _ = s.build_multisig(nodes, [0, 0])
    ms_chg, _, _ = s.build_multisig(nodes, [1, 0])
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
    return [inp], outs, {txid: prev}


def sign_in_thread(session, inputs, outputs, prev):
    result = {}

    def run():
        t0 = time.time()
        try:
            btc.sign_tx(session, s.COIN, inputs, outputs, prev_txes=prev)
            result["outcome"] = "signed"
        except TrezorFailure as e:
            result["outcome"] = f"failure: {e.message}"
        except Exception as e:  # noqa: BLE001
            result["outcome"] = f"error: {e!r}"
        result["seconds"] = time.time() - t0

    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t, result


def check(cond, ok, bad):
    print(("  PASS  " if cond else "  FAIL  ") + (ok if cond else bad))
    if not cond:
        sys.exit(1)


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

    workdir = Path(tempfile.mkdtemp(prefix="phylax-timeout-"))
    ok = False
    # Key block WITHOUT auto-confirm: it waits for a button nobody presses.
    key = s.Block("Key block 2", s.KEY_SEED, s.KEY_PORT, "key", elf, workdir / "ab",
                  args.headless, autoconfirm=False)
    main_blk = s.Block("Main block", s.MAIN_SEED, s.MAIN_PORT, "main", elf, workdir / "ab",
                       args.headless, autoconfirm=False)
    try:
        print("Scenarios A and B: key block waits for a button press, then is pulled out")
        key.start()  # xpub read before the main block exists (see spike2b)
        key_node = btc.get_public_node(key.session, parse_path(s.ACCOUNT), coin_name=s.COIN).node
        main_blk.start()
        s.wait_for_blocks(main_blk.session, lambda bl: len(s.key_blocks(bl)) == 1,
                          what="the key block to be discovered")
        nodes = {main_blk.name: btc.get_public_node(main_blk.session, parse_path(s.ACCOUNT),
                                                    coin_name=s.COIN).node,
                 key.name: key_node}
        inputs, outputs, prev = build_tx(main_blk.session, nodes)
        t, result = sign_in_thread(main_blk.session, inputs, outputs, prev)

        time.sleep(15)
        check(t.is_alive(),
              "A: after 15 s the main block is still waiting (PINGs keep a slow block alive)",
              f"A: main block gave up although the key block was alive: {result}")

        pulled = time.time()
        key.stop()
        t.join(timeout=TIMEOUT_S + 10)
        waited = time.time() - pulled
        check(not t.is_alive(), "B: signing ended after the key block was pulled",
              "B: main block still hangs 20 s after the key block was pulled")
        check(is_disconnected(result.get("outcome")),
              "B: failure says \"Key block disconnected\"",
              f"B: unexpected outcome: {result.get('outcome')}")
        check(TIMEOUT_S - 1.5 <= waited <= TIMEOUT_S + 3,
              f"B: it took {waited:.1f} s (expected about {TIMEOUT_S:.0f} s)",
              f"B: it took {waited:.1f} s, expected about {TIMEOUT_S:.0f} s")
        main_blk.stop()

        print("\nScenario C: no key block on the bus at all")
        main_c = s.Block("Main block", s.MAIN_SEED, s.MAIN_PORT, "main", elf, workdir / "c",
                         args.headless, autoconfirm=False)
        main_c.start()
        t, result = sign_in_thread(main_c.session, inputs, outputs, prev)
        t.join(timeout=TIMEOUT_S + 10)
        outcome = result.get("outcome") or ""
        check(outcome.startswith("failure:") and outcome.endswith("No key block attached")
              and result.get("seconds", 99) < 2,
              f"C: failed at once ({result.get('seconds', 0):.1f} s) with \"No key block attached\"",
              f"C: unexpected outcome: {result}")
        main_c.stop()

        print("\nTIMEOUT TEST PASSED")
        ok = True
    finally:
        for b in (main_blk, key):
            b.stop()
        if ok:
            shutil.rmtree(workdir, ignore_errors=True)
        else:
            print(f"Emulator logs kept in {workdir}")


if __name__ == "__main__":
    main()
