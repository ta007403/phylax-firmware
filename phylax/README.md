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
| key | `PHYLAX_ROLE=key` | Answers discovery with its id and seed fingerprint; answers Trezor messages that arrive over the bus; keeps a copy of every saved wallet |

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
| `PHYLAX_EMU_PIN_KEYS` | none | key block, with autoconfirm: button presses for PIN entry on the block, `L`/`R`/`B` (both); the carousel then starts on "1" (tests only) |

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

## Saved wallets (a copy on every block)

There is no separate storage block: every block keeps a copy of every saved
wallet in its own flash, so losing any block - the main block included -
loses no wallet. Hardware: one PCB for all blocks; the main block has the
USB port and pins on one side, key blocks pins on both sides. The bus has
120 ohm termination and bias resistors on the main block only.

A wallet is `wsh(sortedmulti(n, ...))`, n-of-n, one key per block with seed.
A record holds its name, a generation counter, the fingerprints and the
BIP48 account xpubs (`m/48'/0'/0'/2'` and `m/48'/1'/0'/2'`): 477 bytes at
most, under public, writable Trezor storage keys (no secrets in it, no PIN
needed). Records travel over the bus in chunks of at most 128 bytes.

The main block reads the copy of every block that joins, merges it with its
own (a wallet is identified by its set of fingerprints; the higher
generation wins) and writes the result back wherever a copy is missing or
older. Then it compares the wallets with the seed fingerprints present:

- the blocks match a saved wallet: "*2-of-2 #1* is ready."
- a new combination of 2 or 3 blocks: "New 2-of-2 wallet?" with the
  fingerprints; on **Create** (right button) it fetches the key blocks'
  xpubs over the bus and saves the wallet on every block; **No** is
  remembered until the blocks change
- a saved wallet is missing one block: "Attach key block 777dfe5d to use
  wallet *3-of-3 #1*"

The PC reads the list with `PhylaxGetWallets` (fingerprints only) and one
wallet with its xpubs with `PhylaxGetWallet` (enough to build the
descriptor).

In the emulator, `PHYLAX_EMU_AUTOCONFIRM=1` on the main block answers
"Create" automatically (tests only).

## Setting up a key block with no PC

When a key block without a seed joins, the main block asks **"New key block
found. Set it up?"** on its own screen (right button: Set up, left: No).
Then the key block does the rest on its own screen and buttons:

1. "Create a new wallet?" - confirm.
2. Choose a PIN, then enter it again. Left/right move through
   `1..9, OK, DEL` (it wraps around and starts on a random digit, like the
   Trezor Safe 3); **both buttons together** select. Digits 1-9 only, the
   same as Suite's PIN grid, so this PIN also unlocks the block from Suite.
3. Write down the 24 words; they are shown twice.

The seed is `SHA256(SHA256(key TRNG || button timings) || main block TRNG)`:
the key block's hardware random number generator, the timing of every
button change during PIN entry (in ms and in loop turns), and 32 bytes from
the main block's TRNG in place of the PC's entropy. Each source can only
add to the others. The words never leave the key block.

On the bus this is the normal Trezor One `ResetDevice` flow, with the main
block in the PC's place (`LOCAL_UI(1)` before it tells the key block to take
the PIN on its own buttons instead of sending a `PinMatrixRequest`). "No",
or cancelling on the key block, is remembered until that block is removed;
mismatched PINs are asked again after a few seconds. Afterwards discovery
shows the block's fingerprint and the main block offers "New 2-of-2 wallet?"
as usual. Signing or the PC selecting a block cancels a setup in progress.

## Reaching a key block from the PC (select mode)

Key blocks have no USB of their own: the PC reaches them through the main
block. `PhylaxSelectBlock(address)` (message 7706) puts the main block in
pass-through mode: every PC message goes to that key block unchanged and
every answer comes back, so the normal Trezor One flows work on it:

- set up: it creates its own seed and shows the words on its own screen
- recover: words typed on the PC, asked in a shuffled order with fake words
- set or change its PIN, wipe it, lock it
- unlock it: any PIN-protected request, e.g. reading its account key with
  `GetPublicKey` (Trezor One's `Ping` does not check the PIN)

The PC's PIN grid is blank: the user clicks the positions shown on the key
block's own scrambled keypad. `PhylaxSelectBlock(1)` returns to the main
block. A wiped block gets a new id (Trezor renews it on wipe), so it shows
up again as a new block, at a new address.

During signing, a locked key block asks for its PIN through the main block:
the PC shows its grid, the main block says which key block's keypad to look
at. The main block's own background requests (fetching xpubs for a new
wallet) never prompt the user: if a key block asks for anything, the main
block cancels it and tries again later.

## Known limits

- A key block must not be driven over USB and the bus at the same time: it
  has one message parser, so the two conversations would mix. On real
  hardware the key blocks' USB ports are covered; the emulator tests read a
  key block's xpub before the main block starts.
- The RS485 (USART2, PA1-PA3) transport is not written yet, so device builds
  behave as stock Trezor firmware until it is.
- No wallet rename or delete from the PC yet.

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
uv run --with embit python ../phylax/tests/test_wallets.py    # wallets on every block, main block replaced
uv run --with embit python ../phylax/tests/test_keyblocks.py  # set up, recover, unlock a key block; PIN mid-signing
uv run --with embit python ../phylax/tests/test_setup.py      # set up a key block from the main block's buttons, no PC
```

Add `--headless` to a single test to run it without emulator windows.
