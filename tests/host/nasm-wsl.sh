#!/bin/sh
# nasm for a WSL shell that has none of its own: runs the Windows nasm.exe (the one tools/build.py uses), turning the paths among the arguments into Windows paths.
# Used through TOUHOU_NASM by tools/hosttest.py on a Windows PC whose WSL Ubuntu has gcc but no nasm. Anything that is not an existing path (or a file in an existing folder) is passed on as it is.
exe="${TOUHOU_NASM_EXE:-}"
if [ -z "$exe" ]; then
    for cand in /mnt/c/Users/*/AppData/Local/bin/NASM/nasm.exe; do
        [ -f "$cand" ] && exe="$cand" && break
    done
fi
[ -n "$exe" ] || { echo "nasm-wsl: no nasm.exe found (set TOUHOU_NASM_EXE)" >&2; exit 127; }
for a in "$@"; do
    case "$a" in
        /*) if [ -e "$a" ] || [ -d "$(dirname "$a")" ]; then a="$(wslpath -w "$a")"; fi ;;
    esac
    set -- "$@" "$a"
    shift
done
exec "$exe" "$@"
