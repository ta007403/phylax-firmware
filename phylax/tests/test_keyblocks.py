#!/usr/bin/env python3
"""PHYLAX Blocks - set up, recover and unlock a key block through the main block.

A key block has no USB of its own (its port is covered): the PC reaches it
only through the main block. PhylaxSelectBlock puts the main block in
pass-through mode, so the normal Trezor One flows work on the key block:

  1. A brand-new key block is found by discovery: "no seed".
  2. Select it; the PC now gets the KEY block's Features.
  3. Set it up: it creates its own seed (words shown on its own screen) and
     a PIN entered on the PC's grid against its own scrambled keypad.
     Back on the main block, discovery reports its new fingerprint.
  4. Wipe it and recover it from 12 words typed on the PC (asked in a
     shuffled order with fake words, Trezor One style). Its fingerprint
     becomes that of the recovered seed.
  5. Lock it: discovery reports it locked, without a fingerprint. Unlock it
     with Ping(pin_protection): the PIN goes through the main block.
  6. Lock it again and sign a 2-of-2: the main block asks for the key
     block's PIN in the middle of signing, the PC enters it, and the result
     is a fully signed transaction.

The emulated key block confirms its own prompts (PHYLAX_EMU_AUTOCONFIRM=1);
on real hardware the user presses its buttons. Testnet, made-up coins.

Run from the legacy folder (emulator built with
EMULATOR=1 DEBUG_LINK=1 BITCOIN_ONLY=1):

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/test_keyblocks.py --headless
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

from embit import bip32, bip39  # noqa: E402
from embit.ec import PublicKey, Signature  # noqa: E402
from embit.script import Script  # noqa: E402
from embit.transaction import Transaction  # noqa: E402
from trezorlib import btc, messages  # noqa: E402
from trezorlib.tools import parse_path  # noqa: E402

PIN = "4321"
RECOVER_SEED = s.KEY_SEED  # "abandon ... about", fingerprint 73c5da0a
KEY_ADDR = 0x10
VERBOSE = "--verbose" in sys.argv


def check(cond, ok, bad):
    print(("  PASS  " if cond else "  FAIL  ") + (ok if cond else bad))
    if not cond:
        sys.exit(1)


def fingerprint(mnemonic: str) -> int:
    root = bip32.HDKey.from_seed(bip39.mnemonic_to_seed(mnemonic))
    return int.from_bytes(root.my_fingerprint, "big")


def key_entries(session):
    return [b for b in s.get_blocks(session) if int(b.kind) == 1]


def wait_key(session, cond, what, timeout=12.0):
    """Wait for a key block matching cond. A wiped block comes back with a
    new id (Trezor renews it on wipe), so it can briefly appear twice and at
    a new address: always use the address returned here."""
    deadline = time.time() + timeout
    keys = []
    while time.time() < deadline:
        keys = key_entries(session)
        match = [k for k in keys if cond(k)]
        if match:
            print("        " + match[0].describe())
            return match[0]
        time.sleep(0.25)
    raise TimeoutError(f"waited {timeout:.0f} s for {what}; key blocks: "
                       + "; ".join(k.describe() for k in keys))


def drive(session, debug, first, words=None):
    """Answer a key block's flow until Success: ButtonAck (the emulated key
    block confirms itself), PIN from its scrambled keypad, entropy, and
    recovery words in the order it asks for them."""
    ret = first
    mnemonic = words.split(" ") if words else None
    for _ in range(200):
        if VERBOSE:
            print(f"          <- {type(ret).__name__}", flush=True)
        if isinstance(ret, (messages.Success, messages.Failure)):
            return ret
        if isinstance(ret, messages.ButtonRequest):
            ret = session.call_raw(messages.ButtonAck())
        elif isinstance(ret, messages.PinMatrixRequest):
            ret = session.call_raw(messages.PinMatrixAck(pin=debug.encode_pin(PIN)))
        elif isinstance(ret, messages.EntropyRequest):
            ret = session.call_raw(messages.EntropyAck(entropy=os.urandom(32)))
        elif isinstance(ret, messages.WordRequest):
            fake, pos = debug.read_recovery_word()
            word = mnemonic[pos - 1] if pos else fake
            ret = session.call_raw(messages.WordAck(word=word))
        else:
            raise AssertionError(f"unexpected {type(ret).__name__}")
    raise AssertionError("flow did not end")


def select(session, addr):
    ret = session.call_raw(pm.PhylaxSelectBlock(address=addr))
    assert isinstance(ret, messages.Success), ret
    return ret


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--elf", default="firmware/trezor.elf")
    ap.add_argument("--headless", action="store_true")
    ap.add_argument("--verbose", action="store_true", help="print every message")
    args = ap.parse_args()
    elf = Path(args.elf).resolve()
    if not elf.exists():
        sys.exit(f"Emulator not found: {elf}")
    for p in (s.MAIN_PORT, s.KEY_PORT):
        if s.port_answers(p):
            sys.exit(f"Something already answers on port {p}. Close running emulators first.")

    workdir = Path(tempfile.mkdtemp(prefix="phylax-keyblocks-"))
    main_blk = s.Block("Main block", s.MAIN_SEED, s.MAIN_PORT, "main", elf, workdir,
                       args.headless, autoconfirm=False, node=0)
    key = s.Block("Key block 2", None, s.KEY_PORT, "key", elf, workdir,
                  args.headless, autoconfirm=True, node=1)
    ok = False
    try:
        print("1. A brand-new key block next to the main block")
        key.start()
        main_blk.start()
        k = wait_key(main_blk.session, lambda k: True, "the key block")
        check(k.initialized is False and k.address == KEY_ADDR,
              "found at 0x10, no seed", "key block reported wrongly")
        addr = k.address

        # The PC's session, through the main block. Its DebugLink is the KEY
        # block's: this stands for the user looking at the key block's screen.
        key_debug = key.ctx.debug
        pc = main_blk.ctx.get_seedless_session()

        print("2. Select the key block through the main block")
        select(pc, addr)
        f = pc.call_raw(messages.Initialize())
        # The main block has a seed; an answer without one is the key block's.
        check(isinstance(f, messages.Features) and f.initialized is False
              and main_blk.session.features.initialized is True,
              "the PC now sees the KEY block's Features (no seed)",
              f"unexpected answer: {f}")

        print("3. Set it up: own seed, PIN entered on the PC's grid")
        ret = pc.call_raw(messages.ResetDevice(strength=128, pin_protection=True,
                                               passphrase_protection=False,
                                               label="Key block 2"))
        ret = drive(pc, key_debug, ret)
        check(isinstance(ret, messages.Success), "set up, words shown on its own screen",
              f"setup failed: {ret}")
        new_seed = key_debug.state().mnemonic_secret.decode()
        select(pc, 1)
        k = wait_key(main_blk.session, lambda k: k.fingerprint is not None, "its fingerprint")
        addr = k.address
        check(k.fingerprint == fingerprint(new_seed),
              f"discovery shows its new fingerprint {k.fingerprint:08x}", "wrong fingerprint")

        print("4. Wipe it and recover it from 12 words typed on the PC")
        select(pc, addr)
        ret = drive(pc, key_debug, pc.call_raw(messages.WipeDevice()))
        check(isinstance(ret, messages.Success), "wiped", f"wipe failed: {ret}")
        # A wiped block gets a new id and comes back at a new address.
        select(pc, 1)
        k = wait_key(main_blk.session, lambda k: k.initialized is False and k.address != addr,
                     "the wiped block to come back")
        addr = k.address
        check(True, f"wiped block back as a new block at 0x{addr:02x} (new id)", "")
        select(pc, addr)
        # Trezor One asks 12-word seeds in order unless this is relaxed;
        # shuffled with fake words is what we want to show.
        drive(pc, key_debug, pc.call_raw(messages.ApplySettings(
            safety_checks=messages.SafetyCheckLevel.PromptTemporarily)))
        ret = pc.call_raw(messages.RecoveryDevice(word_count=12, pin_protection=True,
                                                  passphrase_protection=False,
                                                  label="Key block 2", enforce_wordlist=True))
        ret = drive(pc, key_debug, ret, words=RECOVER_SEED)
        check(isinstance(ret, messages.Success), "recovered (words asked shuffled, with fakes)",
              f"recovery failed: {ret}")
        select(pc, 1)
        k = wait_key(main_blk.session, lambda k: k.fingerprint == fingerprint(RECOVER_SEED),
                     "the recovered fingerprint")
        addr = k.address
        check(True, "discovery shows 73c5da0a, the recovered seed's fingerprint", "")

        print("5. Lock it, then unlock it with its PIN through the main block")
        select(pc, addr)
        check(isinstance(pc.call_raw(messages.LockDevice()), messages.Success), "locked", "lock failed")
        select(pc, 1)
        k = wait_key(main_blk.session, lambda k: k.locked and k.address == addr,
                     "it to be reported locked")
        check(k.fingerprint is None, "reported locked, fingerprint hidden", "fingerprint still shown")
        select(pc, addr)
        # Any PIN-protected request unlocks it (Trezor One's Ping does not
        # check the PIN); reading its multisig account key shows nothing.
        ret = pc.call_raw(messages.GetPublicKey(address_n=parse_path(s.ACCOUNT),
                                                coin_name=s.COIN,
                                                script_type=messages.InputScriptType.SPENDMULTISIG))
        check(isinstance(ret, messages.PinMatrixRequest),
              "it asks for its PIN (the PC shows the grid)", f"no PIN asked: {ret}")
        ret = pc.call_raw(messages.PinMatrixAck(pin=key_debug.encode_pin(PIN)))
        check(isinstance(ret, messages.PublicKey), "unlocked with the PIN, through the main block",
              f"unlock failed: {ret}")
        select(pc, 1)
        wait_key(main_blk.session, lambda k: k.fingerprint == fingerprint(RECOVER_SEED),
                 "it to be unlocked")

        print("6. Lock it again and sign a 2-of-2: its PIN is asked mid-signing")
        select(pc, addr)
        pc.call_raw(messages.LockDevice())
        select(pc, 1)
        wait_key(main_blk.session, lambda k: k.locked and k.address == addr, "it to be locked")
        # the old (wiped) entry must be gone before signing picks a key block
        deadline = time.time() + 10
        while len(key_entries(main_blk.session)) != 1 and time.time() < deadline:
            time.sleep(0.25)
        # PINs the PC is asked for are read off the key block's keypad.
        pins_asked = []

        def key_pin(_req=None):
            pins_asked.append(1)
            return key_debug.encode_pin(PIN)

        main_blk.ctx.app.pin_callback = key_pin
        # The key block's xpub is computed from its known test seed: the PC
        # cannot ask it directly (no USB), and asking through select mode
        # would unlock it again.
        nodes = {main_blk.name: btc.get_public_node(main_blk.session, parse_path(s.ACCOUNT),
                                                    coin_name=s.COIN).node,
                 key.name: _node_from_seed(RECOVER_SEED)}
        ms_in, ws, order = s.build_multisig(nodes, [0, 0])
        ms_chg, ws_chg, _ = s.build_multisig(nodes, [1, 0])
        txid, prev = s.fake_funding_tx(s.p2wsh(ws))
        inp = messages.TxInputType(
            address_n=parse_path(f"{s.ACCOUNT}/0/0"), prev_hash=txid, prev_index=0,
            amount=s.FUNDING, script_type=messages.InputScriptType.SPENDWITNESS, multisig=ms_in)
        outs = [
            messages.TxOutputType(address=s.OUTSIDE_ADDRESS, amount=s.PAY,
                                  script_type=messages.OutputScriptType.PAYTOADDRESS),
            messages.TxOutputType(address_n=parse_path(f"{s.ACCOUNT}/1/0"), amount=s.CHANGE,
                                  script_type=messages.OutputScriptType.PAYTOWITNESS,
                                  multisig=ms_chg),
        ]
        _, raw = btc.sign_tx(main_blk.session, s.COIN, [inp], outs, prev_txes={txid: prev})
        check(len(pins_asked) == 1, "the PC was asked once, for the key block's PIN",
              f"PIN asked {len(pins_asked)} times")
        tx = Transaction.parse(raw)
        items = tx.vin[0].witness.items
        sighash = tx.sighash_segwit(0, Script(ws), s.FUNDING)
        valid = len(items) == 4 and all(
            PublicKey.parse(s.child_pubkey(nodes[name], [0, 0])).verify(
                Signature.parse(items[i][:-1]), sighash)
            for i, name in enumerate(order, start=1))
        check(valid, "fully signed 2-of-2, both signatures valid", "not fully signed")

        print("\nKEY BLOCKS TEST PASSED")
        ok = True
    finally:
        for blk in (main_blk, key):
            blk.stop()
        if ok:
            shutil.rmtree(workdir, ignore_errors=True)
        else:
            print(f"Emulator logs kept in {workdir}")


def _node_from_seed(mnemonic):
    """The key block's account node, computed from its known (test) seed."""
    root = bip32.HDKey.from_seed(bip39.mnemonic_to_seed(mnemonic))
    acc = root.derive(s.ACCOUNT.replace("h", "'"))
    return messages.HDNodeType(depth=acc.depth, fingerprint=int.from_bytes(acc.fingerprint, "big"),
                               child_num=acc.child_number, chain_code=acc.chain_code,
                               public_key=acc.key.sec())


if __name__ == "__main__":
    main()
