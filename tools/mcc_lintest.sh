#!/bin/bash
# Differential test helper (WSL only): compiles each given C file with mcc and with gcc, links both against glibc,
# runs them and compares the output.  The mcc object uses libgap's headers, so tests must stick to simple libc calls.
#   bash tools/mcc_lintest.sh tests/mcc/arith.c ...
cd "$(dirname "$0")/.."
export MCC_ROOT="${MCC_ROOT:-$PWD/build/mcc-root}"
mkdir -p build/lintest
fail=0
for f in "$@"; do
    n=$(basename "$f" .c)
    if ! build/host/mcc -c -I user/libc/include -I shared -I user/bin/mcc/include "$f" -o build/lintest/$n.o 2> build/lintest/$n.err; then
        echo "FAIL $n: mcc could not compile it"; head -5 build/lintest/$n.err; fail=1; continue
    fi
    if ! gcc -no-pie -o build/lintest/$n.mcc build/lintest/$n.o -lm 2> build/lintest/$n.lerr; then
        echo "FAIL $n: link"; head -5 build/lintest/$n.lerr; fail=1; continue
    fi
    gcc -w -O0 -o build/lintest/$n.gcc "$f" -lm
    timeout 10 build/lintest/$n.mcc > build/lintest/$n.mcc.out 2>&1; rc1=$?
    timeout 10 build/lintest/$n.gcc > build/lintest/$n.gcc.out 2>&1; rc2=$?
    if [ $rc1 -ne $rc2 ] || ! cmp -s build/lintest/$n.mcc.out build/lintest/$n.gcc.out; then
        echo "FAIL $n: rc $rc1 vs $rc2"; diff build/lintest/$n.mcc.out build/lintest/$n.gcc.out | head -10; fail=1
    else
        echo "ok   $n"
    fi
done
exit $fail
