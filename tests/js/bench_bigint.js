// BigInt benchmark: factorial and Fibonacci numbers of thousands of digits, modular exponentiation of small numbers, pi to
// 1500 digits with Machin's formula.  The checksum line is compared by tests/host/test_js.py with Python's integers.
function fact(n) { var r = 1n; for (var i = 2n; i <= n; i++) r *= i; return r; }
function fib(n) { var a = 0n, b = 1n; for (var i = 0; i < n; i++) { var t = a + b; a = b; b = t; } return a; }
function modpow(b, e, m) {
  var r = 1n;
  b %= m;
  while (e > 0n) { if (e & 1n) r = r * b % m; b = b * b % m; e >>= 1n; }
  return r;
}
function arccot(x, unity) {                       // unity / x - unity / (3 x^3) + ..., in integers
  var sum = unity / x, xpow = sum, n = 3n, sign = -1n, term;
  x *= x;
  do {
    xpow /= x;
    term = xpow / n;
    sum += sign * term;
    sign = -sign;
    n += 2n;
  } while (term !== 0n);
  return sum;
}
function pi(digits) {
  var unity = 10n ** BigInt(digits + 10);
  return (16n * arccot(5n, unity) - 4n * arccot(239n, unity)) / 10n ** 10n;
}
var f = fact(5000n).toString();
var fb = fib(100000).toString();
var mp = 0n;
for (var i = 1n; i <= 20000n; i++) mp += modpow(i, 1000003n, 1000000007n);
var p = pi(1500).toString();
print("bigint", f.length, fb.length, String(mp), p.slice(0, 12), p.slice(-12));
