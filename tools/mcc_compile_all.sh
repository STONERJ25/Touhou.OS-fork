#!/bin/bash
# Compiles every C file of the OS (libc, libraries, programs) with the host mcc, to see what it can not digest yet.
# Usage: bash tools/mcc_compile_all.sh [dir ...]      (objects go to build/mcc-all/, a summary is printed)
cd "$(dirname "$0")/.."
export MCC_ROOT="${MCC_ROOT:-$PWD/build/mcc-root}"
mkdir -p build/mcc-all
dirs=("$@")
if [ ${#dirs[@]} -eq 0 ]; then dirs=(user/bin user/sbin user/lib user/libc/src); fi
ok=0; bad=0
incs="-I user/libc/include -I shared -I user/bin/mcc/include -I user/lib/kappa/include -I user/lib/fairy/include -I user/lib/z/include -I user/lib/img/include -I user/lib/audio/include"
for d in "${dirs[@]}"; do
    while IFS= read -r f; do
        n=$(echo "$f" | tr '/' '_')
        sub=$(dirname "$f")
        if build/host/mcc -c $incs -I "$sub" "$f" -o build/mcc-all/$n.o 2> build/mcc-all/$n.err; then
            ok=$((ok+1))
        else
            bad=$((bad+1))
            echo "FAIL $f: $(grep -m1 error build/mcc-all/$n.err)"
        fi
    done < <(find "$d" -name '*.c' | sort)
done
echo "compiled $ok files, $bad failed"
