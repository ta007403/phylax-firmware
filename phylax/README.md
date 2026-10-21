# PHYLAX Blocks

Snap-together multisig built on Trezor One (legacy) firmware v1.14.1. Each
key block holds one seed; the main block talks to the PC over USB and to the
other blocks over the PHYLAX bus (RS485 on hardware, UDP in the emulator).

Personal learning project. **Experimental, unaudited firmware: do not store
real savings on it.**

## Firmware roles

| Role | Emulator | What it does |
| --- | --- | --- |
| stock | (default) | Plain Trezor One behaviour, bus unused |
| main | `PHYLAX_ROLE=main` | USB to the PC; finds the other blocks (discovery); relays SignTx to each key block in turn (pass 1), then signs itself, placing each key block's signature in the slot whose key verifies it (pass 2) |
| key | `PHYLAX_ROLE=key` | Answers discovery with its id and seed fingerprint; answers Trezor messages that arrive over the bus |
| storage | `PHYLAX_ROLE=storage` | No seed; keeps up to 8 wallet records in flash, read and written over the bus in chunks of at most 128 bytes |

On real hardware the role is chosen at build time with `-DPHYLAX_ROLE=1`
(main) or `2` (key). The RS485 transport is not written yet, so device
builds behave as stock for now.

## Emulator settings

| Variable | Default | Meaning |
| --- | --- | --- |
| `TREZOR_UDP_PORT` | 21324 | Host port; DebugLink is port + 1 |
| `PHYLAX_ROLE` | (none) | `main` or `key` |
| `PHYLAX_BUS_NODE` | 0 main, 1 key | Slot 0..7 on the emulated shared bus; must be unique |
| `PHYLAX_BUS_PORT_BASE` | 21500 | Bus UDP port = base + node; every frame goes to all nodes |
| `PHYLAX_EMU_AUTOCONFIRM` | off | `1`: confirm prompts that come over the bus (tests only) |

Bus addresses are not configured: the main block is always 0x01 and hands
key blocks 0x10, 0x11, ... during discovery.

## Discovery

Every 2 s the main block broadcasts DISCOVER. Each block answers HELLO with
its 12-byte id, its kind, whether it has a seed or is locked, its seed's
BIP32 master fingerprint (no passphrase) and its firmware version. New blocks
get the next free address; a block silent for 6.5 s is removed. The PC can
ask for the current stack with `PhylaxGetBlocks` (message 7700, answer
`PhylaxBlocks` 7701, see `common/protob/messages-phylax.proto`;
`phylax/tests/phylax_msgs.py` registers them with trezorlib).

Up to two key blocks take part (3-of-3 is the design limit). A key block
whose key is not in the wallet rejects the input before showing anything and
is skipped, so a stranger block in the stack does not stop signing.

If no key block answers for 10 s during signing, the main block cancels with
"Key block disconnected". With no key block in the stack, SignTx fails at
once with "No key block attached".

## Saved wallets (storage block)

A wallet is `wsh(sortedmulti(n, ...))`, n-of-n, one key per block with seed.
The storage block keeps each wallet's name, fingerprints and BIP48 account
xpubs (`m/48'/0'/0'/2'` and `m/48'/1'/0'/2'`) as a 473-byte record at most,
under public, writable Trezor storage keys (no secrets in it, no PIN
needed). Records travel over the bus in chunks of at most 128 bytes.

When a storage block is in the stack, the main block loads all records and
compares them with the seed fingerprints present:

- the blocks match a saved wallet: "*2-of-2 #1* is ready."
- a new combination of 2 or 3 blocks: "New 2-of-2 wallet?" with the
  fingerprints; on **Create** (right button) it fetches the key blocks'
  xpubs over the bus and saves the record; **No** is remembered until the
  blocks change
- a saved wallet is missing one block: "Attach key block 777dfe5d to use
  wallet *3-of-3 #1*"

The PC reads the list with `PhylaxGetWallets` (fingerprints only) and one
wallet with its xpubs with `PhylaxGetWallet` (enough to build the
descriptor). Without a storage block everything still works; there are just
no saved wallets and no prompts.

In the emulator, `PHYLAX_EMU_AUTOCONFIRM=1` on the main block answers
"Create" automatically (tests only).

## Tests

Build the emulator first (`EMULATOR=1 DEBUG_LINK=1 BITCOIN_ONLY=1`), then
from `legacy/` run everything with `../phylax/tests/run_all.sh` (add
`--show` to watch the emulator windows), or one test at a time:

```
uv run --with embit python ../phylax/tests/spike2a.py   # signature hand-over, stock firmware
uv run --with embit python ../phylax/tests/spike2b.py   # main block relays to a key block over the bus
uv run --with embit python ../phylax/tests/test_timeout.py    # 10 s timeout, slow user, no key block
uv run --with embit python ../phylax/tests/test_discovery.py  # blocks snapped on and off, fingerprints
uv run --with embit python ../phylax/tests/test_3of3.py       # 3-of-3, and a stranger key block skipped
uv run --with embit python ../phylax/tests/test_storage.py    # wallets saved, read back, 2-of-2 then 3-of-3
```

Add `--headless` to a single test to run it without emulator windows.
