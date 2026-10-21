#!/usr/bin/env python3
"""PHYLAX Blocks - spike 2b: the main block relays signing to a key block.

Two emulated blocks run AT THE SAME TIME, connected by the emulated PHYLAX
bus (UDP carrying the same frames as RS485):

  main block   PHYLAX_ROLE=main  USB (UDP) port 21324, bus address 0x01
  key block 2  PHYLAX_ROLE=key   USB (UDP) port 21424, bus address 0x10

The host (this script) sends ONE SignTx, and only to the main block. The
main block forwards it over the bus, the key block signs (pass 1), then the
main block signs with the key block's signature inserted (pass 2) and
returns a fully signed 2-of-2 transaction.

The key block runs with PHYLAX_EMU_AUTOCONFIRM=1 because in the emulator
nobody presses its buttons; on real hardware the user confirms on its
screen. Testnet, made-up coins: nothing real is spent.

The key block's xpub is read through its own emulator port for now; reading
it through the main block comes with discovery.

Run from the legacy folder, emulator built with
EMULATOR=1 DEBUG_LINK=1 BITCOIN_ONLY=1:

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/spike2b.py
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from embit.bip32 import HDKey
from embit.ec import PublicKey, Signature
from embit.networks import NETWORKS
from embit.script import Script
from embit.transaction import Transaction

from trezorlib import btc, messages
from trezorlib.debuglink import TrezorTestContext, load_device
from trezorlib.tools import parse_path
from trezorlib.transport import get_transport

MAIN_SEED = "all all all all all all all all all all all all"
KEY_SEED = (
    "abandon abandon abandon abandon abandon abandon "
    "abandon abandon abandon abandon abandon about"
)
ACCOUNT = "m/48h/1h/0h/2h"
COIN = "Testnet"
FUNDING = 100_000
PAY = 60_000
FEE = 1_000
CHANGE = FUNDING - PAY - FEE
OUTSIDE_ADDRESS = "tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx"
MAIN_PORT = 21324
KEY_PORT = 21424


def port_answers(port: int) -> bool:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.3)
    try:
        s.sendto(b"PINGPING", ("127.0.0.1", port))
        return s.recv(64) == b"PONGPONG"
    except OSError:
        return False
    finally:
        s.close()


class Block:
    """One emulated block. `node` is its slot on the emulated shared bus
    (0 = main block); `seed=None` leaves it without a seed."""

    def __init__(self, name, seed, port, role, elf, workdir, headless, autoconfirm,
                 node=None):
        self.name, self.seed, self.port, self.role = name, seed, port, role
        self.elf, self.headless, self.autoconfirm = elf, headless, autoconfirm
        self.node = node if node is not None else (0 if role == "main" else 1)
        self.dir = workdir / f"{role}{self.node}"
        self.dir.mkdir(parents=True, exist_ok=True)

    def start(self):
        env = dict(os.environ)
        env.setdefault("TREZOR_OLED_SCALE", "3")
        env["TREZOR_UDP_PORT"] = str(self.port)
        env["PHYLAX_ROLE"] = self.role
        env["PHYLAX_BUS_NODE"] = str(self.node)
        if self.autoconfirm:
            env["PHYLAX_EMU_AUTOCONFIRM"] = "1"
        if self.headless:
            env["SDL_VIDEODRIVER"] = "dummy"
        self.log = open(self.dir / "emulator.log", "w")
        self.proc = subprocess.Popen([str(self.elf)], cwd=self.dir, env=env,
                                     stdout=self.log, stderr=subprocess.STDOUT)
        for _ in range(100):
            if port_answers(self.port):
                break
            if self.proc.poll() is not None:
                sys.exit(f"[{self.name}] emulator exited at start-up, see {self.dir}/emulator.log")
            time.sleep(0.1)
        else:
            sys.exit(f"[{self.name}] emulator did not start")
        self.ctx = TrezorTestContext(get_transport(f"udp:127.0.0.1:{self.port}"))
        seedless = self.ctx.get_session(passphrase=None)
        if self.seed is not None and not seedless.features.initialized:
            load_device(seedless, mnemonic=self.seed, pin=None,
                        passphrase_protection=False, label=self.name)
        seedless.close()
        if self.seed is not None:
            self.session = self.ctx.get_session(passphrase="")
        return self

    def stop(self):
        if getattr(self, "proc", None) and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()


def get_blocks(session):
    """Ask the main block which blocks are in the stack (PhylaxGetBlocks)."""
    import phylax_msgs as pm
    return session.call(pm.PhylaxGetBlocks(), expect=pm.PhylaxBlocks).blocks


def wait_for_blocks(session, condition, timeout=10.0, what="blocks"):
    """Poll PhylaxGetBlocks until condition(blocks) is true."""
    deadline = time.time() + timeout
    blocks = []
    while time.time() < deadline:
        blocks = get_blocks(session)
        if condition(blocks):
            return blocks
        time.sleep(0.25)
    raise TimeoutError(f"waited {timeout:.0f} s for {what}; main block sees: "
                       + "; ".join(b.describe() for b in blocks))


def key_blocks(blocks):
    return [b for b in blocks if int(b.kind) == 1]


def sha256d(b):
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def child_pubkey(node, path):
    key = HDKey(PublicKey.parse(node.public_key), node.chain_code,
                version=NETWORKS["test"]["xpub"])
    return key.derive(path).key.sec()


def build_multisig(nodes, path):
    order = sorted(nodes, key=lambda n: child_pubkey(nodes[n], path))
    ms = messages.MultisigRedeemScriptType(
        pubkeys=[messages.HDNodePathType(node=nodes[n], address_n=path) for n in order],
        signatures=[b"" for _ in order],  # the host sends NO signatures
        m=len(order),  # n-of-n
    )
    keys = [child_pubkey(nodes[n], path) for n in order]
    op_n = bytes([0x50 + len(order)])  # OP_2, OP_3, ...
    ws = op_n + b"".join(b"\x21" + k for k in keys) + op_n + b"\xae"
    return ms, ws, order


def p2wsh(ws):
    return b"\x00\x20" + hashlib.sha256(ws).digest()


def fake_funding_tx(spk):
    prev = messages.TransactionType(
        version=2, lock_time=0,
        inputs=[messages.TxInputType(prev_hash=b"\x11" * 32, prev_index=0,
                                     script_sig=b"", sequence=0xFFFFFFFF)],
        bin_outputs=[messages.TxOutputBinType(amount=FUNDING, script_pubkey=spk)],
    )
    raw = (struct.pack("<I", 2) + b"\x01" + (b"\x11" * 32)[::-1] + struct.pack("<I", 0)
           + b"\x00" + struct.pack("<I", 0xFFFFFFFF)
           + b"\x01" + struct.pack("<Q", FUNDING) + bytes([len(spk)]) + spk
           + struct.pack("<I", 0))
    return sha256d(raw)[::-1], prev


def check(cond, ok, bad):
    print(("  PASS  " if cond else "  FAIL  ") + (ok if cond else bad))
    if not cond:
        sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--elf", default="firmware/trezor.elf")
    ap.add_argument("--headless", action="store_true")
    ap.add_argument("--keep", action="store_true", help="keep emulator logs")
    args = ap.parse_args()

    elf = Path(args.elf).resolve()
    if not elf.exists():
        sys.exit(f"Emulator not found: {elf}\nRun this from ~/phylax-firmware/legacy")
    for p in (MAIN_PORT, KEY_PORT):
        if port_answers(p):
            sys.exit(f"Something already answers on port {p}. Close running emulators first.")

    workdir = Path(tempfile.mkdtemp(prefix="phylax-spike2b-"))
    key = Block("Key block 2", KEY_SEED, KEY_PORT, "key", elf, workdir, args.headless, True)
    main_blk = Block("Main block", MAIN_SEED, MAIN_PORT, "main", elf, workdir, args.headless, False)
    ok = False
    try:
        print("Step 1  Start key block 2 and the main block (both running)")
        key.start()
        main_blk.start()
        wait_for_blocks(main_blk.session, lambda bl: len(key_blocks(bl)) == 1,
                        what="the key block to be discovered")
        nodes = {
            b.name: btc.get_public_node(b.session, parse_path(ACCOUNT), coin_name=COIN).node
            for b in (main_blk, key)
        }
        ms_in, ws, order = build_multisig(nodes, [0, 0])
        ms_chg, ws_chg, _ = build_multisig(nodes, [1, 0])
        spk = p2wsh(ws)
        txid, prev = fake_funding_tx(spk)
        addr = btc.get_address(main_blk.session, COIN, parse_path(f"{ACCOUNT}/0/0"),
                               multisig=ms_in,
                               script_type=messages.InputScriptType.SPENDWITNESS)
        print(f"        2-of-2 address: {addr}")

        print("Step 2  ONE SignTx, sent only to the main block (no signatures in it)")
        inp = messages.TxInputType(
            address_n=parse_path(f"{ACCOUNT}/0/0"), prev_hash=txid, prev_index=0,
            amount=FUNDING, script_type=messages.InputScriptType.SPENDWITNESS,
            multisig=ms_in,
        )
        outs = [
            messages.TxOutputType(address=OUTSIDE_ADDRESS, amount=PAY,
                                  script_type=messages.OutputScriptType.PAYTOADDRESS),
            messages.TxOutputType(address_n=parse_path(f"{ACCOUNT}/1/0"), amount=CHANGE,
                                  script_type=messages.OutputScriptType.PAYTOWITNESS,
                                  multisig=ms_chg),
        ]
        t0 = time.time()
        sigs, raw = btc.sign_tx(main_blk.session, COIN, [inp], outs,
                                prev_txes={txid: prev})
        print(f"        done in {time.time() - t0:.1f} s")

        print("\nResults")
        tx = Transaction.parse(raw)
        items = tx.vin[0].witness.items
        check(len(items) == 4 and items[0] == b"",
              "witness has 4 items: empty, sig, sig, script (fully signed 2-of-2)",
              f"witness is not fully signed: {len(items)} items")
        check(items[3] == ws, "witness script = the 2-of-2 script",
              "witness script does not match")
        check(tx.vout[0].value == PAY and tx.vout[1].value == CHANGE
              and tx.vout[1].script_pubkey.data == p2wsh(ws_chg),
              "outputs: 60,000 out, change back to the 2-of-2 (1/0)",
              "outputs are wrong")
        sighash = tx.sighash_segwit(0, Script(ws), FUNDING)
        for slot, name in enumerate(order, start=1):
            der = items[slot][:-1]
            pub = PublicKey.parse(child_pubkey(nodes[name], [0, 0]))
            check(items[slot][-1] == 1, f"{name}: SIGHASH_ALL", f"{name}: wrong sighash type")
            check(pub.verify(Signature.parse(der), sighash),
                  f"{name}: signature is valid (BIP143)",
                  f"{name}: signature does NOT verify")
        print("\nSPIKE 2b PASSED: one SignTx to the main block gave a fully signed")
        print("2-of-2. The key block signed over the PHYLAX bus (pass 1) and the")
        print("main block inserted its signature and signed itself (pass 2).")
        print(f"\nSigned transaction ({len(raw)} bytes):\n{raw.hex()}")
        ok = True
    finally:
        main_blk.stop()
        key.stop()
        if args.keep or not ok:
            print(f"\nEmulator logs kept in {workdir}")
        else:
            shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
