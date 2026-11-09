# The shell itself and the text tools of a Linux root: pipelines, redirection, here-documents, command substitution, arithmetic, loops, functions, traps, exit statuses, and sort,
# uniq, tr, cut, sed, grep, wc, head, tail, expr, seq, printf. Run by a POSIX shell (dash and bash) in an empty directory; compared with the same script on a real Linux.
echo "--- pipelines"
echo hello world | tr a-z A-Z
printf 'b\na\nc\na\n' | sort | uniq -c | sort -rn | head -2
seq 1 10 | tail -3 | tr '\n' ' '; echo
seq 5 | wc -l
echo "one two three" | cut -d' ' -f2
printf 'a:b:c\n' | cut -d: -f1,3
seq 1 5 | sed -n '2,4p'
seq 1 3 | sed 's/2/two/'
printf 'apple\nbanana\ncherry\n' | grep an
printf 'apple\nbanana\ncherry\n' | grep -v an | wc -l
printf 'x\ny\n' | tee t.out | wc -l; cat t.out
echo "--- redirection and here-documents"
cat <<EOF
here-document with $((6 * 7)) and $(echo substitution)
EOF
cat <<'EOF'
no expansion of $HOME here
EOF
{ echo first; echo second; } > two.txt
wc -l < two.txt
echo third >> two.txt
cat two.txt
ls nonexistent-file 2> err.txt || true
test -s err.txt && echo "error output went to a file"
echo "to nowhere" > /dev/null
head -c 4 /dev/zero | od -An -tx1
rm -f t.out two.txt err.txt
echo "--- arithmetic, variables and loops"
echo $((1 + 2)) $((7 * 6)) $((10 / 3)) $((10 % 3))
x=5; y=$((x * 2)); echo "x=$x y=$y"
for i in 1 2 3; do printf '%s,' $i; done; echo
i=0; while [ $i -lt 3 ]; do i=$((i + 1)); done; echo "i=$i"
for w in alpha beta; do echo "word $w"; done
case abc in a*) echo "matched a*";; *) echo "no match";; esac
echo "${undefined:-fallback} ${#x}"
f() { echo "function got $1 and $2"; return 4; }
f one two; echo "returned $?"
echo "--- exit statuses"
true; echo "true: $?"
false; echo "false: $?"
(exit 7); echo "subshell: $?"
sh -c 'exit 9'; echo "sh -c: $?"
test 3 -gt 2 && echo "3 > 2"
[ "a" = "a" ] && echo "strings equal"
expr 6 \* 7
expr length hello
echo "--- processes"
sleep 0.1; echo "slept"
( echo in a subshell; ) | cat
echo $$ | grep -q '^[0-9][0-9]*$' && echo "the shell has a process id"
trap 'echo trapped' EXIT
echo "end of the script"
