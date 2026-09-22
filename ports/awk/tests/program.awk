# User functions, recursion, locals, arrays by reference, control flow.
function fact(n) { return n <= 1 ? 1 : n * fact(n - 1) }
function fill(a, n,    i) { for (i = 1; i <= n; i++) a[i] = i * i }
function fib(n,   a, b, t) { a = 0; b = 1; while (n-- > 0) { t = a + b; a = b; b = t } return a }
BEGIN {
    print fact(10), fact(20), fib(30)
    fill(sq, 5); print sq[3], sq[5], length(sq)
    for (i = 0; i < 10; i++) { if (i == 2) continue; if (i == 5) break; out = out i }
    print out
    do { k++ } while (k < 3); print k
    s = ""; for (i = 1; i <= 2000; i++) s = s "x"; print length(s)
    x = 0; while (x < 100000) x++; print x
    printf "%s %s\n", (1 ? "yes" : "no"), (0 ? "yes" : "no")
    exit 4
}
END { print "END runs after exit" }
