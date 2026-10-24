#!/bin/bash
# Builds the whole userland (the libraries and every program of user/bin and user/sbin) with the host mcc into build/mcc-user/
# and writes the system root build/mcc-root3 whose libraries are the mcc-built ones.  The programs keep their names:
#   build/mcc-user/bin/NAME, build/mcc-user/sbin/NAME     (the mcc_userland test of tests/case_mcc.py puts them into the initrd)
#   bash tools/mcc_build_userland.sh
cd "$(dirname "$0")/.."
export MCC_ROOT="${MCC_ROOT:-$PWD/build/mcc-root}"
MCC=build/host/mcc
MAR=build/host/mar
out=build/mcc-user
rm -rf $out && mkdir -p $out/bin $out/sbin $out/lib $out/obj
fail=0

# libc: everything but math.c, fpconv.c and setjmp.asm (x87 long double, unsigned __int128, which mcc does not have, and
# assembly: the clang objects are used)
mkdir -p $out/obj/libc
for f in user/libc/src/*.c; do
    n=$(basename $f .c)
    if [ "$n" = math ] || [ "$n" = fpconv ]; then cp build/obj/libc/user/libc/src/$n.c.o $out/obj/libc/$n.o; continue; fi
    $MCC -c -I user/libc/src $f -o $out/obj/libc/$n.o || fail=1
done
cp build/obj/libc/user/libc/src/setjmp.asm.o $out/obj/libc/setjmp.o
$MAR rcs $out/lib/libgap.a $out/obj/libc/*.o
cp build/obj/libc/user/libc/crt0.asm.o $out/lib/crt0.o

# the other libraries
libincs=""
for d in user/lib/*/include; do libincs="$libincs -I $d"; done
for lib in kappa fairy audio img z; do
    mkdir -p $out/obj/$lib
    srcs=$(find user/lib/$lib/src -name '*.c' | sort)
    if [ "$lib" = kappa ]; then srcs="$srcs shared/font8x16.c shared/yinyang.c"; fi
    for f in $srcs; do
        $MCC -c $libincs $f -o $out/obj/$lib/$(basename $f .c).o || fail=1
    done
    $MAR rcs $out/lib/lib$lib.a $out/obj/$lib/*.o
done

# a system root with these libraries
rm -rf build/mcc-root3 && cp -r build/mcc-root build/mcc-root3
cp $out/lib/*.a build/mcc-root3/lib/
cp $out/lib/crt0.o build/mcc-root3/lib/

# the programs
for kind in bin sbin; do
    for e in user/$kind/*; do
        name=$(basename $e)
        if [ -f "$e" ]; then
            case "$e" in *.c) ;; *) continue ;; esac
            name=${name%.c}
            srcs=$e; dir=user/$kind
        else
            srcs=$(find $e -name '*.c' | sort)
            [ -z "$srcs" ] && continue
            dir=$e
            if [ "$name" = kappabox ]; then srcs="$srcs build/gen/kappabox_table.c"; fi
        fi
        # -I shared as in tools/build.py (fsck compiles shared/fatck.c); libtls stays the clang build (AES-NI intrinsics)
        if ! MCC_ROOT=$PWD/build/mcc-root3 $MCC -I $dir -I shared $srcs -lkappa -lfairy -laudio -limg -lz -ltls -o $out/$kind/$name 2> $out/$name.err; then
            echo "FAIL $kind/$name: $(grep -m1 error $out/$name.err)"
            fail=1
        fi
    done
done
echo "built $(ls $out/bin $out/sbin | grep -vc ':') programs"
exit $fail
