# PHYLAX releases: your own signing keys and bootloader

A Trezor One bootloader only trusts firmware signed with **Trezor's** keys.
PHYLAX firmware is not, so a block with Trezor's bootloader:

- shows **"WARNING! Unofficial firmware detected"** and **"Compare
  fingerprints"** at every start (two button presses), and
- **erases its seed on every firmware update** (the bootloader keeps the seed
  only when both the old and the new firmware are signed).

The **PHYLAX bootloader** is the same bootloader with **PHYLAX's** keys and
the PHYLAX logo. With it, PHYLAX firmware signed on your release Trezor
starts like a real Trezor: no warning, no fingerprint screen, and updates keep
the seed. Firmware signed by anybody else (Trezor's included) gets the
warning.

## The keys

Three keys on **one Trezor One** (the "release Trezor"), at

| key | path |
|-----|------|
| 1 | `m/44'/0'/97'/0/0` |
| 2 | `m/44'/0'/98'/0/0` |
| 3 | `m/44'/0'/99'/0/0` |

Every release is signed with keys 1 and 2 (the bootloader asks for two of the
three, like Trezor's). They come from the release Trezor's 24 words: keep
that backup safe. **If those words are lost, no new firmware can ever be
installed on a PHYLAX-bootloader board without the warning.** Use a Trezor
that does nothing else, or at least never put coins on those three addresses.

## Once: make the keys and the bootloader

1. Connect the release Trezor One and run (from the repository root):

       uv run --with ecdsa python phylax/release/phylax_sign.py keys

   It writes `legacy/phylax_fw_keys_release.h` (public keys only).
2. Commit and push that file. From then on the CI run builds, for the
   PHYLAX roles, the PHYLAX bootloader too, and a fourth artifact:
   `trezor-one-bitcoinonly-blinstall-...` (the bootloader installer). Its
   log prints `bootloader hash: ...`.
3. Add that hash to `legacy/firmware/phylax_bl_hashes.h` and commit. (PHYLAX
   firmware built in the same CI run already knows its own bootloader; the
   list keeps older PHYLAX bootloaders working with newer firmware.)

## Every release: sign the firmware

1. Download the `main` / `key` artifacts of the CI run.
2. Sign each `firmware.bin` (the Trezor shows the header hash twice; check it
   against the first line the script prints):

       uv run --with ecdsa python phylax/release/phylax_sign.py sign firmware.bin

   The script checks the result against the release keys before it writes
   `firmware.signed.bin`. `phylax_sign.py verify FILE` checks a file again.
3. Flash it with the block in bootloader mode (hold both buttons while
   plugging it in). `-s` because trezorctl only knows Trezor's keys:

       trezorctl firmware update -s -f firmware.signed.bin

## Once per board: install the PHYLAX bootloader

Boards that ever ran Trezor's production bootloader (yours did: they show the
"Unofficial firmware" warning) have the STM32's read protection at level 2.
That is permanent: SWD (ST-Link) cannot connect to them any more. The
bootloader can only be replaced from running firmware, the way Trezor updates
its own bootloaders. That is what the installer does.

**This cannot be undone.** Afterwards the board only runs PHYLAX-signed
firmware without warnings; official Trezor firmware would stop with "Unknown
bootloader". A real Trezor One treated this way becomes a PHYLAX board for
good. If a write goes wrong, a locked board cannot be recovered over SWD.
**Try it on a spare board first.**

1. Write down the block's 24 words (flashing unsigned firmware erases them).
2. Bootloader mode, then flash the installer:

       trezorctl firmware update -s -f blinstall/firmware.bin

3. Accept the "Unofficial firmware" warning once. The installer asks
   **"Install the PHYLAX bootloader?"**: right button = Install, left =
   Cancel (nothing changes). It writes and checks the bootloader, then asks
   you to reconnect.
4. After reconnecting it says **"bootloader is installed. Now install signed
   PHYLAX firmware."** Go to bootloader mode (now with the PHYLAX logo) and
   flash a signed `main` or `key` firmware (above).
5. Restore the block's seed if it had one. From now on updates keep it.

PHYLAX firmware itself never replaces a bootloader: it runs on Trezor's and
on PHYLAX's, so a real Trezor One running PHYLAX firmware stays a Trezor One.

## New, blank chips (your own PHYLAX hardware)

A blank STM32F205 has no read protection yet, so the ST-Link can write the
PHYLAX bootloader directly:

    st-flash write bootloader.bin 0x08000000

On its first start the PHYLAX bootloader locks the chip (read protection
level 2, like Trezor's), so check the board works before you rely on SWD.

## Tests

`phylax/tests/test_fw_signatures.py` signs a firmware header on an emulated
Trezor One holding the public test mnemonic and runs the real
`legacy/fw_signatures.c` (built for the PC with the development keys,
`legacy/phylax_fw_keys_dev.h`): signed with keys 1 and 2 is accepted;
unsigned, changed, the same key twice, or Trezor's key paths are refused.
`phylax_sign.py --dev` uses the development keys; they can never be built
into a PRODUCTION bootloader.
