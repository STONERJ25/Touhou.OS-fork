// benchmark: recursive calls and integer arithmetic
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
print("fib", fib(27));
