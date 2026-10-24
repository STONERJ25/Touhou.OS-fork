#!/bin/bash
# Builds libgap.a with the host mcc (everything except math.c (x87 long double and unsigned __int128), fpconv.c (unsigned
# __int128, which mcc does not have) and setjmp.asm: the objects of the normal build are taken for those three)
# and puts it, with the other libraries, into the system root build/mcc-root2.   bash tools/mcc_build_libgap.sh
cd "$(dirname "$0")/.."
set -e
export MCC_ROOT="${MCC_ROOT:-$PWD/build/mcc-root}"
out=build/mcc-libgap
rm -rf $out && mkdir -p $out
for f in user/libc/src/*.c; do
    n=$(basename $f .c)
    if [ "$n" = "math" ] || [ "$n" = "fpconv" ]; then
        cp build/obj/libc/user/libc/src/$n.c.o $out/$n.o
        continue
    fi
    build/host/mcc -c -I user/libc/src "$f" -o $out/$n.o
done
cp build/obj/libc/user/libc/src/setjmp.asm.o $out/setjmp.o     # written in assembly (nasm)
rm -f $out/libgap.a
build/host/mar rcs $out/libgap.a $out/*.o
rm -rf build/mcc-root2
cp -r build/mcc-root build/mcc-root2
cp $out/libgap.a build/mcc-root2/lib/libgap.a
echo "built $out/libgap.a ($(ls $out/*.o | wc -l) objects), system root build/mcc-root2"
