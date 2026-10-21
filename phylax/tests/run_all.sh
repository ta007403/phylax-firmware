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
for t in spike2a spike2b test_timeout test_discovery test_3of3 test_wallets; do
  printf '%-16s ' "$t"
  if out=$(uv run --with embit python ../phylax/tests/$t.py $headless 2>&1); then
    echo "PASSED"
  else
    echo "FAILED"
    echo "$out" | tail -15 | sed 's/^/    /'
    failed=1
  fi
done
exit $failed
