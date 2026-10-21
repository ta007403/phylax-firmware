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
| main | `PHYLAX_ROLE=main` | USB to the PC; relays SignTx to a key block (pass 1), then signs with its signature inserted (pass 2) |
| key | `PHYLAX_ROLE=key` | Answers Trezor messages that arrive over the bus |

On real hardware the role is chosen at build time with `-DPHYLAX_ROLE=1`
(main) or `2` (key). The RS485 transport is not written yet, so device
builds behave as stock for now.

## Emulator settings

| Variable | Default | Meaning |
| --- | --- | --- |
| `TREZOR_UDP_PORT` | 21324 | Host port; DebugLink is port + 1 |
| `PHYLAX_ROLE` | (none) | `main` or `key` |
| `PHYLAX_BUS_ADDR` | 0x01 main, 0x10 key | This block's bus address |
| `PHYLAX_KEY_ADDR` | 0x10 | Key block the main block relays to |
| `PHYLAX_BUS_PORT_BASE` | 21500 | Bus UDP port = base + bus address |
| `PHYLAX_EMU_AUTOCONFIRM` | off | `1`: confirm prompts that come over the bus (tests only) |

## Tests

Build the emulator first (`EMULATOR=1 DEBUG_LINK=1 BITCOIN_ONLY=1`), then
from `legacy/`:

```
uv run --with embit python ../phylax/tests/spike2a.py   # signature hand-over, stock firmware
uv run --with embit python ../phylax/tests/spike2b.py   # main block relays to a key block over the bus
```

Add `--headless` to run without emulator windows.
