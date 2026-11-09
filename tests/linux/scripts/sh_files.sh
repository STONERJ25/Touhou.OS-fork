# Files and directories through the programs of a Linux root (ls, cp, mv, ln, rm, mkdir, touch, chmod, stat, cat, readlink ...). Run by a POSIX shell in an empty directory; the
# output is compared with the same script on a real Linux (tools/linux-oracle.py, tests/case_linux_dyn.py). It prints names, kinds, permission bits and sizes of files only:
# owners, inode numbers, link counts of directories and times differ between file systems and are not facts of the programs.
set -e
mkdir -p tree/a/b tree/c
cd tree
printf 'hello world\n' > f1
printf 'c\nb\na\nb\n' > f2
cp f1 f1copy
cp -p f2 a/f2p
mv f1copy a/moved
ln f1 hard1
ln -s f1 soft1
ln -s /nowhere dangling
touch a/b/t1
chmod 600 f1
chmod u+x,g-r f2
chmod 755 c

echo "--- listing"
ls
ls -a
ls -1 a
ls -R | head -20
echo "--- kinds and permissions"
stat -c '%A %n' *
stat -c '%A %n' a/*
stat -c '%n %F %a' f1 f2 soft1 a c
stat -L -c '%n -> %F' soft1
echo "--- links"
readlink soft1
readlink dangling
test -L soft1 && echo "soft1 is a symbolic link"
test -e dangling || echo "dangling points nowhere"
test f1 -ef hard1 && echo "f1 and hard1 are the same file"
cat soft1 hard1
echo "--- read and write"
cat f2
tac f2
sort f2 | uniq
head -c 5 f1; echo
tail -c 6 f1
wc -c f1
wc -l f2
od -An -c f1 | head -1
dd if=f1 of=dd.out bs=4 count=2 2>/dev/null
cat dd.out; echo
echo "--- moving and removing"
mv a/moved a/b/moved2
ls a a/b
rm -f hard1 dangling
ls
rm -r a
ls
rmdir c
ls
mkdir d && rmdir d && echo "made and removed d"
rm nonexistent 2>/dev/null || echo "rm of a missing file fails ($?)"
cat nonexistent 2>/dev/null || echo "cat of a missing file fails ($?)"
ls nonexistent 2>/dev/null || echo "ls of a missing file fails ($?)"
mkdir f1 2>/dev/null || echo "mkdir of an existing name fails ($?)"
cd ..
rm -r tree
echo "--- done"
