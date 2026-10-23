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

## Which boards can get the PHYLAX bootloader

**Not boards that ran Trezor's production bootloader.** That bootloader sets
the STM32's read protection to level 2 (permanent: SWD/ST-Link is switched
off), and it starts any firmware Trezor did not sign in a restricted mode
where the flash controller is blocked, so that firmware cannot rewrite the
bootloader either. Tested: the installer on such a board stops with "This
bootloader cannot be replaced from here. Nothing was changed." On those
boards PHYLAX firmware always shows the "Unofficial firmware" warning and
every update erases the seed. Real Trezor Ones are the same.

**Yes:** blank or unlocked STM32F205 chips, over ST-Link (below). Check a
board first with `st-info --probe`: an unlocked chip answers with its flash
size; a level-2 chip does not answer.

## Updating a PHYLAX bootloader later (installer)

The `blinstall` build replaces the bootloader from running firmware after
asking on the device ("Install the PHYLAX bootloader?"). It only has the
rights to do so when the bootloader that starts it trusts it, i.e. under a
PHYLAX bootloader and **signed** with the PHYLAX keys like any release:

1. Write down the block's 24 words.
2. Sign `blinstall/firmware/firmware.bin` (`phylax_sign.py sign`), go to
   bootloader mode and flash the signed file.
3. Confirm **Install** (right button); never unplug during "Updating
   bootloader". Then reconnect and flash signed `main`/`key` firmware.

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
