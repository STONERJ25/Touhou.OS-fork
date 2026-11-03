#!/bin/bash
# Runs a list of shell command lines in parallel (WSL / Linux):  bash tools/mcc_jobs.sh JOBFILE [PARALLELISM]
# Every line of JOBFILE is one command (plain words, no quoting tricks: tools/mcc_userland.py writes them); the exit status
# is the number of lines that failed, and every failing line is printed with the first lines of what it said.  Used to compile
# the userland with the host mcc in a fraction of the time one process needs (the files are independent translation units).
cd "$(dirname "$0")/.." || exit 99
jobs_file="$1"
par="${2:-$(( $(nproc) > 2 ? $(nproc) - 2 : 1 ))}"
[ -f "$jobs_file" ] || { echo "mcc_jobs: no job file '$jobs_file'" >&2; exit 99; }
export fails="$jobs_file.failed"
rm -f "$fails"
run_one() {
    local out
    if ! out=$(sh -c "$1" 2>&1); then
        { echo "FAIL: $1"; echo "$out" | head -8; } >> "$fails"
    fi
}
export -f run_one
xargs -a "$jobs_file" -d '\n' -P "$par" -I{} bash -c 'run_one "$1"' _ {}
if [ -s "$fails" ]; then
    cat "$fails"
    exit "$(grep -c '^FAIL: ' "$fails")"
fi
exit 0
