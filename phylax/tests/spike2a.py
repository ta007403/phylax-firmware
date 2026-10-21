#!/usr/bin/env python3
"""PHYLAX Blocks - spike 2a: signature hand-over between two blocks.

Proves, with stock Trezor One firmware (emulator) and no firmware changes,
the trick the main block depends on:

  1. "Key block 2" signs a 2-of-2 P2WSH transaction and returns its signature.
  2. The "main block" gets the same transaction PLUS block 2's signature
     (in the input's multisig.signatures field) and must return a FULLY
     signed transaction.
  3. The script checks both signatures against the transaction (BIP143).

Each block is a Trezor One emulator with its own test seed and its own
storage folder. They run one after the other because the emulator's UDP
port is fixed. Buttons are pressed automatically through DebugLink.
Testnet, made-up coins: nothing real is spent.

Run from the legacy folder of the firmware repo (emulator built with
EMULATOR=1 DEBUG_LINK=1 BITCOIN_ONLY=1):

    cd ~/phylax-firmware/legacy
    uv run --with embit python ../phylax/tests/spike2a.py

Options:
    --elf PATH     emulator binary (default: ./firmware/trezor.elf)
    --headless     do not open the emulator windows
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
BLOCK2_SEED = (
    "abandon abandon abandon abandon abandon abandon "
    "abandon abandon abandon abandon abandon about"
)
ACCOUNT = "m/48h/1h/0h/2h"  # BIP48 P2WSH multisig, testnet
COIN = "Testnet"
FUNDING = 100_000  # sats in the made-up coin we spend
PAY = 60_000  # to an outside address
FEE = 1_000
CHANGE = FUNDING - PAY - FEE  # back to the 2-of-2 wallet
# BIP173 test vector, a valid testnet P2WPKH address
OUTSIDE_ADDRESS = "tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx"
EMU_PORT = 21324


# --------------------------------------------------------------- emulator


def port_answers() -> bool:
    """True if something already answers on the emulator's UDP port."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.3)
    try:
        s.sendto(b"PINGPING", ("127.0.0.1", EMU_PORT))
        return s.recv(64) == b"PONGPONG"
    except OSError:
        return False
    finally:
        s.close()


class Block:
    """One emulated block: a Trezor One emulator with its own storage folder."""

    def __init__(self, name: str, seed: str, elf: Path, workdir: Path, headless: bool):
        self.name, self.seed, self.elf = name, seed, elf
        self.dir = workdir / name.replace(" ", "_")
        self.dir.mkdir(parents=True, exist_ok=True)
        self.headless = headless
        self.proc: subprocess.Popen | None = None
        self.loaded = False

    def __enter__(self):
        env = dict(os.environ)
        env.setdefault("TREZOR_OLED_SCALE", "3")
        if self.headless:
            env["SDL_VIDEODRIVER"] = "dummy"
        self.proc = subprocess.Popen(
            [str(self.elf)], cwd=self.dir, env=env,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        for _ in range(100):
            if port_answers():
                break
            if self.proc.poll() is not None:
                sys.exit(f"[{self.name}] emulator exited at start-up")
            time.sleep(0.1)
        else:
            sys.exit(f"[{self.name}] emulator did not start")
        self.ctx = TrezorTestContext(get_transport(f"udp:127.0.0.1:{EMU_PORT}"))
        if not self.loaded:
            seedless = self.ctx.get_session(passphrase=None)
            if seedless.features.initialized:
                self.ctx.wipe_device()
                seedless = self.ctx.get_session(passphrase=None)
            load_device(seedless, mnemonic=self.seed, pin=None,
                        passphrase_protection=False, label=self.name)
            seedless.close()
            self.loaded = True
        self.session = self.ctx.get_session(passphrase="")
        return self

    def __exit__(self, *exc):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        for _ in range(50):  # wait until the port is free again
            if not port_answers():
                break
            time.sleep(0.1)


# ------------------------------------------------------------ bitcoin bits


def sha256d(b: bytes) -> bytes:
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def varint(n: int) -> bytes:
    assert n < 0xFD
    return bytes([n])


def child_pubkey(node: messages.HDNodeType, path: list[int]) -> bytes:
    """Derive a non-hardened child public key from an account node."""
    key = HDKey(PublicKey.parse(node.public_key), node.chain_code,
                version=NETWORKS["test"]["xpub"])
    return key.derive(path).key.sec()


def build_multisig(nodes: dict[str, messages.HDNodeType], path: list[int],
                   sigs: dict[str, bytes] | None = None):
    """sortedmulti-style 2-of-2: keys sorted by their child public key.

    Returns (MultisigRedeemScriptType, witness_script, sorted block names).
    """
    order = sorted(nodes, key=lambda n: child_pubkey(nodes[n], path))
    sigs = sigs or {}
    ms = messages.MultisigRedeemScriptType(
        pubkeys=[messages.HDNodePathType(node=nodes[n], address_n=path) for n in order],
        signatures=[sigs.get(n, b"") for n in order],
        m=2,
    )
    keys = [child_pubkey(nodes[n], path) for n in order]
    ws = b"\x52" + b"".join(b"\x21" + k for k in keys) + b"\x52\xae"
    return ms, ws, order


def p2wsh(ws: bytes) -> bytes:
    return b"\x00\x20" + hashlib.sha256(ws).digest()


def fake_funding_tx(script_pubkey: bytes):
    """A made-up previous transaction paying FUNDING sats to our 2-of-2."""
    prev = messages.TransactionType(
        version=2, lock_time=0,
        inputs=[messages.TxInputType(prev_hash=b"\x11" * 32, prev_index=0,
                                     script_sig=b"", sequence=0xFFFFFFFF)],
        bin_outputs=[messages.TxOutputBinType(amount=FUNDING, script_pubkey=script_pubkey)],
    )
    raw = (
        struct.pack("<I", 2)
        + varint(1) + (b"\x11" * 32)[::-1] + struct.pack("<I", 0)
        + varint(0) + struct.pack("<I", 0xFFFFFFFF)
        + varint(1) + struct.pack("<Q", FUNDING) + varint(len(script_pubkey)) + script_pubkey
        + struct.pack("<I", 0)
    )
    txid = sha256d(raw)[::-1]  # display byte order, as Trezor expects
    return txid, prev


def sign(block: Block, nodes, txid, prev, sigs=None):
    ms_in, _, _ = build_multisig(nodes, [0, 0], sigs)
    ms_chg, _, _ = build_multisig(nodes, [1, 0])
    inp = messages.TxInputType(
        address_n=parse_path(f"{ACCOUNT}/0/0"), prev_hash=txid, prev_index=0,
        amount=FUNDING, script_type=messages.InputScriptType.SPENDWITNESS,
        multisig=ms_in,
    )
    out_pay = messages.TxOutputType(
        address=OUTSIDE_ADDRESS, amount=PAY,
        script_type=messages.OutputScriptType.PAYTOADDRESS,
    )
    out_change = messages.TxOutputType(
        address_n=parse_path(f"{ACCOUNT}/1/0"), amount=CHANGE,
        script_type=messages.OutputScriptType.PAYTOWITNESS, multisig=ms_chg,
    )
    sigs_out, raw = btc.sign_tx(block.session, COIN, [inp], [out_pay, out_change],
                                prev_txes={txid: prev})
    return sigs_out[0], raw


def check(cond: bool, ok: str, bad: str) -> None:
    print(("  PASS  " if cond else "  FAIL  ") + (ok if cond else bad))
    if not cond:
        sys.exit(1)


# ------------------------------------------------------------------ main


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--elf", default="firmware/trezor.elf")
    ap.add_argument("--headless", action="store_true")
    args = ap.parse_args()

    elf = Path(args.elf).resolve()
    if not elf.exists():
        sys.exit(f"Emulator not found: {elf}\nRun this from ~/phylax-firmware/legacy")
    if port_answers():
        sys.exit("An emulator is already running on port 21324. Close it first "
                 "(Ctrl+C in its Ubuntu window), then run this again.")

    workdir = Path(tempfile.mkdtemp(prefix="phylax-spike2a-"))
    main_blk = Block("Main block", MAIN_SEED, elf, workdir, args.headless)
    blk2 = Block("Key block 2", BLOCK2_SEED, elf, workdir, args.headless)
    nodes: dict[str, messages.HDNodeType] = {}

    try:
        print("Step 1  Main block: load seed, read its account xpub")
        with main_blk as b:
            nodes[b.name] = btc.get_public_node(b.session, parse_path(ACCOUNT), coin_name=COIN).node

        print("Step 2  Key block 2: load seed, read xpub, sign (pass 1)")
        with blk2 as b:
            nodes[b.name] = btc.get_public_node(b.session, parse_path(ACCOUNT), coin_name=COIN).node
            _, ws, order = build_multisig(nodes, [0, 0])
            spk = p2wsh(ws)
            txid, prev = fake_funding_tx(spk)
            addr = btc.get_address(b.session, COIN, parse_path(f"{ACCOUNT}/0/0"),
                                   multisig=build_multisig(nodes, [0, 0])[0],
                                   script_type=messages.InputScriptType.SPENDWITNESS)
            sig2, raw2 = sign(b, nodes, txid, prev)
        print(f"        2-of-2 address: {addr}")
        print(f"        block 2 signature: {sig2.hex()[:32]}...")

        print("Step 3  Main block: sign again WITH block 2's signature (pass 2)")
        with main_blk as b:
            sig_main, raw = sign(b, nodes, txid, prev, sigs={blk2.name: sig2})

        print("\nResults")
        tx = Transaction.parse(raw)
        items = tx.vin[0].witness.items
        check(len(items) == 4 and items[0] == b"",
              "witness has 4 items: empty, sig, sig, script (fully signed 2-of-2)",
              f"witness is not fully signed: {len(items)} items")
        check(items[3] == ws, "witness script = the 2-of-2 script both blocks agreed on",
              "witness script does not match")
        check(tx.vout[1].script_pubkey.data == p2wsh(build_multisig(nodes, [1, 0])[1])
              and tx.vout[1].value == CHANGE,
              "change went back to the 2-of-2 wallet (address 1/0)",
              "change output is wrong")

        sighash = tx.sighash_segwit(0, Script(ws), FUNDING)
        keys = {n: child_pubkey(nodes[n], [0, 0]) for n in nodes}
        expected = {blk2.name: sig2, main_blk.name: sig_main}
        for slot, name in enumerate(order, start=1):
            der = items[slot][:-1]
            check(items[slot][-1] == 1, f"{name}: SIGHASH_ALL", f"{name}: wrong sighash type")
            check(der == expected[name], f"{name}: signature in the tx is the one it produced",
                  f"{name}: signature was changed")
            check(PublicKey.parse(keys[name]).verify(Signature.parse(der), sighash),
                  f"{name}: signature is mathematically valid (BIP143)",
                  f"{name}: signature does NOT verify")

        print("\nSPIKE 2a PASSED: the main block accepted block 2's signature and")
        print("returned a fully signed 2-of-2 transaction. Pass 2 works on stock firmware.")
        print(f"\nSigned transaction ({len(raw)} bytes):\n{raw.hex()}")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
