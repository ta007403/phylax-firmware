#!/usr/bin/env bash
# Run every PHYLAX emulator test. From the legacy folder, after building the
# emulator with EMULATOR=1 DEBUG_LINK=1 BITCOIN_ONLY=1:
#
#   cd ~/phylax-firmware/legacy
#   ../phylax/tests/run_all.sh            # add --show to see the emulator windows
set -u
cd "$(dirname "$0")/../../legacy"
headless="--headless"
[ "${1:-}" = "--show" ] && headless=""
failed=0
for t in spike2a spike2b test_timeout test_discovery test_3of3 test_wallets test_keyblocks test_setup test_rename \
  test_entropy:coin test_entropy:dice test_entropy:short test_fw_signatures; do
  printf '%-20s ' "$t"
  name=${t%%:*}
  mode=""
  [ "$name" != "$t" ] && mode="--mode ${t#*:}"
  # shellcheck disable=SC2086
  if out=$(timeout 600 uv run --with embit --with ecdsa python ../phylax/tests/$name.py $headless $mode 2>&1); then
    echo "PASSED"
  else
    echo "FAILED"
    echo "$out" | tail -15 | sed 's/^/    /'
    failed=1
  fi
done
exit $failed
