# Enough work to notice: 20000 records made and summed, and a table.
BEGIN {
    for (i = 1; i <= 20000; i++) { sum += i; words[i % 97]++ }
    print sum
    for (k = 0; k < 97; k += 24) print k, words[k]
    s = sprintf("%0500d", 0); gsub(/0/, "ab", s); print length(s)
}
