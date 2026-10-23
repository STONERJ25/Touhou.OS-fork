#!/bin/bash
# Builds the host copy of mcc/mld/mar with gcc (run it through WSL on Windows):  bash tools/mcc_host.sh [extra gcc flags]
# The binaries land in build/host/ (inside the checkout, so the Windows side can see them too).
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-but-set-variable "$@" \
    -o build/host/mcc user/bin/mcc/main.c
cp -f build/host/mcc build/host/mld
cp -f build/host/mcc build/host/mar
echo "built build/host/mcc"
