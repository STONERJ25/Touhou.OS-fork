#!/bin/bash
# Compiles the files in build/stress/ with mcc and with gcc, runs both and compares (WSL).  See tests/mcc_util.py.
cd "$(dirname "$0")/.."
export MCC_ROOT="${MCC_ROOT:-$PWD/build/mcc-root}"
for f in build/stress/*.c; do
    n=$(basename $f .c)
    s=$(date +%s.%N)
    if ! build/host/mcc -c $f -o build/stress/$n.o 2> build/stress/$n.err; then echo "FAIL $n (mcc): $(head -c 300 build/stress/$n.err)"; continue; fi
    e=$(date +%s.%N)
    gcc -no-pie -o build/stress/$n.mcc build/stress/$n.o 2>&1 | head -3
    gcc -w -O0 -o build/stress/$n.gcc $f
    a=$(./build/stress/$n.mcc 2>&1 | md5sum); b=$(./build/stress/$n.gcc 2>&1 | md5sum)
    if [ "$a" = "$b" ]; then echo "ok   $n ($(echo "$e - $s" | bc) s)"; else echo "FAIL $n: output differs"; ./build/stress/$n.mcc | head -c 200; ./build/stress/$n.gcc | head -c 200; fi
done
