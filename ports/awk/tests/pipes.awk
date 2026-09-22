# Commands: getline from a pipe, print to a pipe, system(), close().
BEGIN {
    "echo piped-in" | getline x
    print "got", x
    close("echo piped-in")
    while (("echo one; echo two" | getline line) > 0) n++
    print "lines", n
    print "via cat" | "cat"
    close("cat")
    r = system("echo from-system")
    print "system returned", r
    r = system("exit 3")
    print "exit status", r
}
