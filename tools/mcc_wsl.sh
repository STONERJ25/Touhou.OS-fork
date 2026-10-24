#!/bin/bash
# Runs a command from the checkout root inside WSL with the mcc environment:  wsl -e bash tools/mcc_wsl.sh build/host/mcc -E x.c
cd "$(dirname "$0")/.."
export MCC_ROOT="${MCC_ROOT:-$PWD/build/mcc-root}"
exec "$@"
