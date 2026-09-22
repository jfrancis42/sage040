# The string functions.
BEGIN {
    s = "The Quick Brown Fox"
    print substr(s, 5, 5) "|" substr(s, 17) "|" substr(s, 0, 3) "|" substr(s, 50) "|"
    print index(s, "Brown"), index(s, "cat"), length(s), length()
    print toupper(s), tolower(s)
    n = split("a,b;c,,d", parts, /[,;]/)
    print n, parts[1], parts[3], "[" parts[4] "]", parts[5]
    t = "banana"; c = gsub(/an/, "AN", t); print c, t
    t = "banana"; c = sub(/a/, "[&]", t); print c, t
    t = "aaa"; gsub(/a/, "\\&", t); print t
    print match("foo123bar", /[0-9]+/), RSTART, RLENGTH
    print match("foobar", /z/), RSTART, RLENGTH
    print sprintf("%5s|%-5s|%.2s", "ab", "cd", "efgh")
}
