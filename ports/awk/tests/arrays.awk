# Associative arrays, in, delete, SUBSEP; key order printed sorted by
# hand, since for-in order is unspecified.
{ count[$1]++; total[$1] += $2; grid[NR, $1] = $2 }
END {
    n = 0
    for (k in count) keys[++n] = k
    for (i = 2; i <= n; i++)
        for (j = i; j > 1 && keys[j - 1] > keys[j]; j--) {
            t = keys[j]; keys[j] = keys[j - 1]; keys[j - 1] = t
        }
    for (i = 1; i <= n; i++) print keys[i], count[keys[i]], total[keys[i]]
    print ("apple" in count), ("kiwi" in count)
    delete count["apple"]
    print ("apple" in count), length(count)
    print ((2, "pear") in grid), grid[2, "pear"]
    for (k in grid) { split(k, p, SUBSEP); if (p[2] == "plum") print "plum at", p[1] }
}
