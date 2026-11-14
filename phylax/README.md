# PHYLAX Snap

(Formerly PHYLAX Blocks; the code still uses `phylax_` names.) Snap-together multisig built on Trezor One (legacy) firmware v1.14.1. Each
key block holds one seed; the main block talks to the PC over USB and to the
other blocks over the PHYLAX bus (RS485 on hardware, UDP in the emulator).

Personal learning project. **Experimental, unaudited firmware: do not store
real savings on it.**

## Firmware roles

| Role | Emulator | What it does |
| --- | --- | --- |
| stock | (default) | Plain Trezor One behaviour, bus unused |
| main | `PHYLAX_ROLE=main` | USB to the PC; finds the other blocks (discovery); relays SignTx to each key block of the saved wallet being spent in turn (pass 1; the wallet whose keys are input 0's cosigners; every set-up key block if none matches), then signs itself, placing each key block's signature in the slot whose key verifies it (pass 2) |
| key | `PHYLAX_ROLE=key` | Answers discovery with its id and seed fingerprint; answers Trezor messages that arrive over the bus; keeps a copy of every saved wallet |

On real hardware the role is chosen at build time (`PHYLAX_BUILD_ROLE=main`
or `key`).

### The bus on real hardware (Phylax_Version_3 board)

| | |
|---|---|
| Transceiver | MAX3485 on USART2: PA2 = TX -> DI, PA3 = RX <- RO |
| Direction | PA1 drives DE and /RE together (R8 10k pull-down = receive) |
| Speed | 250 kbit/s, 8N1 (a 64-byte Trezor packet takes about 3 ms) |
| Connector J1 | 1 = +5V, 2 = GND, 3 = A, 4 = B |
| Termination | close JP1 (120R) on the **main block only** |

Received bytes go from the USART2 interrupt into a 2 KB ring buffer in RAM
and from there to the frame decoder in the main loop. Sending waits until
the line has been quiet for at least 1 ms, raises PA1, sends the frame,
waits for the last stop bit and lowers PA1. Only the main block starts a
conversation; key blocks answer, and answer DISCOVER in one of 32 random
4 ms slots, a new slot every round.

The UART interrupt is set up while the firmware is still privileged (before
the MPU is enabled), so the bus needs firmware signed for the bootloader on
the board, i.e. PHYLAX firmware on a PHYLAX bootloader. Unsigned firmware
has no bus.

Only the main block is plugged into the PC; it powers the key blocks over
the bus +5V (each board's USB VBUS reaches +5V through a BAT60 diode, so a
second USB cable cannot feed back into a PC).

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

Up to three key blocks take part (4-of-4 is the design limit: main block +
3 key blocks). With a fourth key block attached the main block shows "Too
many key blocks, use at most 3 at a time" and refuses multisig signing with
"Too many key blocks (max 3)". A key block
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
BIP48 account xpubs (`m/48'/0'/0'/2'` and `m/48'/1'/0'/2'`): 627 bytes at
most (4-of-4), under public, writable Trezor storage keys (no secrets in it,
no PIN needed). 8 slots: every combination of the main block and up to 3 key
blocks (7 wallets) fits, about 3 KB. Every block must run firmware that knows
4-of-4 records (older firmware refuses records of more than 3 keys). Records travel over the bus in chunks of at most 128 bytes.

The main block reads the copy of every block that joins, merges it with its
own (a wallet is identified by its set of fingerprints; the higher
generation wins) and writes the result back wherever a copy is missing or
older. Then it compares the wallets with the seed fingerprints present:

- the blocks match a saved wallet: "*2-of-2 #1* is ready."
- a new combination of 2 to 4 blocks: "New 2-of-2 wallet?" with the
  fingerprints; on **Create** (right button) it fetches the key blocks'
  xpubs over the bus and saves the wallet on every block; **No** then asks
  "Ask again later?": **Later** is remembered until the blocks change,
  **Never** in the main block's own flash (see below). No new wallet is offered while
  a key block in the stack is locked (also one put off with "Later"): the
  blocks without it are not the combination the user put together
- a saved wallet is missing one block: "Attach key block 777dfe5d to use
  wallet *3-of-3 #1*"

The PC reads the list with `PhylaxGetWallets` (fingerprints only) and one
wallet with its xpubs with `PhylaxGetWallet` (enough to build the
descriptor).

In the emulator, `PHYLAX_EMU_AUTOCONFIRM=1` on the main block answers
"Create" automatically (tests only); with `PHYLAX_EMU_NEW_WALLET=never` or
`later` it answers "No" and then that.

### "Never"

A combination answered **Never**, or whose wallet the PC deleted, is kept in
the main block's config storage (`KEY_PHYLAX_IGNORED`, until the main block
is wiped; at most 8, the oldest is dropped) and is not offered again, also
after a restart or when the blocks are swapped and put back. Other
combinations of the same blocks are still offered. `PhylaxWallets.ignored`
lists them for the PC, and `PhylaxForgetIgnored(fingerprints)` (7710, no
confirmation) forgets one: the main block offers it again as soon as those
blocks are all attached. PHYLAX Suite: "Never" card on the PHYLAX Snap page,
"Ask again". Firmware without the list reports none.

## Renaming and deleting wallets from the PC

- `PhylaxSetWalletName(slot, name)` (7707): 1-15 printable ASCII characters,
  not the name of another saved wallet (case-insensitive). Bad names are
  refused at once; otherwise the main block shows "Rename wallet <old> to
  <new>" and the user confirms on its buttons.
- `PhylaxDeleteWallet(slot)` (7708): "Delete wallet <name> from every
  block?", confirmed on the main block. Only the record goes (fingerprints
  and xpubs); the coins stay safe and the seeds can rebuild the descriptor.
- `PhylaxUnlockBlock(address)` (7709): wake up (unlock) the locked key block
  at that bus address, as after "Unlock" on the main block's own prompt: the
  PIN goes on the key block's own buttons. Answered at once; the stack shows
  the block unlocked when it is done. PHYLAX Suite's "Wake up" button.

Both raise the wallet's generation, so the change reaches every block's copy,
including blocks attached later with an older copy. A deleted wallet stays
as a hidden record (same identity, deleted flag) so such a block cannot
bring it back. A deleted wallet's blocks go on the main block's "Never" list
(above); after "Ask again" the main block offers them again and the wallet
returns in its old slot. Refused while a key block is
being set up or a transaction is being signed.

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

Before that, the main block asks **"Add your own randomness?"** (Skip / Add)
and, on Add, **"Coin flips or dice?"** (Coin / Dice):

- **Coin**: flip a real coin 256 times; left = heads, right = tails, both
  buttons = undo the last flip (or go back, before the first).
- **Dice**: roll a real 6-sided die 99 times (99 x log2 6 = 255.9 bits); the
  carousel `1..6, DEL` works like the PIN entry, both buttons select. DEL
  before the first roll goes back.

Either is full strength on its own, and is mixed in, never used alone.

The seed is `SHA256(SHA256(key TRNG || button timings) || main bytes)` where
`main bytes = SHA256(SHA256(main TRNG || main button timings) ||
SHA256("PHYLAX coin flips v1" or "PHYLAX dice rolls v1" || your flips))`
(the last part only if you added your own): the key block's hardware random
number generator, the timing of every button change on both blocks, and the
main block's TRNG in place of the PC's entropy, plus your coin or dice.
Each source can only add to the others. The flips are wiped from memory once
hashed, and the words never leave the key block.

On the bus this is the normal Trezor One `ResetDevice` flow, with the main
block in the PC's place (`LOCAL_UI(1)` before it tells the key block to take
the PIN on its own buttons instead of sending a `PinMatrixRequest`). "No",
or cancelling on the key block, is remembered until that block is removed;
mismatched PINs are asked again after a few seconds. Afterwards discovery
shows the block's fingerprint and the main block offers "New 2-of-2 wallet?"
as usual. Signing or the PC selecting a block cancels a setup in progress.

### Your own randomness when a block creates its own wallet

The same two questions (and the same coin and dice screens) also come up when
a PHYLAX block creates **its own** wallet from the PC: the main block, or a key
block set up from Suite in select mode. After "Create a new wallet?" and the
PIN, the block asks "Add your own randomness?"; meanwhile the PC just waits
(it got a normal `ButtonRequest`, so Suite shows its "follow the device"
screen). Nothing changes in Suite.

The hashed flips or rolls are mixed into the block's own random bytes in every
round: `int_entropy = SHA256(SHA256(TRNG || button timings) || your digest)`,
and the seed is `SHA256(int_entropy || PC entropy)` as on any Trezor One.
Suite's entropy check still passes, because what the block commits to and
later reveals is the already-mixed value. Cancelling on the PC while the block
asks stops the wallet creation and wipes the flips. A key block being set up
by the main block is not asked twice (the main block asked already). Stock
firmware builds never ask.

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

### A locked key block

A key block locks itself after a while, like any Trezor One. It still tells
the main block who it is: its seed fingerprint is kept in its public storage
from its last unlock (it is in the descriptor of every wallet it is part of;
a wipe erases it). The main block does not count a locked block as present,
says "Key block 2b69c617 is locked" for the wallet that needs it, and asks
**"Key block 2b69c617 is locked. Unlock it with its PIN on its own buttons?"**
(Later / Unlock). On Unlock the main block sends the key block a
PIN-protected request (GetPublicKey m/0', result discarded) with
`LOCAL_UI(1)`, so the PIN is entered on the key block's own buttons, never on
the PC; Cancel on the main block stops it. "Later" is not asked again until
that block is unplugged or locks again after an unlock.

While the main block asks "Unlock it?" (or "New key block found. Set it
up?"), the key block it means shows **"Main block asks: Unlock this block?"**
with its own fingerprint (or "Set up this block?"), "Answer on the main
block.", so it can be found in the stack. The main block sends
`ATTENTION(kind)` (0x31: 0 off, 1 unlock, 2 setup) every 2 s while its prompt
is up and `ATTENTION(0)` when it goes; without a repeat for 6 s (main block
unplugged) the key block shows its home screen again. Only the home screen
(or the screensaver) is replaced: the PIN entry of an unlock, or a PC
request, stays. On Unlock / Set up the main block sends `ATTENTION(0)` before
it starts, and the key block never redraws during the main block's
conversation (`LOCAL_UI`): its PIN entry does not count as a screen of its
own, so the home screen used to be drawn over it. While the request is shown
the key block does not go to sleep (auto-lock screensaver): asleep for longer
than its auto-lock delay, it used to fall asleep again at once after each
repeat (a blinking screen). Older key firmware ignores the message
(`phylax_attention.c`).

### Screens

On a main block the home screen is the PHYLAX prompt, setup step or status,
so the PC's requests (which all end with the home screen; PHYLAX Suite asks
every 2-3 s) do not flip the screen. A prompt takes only a press made while it
is on screen, started after it was drawn. With buttons, a dialog has room for
five lines of text.

## Known limits

- A key block must not be driven over USB and the bus at the same time: it
  has one message parser, so the two conversations would mix. On real
  hardware the key blocks' USB ports are covered; the emulator tests read a
  key block's xpub before the main block starts.
- The RS485 transport is built but not yet tried on hardware.
- Bytes that arrive while the CPU is stalled by a flash erase (the
  interrupt code lives in flash) are lost; the frame fails its CRC and the
  request is retried.
- A deleted wallet's hidden record is overwritten when all 8 slots are
  needed; a block that missed that delete could then bring the wallet back.

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
uv run --with embit python ../phylax/tests/test_rename.py     # rename and delete wallets from the PC, confirmed on the main block
uv run --with embit python ../phylax/tests/test_entropy.py --mode coin        # coin/dice when setting up a key block
uv run --with embit python ../phylax/tests/test_reset_entropy.py --mode coin  # coin/dice when the PC creates a wallet
uv run --with embit python ../phylax/tests/test_receive.py    # a wallet's receive address on the main block
uv run --with embit python ../phylax/tests/test_screen.py     # the screen stays put while the PC polls
uv run --with embit python ../phylax/tests/test_unlock.py     # a locked key block unlocked from the main block
uv run --with embit python ../phylax/tests/test_pin_screen.py # the PIN grid stays on screen
uv run --with embit python ../phylax/tests/test_pin_seed.py   # the right key right after the PIN (no nested seed computation)
uv run --with embit python ../phylax/tests/test_wake.py       # wake up a locked key block from the PC
uv run --with embit python ../phylax/tests/test_missing_sig.py # never a half-signed transaction
uv run --with embit python ../phylax/tests/test_sortedmulti.py # sign when the PC lists the keys unsorted (sortedmulti)
uv run --with embit python ../phylax/tests/test_rbf_recipients.py # replaceable (RBF) send to two recipients with change
uv run --with embit python ../phylax/tests/test_singlesig.py   # a single-sig spend is signed by the main block alone
uv run --with embit python ../phylax/tests/test_set_pin_locked.py # set a PIN on a block without one after it auto-locked
uv run --with embit python ../phylax/tests/test_4of4.py       # 4-of-4 (main + 3 key blocks), and a 4th key block refused
uv run --with embit python ../phylax/tests/test_never.py      # "Never" kept across restarts, forgotten from the PC, delete = Never
uv run --with embit python ../phylax/tests/test_locked_offer.py # no new wallet offered while a key block is locked
uv run --with embit python ../phylax/tests/test_attention.py  # the key block asked about shows "Unlock this block?" (--shots DIR: PNGs)
uv run --with embit python ../phylax/tests/test_relay_wallet.py # signing asks only the spent wallet's key blocks (a locked other one is not asked)
```

The emulator is built with `LOCAL_CONFIDENTIAL` static, as on the device
(`legacy/Makefile.include`), so re-entrancy bugs show up in these tests too.

Add `--headless` to a single test to run it without emulator windows.

## Releases, signing and the PHYLAX bootloader

See [release/README.md](release/README.md): PHYLAX's own firmware signing keys
(on one Trezor One), the PHYLAX bootloader (no "Unofficial firmware" warning,
updates keep the seed) and the one-time bootloader installer.
