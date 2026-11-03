#!/bin/bash
# Speed of mcc's code against gcc's (WSL / Linux):  bash tools/mcc_bench.sh
# tests/mcc/bench/bench.c is compiled by mcc (its object linked against glibc, as tools/mcc_lintest.sh does) and by gcc -O0 and -O2; each kernel
# prints its time and a checksum, and the checksums must agree.  Takes about half a minute.
cd "$(dirname "$0")/.."
export MCC_ROOT="${MCC_ROOT:-$PWD/build/mcc-root}"
mkdir -p build/bench
src=tests/mcc/bench/bench.c
build/host/mcc -c -I user/libc/include -I shared -I user/bin/mcc/include $src -o build/bench/bench.o || exit 1
gcc -no-pie -o build/bench/bench.mcc build/bench/bench.o || exit 1
gcc -w -O0 -o build/bench/bench.gcc0 $src
gcc -w -O2 -o build/bench/bench.gcc2 $src
for k in sort sieve sha256 mandel hash; do
    for v in mcc gcc0 gcc2; do build/bench/bench.$v $k > build/bench/$k.$v.txt; done
    a=$(awk '{print $2}' build/bench/$k.mcc.txt); b=$(awk '{print $2}' build/bench/$k.gcc0.txt); c=$(awk '{print $2}' build/bench/$k.gcc2.txt)
    sa=$(awk '{print $4}' build/bench/$k.mcc.txt); sb=$(awk '{print $4}' build/bench/$k.gcc0.txt); sc=$(awk '{print $4}' build/bench/$k.gcc2.txt)
    ok=ok; [ "$sa" = "$sb" ] && [ "$sa" = "$sc" ] || ok=CHECKSUM-DIFFERS
    printf "%-7s mcc %6s ms   gcc -O0 %6s ms   gcc -O2 %6s ms   mcc/-O0 %5.2f   mcc/-O2 %5.2f   %s\n" $k $a $b $c \
        $(echo "$a $b" | awk '{printf "%.2f", $1 / ($2 > 0 ? $2 : 1)}') $(echo "$a $c" | awk '{printf "%.2f", $1 / ($2 > 0 ? $2 : 1)}') $ok
done
