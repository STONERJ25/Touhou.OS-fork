#!/bin/bash
# Builds the host copy of mcc/mld/mar with gcc (run it through WSL on Windows):  bash tools/mcc_host.sh [extra gcc flags]
# The binaries land in build/host/ (inside the checkout, so the Windows side can see them too).
# Debugging: MCC_ASAN=1 bash tools/mcc_host.sh   builds build/host/mcc-asan with the address sanitizer.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
if [ -n "$MCC_ASAN" ]; then
    gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w -o build/host/mcc-asan user/bin/mcc/main.c
    echo "built build/host/mcc-asan"
    exit 0
fi
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-but-set-variable -Wno-missing-field-initializers "$@" \
    -o build/host/mcc user/bin/mcc/main.c
cp -f build/host/mcc build/host/mld
cp -f build/host/mcc build/host/mar
echo "built build/host/mcc"
