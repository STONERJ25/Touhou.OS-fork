#!/bin/bash
# M5 survey: compiles every C file of the kernel (and shared/*.c) with the host mcc, with the kernel's flags, and lists what fails.
#   bash tools/mcc_kernel.sh [file.c ...]       (objects land in build/mcc-kernel/, one .err file per failure)
cd "$(dirname "$0")/.."
export MCC_ROOT="${MCC_ROOT:-$PWD/build/mcc-root}"
mkdir -p build/mcc-kernel
if [ $# -gt 0 ]; then files=("$@"); else mapfile -t files < <(find kernel shared -name '*.c' | sort); fi
ok=0; bad=0
for f in "${files[@]}"; do
    n=$(echo "$f" | tr '/' '_')
    if build/host/mcc -c -nostdinc -DKERNEL=1 -I kernel/include -I shared -I user/bin/mcc/include "$f" -o build/mcc-kernel/$n.o 2> build/mcc-kernel/$n.err; then
        ok=$((ok+1))
    else
        bad=$((bad+1))
        echo "FAIL $f: $(grep -m1 'error' build/mcc-kernel/$n.err)"
    fi
done
echo "kernel: $ok files compiled, $bad failed"
