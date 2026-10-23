# bc and dc

Two kappabox applets for arbitrary precision arithmetic, sharing one bignum core.

## Files

- `user/bin/kappabox/bignum.c`, `bignum.h`: decimal numbers as sign + scale + magnitude in base 10^9 limbs (not an applet).
  Add, subtract, multiply (schoolbook), divide (Knuth D), bc-style `%`, power, square root (Newton), modular exponentiation,
  parsing and printing in bases 2 and up. Scale rules follow GNU bc.
- `user/bin/kappabox/bc.c`: the bc language (tree-walking interpreter, one top-level statement parsed and run at a time) and
  the math library (`-l`), which is bc source in a string at the bottom of the file.
- `user/bin/kappabox/dc.c`: the dc stack machine; macros called as the last command of a macro reuse the frame (tail calls).
- Manual pages `bc.pw`, `dc.pw`; tests in `tests/case_bc.py` (`bc_language`, `bc_mathlib`, `dc_stack_machine`,
  `bc_dc_manual_pages`).

## State

Done: everything in the task list for both tools. Last full run in the guest (one text-mode session, before the final commit):
the math table (20 checks) and the dc table (71 checks) passed completely, the bc table passed 169 of 169 apart from the
banner checks that were fixed right after (the banner is now only printed when the program is read from a terminal).

Half done / not verified: the last change was shortening one example line in `bc.pw` (the manual checker rejected a verbatim
line wider than 78 columns); that fix was not run in the guest. After it the `bc_language` table has not been re-run as a whole.
Not covered by tests: `print` escapes other than `\n`, `limits`, `-f`, `read()`, obase above 99.

Known differences from GNU:

- Runtime errors show `adr=` as the number of saved locals, not a bytecode address.
- Unary minus binds tighter than `^` as in the GNU grammar (`-2^2` is 4).
- `length()` counts the integer part as at least one digit (`length(.5)` is 2).
- Fractions in a non-decimal `ibase` are truncated to as many decimal digits as they have digits.
- `4*a(1)` is 4 times the truncated arc tangent, as in GNU bc: at scale 20 it prints 3.14159265358979323844.
- dc strings have no escapes; `R` drops the top of the stack; error text is `dc: division by zero` and `dc: remainder by zero`.
- A deep recursion limit (about 6 MiB of C stack) turns into a runtime error instead of a crash.

## Resuming

`python tools/build.py`, then `python tests/run_all.py bc_` (all four tests, about 4 minutes). `python tests/run_all.py manual`
needs the graphical boot to reach a shell prompt on the serial line; in the session that wrote this it timed out before the
prompt (no relation to the bc/dc pages, `patchouli --check-all` in `bc_dc_manual_pages` checks them in text mode instead).
Ideas not done: Karatsuba multiplication, faster `l()`/`e()` for very high scales, `ibase` above 16 in dc, `Y` and `e` dc commands.
